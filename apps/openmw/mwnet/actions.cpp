#include "actions.hpp"

#include "bytestream.hpp"

namespace MWNet
{
    namespace
    {
        constexpr std::uint8_t sVersion = 1;
        // Smallest encoded CombatHit: attacker RefNum (4+4) + victim RefNum (4+4).
        constexpr std::uint32_t sMinHitBytes = 16;
    }

    std::vector<std::byte> serializeActions(const ActionBatch& batch)
    {
        std::vector<std::byte> out;
        ByteWriter writer(out);

        writer.write(sVersion);
        writer.write(static_cast<std::uint32_t>(batch.mHits.size()));
        for (const CombatHit& hit : batch.mHits)
        {
            writer.write(hit.mAttacker.mIndex);
            writer.write(hit.mAttacker.mContentFile);
            writer.write(hit.mVictim.mIndex);
            writer.write(hit.mVictim.mContentFile);
        }
        return out;
    }

    std::optional<ActionBatch> deserializeActions(std::span<const std::byte> data)
    {
        ByteReader reader(data);

        std::uint8_t version = 0;
        if (!reader.read(version) || version != sVersion)
            return std::nullopt;

        ActionBatch batch;
        std::uint32_t count = 0;
        if (!reader.read(count))
            return std::nullopt;
        if (count > reader.remaining() / sMinHitBytes)
            return std::nullopt;
        batch.mHits.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i)
        {
            CombatHit hit;
            if (!reader.read(hit.mAttacker.mIndex) || !reader.read(hit.mAttacker.mContentFile)
                || !reader.read(hit.mVictim.mIndex) || !reader.read(hit.mVictim.mContentFile))
                return std::nullopt;
            batch.mHits.push_back(hit);
        }
        return batch;
    }
}
