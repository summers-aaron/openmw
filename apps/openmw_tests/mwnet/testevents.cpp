#include <gtest/gtest.h>

#include <random>

#include "apps/openmw/mwnet/events.hpp"

namespace MWNet
{
    namespace
    {
        EventBatch sampleBatch()
        {
            EventBatch batch;
            batch.mGlobal.push_back({ "onSomething", std::string("\0\1\2binary", 9) });
            batch.mGlobal.push_back({ "", "" }); // empty name and data
            batch.mLocal.push_back({ ESM::RefNum{ 42, 1 }, "onActivated", "payload" });
            batch.mLocal.push_back({ ESM::RefNum{ 0xfffffff, -1 }, "", std::string("\0\0", 2) });
            return batch;
        }

        TEST(MWNetEventsTest, emptyBatchRoundTrips)
        {
            const EventBatch batch;
            const std::optional<EventBatch> parsed = deserializeEvents(serializeEvents(batch));
            ASSERT_TRUE(parsed.has_value());
            EXPECT_EQ(*parsed, batch);
        }

        TEST(MWNetEventsTest, mixedBatchRoundTrips)
        {
            const EventBatch batch = sampleBatch();
            const std::optional<EventBatch> parsed = deserializeEvents(serializeEvents(batch));
            ASSERT_TRUE(parsed.has_value());
            EXPECT_EQ(*parsed, batch);
        }

        TEST(MWNetEventsTest, preservesBinaryEventDataExactly)
        {
            EventBatch batch;
            const std::string binary("\0\1\xff\x7f\0payload\0", 12);
            batch.mGlobal.push_back({ "e", binary });
            const std::optional<EventBatch> parsed = deserializeEvents(serializeEvents(batch));
            ASSERT_TRUE(parsed.has_value());
            ASSERT_EQ(parsed->mGlobal.size(), 1u);
            EXPECT_EQ(parsed->mGlobal[0].mEventData, binary);
        }

        TEST(MWNetEventsTest, rejectsEmptyBuffer)
        {
            EXPECT_FALSE(deserializeEvents(std::span<const std::byte>{}).has_value());
        }

        TEST(MWNetEventsTest, rejectsWrongVersion)
        {
            std::vector<std::byte> bytes = serializeEvents(EventBatch{});
            bytes[0] = std::byte{ 0xff };
            EXPECT_FALSE(deserializeEvents(bytes).has_value());
        }

        TEST(MWNetEventsTest, rejectsTruncatedString)
        {
            const EventBatch batch = sampleBatch();
            std::vector<std::byte> bytes = serializeEvents(batch);
            bytes.resize(bytes.size() - 1); // drop a byte from the last string
            EXPECT_FALSE(deserializeEvents(bytes).has_value());
        }

        TEST(MWNetEventsTest, rejectsImplausibleStringLength)
        {
            EventBatch batch;
            batch.mGlobal.push_back({ "name", "data" });
            std::vector<std::byte> bytes = serializeEvents(batch);
            // Layout: [version][globalCount:4][nameLen:4]...; overwrite the name length with a huge value.
            for (std::size_t i = 0; i < 4; ++i)
                bytes[5 + i] = std::byte{ 0xff };
            EXPECT_FALSE(deserializeEvents(bytes).has_value());
        }

        // Hostile/corrupt input must never crash, over-read, or over-allocate; and any
        // buffer that does parse must re-serialize/parse to the same value.
        TEST(MWNetEventsTest, fuzzNeverCrashesAndReparsesConsistently)
        {
            std::mt19937 rng(0xBEEF);
            std::uniform_int_distribution<int> sizeDist(0, 96);
            std::uniform_int_distribution<int> byteDist(0, 255);

            for (int iteration = 0; iteration < 20000; ++iteration)
            {
                std::vector<std::byte> bytes(sizeDist(rng));
                for (std::byte& b : bytes)
                    b = std::byte{ static_cast<unsigned char>(byteDist(rng)) };

                const std::optional<EventBatch> parsed = deserializeEvents(bytes);
                if (parsed.has_value())
                {
                    const std::optional<EventBatch> reparsed = deserializeEvents(serializeEvents(*parsed));
                    ASSERT_TRUE(reparsed.has_value());
                    EXPECT_EQ(*reparsed, *parsed);
                }
            }
        }

        // Property test: any well-formed batch survives a serialize/deserialize round trip.
        TEST(MWNetEventsTest, randomValidBatchesRoundTrip)
        {
            std::mt19937 rng(0x5151);
            std::uniform_int_distribution<std::uint32_t> u32(0, 0xffffffff);
            std::uniform_int_distribution<int> countDist(0, 16);
            std::uniform_int_distribution<int> lenDist(0, 24);
            std::uniform_int_distribution<int> byteDist(0, 255);

            const auto randomString = [&] {
                std::string s(lenDist(rng), '\0');
                for (char& c : s)
                    c = static_cast<char>(byteDist(rng));
                return s;
            };

            for (int iteration = 0; iteration < 2000; ++iteration)
            {
                EventBatch batch;
                const int globals = countDist(rng);
                for (int e = 0; e < globals; ++e)
                    batch.mGlobal.push_back({ randomString(), randomString() });
                const int locals = countDist(rng);
                for (int e = 0; e < locals; ++e)
                    batch.mLocal.push_back({ ESM::RefNum{ u32(rng), static_cast<std::int32_t>(u32(rng)) },
                        randomString(), randomString() });

                const std::optional<EventBatch> parsed = deserializeEvents(serializeEvents(batch));
                ASSERT_TRUE(parsed.has_value());
                EXPECT_EQ(*parsed, batch);
            }
        }
    }
}
