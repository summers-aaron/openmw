// Generator for a minimal, bootable .omwgame used to runtime-test the headless
// dedicated server. Run explicitly:
//   openmw-tests --gtest_also_run_disabled_tests --gtest_filter='*GenerateBootData*'
// It writes <build>/testdata/min.omwgame. This is content authoring, not a unit
// test, hence DISABLED_ so it never runs in the normal suite.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include <components/esm/defs.hpp>
#include <components/esm/refid.hpp>
#include <components/esm3/variant.hpp>
#include <components/esm3/esmwriter.hpp>
#include <components/esm3/loadclas.hpp>
#include <components/esm3/loadcell.hpp>
#include <components/esm3/loadglob.hpp>
#include <components/esm3/loadgmst.hpp>
#include <components/esm3/loadmgef.hpp>
#include <components/esm3/loadnpc.hpp>
#include <components/esm3/loadrace.hpp>
#include <components/esm3/loadskil.hpp>

#include "apps/opencs/model/world/defaultgmsts.hpp"

namespace
{
    template <class Record>
    void writeRecord(ESM::ESMWriter& writer, const Record& record)
    {
        writer.startRecord(Record::sRecordId);
        record.save(writer);
        writer.endRecord(Record::sRecordId);
    }

    ESM::RefId id(std::string_view s) { return ESM::RefId::stringRefId(s); }

    TEST(BootDataGenTest, DISABLED_GenerateBootData)
    {
        const std::filesystem::path outDir = std::filesystem::path("testdata");
        std::filesystem::create_directories(outDir);
        const std::filesystem::path outPath = outDir / "min.omwgame";

        std::ofstream stream(outPath, std::ios::binary);
        ASSERT_TRUE(stream.is_open());

        ESM::ESMWriter writer;
        writer.setAuthor("genbootdata");
        writer.setDescription("minimal bootable test game");
        writer.setVersion();
        writer.clearMaster();
        writer.setFormatVersion(ESM::DefaultFormatVersion);
        writer.setRecordCount(0); // header record count is advisory for OpenMW
        writer.save(stream);

        // --- GMSTs (the engine looks many of these up by name and throws if missing) ---
        using namespace CSMWorld;
        for (size_t i = 0; i < DefaultGmsts::FloatCount; ++i)
        {
            ESM::GameSetting g;
            g.blank();
            g.mId = id(DefaultGmsts::Floats[i]);
            g.mValue.setType(ESM::VT_Float);
            g.mValue.setFloat(DefaultGmsts::FloatsDefaultValues[i]);
            writeRecord(writer, g);
        }
        for (size_t i = 0; i < DefaultGmsts::IntCount; ++i)
        {
            ESM::GameSetting g;
            g.blank();
            g.mId = id(DefaultGmsts::Ints[i]);
            g.mValue.setType(ESM::VT_Int);
            g.mValue.setInteger(DefaultGmsts::IntsDefaultValues[i]);
            writeRecord(writer, g);
        }
        for (size_t i = 0; i < DefaultGmsts::StringCount; ++i)
        {
            ESM::GameSetting g;
            g.blank();
            g.mId = id(DefaultGmsts::Strings[i]);
            g.mValue.setType(ESM::VT_String);
            g.mValue.setString("");
            writeRecord(writer, g);
        }

        // --- Skills, magic effects, globals ---
        for (int i = 0; i < ESM::Skill::Length; ++i)
        {
            ESM::Skill s;
            s.blank();
            s.mId = *ESM::Skill::indexToRefId(i).getIf<ESM::SkillId>();
            writeRecord(writer, s);
        }
        for (int i = 0; i < ESM::MagicEffect::Length; ++i)
        {
            ESM::MagicEffect e;
            e.blank();
            e.mId = ESM::MagicEffect::indexToRefId(i);
            writeRecord(writer, e);
        }
        for (const char* g : { "Day", "DaysPassed", "GameHour", "Month", "PCRace", "PCVampire", "PCWerewolf", "PCYear" })
        {
            ESM::Global global;
            global.mRecordFlags = 0;
            global.mId = id(g);
            global.mValue.setType(ESM::VT_Long);
            writeRecord(writer, global);
        }

        // --- One playable race ---
        ESM::Race race;
        race.blank();
        race.mId = id("TestRace");
        race.mName = "Test Race";
        race.mData.mAttributeValues.fill(50);
        race.mData.mFlags = 0x01; // Playable
        writeRecord(writer, race);

        // --- One playable class ---
        ESM::Class clas;
        clas.blank();
        clas.mId = id("TestClass");
        clas.mName = "Test Class";
        clas.mData.mIsPlayable = 1;
        writeRecord(writer, clas);

        // --- The Player NPC ---
        ESM::NPC npc;
        npc.blank();
        npc.mId = id("Player");
        npc.mName = "Player";
        npc.mRace = id("TestRace");
        npc.mClass = id("TestClass");
        npc.mNpdtType = ESM::NPC::NPC_DEFAULT;
        npc.mNpdt.mLevel = 1;
        npc.mNpdt.mAttributes.fill(50);
        npc.mNpdt.mSkills.fill(30);
        npc.mNpdt.mHealth = 100;
        npc.mNpdt.mMana = 100;
        npc.mNpdt.mFatigue = 100;
        writeRecord(writer, npc);

        // --- One interior cell to start in ---
        ESM::Cell cell;
        cell.blank();
        cell.mName = "TestCell";
        cell.mId = id("TestCell");
        cell.mData.mFlags = ESM::Cell::Interior;
        writeRecord(writer, cell);

        writer.close();
        stream.close();

        ASSERT_TRUE(std::filesystem::exists(outPath));
        EXPECT_GT(std::filesystem::file_size(outPath), 0u);
        std::cerr << "Wrote " << std::filesystem::absolute(outPath) << " ("
                  << std::filesystem::file_size(outPath) << " bytes)\n";
    }
}
