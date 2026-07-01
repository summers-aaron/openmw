#include "events.hpp"

#include "bytestream.hpp"

namespace MWNet
{
    namespace
    {
        // Wire format version. Bumped if the layout below changes incompatibly.
        constexpr std::uint8_t sVersion = 1;

        // Smallest possible encoded global event: two empty strings (length 0).
        constexpr std::uint32_t sMinGlobalBytes = 8;
        // Smallest possible encoded local event: RefNum (4 + 4) + two empty strings.
        constexpr std::uint32_t sMinLocalBytes = 16;
    }

    std::vector<std::byte> serializeEvents(const EventBatch& batch)
    {
        std::vector<std::byte> out;
        ByteWriter writer(out);

        writer.write(sVersion);

        writer.write(static_cast<std::uint32_t>(batch.mGlobal.size()));
        for (const GlobalEvent& event : batch.mGlobal)
        {
            writer.writeString(event.mEventName);
            writer.writeString(event.mEventData);
        }

        writer.write(static_cast<std::uint32_t>(batch.mLocal.size()));
        for (const LocalEvent& event : batch.mLocal)
        {
            writer.write(event.mDest.mIndex);
            writer.write(event.mDest.mContentFile);
            writer.writeString(event.mEventName);
            writer.writeString(event.mEventData);
        }

        return out;
    }

    std::optional<EventBatch> deserializeEvents(std::span<const std::byte> data)
    {
        ByteReader reader(data);

        std::uint8_t version = 0;
        if (!reader.read(version) || version != sVersion)
            return std::nullopt;

        EventBatch batch;

        std::uint32_t globalCount = 0;
        if (!reader.read(globalCount))
            return std::nullopt;
        // Reject counts that cannot fit in the remaining bytes before reserving.
        if (globalCount > reader.remaining() / sMinGlobalBytes)
            return std::nullopt;
        batch.mGlobal.reserve(globalCount);
        for (std::uint32_t i = 0; i < globalCount; ++i)
        {
            GlobalEvent event;
            if (!reader.readString(event.mEventName) || !reader.readString(event.mEventData))
                return std::nullopt;
            batch.mGlobal.push_back(std::move(event));
        }

        std::uint32_t localCount = 0;
        if (!reader.read(localCount))
            return std::nullopt;
        if (localCount > reader.remaining() / sMinLocalBytes)
            return std::nullopt;
        batch.mLocal.reserve(localCount);
        for (std::uint32_t i = 0; i < localCount; ++i)
        {
            LocalEvent event;
            if (!reader.read(event.mDest.mIndex) || !reader.read(event.mDest.mContentFile))
                return std::nullopt;
            if (!reader.readString(event.mEventName) || !reader.readString(event.mEventData))
                return std::nullopt;
            batch.mLocal.push_back(std::move(event));
        }

        return batch;
    }
}
