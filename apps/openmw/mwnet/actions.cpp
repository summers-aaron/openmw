#include "actions.hpp"

#include "bytestream.hpp"

namespace MWNet
{
    namespace
    {
        constexpr std::uint8_t sVersion = 1;
        // Smallest encoded CombatHit: attacker RefNum (4+4) + victim RefNum (4+4) + damage
        // (float, 4) + health-damage flag (1).
        constexpr std::uint32_t sMinHitBytes = 21;
        // Smallest encoded PlayerDamage: target RefNum (4+4) + damage (4) + flag (1).
        constexpr std::uint32_t sMinPlayerDamageBytes = 13;
        // Smallest encoded PlayerBounty: target RefNum (4+4) + bounty (int32, 4).
        constexpr std::uint32_t sMinPlayerBountyBytes = 12;
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
        writer.write(static_cast<std::uint32_t>(batch.mBounties.size()));
        for (const PlayerBounty& pb : batch.mBounties)
        {
            writer.write(pb.mTarget.mIndex);
            writer.write(pb.mTarget.mContentFile);
            writer.write(pb.mBounty);
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

        std::uint32_t bountyCount = 0;
        if (!reader.read(bountyCount))
            return std::nullopt;
        if (bountyCount > reader.remaining() / sMinPlayerBountyBytes)
            return std::nullopt;
        batch.mBounties.reserve(bountyCount);
        for (std::uint32_t i = 0; i < bountyCount; ++i)
        {
            PlayerBounty pb;
            if (!reader.read(pb.mTarget.mIndex) || !reader.read(pb.mTarget.mContentFile) || !reader.read(pb.mBounty))
                return std::nullopt;
            batch.mBounties.push_back(pb);
        }
        return batch;
    }
}
