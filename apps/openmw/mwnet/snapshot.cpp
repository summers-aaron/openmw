#include "snapshot.hpp"

#include "bytestream.hpp"

namespace MWNet
{
    namespace
    {
        // Wire format version. Bumped if the layout below changes incompatibly.
        constexpr std::uint8_t sVersion = 1;

        // EntityState field bits (mFieldMask).
        constexpr std::uint8_t sFieldTransform = 1 << 0;
        constexpr std::uint8_t sFieldStats = 1 << 1;
        constexpr std::uint8_t sFieldDrawState = 1 << 2;
        constexpr std::uint8_t sKnownFields = sFieldTransform | sFieldStats | sFieldDrawState;

        // Smallest possible encoded entity: RefNum (4 + 4) + field mask (1).
        constexpr std::uint32_t sMinEntityBytes = 9;
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

            std::uint8_t fieldMask = 0;
            if (entity.mTransform)
                fieldMask |= sFieldTransform;
            if (entity.mStats)
                fieldMask |= sFieldStats;
            if (entity.mDrawState)
                fieldMask |= sFieldDrawState;
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

            std::uint8_t fieldMask = 0;
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

            delta.mEntities.push_back(entity);
        }

        return delta;
    }
}
