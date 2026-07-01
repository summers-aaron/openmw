#include <gtest/gtest.h>

#include <components/esm/refid.hpp>
#include <components/esm3/player.hpp>

#include "apps/openmw/mwnet/charactercodec.hpp"

namespace MWNet
{
    namespace
    {
        // Build a character with distinctive scalar state we can assert survives the round trip. The
        // bulk (mObject / NpcState — inventory, stats) is exercised by the save-file tests; here we
        // confirm the wrapper (header + REC_PLAY framing) preserves an ESM::Player faithfully.
        // ESM::Player is neither copyable nor movable, so fill a caller-provided instance.
        void fillSampleCharacter(ESM::Player& player)
        {
            player.mCellId = ESM::RefId::stringRefId("Seyda Neen");
            player.mBirthsign = ESM::RefId::stringRefId("sign_warrior");
            player.mMarkedCell = ESM::RefId::stringRefId("Balmora");
            player.mCurrentCrimeId = 7;
            player.mPaidCrimeId = 3;
            player.mHasMark = 1;
            player.mLastKnownExteriorPosition[0] = 1.5f;
            player.mLastKnownExteriorPosition[1] = -2.5f;
            player.mLastKnownExteriorPosition[2] = 42.f;
            player.mSaveAttributes[0] = 55.f;
            player.mSaveSkills[0] = 33.f;
        }

        TEST(MWNetCharacterCodecTest, roundTripsScalarState)
        {
            ESM::Player original{}; // value-initialize scalars/arrays
            fillSampleCharacter(original);
            const std::unique_ptr<ESM::Player> parsed
                = deserializeCharacter(serializeCharacter(original, { "Morrowind.esm" }));
            ASSERT_NE(parsed, nullptr);
            EXPECT_EQ(parsed->mCellId, original.mCellId);
            EXPECT_EQ(parsed->mBirthsign, original.mBirthsign);
            EXPECT_EQ(parsed->mMarkedCell, original.mMarkedCell);
            EXPECT_EQ(parsed->mCurrentCrimeId, original.mCurrentCrimeId);
            EXPECT_EQ(parsed->mPaidCrimeId, original.mPaidCrimeId);
            EXPECT_EQ(parsed->mHasMark, original.mHasMark);
            EXPECT_FLOAT_EQ(parsed->mLastKnownExteriorPosition[2], 42.f);
            EXPECT_FLOAT_EQ(parsed->mSaveAttributes[0], 55.f);
            EXPECT_FLOAT_EQ(parsed->mSaveSkills[0], 33.f);
        }

        TEST(MWNetCharacterCodecTest, rejectsGarbageBlob)
        {
            EXPECT_EQ(deserializeCharacter(std::string(64, '\xab')), nullptr);
        }

        TEST(MWNetCharacterCodecTest, rejectsEmptyBlob)
        {
            EXPECT_EQ(deserializeCharacter(std::string{}), nullptr);
        }

        TEST(MWNetCharacterCodecTest, rejectsTruncatedBlob)
        {
            ESM::Player original{};
            fillSampleCharacter(original);
            std::string blob = serializeCharacter(original, { "Morrowind.esm" });
            blob.resize(blob.size() / 2); // cut the record in half
            EXPECT_EQ(deserializeCharacter(blob), nullptr);
        }
    }
}
