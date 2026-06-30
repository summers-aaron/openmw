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
            // version + 12 4-byte 0 counts (hits, playerDamages, drops, taken, containers, changes,
            // revokes, summons, bounties, speech, arrests, combatRequests)
            ASSERT_EQ(bytes.size(), 49u);
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
