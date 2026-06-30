#include "actions.hpp"

#include "bytestream.hpp"

namespace MWNet
{
    namespace
    {
        constexpr std::uint8_t sVersion = 3;
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
        // Smallest encoded ContainerState: RefNum (4 + 4) + item count (4).
        constexpr std::uint32_t sMinContainerBytes = 12;
        // Smallest encoded ContainerItem: zero-length RefId (4) + count (4) + charge (4) +
        // enchant charge (4) + zero-length soul (4).
        constexpr std::uint32_t sMinContainerItemBytes = 20;
        // Smallest encoded ContainerChange: actor RefNum (8) + container RefNum (8) + item (20) + flag (1).
        constexpr std::uint32_t sMinContainerChangeBytes = 37;
        // Smallest encoded ContainerRevoke: target RefNum (8) + item (20).
        constexpr std::uint32_t sMinContainerRevokeBytes = 28;
        // Smallest encoded SummonAction: summoner RefNum (8) + zero-length effect id (4) + flag (1).
        constexpr std::uint32_t sMinSummonBytes = 13;

        void writeContainerItem(ByteWriter& writer, const ContainerItem& item)
        {
            writer.writeString(item.mRefId);
            writer.write(item.mCount);
            writer.write(item.mCharge);
            writer.write(item.mEnchantCharge);
            writer.writeString(item.mSoul);
        }

        bool readContainerItem(ByteReader& reader, ContainerItem& item)
        {
            return reader.readString(item.mRefId) && reader.read(item.mCount) && reader.read(item.mCharge)
                && reader.read(item.mEnchantCharge) && reader.readString(item.mSoul);
        }
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
        writer.write(static_cast<std::uint32_t>(batch.mContainers.size()));
        for (const ContainerState& container : batch.mContainers)
        {
            writer.write(container.mId.mIndex);
            writer.write(container.mId.mContentFile);
            writer.write(static_cast<std::uint32_t>(container.mItems.size()));
            for (const ContainerItem& item : container.mItems)
                writeContainerItem(writer, item);
        }
        writer.write(static_cast<std::uint32_t>(batch.mContainerChanges.size()));
        for (const ContainerChange& change : batch.mContainerChanges)
        {
            writer.write(change.mActor.mIndex);
            writer.write(change.mActor.mContentFile);
            writer.write(change.mContainer.mIndex);
            writer.write(change.mContainer.mContentFile);
            writer.write(static_cast<std::uint8_t>(change.mTake ? 1 : 0));
            writeContainerItem(writer, change.mItem);
        }
        writer.write(static_cast<std::uint32_t>(batch.mContainerRevokes.size()));
        for (const ContainerRevoke& revoke : batch.mContainerRevokes)
        {
            writer.write(revoke.mTarget.mIndex);
            writer.write(revoke.mTarget.mContentFile);
            writeContainerItem(writer, revoke.mItem);
        }
        writer.write(static_cast<std::uint32_t>(batch.mSummons.size()));
        for (const SummonAction& summon : batch.mSummons)
        {
            writer.write(summon.mSummoner.mIndex);
            writer.write(summon.mSummoner.mContentFile);
            writer.writeString(summon.mEffectId);
            writer.write(static_cast<std::uint8_t>(summon.mEnd ? 1 : 0));
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

        std::uint32_t containerCount = 0;
        if (!reader.read(containerCount))
            return std::nullopt;
        if (containerCount > reader.remaining() / sMinContainerBytes)
            return std::nullopt;
        batch.mContainers.reserve(containerCount);
        for (std::uint32_t i = 0; i < containerCount; ++i)
        {
            ContainerState container;
            std::uint32_t itemCount = 0;
            if (!reader.read(container.mId.mIndex) || !reader.read(container.mId.mContentFile)
                || !reader.read(itemCount))
                return std::nullopt;
            if (itemCount > reader.remaining() / sMinContainerItemBytes)
                return std::nullopt;
            container.mItems.reserve(itemCount);
            for (std::uint32_t j = 0; j < itemCount; ++j)
            {
                ContainerItem item;
                if (!readContainerItem(reader, item))
                    return std::nullopt;
                container.mItems.push_back(std::move(item));
            }
            batch.mContainers.push_back(std::move(container));
        }

        std::uint32_t changeCount = 0;
        if (!reader.read(changeCount))
            return std::nullopt;
        if (changeCount > reader.remaining() / sMinContainerChangeBytes)
            return std::nullopt;
        batch.mContainerChanges.reserve(changeCount);
        for (std::uint32_t i = 0; i < changeCount; ++i)
        {
            ContainerChange change;
            std::uint8_t take = 0;
            if (!reader.read(change.mActor.mIndex) || !reader.read(change.mActor.mContentFile)
                || !reader.read(change.mContainer.mIndex) || !reader.read(change.mContainer.mContentFile)
                || !reader.read(take) || !readContainerItem(reader, change.mItem))
                return std::nullopt;
            change.mTake = take != 0;
            batch.mContainerChanges.push_back(std::move(change));
        }

        std::uint32_t revokeCount = 0;
        if (!reader.read(revokeCount))
            return std::nullopt;
        if (revokeCount > reader.remaining() / sMinContainerRevokeBytes)
            return std::nullopt;
        batch.mContainerRevokes.reserve(revokeCount);
        for (std::uint32_t i = 0; i < revokeCount; ++i)
        {
            ContainerRevoke revoke;
            if (!reader.read(revoke.mTarget.mIndex) || !reader.read(revoke.mTarget.mContentFile)
                || !readContainerItem(reader, revoke.mItem))
                return std::nullopt;
            batch.mContainerRevokes.push_back(std::move(revoke));
        }

        std::uint32_t summonCount = 0;
        if (!reader.read(summonCount))
            return std::nullopt;
        if (summonCount > reader.remaining() / sMinSummonBytes)
            return std::nullopt;
        batch.mSummons.reserve(summonCount);
        for (std::uint32_t i = 0; i < summonCount; ++i)
        {
            SummonAction summon;
            std::uint8_t end = 0;
            if (!reader.read(summon.mSummoner.mIndex) || !reader.read(summon.mSummoner.mContentFile)
                || !reader.readString(summon.mEffectId) || !reader.read(end))
                return std::nullopt;
            summon.mEnd = end != 0;
            batch.mSummons.push_back(std::move(summon));
        }
        return batch;
    }
}
