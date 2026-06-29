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
            // [version][hitCount:4 == 0][playerDamageCount:4 == 0][dropCount:4 == 0][takenCount:4 == 0]
            ASSERT_EQ(bytes.size(), 17u);
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
