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
            batch.mHits.push_back({ ESM::RefNum{ 1, -1000 }, ESM::RefNum{ 42, 0 } });
            batch.mHits.push_back({ ESM::RefNum{ 0, -1000 }, ESM::RefNum{ 0xffffff, 5 } });
            const std::optional<ActionBatch> parsed = deserializeActions(serializeActions(batch));
            ASSERT_TRUE(parsed.has_value());
            EXPECT_EQ(*parsed, batch);
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
            ASSERT_EQ(bytes.size(), 5u); // [version][count:4 == 0]
            for (std::size_t i = 0; i < 4; ++i)
                bytes[1 + i] = std::byte{ 0xff };
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
