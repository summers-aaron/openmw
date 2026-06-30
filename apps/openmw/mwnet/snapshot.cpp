#include "snapshot.hpp"

#include "bytestream.hpp"

namespace MWNet
{
    namespace
    {
        // Wire format version. Bumped if the layout below changes incompatibly.
        constexpr std::uint8_t sVersion = 9;

        // EntityState field bits (mFieldMask, a uint16 to leave room for more states).
        constexpr std::uint16_t sFieldTransform = 1 << 0;
        constexpr std::uint16_t sFieldStats = 1 << 1;
        constexpr std::uint16_t sFieldDrawState = 1 << 2;
        constexpr std::uint16_t sFieldAppearance = 1 << 3;
        constexpr std::uint16_t sFieldEquipment = 1 << 4;
        constexpr std::uint16_t sFieldMoveFlags = 1 << 5;
        constexpr std::uint16_t sFieldSwing = 1 << 6;
        constexpr std::uint16_t sFieldSpeed = 1 << 7;
        constexpr std::uint16_t sFieldCell = 1 << 8;
        constexpr std::uint16_t sFieldItem = 1 << 9;
        constexpr std::uint16_t sKnownFields = sFieldTransform | sFieldStats | sFieldDrawState | sFieldAppearance
            | sFieldEquipment | sFieldMoveFlags | sFieldSwing | sFieldSpeed | sFieldCell | sFieldItem;

        // Smallest possible encoded equipment entry: slot (1) + a zero-length item string (4).
        constexpr std::uint32_t sMinEquipmentBytes = 5;

        // Smallest possible encoded entity: RefNum (4 + 4) + field mask (2).
        constexpr std::uint32_t sMinEntityBytes = 10;
    }

    std::vector<std::byte> serializeSnapshot(const SnapshotDelta& delta)
    {
        std::vector<std::byte> out;
        ByteWriter writer(out);

        writer.write(sVersion);
        writer.write(delta.mTick);
        writer.write(static_cast<std::uint32_t>(delta.mEntities.size()));

        for (const EntityState& entity : delta.mEntities)
        {
            writer.write(entity.mId.mIndex);
            writer.write(entity.mId.mContentFile);

            std::uint16_t fieldMask = 0;
            if (entity.mTransform)
                fieldMask |= sFieldTransform;
            if (entity.mStats)
                fieldMask |= sFieldStats;
            if (entity.mDrawState)
                fieldMask |= sFieldDrawState;
            if (entity.mAppearance)
                fieldMask |= sFieldAppearance;
            if (entity.mEquipment)
                fieldMask |= sFieldEquipment;
            if (entity.mMoveFlags)
                fieldMask |= sFieldMoveFlags;
            if (entity.mSwing)
                fieldMask |= sFieldSwing;
            if (entity.mSpeed)
                fieldMask |= sFieldSpeed;
            if (entity.mCellId)
                fieldMask |= sFieldCell;
            if (entity.mItem)
                fieldMask |= sFieldItem;
            writer.write(fieldMask);

            if (entity.mTransform)
            {
                for (int i = 0; i < 3; ++i)
                    writer.write(entity.mTransform->mPosition[i]);
                for (int i = 0; i < 3; ++i)
                    writer.write(entity.mTransform->mRotation[i]);
            }
            if (entity.mStats)
            {
                writer.write(entity.mStats->mHealth);
                writer.write(entity.mStats->mMagicka);
                writer.write(entity.mStats->mFatigue);
            }
            if (entity.mDrawState)
                writer.write(*entity.mDrawState);
            if (entity.mMoveFlags)
                writer.write(*entity.mMoveFlags);
            if (entity.mSwing)
            {
                writer.writeString(entity.mSwing->mGroup);
                writer.writeString(entity.mSwing->mType);
                writer.write(entity.mSwing->mSeq);
                writer.writeString(entity.mSwing->mSpell);
                writer.write(entity.mSwing->mPhase);
                writer.write(entity.mSwing->mStrength);
            }
            if (entity.mSpeed)
                writer.write(*entity.mSpeed);
            if (entity.mAppearance)
            {
                writer.writeString(entity.mAppearance->mRace);
                writer.writeString(entity.mAppearance->mHead);
                writer.writeString(entity.mAppearance->mHair);
                writer.writeString(entity.mAppearance->mClass);
                writer.writeString(entity.mAppearance->mName);
                writer.write(static_cast<std::uint8_t>(entity.mAppearance->mIsMale ? 1 : 0));
            }
            if (entity.mEquipment)
            {
                writer.write(static_cast<std::uint32_t>(entity.mEquipment->size()));
                for (const EquipmentSlot& worn : *entity.mEquipment)
                {
                    writer.write(worn.mSlot);
                    writer.writeString(worn.mItem);
                }
            }
            if (entity.mCellId)
                writer.writeString(*entity.mCellId);
            if (entity.mItem)
            {
                writer.writeString(entity.mItem->mRefId);
                writer.write(entity.mItem->mCount);
            }
        }

        writer.write(static_cast<std::uint32_t>(delta.mRemovedItems.size()));
        for (const ESM::RefNum& removed : delta.mRemovedItems)
        {
            writer.write(removed.mIndex);
            writer.write(removed.mContentFile);
        }

        return out;
    }

    std::optional<SnapshotDelta> deserializeSnapshot(std::span<const std::byte> data)
    {
        ByteReader reader(data);

        std::uint8_t version = 0;
        if (!reader.read(version) || version != sVersion)
            return std::nullopt;

        SnapshotDelta delta;
        if (!reader.read(delta.mTick))
            return std::nullopt;

        std::uint32_t count = 0;
        if (!reader.read(count))
            return std::nullopt;

        // Reject any count that cannot possibly fit in the remaining bytes before
        // reserving — this bounds the allocation and the loop on hostile input.
        if (count > reader.remaining() / sMinEntityBytes)
            return std::nullopt;
        delta.mEntities.reserve(count);

        for (std::uint32_t i = 0; i < count; ++i)
        {
            EntityState entity;
            if (!reader.read(entity.mId.mIndex) || !reader.read(entity.mId.mContentFile))
                return std::nullopt;

            std::uint16_t fieldMask = 0;
            if (!reader.read(fieldMask))
                return std::nullopt;
            if (fieldMask & ~sKnownFields)
                return std::nullopt;

            if (fieldMask & sFieldTransform)
            {
                TransformState transform;
                for (int axis = 0; axis < 3; ++axis)
                    if (!reader.read(transform.mPosition[axis]))
                        return std::nullopt;
                for (int axis = 0; axis < 3; ++axis)
                    if (!reader.read(transform.mRotation[axis]))
                        return std::nullopt;
                entity.mTransform = transform;
            }
            if (fieldMask & sFieldStats)
            {
                DynamicStats stats;
                if (!reader.read(stats.mHealth) || !reader.read(stats.mMagicka) || !reader.read(stats.mFatigue))
                    return std::nullopt;
                entity.mStats = stats;
            }
            if (fieldMask & sFieldDrawState)
            {
                std::uint8_t drawState = 0;
                if (!reader.read(drawState))
                    return std::nullopt;
                entity.mDrawState = drawState;
            }
            if (fieldMask & sFieldMoveFlags)
            {
                std::uint8_t moveFlags = 0;
                if (!reader.read(moveFlags))
                    return std::nullopt;
                entity.mMoveFlags = moveFlags;
            }
            if (fieldMask & sFieldSwing)
            {
                SwingState swing;
                if (!reader.readString(swing.mGroup) || !reader.readString(swing.mType)
                    || !reader.read(swing.mSeq) || !reader.readString(swing.mSpell) || !reader.read(swing.mPhase)
                    || !reader.read(swing.mStrength))
                    return std::nullopt;
                entity.mSwing = std::move(swing);
            }
            if (fieldMask & sFieldSpeed)
            {
                float speed = 0.f;
                if (!reader.read(speed))
                    return std::nullopt;
                entity.mSpeed = speed;
            }
            if (fieldMask & sFieldAppearance)
            {
                AppearanceState appearance;
                std::uint8_t isMale = 0;
                if (!reader.readString(appearance.mRace) || !reader.readString(appearance.mHead)
                    || !reader.readString(appearance.mHair) || !reader.readString(appearance.mClass)
                    || !reader.readString(appearance.mName) || !reader.read(isMale))
                    return std::nullopt;
                appearance.mIsMale = isMale != 0;
                entity.mAppearance = appearance;
            }
            if (fieldMask & sFieldEquipment)
            {
                std::uint32_t wornCount = 0;
                if (!reader.read(wornCount))
                    return std::nullopt;
                // Bound the allocation/loop against the remaining buffer before reserving.
                if (wornCount > reader.remaining() / sMinEquipmentBytes)
                    return std::nullopt;
                std::vector<EquipmentSlot> equipment;
                equipment.reserve(wornCount);
                for (std::uint32_t w = 0; w < wornCount; ++w)
                {
                    EquipmentSlot worn;
                    if (!reader.read(worn.mSlot) || !reader.readString(worn.mItem))
                        return std::nullopt;
                    equipment.push_back(std::move(worn));
                }
                entity.mEquipment = std::move(equipment);
            }
            if (fieldMask & sFieldCell)
            {
                std::string cellId;
                if (!reader.readString(cellId))
                    return std::nullopt;
                entity.mCellId = std::move(cellId);
            }
            if (fieldMask & sFieldItem)
            {
                ItemState item;
                if (!reader.readString(item.mRefId) || !reader.read(item.mCount))
                    return std::nullopt;
                entity.mItem = std::move(item);
            }

            delta.mEntities.push_back(entity);
        }

        std::uint32_t removedCount = 0;
        if (!reader.read(removedCount))
            return std::nullopt;
        // Each removed entry is a RefNum (4 + 4); bound the loop/allocation against the buffer.
        if (removedCount > reader.remaining() / 8)
            return std::nullopt;
        delta.mRemovedItems.reserve(removedCount);
        for (std::uint32_t i = 0; i < removedCount; ++i)
        {
            ESM::RefNum removed;
            if (!reader.read(removed.mIndex) || !reader.read(removed.mContentFile))
                return std::nullopt;
            delta.mRemovedItems.push_back(removed);
        }

        return delta;
    }
}
