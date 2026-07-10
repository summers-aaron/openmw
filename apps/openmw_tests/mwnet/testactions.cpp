#include <gtest/gtest.h>

#include <random>

#include "apps/openmw/mwnet/actions.hpp"

namespace MWNet
{
    namespace
    {
        TEST(MWNetActionsTest, emptyBatchRoundTrips)
        {
            const ActionBatch batch;
            const std::optional<ActionBatch> parsed = deserializeActions(serializeActions(batch));
            ASSERT_TRUE(parsed.has_value());
            EXPECT_EQ(*parsed, batch);
        }

        TEST(MWNetActionsTest, hitsRoundTrip)
        {
            ActionBatch batch;
            batch.mHits.push_back({ ESM::RefNum{ 1, -1000 }, ESM::RefNum{ 42, 0 }, 13.5f, true });
            batch.mHits.push_back({ ESM::RefNum{ 0, -1000 }, ESM::RefNum{ 0xffffff, 5 }, 4.f, false });
            const std::optional<ActionBatch> parsed = deserializeActions(serializeActions(batch));
            ASSERT_TRUE(parsed.has_value());
            EXPECT_EQ(*parsed, batch);
            ASSERT_EQ(parsed->mHits.size(), 2u);
            EXPECT_EQ(parsed->mHits[0].mDamage, 13.5f);
            EXPECT_TRUE(parsed->mHits[0].mHealthDamage);
            EXPECT_FALSE(parsed->mHits[1].mHealthDamage);
        }

        TEST(MWNetActionsTest, playerDamagesRoundTrip)
        {
            ActionBatch batch;
            batch.mHits.push_back({ ESM::RefNum{ 7, -1000 }, ESM::RefNum{ 9, 0 }, 5.f, true });
            batch.mPlayerDamages.push_back({ ESM::RefNum{ 3, -1000 }, 8.25f, true });
            batch.mPlayerDamages.push_back({ ESM::RefNum{ 99, -1000 }, 2.f, false });
            const std::optional<ActionBatch> parsed = deserializeActions(serializeActions(batch));
            ASSERT_TRUE(parsed.has_value());
            EXPECT_EQ(*parsed, batch);
            ASSERT_EQ(parsed->mPlayerDamages.size(), 2u);
            EXPECT_EQ(parsed->mPlayerDamages[0].mDamage, 8.25f);
            EXPECT_FALSE(parsed->mPlayerDamages[1].mHealthDamage);
        }

        TEST(MWNetActionsTest, dropsAndTakesRoundTrip)
        {
            ActionBatch batch;
            batch.mDrops.push_back({ "iron_dagger", 1, osg::Vec3f(10.f, -20.f, 30.5f), "Balmora, Guild of Mages" });
            batch.mDrops.push_back({ "gold_001", 250, osg::Vec3f(0.f, 0.f, 0.f), "" });
            batch.mItemsTaken.push_back(ESM::RefNum{ 5, -2 });
            batch.mItemsTaken.push_back(ESM::RefNum{ 99, -2 });
            const std::optional<ActionBatch> parsed = deserializeActions(serializeActions(batch));
            ASSERT_TRUE(parsed.has_value());
            EXPECT_EQ(*parsed, batch);
            ASSERT_EQ(parsed->mDrops.size(), 2u);
            EXPECT_EQ(parsed->mDrops[0].mRefId, "iron_dagger");
            EXPECT_EQ(parsed->mDrops[1].mCount, 250);
            EXPECT_EQ(parsed->mDrops[0].mPosition, osg::Vec3f(10.f, -20.f, 30.5f));
            ASSERT_EQ(parsed->mItemsTaken.size(), 2u);
            EXPECT_EQ(parsed->mItemsTaken[1].mIndex, 99u);
        }

        TEST(MWNetActionsTest, containersRoundTrip)
        {
            ActionBatch batch;
            batch.mContainers.push_back({ ESM::RefNum{ 12, 0 },
                { { "gold_001", 137 }, { "iron_dagger", 1, 250, -1.f, "" },
                    { "misc_soulgem_common", 1, -1, -1.f, "rat" }, { "potion_cyrodilic_brandy_01", 3 } } });
            batch.mContainers.push_back({ ESM::RefNum{ 99, -2 }, {} }); // emptied container
            const std::optional<ActionBatch> parsed = deserializeActions(serializeActions(batch));
            ASSERT_TRUE(parsed.has_value());
            EXPECT_EQ(*parsed, batch);
            ASSERT_EQ(parsed->mContainers.size(), 2u);
            ASSERT_EQ(parsed->mContainers[0].mItems.size(), 4u);
            EXPECT_EQ(parsed->mContainers[0].mItems[0].mRefId, "gold_001");
            EXPECT_EQ(parsed->mContainers[0].mItems[0].mCount, 137);
            EXPECT_EQ(parsed->mContainers[0].mItems[1].mCharge, 250); // weapon condition preserved
            EXPECT_EQ(parsed->mContainers[0].mItems[2].mSoul, "rat"); // soul gem soul preserved
            EXPECT_TRUE(parsed->mContainers[1].mItems.empty());
        }

        TEST(MWNetActionsTest, containerChangesAndRevokesRoundTrip)
        {
            ActionBatch batch;
            batch.mContainerChanges.push_back(
                { ESM::RefNum{ 3, -1000 }, ESM::RefNum{ 50, 0 }, { "gold_001", 40 }, /*take=*/true });
            batch.mContainerChanges.push_back(
                { ESM::RefNum{ 3, -1000 }, ESM::RefNum{ 50, 0 }, { "iron_dagger", 1, 200, -1.f, "" }, false });
            batch.mContainerRevokes.push_back({ ESM::RefNum{ 7, -1000 }, { "gold_001", 15 } });
            const std::optional<ActionBatch> parsed = deserializeActions(serializeActions(batch));
            ASSERT_TRUE(parsed.has_value());
            EXPECT_EQ(*parsed, batch);
            ASSERT_EQ(parsed->mContainerChanges.size(), 2u);
            EXPECT_TRUE(parsed->mContainerChanges[0].mTake);
            EXPECT_FALSE(parsed->mContainerChanges[1].mTake);
            EXPECT_EQ(parsed->mContainerChanges[1].mItem.mCharge, 200);
            ASSERT_EQ(parsed->mContainerRevokes.size(), 1u);
            EXPECT_EQ(parsed->mContainerRevokes[0].mItem.mCount, 15);
        }

        TEST(MWNetActionsTest, summonsRoundTrip)
        {
            ActionBatch batch;
            batch.mSummons.push_back({ ESM::RefNum{ 3, -1000 }, "summon scamp", /*end=*/false });
            batch.mSummons.push_back({ ESM::RefNum{ 3, -1000 }, "summon scamp", /*end=*/true });
            const std::optional<ActionBatch> parsed = deserializeActions(serializeActions(batch));
            ASSERT_TRUE(parsed.has_value());
            EXPECT_EQ(*parsed, batch);
            ASSERT_EQ(parsed->mSummons.size(), 2u);
            EXPECT_EQ(parsed->mSummons[0].mEffectId, "summon scamp");
            EXPECT_FALSE(parsed->mSummons[0].mEnd);
            EXPECT_TRUE(parsed->mSummons[1].mEnd);
        }

        TEST(MWNetActionsTest, bountiesRoundTrip)
        {
            ActionBatch batch;
            batch.mBounties.push_back({ ESM::RefNum{ 7, -1000 }, 40 });
            batch.mBounties.push_back({ ESM::RefNum{ 8, -1000 }, 0 });
            const std::optional<ActionBatch> parsed = deserializeActions(serializeActions(batch));
            ASSERT_TRUE(parsed.has_value());
            EXPECT_EQ(*parsed, batch);
            ASSERT_EQ(parsed->mBounties.size(), 2u);
            EXPECT_EQ(parsed->mBounties[0].mBounty, 40);
            EXPECT_EQ(parsed->mBounties[1].mBounty, 0);
        }

        TEST(MWNetActionsTest, speechRoundTrips)
        {
            ActionBatch batch;
            batch.mSpeech.push_back({ ESM::RefNum{ 42, 0 }, "Vo\\Misc\\hello.mp3", "Greetings, outlander." });
            batch.mSpeech.push_back({ ESM::RefNum{ 0xffffff, 3 }, "", "" });
            const std::optional<ActionBatch> parsed = deserializeActions(serializeActions(batch));
            ASSERT_TRUE(parsed.has_value());
            EXPECT_EQ(*parsed, batch);
            ASSERT_EQ(parsed->mSpeech.size(), 2u);
            EXPECT_EQ(parsed->mSpeech[0].mActor.mIndex, 42u);
            EXPECT_EQ(parsed->mSpeech[0].mSound, "Vo\\Misc\\hello.mp3");
            EXPECT_EQ(parsed->mSpeech[0].mText, "Greetings, outlander.");
            EXPECT_TRUE(parsed->mSpeech[1].mSound.empty());
            EXPECT_TRUE(parsed->mSpeech[1].mText.empty());
        }

        TEST(MWNetActionsTest, worldSoundsRoundTrip)
        {
            ActionBatch batch;
            // Host-originated, anchored on a world actor.
            batch.mSounds.push_back(
                { ESM::RefNum{ 42, 0 }, { 0.f, 0.f, 0.f }, "Heavy Armor Hit", 1.f, 0.9f, ESM::RefNum{ 0, -1000 } });
            // Client-originated positional (an area explosion), unset anchor.
            batch.mSounds.push_back(
                { ESM::RefNum{}, { 10.f, -20.f, 30.5f }, "mysticism area", 0.5f, 1.f, ESM::RefNum{ 3, -1000 } });
            const std::optional<ActionBatch> parsed = deserializeActions(serializeActions(batch));
            ASSERT_TRUE(parsed.has_value());
            EXPECT_EQ(*parsed, batch);
            ASSERT_EQ(parsed->mSounds.size(), 2u);
            EXPECT_EQ(parsed->mSounds[0].mObject.mIndex, 42u);
            EXPECT_EQ(parsed->mSounds[0].mSound, "Heavy Armor Hit");
            EXPECT_EQ(parsed->mSounds[0].mPitch, 0.9f);
            EXPECT_EQ(parsed->mSounds[0].mOrigin, (ESM::RefNum{ 0, -1000 }));
            EXPECT_TRUE(parsed->mSounds[0].mOrigin.isSet()); // the host id {0,-1000} must count as set
            EXPECT_FALSE(parsed->mSounds[1].mObject.isSet());
            EXPECT_EQ(parsed->mSounds[1].mPosition[2], 30.5f);
            EXPECT_EQ(parsed->mSounds[1].mVolume, 0.5f);
            EXPECT_EQ(parsed->mSounds[1].mOrigin.mIndex, 3u);
        }

        TEST(MWNetActionsTest, arrestsRoundTrip)
        {
            ActionBatch batch;
            batch.mArrests.push_back({ ESM::RefNum{ 7, -1000 }, ESM::RefNum{ 88, 0 } });
            batch.mArrests.push_back({ ESM::RefNum{ 3, -1000 }, ESM::RefNum{ 0xffff, 2 } });
            const std::optional<ActionBatch> parsed = deserializeActions(serializeActions(batch));
            ASSERT_TRUE(parsed.has_value());
            EXPECT_EQ(*parsed, batch);
            ASSERT_EQ(parsed->mArrests.size(), 2u);
            EXPECT_EQ(parsed->mArrests[0].mTarget.mIndex, 7u);
            EXPECT_EQ(parsed->mArrests[0].mGuard.mIndex, 88u);
            EXPECT_EQ(parsed->mArrests[1].mGuard.mContentFile, 2);
        }

        TEST(MWNetActionsTest, combatRequestsRoundTrip)
        {
            ActionBatch batch;
            batch.mCombatRequests.push_back({ ESM::RefNum{ 88, 0 }, ESM::RefNum{ 7, -1000 } });
            batch.mCombatRequests.push_back({ ESM::RefNum{ 0xabcd, 4 }, ESM::RefNum{ 3, -1000 } });
            const std::optional<ActionBatch> parsed = deserializeActions(serializeActions(batch));
            ASSERT_TRUE(parsed.has_value());
            EXPECT_EQ(*parsed, batch);
            ASSERT_EQ(parsed->mCombatRequests.size(), 2u);
            EXPECT_EQ(parsed->mCombatRequests[0].mInstigator.mIndex, 88u);
            EXPECT_EQ(parsed->mCombatRequests[0].mTarget.mIndex, 7u);
            EXPECT_EQ(parsed->mCombatRequests[1].mInstigator.mContentFile, 4);
        }

        TEST(MWNetActionsTest, journalDeltasRoundTrip)
        {
            ActionBatch batch;
            // Full rendered entry from a client (origin 3).
            batch.mJournalDeltas.push_back({ "_mp_quest", 20, "1234567890", "The dreamers are awake.",
                "Dagoth Gares", 12, 3, 27, ESM::RefNum{ 3, -1000 } });
            // Index-only re-assert from the host.
            batch.mJournalDeltas.push_back({ "_mp_quest", 15, "", "", "", 0, 0, 0, ESM::RefNum{ 0, -1000 } });
            const std::optional<ActionBatch> parsed = deserializeActions(serializeActions(batch));
            ASSERT_TRUE(parsed.has_value());
            EXPECT_EQ(*parsed, batch);
            ASSERT_EQ(parsed->mJournalDeltas.size(), 2u);
            EXPECT_EQ(parsed->mJournalDeltas[0].mTopic, "_mp_quest");
            EXPECT_EQ(parsed->mJournalDeltas[0].mIndex, 20);
            EXPECT_EQ(parsed->mJournalDeltas[0].mText, "The dreamers are awake.");
            EXPECT_EQ(parsed->mJournalDeltas[0].mDayOfMonth, 27);
            EXPECT_EQ(parsed->mJournalDeltas[0].mOrigin, (ESM::RefNum{ 3, -1000 }));
            EXPECT_TRUE(parsed->mJournalDeltas[1].mInfoId.empty()); // index-only
            EXPECT_EQ(parsed->mJournalDeltas[1].mIndex, 15);
        }

        TEST(MWNetActionsTest, globalsAndTimeRoundTrip)
        {
            ActionBatch batch;
            batch.mGlobalDeltas.push_back({ "sixthhouseglobal", 'i', 7, 0.f, ESM::RefNum{ 2, -1000 } });
            batch.mGlobalDeltas.push_back({ "werewolfclawmult", 'f', 0, 50.f, ESM::RefNum{ 0, -1000 } });
            batch.mTimeSyncs.push_back({ 13.5f, 16, 8, 427, 42, 30.f });
            batch.mTimeRequests.push_back({ 8.f, ESM::RefNum{ 2, -1000 } });
            const std::optional<ActionBatch> parsed = deserializeActions(serializeActions(batch));
            ASSERT_TRUE(parsed.has_value());
            EXPECT_EQ(*parsed, batch);
            ASSERT_EQ(parsed->mGlobalDeltas.size(), 2u);
            EXPECT_EQ(parsed->mGlobalDeltas[0].mType, 'i');
            EXPECT_EQ(parsed->mGlobalDeltas[0].mIntValue, 7);
            EXPECT_EQ(parsed->mGlobalDeltas[1].mFloatValue, 50.f);
            ASSERT_EQ(parsed->mTimeSyncs.size(), 1u);
            EXPECT_EQ(parsed->mTimeSyncs[0].mGameHour, 13.5f);
            EXPECT_EQ(parsed->mTimeSyncs[0].mDaysPassed, 42);
            ASSERT_EQ(parsed->mTimeRequests.size(), 1u);
            EXPECT_EQ(parsed->mTimeRequests[0].mHours, 8.f);
        }

        TEST(MWNetActionsTest, unsyncedGlobalFamilies)
        {
            // The game clock (own TimeSync channel), the chargen gate, and per-player state.
            EXPECT_TRUE(isUnsyncedGlobal("gamehour"));
            EXPECT_TRUE(isUnsyncedGlobal("GameHour")); // case-insensitive, like script names
            EXPECT_TRUE(isUnsyncedGlobal("dayspassed"));
            EXPECT_TRUE(isUnsyncedGlobal("timescale"));
            EXPECT_TRUE(isUnsyncedGlobal("chargenstate"));
            EXPECT_TRUE(isUnsyncedGlobal("pchascrimegold"));
            EXPECT_TRUE(isUnsyncedGlobal("crimegoldturnin"));
            EXPECT_TRUE(isUnsyncedGlobal("PCVampire"));
            // Everything else syncs, including mod globals.
            EXPECT_FALSE(isUnsyncedGlobal("werewolfclawmult"));
            EXPECT_FALSE(isUnsyncedGlobal("mymod_progress"));
            EXPECT_FALSE(isUnsyncedGlobal(""));
        }

        TEST(MWNetActionsTest, refEnablesRoundTrip)
        {
            ActionBatch batch;
            batch.mRefEnables.push_back({ ESM::RefNum{ 12345, 0 }, true, ESM::RefNum{ 0, -1000 } });
            batch.mRefEnables.push_back({ ESM::RefNum{ 678, 2 }, false, ESM::RefNum{ 3, -1000 } });
            const std::optional<ActionBatch> parsed = deserializeActions(serializeActions(batch));
            ASSERT_TRUE(parsed.has_value());
            EXPECT_EQ(*parsed, batch);
            ASSERT_EQ(parsed->mRefEnables.size(), 2u);
            EXPECT_TRUE(parsed->mRefEnables[0].mEnabled);
            EXPECT_FALSE(parsed->mRefEnables[1].mEnabled);
            EXPECT_EQ(parsed->mRefEnables[1].mRef.mContentFile, 2);
        }

        TEST(MWNetActionsTest, scriptRunsRoundTrip)
        {
            ActionBatch batch;
            // Untargeted start (the common case) and a targeted stop.
            batch.mScriptRuns.push_back({ "_mp_dreamer_watch", true, ESM::RefNum{}, "", ESM::RefNum{ 2, -1000 } });
            batch.mScriptRuns.push_back(
                { "escortscript", false, ESM::RefNum{ 42, 0 }, "fargoth", ESM::RefNum{ 0, -1000 } });
            const std::optional<ActionBatch> parsed = deserializeActions(serializeActions(batch));
            ASSERT_TRUE(parsed.has_value());
            EXPECT_EQ(*parsed, batch);
            ASSERT_EQ(parsed->mScriptRuns.size(), 2u);
            EXPECT_TRUE(parsed->mScriptRuns[0].mRunning);
            EXPECT_FALSE(parsed->mScriptRuns[0].mTargetRef.isSet());
            EXPECT_FALSE(parsed->mScriptRuns[1].mRunning);
            EXPECT_EQ(parsed->mScriptRuns[1].mTargetId, "fargoth");
        }

        TEST(MWNetActionsTest, weatherSyncsRoundTrip)
        {
            ActionBatch batch;
            batch.mWeatherSyncs.push_back({ "Bitter Coast Region", 4, ESM::RefNum{ 0, -1000 } }); // rain
            batch.mWeatherSyncs.push_back({ "Ascadian Isles Region", 0, ESM::RefNum{ 0, -1000 } }); // clear
            const std::optional<ActionBatch> parsed = deserializeActions(serializeActions(batch));
            ASSERT_TRUE(parsed.has_value());
            EXPECT_EQ(*parsed, batch);
            ASSERT_EQ(parsed->mWeatherSyncs.size(), 2u);
            EXPECT_EQ(parsed->mWeatherSyncs[0].mRegion, "Bitter Coast Region");
            EXPECT_EQ(parsed->mWeatherSyncs[0].mWeatherId, 4);
            EXPECT_EQ(parsed->mWeatherSyncs[1].mWeatherId, 0);
        }

        TEST(MWNetActionsTest, rejectsEmptyBuffer)
        {
            EXPECT_FALSE(deserializeActions(std::span<const std::byte>{}).has_value());
        }

        TEST(MWNetActionsTest, rejectsWrongVersion)
        {
            std::vector<std::byte> bytes = serializeActions(ActionBatch{});
            bytes[0] = std::byte{ 0xff };
            EXPECT_FALSE(deserializeActions(bytes).has_value());
        }

        TEST(MWNetActionsTest, rejectsImplausibleCount)
        {
            std::vector<std::byte> bytes = serializeActions(ActionBatch{});
            // version + 20 4-byte 0 counts (hits, playerDamages, drops, taken, containers, changes,
            // revokes, summons, bounties, speech, sounds, arrests, combatRequests, journalDeltas,
            // globalDeltas, timeSyncs, timeRequests, refEnables, scriptRuns, weatherSyncs)
            ASSERT_EQ(bytes.size(), 81u);
            for (std::size_t i = 0; i < 4; ++i)
                bytes[1 + i] = std::byte{ 0xff }; // implausible hit count
            EXPECT_FALSE(deserializeActions(bytes).has_value());
        }

        TEST(MWNetActionsTest, fuzzNeverCrashesAndReparsesConsistently)
        {
            std::mt19937 rng(0xAC10);
            std::uniform_int_distribution<int> sizeDist(0, 80);
            std::uniform_int_distribution<int> byteDist(0, 255);
            for (int iteration = 0; iteration < 20000; ++iteration)
            {
                std::vector<std::byte> bytes(sizeDist(rng));
                for (std::byte& b : bytes)
                    b = std::byte{ static_cast<unsigned char>(byteDist(rng)) };
                const std::optional<ActionBatch> parsed = deserializeActions(bytes);
                if (parsed.has_value())
                {
                    const std::optional<ActionBatch> reparsed = deserializeActions(serializeActions(*parsed));
                    ASSERT_TRUE(reparsed.has_value());
                    EXPECT_EQ(*reparsed, *parsed);
                }
            }
        }
    }
}
