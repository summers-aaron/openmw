#include <gtest/gtest.h>

#include <random>

#include "apps/openmw/mwnet/control.hpp"

namespace MWNet
{
    namespace
    {
        // One representative value of every control message variant.
        std::vector<ControlMessage> sampleMessages()
        {
            std::vector<ControlMessage> messages;
            messages.push_back(LoginRequest{ "Alice", { "Morrowind.esm", "Tribunal.esm", "Bloodmoon.esm" } });
            messages.push_back(LoginRequest{ "", {} }); // anonymous, no content files
            messages.push_back(CharacterList{ { { 1, "Alice", 7, "Seyda Neen" }, { 2, "Bob", 12, "Balmora" } } });
            messages.push_back(CharacterList{ {} }); // empty roster
            messages.push_back(SelectCharacter{ 42 });
            messages.push_back(CreateNew{});
            messages.push_back(CharacterData{ 3, std::string("\0\1\2binary blob\0", 15) });
            messages.push_back(LoginAccept{ ESM::RefNum{ 5, -1000 } }); // -1000 = the net-player content file
            messages.push_back(LoginReject{ "content files do not match the server" });
            return messages;
        }

        TEST(MWNetControlTest, everyMessageRoundTrips)
        {
            for (const ControlMessage& message : sampleMessages())
            {
                const std::optional<ControlMessage> parsed = deserializeControl(serializeControl(message));
                ASSERT_TRUE(parsed.has_value());
                EXPECT_EQ(*parsed, message);
            }
        }

        TEST(MWNetControlTest, preservesBinaryBlobExactly)
        {
            const std::string blob("\0\1\xff\x7f\0inventory\0", 14);
            const std::optional<ControlMessage> parsed = deserializeControl(serializeControl(CharacterData{ 9, blob }));
            ASSERT_TRUE(parsed.has_value());
            const auto* data = std::get_if<CharacterData>(&*parsed);
            ASSERT_NE(data, nullptr);
            EXPECT_EQ(data->mBlob, blob);
        }

        TEST(MWNetControlTest, rejectsEmptyBuffer)
        {
            EXPECT_FALSE(deserializeControl(std::span<const std::byte>{}).has_value());
        }

        TEST(MWNetControlTest, rejectsWrongVersion)
        {
            std::vector<std::byte> bytes = serializeControl(CreateNew{});
            bytes[0] = std::byte{ 0xff };
            EXPECT_FALSE(deserializeControl(bytes).has_value());
        }

        TEST(MWNetControlTest, rejectsUnknownType)
        {
            std::vector<std::byte> bytes = serializeControl(CreateNew{});
            bytes[1] = std::byte{ 0x7f }; // no such message type
            EXPECT_FALSE(deserializeControl(bytes).has_value());
        }

        TEST(MWNetControlTest, rejectsTruncatedMessage)
        {
            std::vector<std::byte> bytes = serializeControl(LoginReject{ "some reason" });
            bytes.resize(bytes.size() - 1); // drop a byte from the trailing string
            EXPECT_FALSE(deserializeControl(bytes).has_value());
        }

        // Hostile/corrupt input must never crash, over-read, or over-allocate; anything that parses
        // must re-serialize/parse to the same value.
        TEST(MWNetControlTest, fuzzNeverCrashesAndReparsesConsistently)
        {
            std::mt19937 rng(0xC0FFEE);
            std::uniform_int_distribution<int> sizeDist(0, 96);
            std::uniform_int_distribution<int> byteDist(0, 255);

            for (int iteration = 0; iteration < 20000; ++iteration)
            {
                std::vector<std::byte> bytes(sizeDist(rng));
                for (std::byte& b : bytes)
                    b = std::byte{ static_cast<unsigned char>(byteDist(rng)) };

                const std::optional<ControlMessage> parsed = deserializeControl(bytes);
                if (parsed.has_value())
                {
                    const std::optional<ControlMessage> reparsed = deserializeControl(serializeControl(*parsed));
                    ASSERT_TRUE(reparsed.has_value());
                    EXPECT_EQ(*reparsed, *parsed);
                }
            }
        }
    }
}
