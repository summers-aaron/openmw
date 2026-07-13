#include "cellstatecodec.hpp"

#include <memory>
#include <sstream>

#include <components/debug/debuglog.hpp>
#include <components/esm/defs.hpp>
#include <components/esm3/cellstate.hpp>
#include <components/esm3/esmreader.hpp>
#include <components/esm3/esmwriter.hpp>
#include <components/esm3/formatversion.hpp>

#include "../mwbase/environment.hpp"

#include "../mwworld/cellstore.hpp"
#include "../mwworld/worldmodel.hpp"

#include "snapshot.hpp"

namespace MWNet
{
    namespace
    {
        // Refs a cell blob must not carry: they are owned end-to-end by the live replication
        // channels, and the receiving side (readReferences' new-ref branch) would duplicate them.
        // Summons (-3000) are effect-bound and instantiated from spawn descriptors; avatars (-2000)
        // never sit in a cell's ref lists at all, but are excluded defensively. Reserved spawns
        // (-3001) ARE carried — they are durable world actors, and the descriptor path adopts an
        // already-present RefNum instead of duplicating it.
        bool skipLiveNetworkRef(ESM::RefNum refNum)
        {
            return refNum.mContentFile == sNetworkSummonRefNumContentFile
                || refNum.mContentFile == sNetworkPlayerRefNumContentFile;
        }
    }

    std::string serializeCellState(MWWorld::CellStore& cell, const std::vector<std::string>& contentFiles)
    {
        std::ostringstream stream;

        ESM::ESMWriter writer;
        for (const std::string& contentFile : contentFiles)
            writer.addMaster(contentFile, 0); // size is unused, as in StateManager::saveGame
        writer.setFormatVersion(ESM::CurrentSaveGameFormatVersion);
        writer.setVersion(0);
        writer.setType(0);
        writer.setAuthor("");
        writer.setDescription("");
        writer.setRecordCount(1);
        writer.save(stream);

        // writeCell's precondition: the ref lists must exist. A CellStore load is cheap (content
        // refs + any save state already read at boot) and needs no scene, so the host can serve
        // cells it has never itself activated.
        if (cell.getState() != MWWorld::CellStore::State_Loaded)
            cell.load();

        ESM::CellState state;
        cell.saveState(state);
        state.mHasFogOfWar = 0; // per-player exploration, never shared

        // Exactly WorldModel::writeCell, minus writeFog (matching mHasFogOfWar above) and with the
        // live-network-ref filter on the reference stream.
        writer.startRecord(ESM::REC_CSTA);
        writer.writeCellId(state.mId);
        state.save(writer);
        cell.writeReferences(writer, skipLiveNetworkRef);
        writer.endRecord(ESM::REC_CSTA);
        writer.close();

        return stream.str();
    }

    bool applyCellState(const std::string& blob)
    {
        try
        {
            ESM::ESMReader reader;
            reader.open(std::make_unique<std::istringstream>(blob), "<network cell state>");
            MWWorld::WorldModel& worldModel = *MWBase::Environment::get().getWorldModel();
            bool applied = false;
            while (reader.hasMoreRecs())
            {
                const ESM::NAME name = reader.getRecName();
                reader.getRecHeader();
                if (name.toInt() == ESM::REC_CSTA)
                    applied = worldModel.readRecord(reader, ESM::REC_CSTA) || applied;
                else
                    reader.skipRecord();
            }
            return applied;
        }
        catch (const std::exception& e)
        {
            // Truncated/corrupt blob, or one built against different data — reject cleanly.
            Log(Debug::Warning) << "Rejecting malformed cell-state blob: " << e.what();
            return false;
        }
    }
}
