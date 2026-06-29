#include "actions.hpp"

#include "bytestream.hpp"

namespace MWNet
{
    namespace
    {
        constexpr std::uint8_t sVersion = 2;
        // Smallest encoded CombatHit: attacker RefNum (4+4) + victim RefNum (4+4) + damage
        // (float, 4) + health-damage flag (1).
        constexpr std::uint32_t sMinHitBytes = 21;
        // Smallest encoded PlayerDamage: target RefNum (4+4) + damage (4) + flag (1).
        constexpr std::uint32_t sMinPlayerDamageBytes = 13;
        // Smallest encoded ItemDrop: zero-length RefId (4) + count (4) + position (3*4) + zero-length
        // cell id (4).
        constexpr std::uint32_t sMinDropBytes = 24;
        // Smallest encoded items-taken entry: a RefNum (4 + 4).
        constexpr std::uint32_t sMinTakenBytes = 8;
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
            writer.write(hit.mDamage);
            writer.write(static_cast<std::uint8_t>(hit.mHealthDamage ? 1 : 0));
        }
        writer.write(static_cast<std::uint32_t>(batch.mPlayerDamages.size()));
        for (const PlayerDamage& pd : batch.mPlayerDamages)
        {
            writer.write(pd.mTarget.mIndex);
            writer.write(pd.mTarget.mContentFile);
            writer.write(pd.mDamage);
            writer.write(static_cast<std::uint8_t>(pd.mHealthDamage ? 1 : 0));
        }
        writer.write(static_cast<std::uint32_t>(batch.mDrops.size()));
        for (const ItemDrop& drop : batch.mDrops)
        {
            writer.writeString(drop.mRefId);
            writer.write(drop.mCount);
            for (int i = 0; i < 3; ++i)
                writer.write(drop.mPosition[i]);
            writer.writeString(drop.mCellId);
        }
        writer.write(static_cast<std::uint32_t>(batch.mItemsTaken.size()));
        for (const ESM::RefNum& taken : batch.mItemsTaken)
        {
            writer.write(taken.mIndex);
            writer.write(taken.mContentFile);
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
            std::uint8_t healthDamage = 0;
            if (!reader.read(hit.mAttacker.mIndex) || !reader.read(hit.mAttacker.mContentFile)
                || !reader.read(hit.mVictim.mIndex) || !reader.read(hit.mVictim.mContentFile)
                || !reader.read(hit.mDamage) || !reader.read(healthDamage))
                return std::nullopt;
            hit.mHealthDamage = healthDamage != 0;
            batch.mHits.push_back(hit);
        }

        std::uint32_t pdCount = 0;
        if (!reader.read(pdCount))
            return std::nullopt;
        if (pdCount > reader.remaining() / sMinPlayerDamageBytes)
            return std::nullopt;
        batch.mPlayerDamages.reserve(pdCount);
        for (std::uint32_t i = 0; i < pdCount; ++i)
        {
            PlayerDamage pd;
            std::uint8_t healthDamage = 0;
            if (!reader.read(pd.mTarget.mIndex) || !reader.read(pd.mTarget.mContentFile) || !reader.read(pd.mDamage)
                || !reader.read(healthDamage))
                return std::nullopt;
            pd.mHealthDamage = healthDamage != 0;
            batch.mPlayerDamages.push_back(pd);
        }

        std::uint32_t dropCount = 0;
        if (!reader.read(dropCount))
            return std::nullopt;
        if (dropCount > reader.remaining() / sMinDropBytes)
            return std::nullopt;
        batch.mDrops.reserve(dropCount);
        for (std::uint32_t i = 0; i < dropCount; ++i)
        {
            ItemDrop drop;
            if (!reader.readString(drop.mRefId) || !reader.read(drop.mCount))
                return std::nullopt;
            for (int axis = 0; axis < 3; ++axis)
                if (!reader.read(drop.mPosition[axis]))
                    return std::nullopt;
            if (!reader.readString(drop.mCellId))
                return std::nullopt;
            batch.mDrops.push_back(std::move(drop));
        }

        std::uint32_t takenCount = 0;
        if (!reader.read(takenCount))
            return std::nullopt;
        if (takenCount > reader.remaining() / sMinTakenBytes)
            return std::nullopt;
        batch.mItemsTaken.reserve(takenCount);
        for (std::uint32_t i = 0; i < takenCount; ++i)
        {
            ESM::RefNum taken;
            if (!reader.read(taken.mIndex) || !reader.read(taken.mContentFile))
                return std::nullopt;
            batch.mItemsTaken.push_back(taken);
        }
        return batch;
    }
}
