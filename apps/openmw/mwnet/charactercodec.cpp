#include "charactercodec.hpp"

#include <memory>
#include <sstream>

#include <components/esm/defs.hpp>
#include <components/esm3/esmreader.hpp>
#include <components/esm3/esmwriter.hpp>
#include <components/esm3/formatversion.hpp>
#include <components/esm3/player.hpp>
#include <components/esm3/savedgame.hpp>

namespace MWNet
{
    std::string serializeCharacter(const ESM::Player& player, const std::vector<std::string>& contentFiles)
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

        // Exactly the record StateManager writes per player, minus the PLIX slot index; on receipt it
        // feeds straight into MWWorld::Player::readRecord.
        writer.startRecord(ESM::REC_PLAY);
        player.save(writer);
        writer.endRecord(ESM::REC_PLAY);
        writer.close();

        return stream.str();
    }

    std::string serializeCharacterSave(
        const ESM::Player& player, const ESM::SavedGame& profile, const std::vector<std::string>& contentFiles)
    {
        std::ostringstream stream;

        ESM::ESMWriter writer;
        for (const std::string& contentFile : contentFiles)
            writer.addMaster(contentFile, 0);
        writer.setFormatVersion(ESM::CurrentSaveGameFormatVersion);
        writer.setVersion(0);
        writer.setType(0);
        writer.setAuthor("");
        writer.setDescription("");
        writer.setRecordCount(2); // REC_SAVE + REC_PLAY

        writer.save(stream);
        writer.startRecord(ESM::REC_SAVE);
        profile.save(writer);
        writer.endRecord(ESM::REC_SAVE);
        writer.startRecord(ESM::REC_PLAY);
        player.save(writer);
        writer.endRecord(ESM::REC_PLAY);
        writer.close();

        return stream.str();
    }

    std::unique_ptr<ESM::Player> deserializeCharacter(const std::string& blob)
    {
        try
        {
            ESM::ESMReader reader;
            reader.open(std::make_unique<std::istringstream>(blob), "<network character>");
            while (reader.hasMoreRecs())
            {
                const ESM::NAME name = reader.getRecName();
                reader.getRecHeader();
                if (name.toInt() == ESM::REC_PLAY)
                {
                    auto player = std::make_unique<ESM::Player>();
                    player->load(reader);
                    return player;
                }
                reader.skipRecord();
            }
        }
        catch (const std::exception&)
        {
            // Truncated/corrupt blob, or one built against different data — reject cleanly.
        }
        return nullptr;
    }
}
