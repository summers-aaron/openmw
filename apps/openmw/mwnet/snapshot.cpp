#include "snapshot.hpp"

#include <cstring>
#include <type_traits>

namespace MWNet
{
    namespace
    {
        // Wire format version. Bumped if the layout below changes incompatibly.
        constexpr std::uint8_t sVersion = 1;

        // EntityState field bits (mFieldMask).
        constexpr std::uint8_t sFieldTransform = 1 << 0;
        constexpr std::uint8_t sKnownFields = sFieldTransform;

        // Smallest possible encoded entity: RefNum (4 + 4) + field mask (1).
        constexpr std::uint32_t sMinEntityBytes = 9;

        // Values cross the seam in host byte order. That is correct for the
        // loopback (same process) and for the in-process server/client split;
        // the real network transport (M11) is responsible for byte-order
        // normalization across heterogeneous hosts.
        class ByteWriter
        {
            std::vector<std::byte>& mOut;

        public:
            explicit ByteWriter(std::vector<std::byte>& out)
                : mOut(out)
            {
            }

            template <class T>
            void write(const T& value)
            {
                static_assert(std::is_trivially_copyable_v<T>);
                const auto* bytes = reinterpret_cast<const std::byte*>(&value);
                mOut.insert(mOut.end(), bytes, bytes + sizeof(T));
            }
        };

        class ByteReader
        {
            std::span<const std::byte> mData;
            std::size_t mPos = 0;

        public:
            explicit ByteReader(std::span<const std::byte> data)
                : mData(data)
            {
            }

            // mPos never exceeds size(), so size() - mPos cannot underflow.
            std::size_t remaining() const { return mData.size() - mPos; }

            template <class T>
            [[nodiscard]] bool read(T& value)
            {
                static_assert(std::is_trivially_copyable_v<T>);
                if (sizeof(T) > remaining())
                    return false;
                std::memcpy(&value, mData.data() + mPos, sizeof(T));
                mPos += sizeof(T);
                return true;
            }
        };
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
            writer.write(fieldMask);

            if (entity.mTransform)
            {
                for (int i = 0; i < 3; ++i)
                    writer.write(entity.mTransform->mPosition[i]);
                for (int i = 0; i < 3; ++i)
                    writer.write(entity.mTransform->mRotation[i]);
            }
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

            delta.mEntities.push_back(entity);
        }

        return delta;
    }
}
