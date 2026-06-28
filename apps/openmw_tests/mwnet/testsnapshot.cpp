#include <gtest/gtest.h>

#include <random>

#include "apps/openmw/mwnet/snapshot.hpp"

namespace MWNet
{
    namespace
    {
        EntityState makeEntity(std::uint32_t index, std::int32_t file, std::optional<TransformState> transform)
        {
            EntityState entity;
            entity.mId = ESM::RefNum{ index, file };
            entity.mTransform = transform;
            return entity;
        }

        TransformState makeTransform(float base)
        {
            return TransformState{ osg::Vec3f(base, base + 1.f, base + 2.f), osg::Vec3f(base + 3.f, base + 4.f, base + 5.f) };
        }

        TEST(MWNetSnapshotTest, emptyDeltaRoundTrips)
        {
            SnapshotDelta delta;
            delta.mTick = 0;
            const std::optional<SnapshotDelta> parsed = deserializeSnapshot(serializeSnapshot(delta));
            ASSERT_TRUE(parsed.has_value());
            EXPECT_EQ(*parsed, delta);
        }

        TEST(MWNetSnapshotTest, mixedEntitiesRoundTrip)
        {
            SnapshotDelta delta;
            delta.mTick = 4242;
            delta.mEntities.push_back(makeEntity(1, 0, makeTransform(10.f)));
            delta.mEntities.push_back(makeEntity(7, -1, std::nullopt)); // present but no fields changed
            delta.mEntities.push_back(makeEntity(0xfffffff, 5, makeTransform(-99.5f)));

            const std::vector<std::byte> bytes = serializeSnapshot(delta);
            const std::optional<SnapshotDelta> parsed = deserializeSnapshot(bytes);
            ASSERT_TRUE(parsed.has_value());
            EXPECT_EQ(*parsed, delta);
        }

        TEST(MWNetSnapshotTest, transformValuesArePreservedExactly)
        {
            SnapshotDelta delta;
            delta.mEntities.push_back(makeEntity(3, 1, makeTransform(123.456f)));
            const std::optional<SnapshotDelta> parsed = deserializeSnapshot(serializeSnapshot(delta));
            ASSERT_TRUE(parsed.has_value());
            ASSERT_EQ(parsed->mEntities.size(), 1u);
            ASSERT_TRUE(parsed->mEntities[0].mTransform.has_value());
            EXPECT_EQ(parsed->mEntities[0].mTransform->mPosition, osg::Vec3f(123.456f, 124.456f, 125.456f));
        }

        TEST(MWNetSnapshotTest, statsRoundTrip)
        {
            SnapshotDelta delta;
            EntityState withStats = makeEntity(5, 1, makeTransform(3.f));
            withStats.mStats = DynamicStats{ 42.5f, 10.f, 0.f }; // health 42.5, magicka 10, fatigue 0
            delta.mEntities.push_back(withStats);
            withStats.mDrawState = std::uint8_t{ 1 }; // weapon drawn
            delta.mEntities.back() = withStats;
            EntityState statsOnly;
            statsOnly.mId = ESM::RefNum{ 9, 2 };
            statsOnly.mStats = DynamicStats{ 0.f, 0.f, 0.f }; // dead, no transform
            statsOnly.mDrawState = std::uint8_t{ 2 }; // spell stance, drawstate-only entity
            delta.mEntities.push_back(statsOnly);

            const std::optional<SnapshotDelta> parsed = deserializeSnapshot(serializeSnapshot(delta));
            ASSERT_TRUE(parsed.has_value());
            EXPECT_EQ(*parsed, delta);
            ASSERT_EQ(parsed->mEntities.size(), 2u);
            ASSERT_TRUE(parsed->mEntities[0].mStats.has_value());
            EXPECT_EQ(parsed->mEntities[0].mStats->mHealth, 42.5f);
            EXPECT_EQ(parsed->mEntities[0].mDrawState, std::uint8_t{ 1 });
            EXPECT_FALSE(parsed->mEntities[1].mTransform.has_value());
            EXPECT_EQ(parsed->mEntities[1].mDrawState, std::uint8_t{ 2 });
        }

        TEST(MWNetSnapshotTest, appearanceRoundTrips)
        {
            SnapshotDelta delta;
            EntityState withAppearance = makeEntity(11, -1000, makeTransform(7.f));
            withAppearance.mAppearance = AppearanceState{ "Dark Elf", "b_n_dark elf_m_head_01",
                "b_n_dark elf_m_hair_01", "Acrobat", "Jiub", /*isMale=*/true };
            delta.mEntities.push_back(withAppearance);
            // An empty-string / female appearance must round-trip too.
            EntityState minimal = makeEntity(12, -1000, std::nullopt);
            minimal.mAppearance = AppearanceState{ "", "", "", "", "", /*isMale=*/false };
            delta.mEntities.push_back(minimal);

            const std::optional<SnapshotDelta> parsed = deserializeSnapshot(serializeSnapshot(delta));
            ASSERT_TRUE(parsed.has_value());
            EXPECT_EQ(*parsed, delta);
            ASSERT_EQ(parsed->mEntities.size(), 2u);
            ASSERT_TRUE(parsed->mEntities[0].mAppearance.has_value());
            EXPECT_EQ(parsed->mEntities[0].mAppearance->mRace, "Dark Elf");
            EXPECT_TRUE(parsed->mEntities[0].mAppearance->mIsMale);
            EXPECT_FALSE(parsed->mEntities[1].mAppearance->mIsMale);
        }

        TEST(MWNetSnapshotTest, equipmentRoundTrips)
        {
            SnapshotDelta delta;
            EntityState dressed = makeEntity(20, -1000, makeTransform(2.f));
            dressed.mEquipment = std::vector<EquipmentSlot>{
                EquipmentSlot{ 1, "iron_cuirass" }, // Slot_Cuirass
                EquipmentSlot{ 16, "iron_longsword" }, // Slot_CarriedRight
            };
            delta.mEntities.push_back(dressed);
            // An explicit empty list ("wearing nothing") must round-trip distinct from absent.
            EntityState naked = makeEntity(21, -1000, std::nullopt);
            naked.mEquipment = std::vector<EquipmentSlot>{};
            delta.mEntities.push_back(naked);

            const std::optional<SnapshotDelta> parsed = deserializeSnapshot(serializeSnapshot(delta));
            ASSERT_TRUE(parsed.has_value());
            EXPECT_EQ(*parsed, delta);
            ASSERT_EQ(parsed->mEntities.size(), 2u);
            ASSERT_TRUE(parsed->mEntities[0].mEquipment.has_value());
            ASSERT_EQ(parsed->mEntities[0].mEquipment->size(), 2u);
            EXPECT_EQ((*parsed->mEntities[0].mEquipment)[0].mSlot, 1u);
            EXPECT_EQ((*parsed->mEntities[0].mEquipment)[1].mItem, "iron_longsword");
            ASSERT_TRUE(parsed->mEntities[1].mEquipment.has_value());
            EXPECT_TRUE(parsed->mEntities[1].mEquipment->empty());
        }

        TEST(MWNetSnapshotTest, rejectsEmptyBuffer)
        {
            EXPECT_FALSE(deserializeSnapshot(std::span<const std::byte>{}).has_value());
        }

        TEST(MWNetSnapshotTest, rejectsWrongVersion)
        {
            std::vector<std::byte> bytes = serializeSnapshot(SnapshotDelta{});
            bytes[0] = std::byte{ 0xff };
            EXPECT_FALSE(deserializeSnapshot(bytes).has_value());
        }

        TEST(MWNetSnapshotTest, rejectsTruncatedEntity)
        {
            SnapshotDelta delta;
            delta.mEntities.push_back(makeEntity(1, 0, makeTransform(1.f)));
            std::vector<std::byte> bytes = serializeSnapshot(delta);
            bytes.resize(bytes.size() - 1); // drop the last transform byte
            EXPECT_FALSE(deserializeSnapshot(bytes).has_value());
        }

        TEST(MWNetSnapshotTest, rejectsImplausibleEntityCount)
        {
            // version + tick + count(0xffffffff) but no entity bytes follow.
            std::vector<std::byte> bytes = serializeSnapshot(SnapshotDelta{});
            // bytes = [version][tick:4][count:4 == 0]; overwrite the count with a huge value.
            ASSERT_EQ(bytes.size(), 9u);
            for (std::size_t i = 0; i < 4; ++i)
                bytes[5 + i] = std::byte{ 0xff };
            EXPECT_FALSE(deserializeSnapshot(bytes).has_value());
        }

        TEST(MWNetSnapshotTest, rejectsUnknownFieldBits)
        {
            SnapshotDelta delta;
            delta.mEntities.push_back(makeEntity(1, 0, std::nullopt));
            std::vector<std::byte> bytes = serializeSnapshot(delta);
            // Layout: [version][tick:4][count:4][index:4][file:4][mask:1]; flip an unknown bit in the mask.
            bytes.back() = std::byte{ 0x80 };
            EXPECT_FALSE(deserializeSnapshot(bytes).has_value());
        }

        // Hostile/corrupt input must never crash, over-read, or over-allocate; and any
        // buffer that does parse must re-serialize/parse to the same value (self-consistency).
        TEST(MWNetSnapshotTest, fuzzNeverCrashesAndReparsesConsistently)
        {
            std::mt19937 rng(0xC0FFEE);
            std::uniform_int_distribution<int> sizeDist(0, 64);
            std::uniform_int_distribution<int> byteDist(0, 255);

            for (int iteration = 0; iteration < 20000; ++iteration)
            {
                std::vector<std::byte> bytes(sizeDist(rng));
                for (std::byte& b : bytes)
                    b = std::byte{ static_cast<unsigned char>(byteDist(rng)) };

                const std::optional<SnapshotDelta> parsed = deserializeSnapshot(bytes);
                if (parsed.has_value())
                {
                    const std::optional<SnapshotDelta> reparsed = deserializeSnapshot(serializeSnapshot(*parsed));
                    ASSERT_TRUE(reparsed.has_value());
                    EXPECT_EQ(*reparsed, *parsed);
                }
            }
        }

        // Property test: any well-formed delta survives a serialize/deserialize round trip.
        TEST(MWNetSnapshotTest, randomValidDeltasRoundTrip)
        {
            std::mt19937 rng(0x1234);
            std::uniform_int_distribution<std::uint32_t> u32(0, 0xffffffff);
            std::uniform_int_distribution<int> countDist(0, 50);
            std::uniform_int_distribution<int> coin(0, 1);
            std::uniform_real_distribution<float> coord(-1e6f, 1e6f);

            for (int iteration = 0; iteration < 2000; ++iteration)
            {
                SnapshotDelta delta;
                delta.mTick = u32(rng);
                const int count = countDist(rng);
                for (int e = 0; e < count; ++e)
                {
                    std::optional<TransformState> transform;
                    if (coin(rng))
                        transform = TransformState{ osg::Vec3f(coord(rng), coord(rng), coord(rng)),
                            osg::Vec3f(coord(rng), coord(rng), coord(rng)) };
                    delta.mEntities.push_back(
                        makeEntity(u32(rng), static_cast<std::int32_t>(u32(rng)), transform));
                }

                const std::optional<SnapshotDelta> parsed = deserializeSnapshot(serializeSnapshot(delta));
                ASSERT_TRUE(parsed.has_value());
                EXPECT_EQ(*parsed, delta);
            }
        }
    }
}
