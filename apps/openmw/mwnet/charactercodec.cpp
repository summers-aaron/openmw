#include "charactercodec.hpp"

#include <memory>
#include <sstream>

#include <components/esm/defs.hpp>
#include <components/esm3/esmreader.hpp>
#include <components/esm3/esmwriter.hpp>
#include <components/esm3/formatversion.hpp>
#include <components/esm3/loadnpc.hpp>
#include <components/esm3/player.hpp>
#include <components/esm3/savedgame.hpp>
#include <components/loadinglistener/loadinglistener.hpp>

#include "../mwbase/dialoguemanager.hpp"
#include "../mwbase/journal.hpp"

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

    std::string serializeCharacterSave(const ESM::Player& player, const ESM::SavedGame& profile,
        const std::vector<std::string>& contentFiles, const ESM::NPC* baseRecord)
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
        writer.setRecordCount(baseRecord ? 3 : 2); // REC_SAVE + [REC_NPC_] + REC_PLAY

        writer.save(stream);
        writer.startRecord(ESM::REC_SAVE);
        profile.save(writer);
        writer.endRecord(ESM::REC_SAVE);
        // The character's dynamic NPC record must land in the store before the player record that
        // references it (mBaseRecord), exactly as a real save orders its dynamic records first.
        if (baseRecord)
        {
            writer.startRecord(ESM::REC_NPC_);
            baseRecord->save(writer);
            writer.endRecord(ESM::REC_NPC_);
        }
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

    std::string serializeJournal(const MWBase::Journal& journal, const MWBase::DialogueManager& dialogue,
        const std::vector<std::string>& contentFiles)
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
        writer.setRecordCount(
            static_cast<int>(journal.countSavedGameRecords() + dialogue.countSavedGameRecords()));
        writer.save(stream);

        // Exactly the journal-side records a real save writes: quests and journal entries
        // (REC_QUES / REC_JOUR), then the known dialogue topics (REC_DIAS).
        Loading::Listener progress; // no-op: an in-memory stream, not a loading screen
        journal.write(writer, progress);
        dialogue.write(writer, progress);
        writer.close();

        return stream.str();
    }

    bool restoreJournal(MWBase::Journal& journal, MWBase::DialogueManager& dialogue, const std::string& blob)
    {
        // Start from empty either way: an adopted character must not inherit whatever journal the
        // receiving session had.
        journal.clear();
        dialogue.clear();
        try
        {
            ESM::ESMReader reader;
            reader.open(std::make_unique<std::istringstream>(blob), "<network journal>");
            while (reader.hasMoreRecs())
            {
                const ESM::NAME name = reader.getRecName();
                reader.getRecHeader();
                switch (name.toInt())
                {
                    case ESM::REC_JOUR:
                    case ESM::REC_QUES:
                        journal.readRecord(reader, name.toInt());
                        break;
                    case ESM::REC_DIAS:
                        dialogue.readRecord(reader, name.toInt());
                        break;
                    default:
                        reader.skipRecord();
                }
            }
            return true;
        }
        catch (const std::exception&)
        {
            // Malformed blob: leave the fresh empty state rather than something half-restored.
            journal.clear();
            dialogue.clear();
            return false;
        }
    }
}
