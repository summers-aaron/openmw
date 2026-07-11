#include "actions.hpp"

#include <components/misc/strings/algorithm.hpp>

#include "bytestream.hpp"

namespace MWNet
{
    namespace
    {
        constexpr std::uint8_t sVersion = 17;
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
        // Smallest encoded PlayerBounty: target RefNum (4 + 4) + bounty (int32, 4).
        constexpr std::uint32_t sMinBountyBytes = 12;
        // Smallest encoded NpcSpeech: actor RefNum (4 + 4) + zero-length sound path (4) + zero-length
        // subtitle (4).
        constexpr std::uint32_t sMinSpeechBytes = 16;
        // Smallest encoded WorldSound: object RefNum (4 + 4) + position (3*4) + zero-length sound
        // id (4) + volume (4) + pitch (4) + origin RefNum (4 + 4).
        constexpr std::uint32_t sMinWorldSoundBytes = 40;
        // Smallest encoded ArrestRequest: target RefNum (4 + 4) + guard RefNum (4 + 4).
        constexpr std::uint32_t sMinArrestBytes = 16;
        // Smallest encoded CombatRequest: instigator RefNum (4 + 4) + target RefNum (4 + 4).
        constexpr std::uint32_t sMinCombatRequestBytes = 16;
        // Smallest encoded JournalDelta: zero-length topic (4) + index (4) + zero-length info id
        // (4) + zero-length text (4) + zero-length actor name (4) + 3 date ints (12) + origin
        // RefNum (4 + 4).
        constexpr std::uint32_t sMinJournalDeltaBytes = 40;
        // Smallest encoded GlobalDelta: zero-length name (4) + type (1) + int (4) + float (4) +
        // origin RefNum (4 + 4).
        constexpr std::uint32_t sMinGlobalDeltaBytes = 21;
        // Encoded TimeSync: hour (4) + 4 ints (16) + timescale (4).
        constexpr std::uint32_t sMinTimeSyncBytes = 24;
        // Encoded TimeRequest: hours (4) + origin RefNum (4 + 4).
        constexpr std::uint32_t sMinTimeRequestBytes = 12;
        // Encoded RefEnable: ref RefNum (4 + 4) + flag (1) + origin RefNum (4 + 4).
        constexpr std::uint32_t sMinRefEnableBytes = 17;
        // Smallest encoded ScriptRun: zero-length script (4) + flag (1) + target RefNum (4 + 4) +
        // zero-length target id (4) + origin RefNum (4 + 4).
        constexpr std::uint32_t sMinScriptRunBytes = 25;
        // Smallest encoded WeatherSync: zero-length region (4) + weatherId (4) + origin RefNum (4 + 4).
        constexpr std::uint32_t sMinWeatherSyncBytes = 16;
        // Encoded DoorMove: ref RefNum (4 + 4) + state (1) + lock level (4) + origin RefNum (4 + 4).
        constexpr std::uint32_t sMinDoorMoveBytes = 21;
        // Encoded SpellCast with zero effects: caster RefNum (4 + 4) + target RefNum (4 + 4) +
        // zero-length spell id (4) + zero-length name (4) + item RefNum (4 + 4) + flags (4) +
        // effect count (4) + origin RefNum (4 + 4).
        constexpr std::uint32_t sMinSpellCastBytes = 48;
        // Encoded SpellEffect: zero-length effect id (4) + zero-length arg (4) + min/max/duration
        // (3 * 4) + effect index (4) + flags (4).
        constexpr std::uint32_t sMinSpellEffectBytes = 28;

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

    bool isUnsyncedGlobal(std::string_view name)
    {
        // Lowercased comparison: global names reach the hook in whatever case the script wrote.
        static const std::string_view unsynced[] = {
            // The game clock — synced coherently via TimeSync, never per-write.
            "gamehour", "day", "month", "year", "dayspassed", "timescale",
            // Gates each client's private chargen bubble; leaking another peer's value would pop
            // a mid-chargen client into the shared world half-made (or re-open a finished one).
            "chargenstate",
            // One player's own dialogue-derived state: crime gold owed to the guard talking to
            // THEM, their race/vampirism checks. Re-derived per machine, never shared.
            "pchascrimegold", "pchasgolddiscount", "crimegolddiscount", "crimegoldturnin",
            "pchasturnin", "pcknownwerewolf", "pcrace", "pcvampire",
        };
        for (const std::string_view candidate : unsynced)
            if (Misc::StringUtils::ciEqual(name, candidate))
                return true;
        return false;
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
        writer.write(static_cast<std::uint32_t>(batch.mBounties.size()));
        for (const PlayerBounty& bounty : batch.mBounties)
        {
            writer.write(bounty.mTarget.mIndex);
            writer.write(bounty.mTarget.mContentFile);
            writer.write(bounty.mBounty);
        }
        writer.write(static_cast<std::uint32_t>(batch.mSpeech.size()));
        for (const NpcSpeech& speech : batch.mSpeech)
        {
            writer.write(speech.mActor.mIndex);
            writer.write(speech.mActor.mContentFile);
            writer.writeString(speech.mSound);
            writer.writeString(speech.mText);
        }
        writer.write(static_cast<std::uint32_t>(batch.mSounds.size()));
        for (const WorldSound& sound : batch.mSounds)
        {
            writer.write(sound.mObject.mIndex);
            writer.write(sound.mObject.mContentFile);
            for (int axis = 0; axis < 3; ++axis)
                writer.write(sound.mPosition[axis]);
            writer.writeString(sound.mSound);
            writer.write(sound.mVolume);
            writer.write(sound.mPitch);
            writer.write(sound.mOrigin.mIndex);
            writer.write(sound.mOrigin.mContentFile);
        }
        writer.write(static_cast<std::uint32_t>(batch.mArrests.size()));
        for (const ArrestRequest& arrest : batch.mArrests)
        {
            writer.write(arrest.mTarget.mIndex);
            writer.write(arrest.mTarget.mContentFile);
            writer.write(arrest.mGuard.mIndex);
            writer.write(arrest.mGuard.mContentFile);
        }
        writer.write(static_cast<std::uint32_t>(batch.mCombatRequests.size()));
        for (const CombatRequest& request : batch.mCombatRequests)
        {
            writer.write(request.mInstigator.mIndex);
            writer.write(request.mInstigator.mContentFile);
            writer.write(request.mTarget.mIndex);
            writer.write(request.mTarget.mContentFile);
        }
        writer.write(static_cast<std::uint32_t>(batch.mJournalDeltas.size()));
        for (const JournalDelta& delta : batch.mJournalDeltas)
        {
            writer.writeString(delta.mTopic);
            writer.write(delta.mIndex);
            writer.writeString(delta.mInfoId);
            writer.writeString(delta.mText);
            writer.writeString(delta.mActorName);
            writer.write(delta.mDay);
            writer.write(delta.mMonth);
            writer.write(delta.mDayOfMonth);
            writer.write(delta.mOrigin.mIndex);
            writer.write(delta.mOrigin.mContentFile);
        }
        writer.write(static_cast<std::uint32_t>(batch.mGlobalDeltas.size()));
        for (const GlobalDelta& delta : batch.mGlobalDeltas)
        {
            writer.writeString(delta.mName);
            writer.write(delta.mType);
            writer.write(delta.mIntValue);
            writer.write(delta.mFloatValue);
            writer.write(delta.mOrigin.mIndex);
            writer.write(delta.mOrigin.mContentFile);
        }
        writer.write(static_cast<std::uint32_t>(batch.mTimeSyncs.size()));
        for (const TimeSync& sync : batch.mTimeSyncs)
        {
            writer.write(sync.mGameHour);
            writer.write(sync.mDay);
            writer.write(sync.mMonth);
            writer.write(sync.mYear);
            writer.write(sync.mDaysPassed);
            writer.write(sync.mTimeScale);
        }
        writer.write(static_cast<std::uint32_t>(batch.mTimeRequests.size()));
        for (const TimeRequest& request : batch.mTimeRequests)
        {
            writer.write(request.mHours);
            writer.write(request.mOrigin.mIndex);
            writer.write(request.mOrigin.mContentFile);
        }
        writer.write(static_cast<std::uint32_t>(batch.mRefEnables.size()));
        for (const RefEnable& refEnable : batch.mRefEnables)
        {
            writer.write(refEnable.mRef.mIndex);
            writer.write(refEnable.mRef.mContentFile);
            writer.write(static_cast<std::uint8_t>(refEnable.mEnabled ? 1 : 0));
            writer.write(refEnable.mOrigin.mIndex);
            writer.write(refEnable.mOrigin.mContentFile);
        }
        writer.write(static_cast<std::uint32_t>(batch.mScriptRuns.size()));
        for (const ScriptRun& run : batch.mScriptRuns)
        {
            writer.writeString(run.mScript);
            writer.write(static_cast<std::uint8_t>(run.mRunning ? 1 : 0));
            writer.write(run.mTargetRef.mIndex);
            writer.write(run.mTargetRef.mContentFile);
            writer.writeString(run.mTargetId);
            writer.write(run.mOrigin.mIndex);
            writer.write(run.mOrigin.mContentFile);
        }
        writer.write(static_cast<std::uint32_t>(batch.mWeatherSyncs.size()));
        for (const WeatherSync& weather : batch.mWeatherSyncs)
        {
            writer.writeString(weather.mRegion);
            writer.write(weather.mWeatherId);
            writer.write(weather.mOrigin.mIndex);
            writer.write(weather.mOrigin.mContentFile);
        }
        writer.write(static_cast<std::uint32_t>(batch.mDoorMoves.size()));
        for (const DoorMove& move : batch.mDoorMoves)
        {
            writer.write(move.mRef.mIndex);
            writer.write(move.mRef.mContentFile);
            writer.write(move.mState);
            writer.write(move.mLockLevel);
            writer.write(move.mOrigin.mIndex);
            writer.write(move.mOrigin.mContentFile);
        }
        writer.write(static_cast<std::uint32_t>(batch.mSpellCasts.size()));
        for (const SpellCast& cast : batch.mSpellCasts)
        {
            writer.write(cast.mCaster.mIndex);
            writer.write(cast.mCaster.mContentFile);
            writer.write(cast.mTarget.mIndex);
            writer.write(cast.mTarget.mContentFile);
            writer.writeString(cast.mSourceSpellId);
            writer.writeString(cast.mDisplayName);
            writer.write(cast.mItem.mIndex);
            writer.write(cast.mItem.mContentFile);
            writer.write(cast.mFlags);
            writer.write(static_cast<std::uint32_t>(cast.mEffects.size()));
            for (const SpellEffect& effect : cast.mEffects)
            {
                writer.writeString(effect.mEffectId);
                writer.writeString(effect.mArg);
                writer.write(effect.mMinMagnitude);
                writer.write(effect.mMaxMagnitude);
                writer.write(effect.mDuration);
                writer.write(effect.mEffectIndex);
                writer.write(effect.mFlags);
            }
            writer.write(cast.mOrigin.mIndex);
            writer.write(cast.mOrigin.mContentFile);
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

        std::uint32_t bountyCount = 0;
        if (!reader.read(bountyCount))
            return std::nullopt;
        if (bountyCount > reader.remaining() / sMinBountyBytes)
            return std::nullopt;
        batch.mBounties.reserve(bountyCount);
        for (std::uint32_t i = 0; i < bountyCount; ++i)
        {
            PlayerBounty bounty;
            if (!reader.read(bounty.mTarget.mIndex) || !reader.read(bounty.mTarget.mContentFile)
                || !reader.read(bounty.mBounty))
                return std::nullopt;
            batch.mBounties.push_back(bounty);
        }

        std::uint32_t speechCount = 0;
        if (!reader.read(speechCount))
            return std::nullopt;
        if (speechCount > reader.remaining() / sMinSpeechBytes)
            return std::nullopt;
        batch.mSpeech.reserve(speechCount);
        for (std::uint32_t i = 0; i < speechCount; ++i)
        {
            NpcSpeech speech;
            if (!reader.read(speech.mActor.mIndex) || !reader.read(speech.mActor.mContentFile)
                || !reader.readString(speech.mSound) || !reader.readString(speech.mText))
                return std::nullopt;
            batch.mSpeech.push_back(std::move(speech));
        }

        std::uint32_t soundCount = 0;
        if (!reader.read(soundCount))
            return std::nullopt;
        if (soundCount > reader.remaining() / sMinWorldSoundBytes)
            return std::nullopt;
        batch.mSounds.reserve(soundCount);
        for (std::uint32_t i = 0; i < soundCount; ++i)
        {
            WorldSound sound;
            if (!reader.read(sound.mObject.mIndex) || !reader.read(sound.mObject.mContentFile))
                return std::nullopt;
            for (int axis = 0; axis < 3; ++axis)
                if (!reader.read(sound.mPosition[axis]))
                    return std::nullopt;
            if (!reader.readString(sound.mSound) || !reader.read(sound.mVolume) || !reader.read(sound.mPitch)
                || !reader.read(sound.mOrigin.mIndex) || !reader.read(sound.mOrigin.mContentFile))
                return std::nullopt;
            batch.mSounds.push_back(std::move(sound));
        }

        std::uint32_t arrestCount = 0;
        if (!reader.read(arrestCount))
            return std::nullopt;
        if (arrestCount > reader.remaining() / sMinArrestBytes)
            return std::nullopt;
        batch.mArrests.reserve(arrestCount);
        for (std::uint32_t i = 0; i < arrestCount; ++i)
        {
            ArrestRequest arrest;
            if (!reader.read(arrest.mTarget.mIndex) || !reader.read(arrest.mTarget.mContentFile)
                || !reader.read(arrest.mGuard.mIndex) || !reader.read(arrest.mGuard.mContentFile))
                return std::nullopt;
            batch.mArrests.push_back(arrest);
        }

        std::uint32_t combatRequestCount = 0;
        if (!reader.read(combatRequestCount))
            return std::nullopt;
        if (combatRequestCount > reader.remaining() / sMinCombatRequestBytes)
            return std::nullopt;
        batch.mCombatRequests.reserve(combatRequestCount);
        for (std::uint32_t i = 0; i < combatRequestCount; ++i)
        {
            CombatRequest request;
            if (!reader.read(request.mInstigator.mIndex) || !reader.read(request.mInstigator.mContentFile)
                || !reader.read(request.mTarget.mIndex) || !reader.read(request.mTarget.mContentFile))
                return std::nullopt;
            batch.mCombatRequests.push_back(request);
        }

        std::uint32_t journalCount = 0;
        if (!reader.read(journalCount))
            return std::nullopt;
        if (journalCount > reader.remaining() / sMinJournalDeltaBytes)
            return std::nullopt;
        batch.mJournalDeltas.reserve(journalCount);
        for (std::uint32_t i = 0; i < journalCount; ++i)
        {
            JournalDelta delta;
            if (!reader.readString(delta.mTopic) || !reader.read(delta.mIndex) || !reader.readString(delta.mInfoId)
                || !reader.readString(delta.mText) || !reader.readString(delta.mActorName) || !reader.read(delta.mDay)
                || !reader.read(delta.mMonth) || !reader.read(delta.mDayOfMonth) || !reader.read(delta.mOrigin.mIndex)
                || !reader.read(delta.mOrigin.mContentFile))
                return std::nullopt;
            batch.mJournalDeltas.push_back(std::move(delta));
        }

        std::uint32_t globalCount = 0;
        if (!reader.read(globalCount))
            return std::nullopt;
        if (globalCount > reader.remaining() / sMinGlobalDeltaBytes)
            return std::nullopt;
        batch.mGlobalDeltas.reserve(globalCount);
        for (std::uint32_t i = 0; i < globalCount; ++i)
        {
            GlobalDelta delta;
            if (!reader.readString(delta.mName) || !reader.read(delta.mType) || !reader.read(delta.mIntValue)
                || !reader.read(delta.mFloatValue) || !reader.read(delta.mOrigin.mIndex)
                || !reader.read(delta.mOrigin.mContentFile))
                return std::nullopt;
            batch.mGlobalDeltas.push_back(std::move(delta));
        }

        std::uint32_t timeSyncCount = 0;
        if (!reader.read(timeSyncCount))
            return std::nullopt;
        if (timeSyncCount > reader.remaining() / sMinTimeSyncBytes)
            return std::nullopt;
        batch.mTimeSyncs.reserve(timeSyncCount);
        for (std::uint32_t i = 0; i < timeSyncCount; ++i)
        {
            TimeSync sync;
            if (!reader.read(sync.mGameHour) || !reader.read(sync.mDay) || !reader.read(sync.mMonth)
                || !reader.read(sync.mYear) || !reader.read(sync.mDaysPassed) || !reader.read(sync.mTimeScale))
                return std::nullopt;
            batch.mTimeSyncs.push_back(sync);
        }

        std::uint32_t timeRequestCount = 0;
        if (!reader.read(timeRequestCount))
            return std::nullopt;
        if (timeRequestCount > reader.remaining() / sMinTimeRequestBytes)
            return std::nullopt;
        batch.mTimeRequests.reserve(timeRequestCount);
        for (std::uint32_t i = 0; i < timeRequestCount; ++i)
        {
            TimeRequest request;
            if (!reader.read(request.mHours) || !reader.read(request.mOrigin.mIndex)
                || !reader.read(request.mOrigin.mContentFile))
                return std::nullopt;
            batch.mTimeRequests.push_back(request);
        }

        std::uint32_t refEnableCount = 0;
        if (!reader.read(refEnableCount))
            return std::nullopt;
        if (refEnableCount > reader.remaining() / sMinRefEnableBytes)
            return std::nullopt;
        batch.mRefEnables.reserve(refEnableCount);
        for (std::uint32_t i = 0; i < refEnableCount; ++i)
        {
            RefEnable refEnable;
            std::uint8_t enabled = 0;
            if (!reader.read(refEnable.mRef.mIndex) || !reader.read(refEnable.mRef.mContentFile)
                || !reader.read(enabled) || !reader.read(refEnable.mOrigin.mIndex)
                || !reader.read(refEnable.mOrigin.mContentFile))
                return std::nullopt;
            refEnable.mEnabled = enabled != 0;
            batch.mRefEnables.push_back(refEnable);
        }

        std::uint32_t scriptRunCount = 0;
        if (!reader.read(scriptRunCount))
            return std::nullopt;
        if (scriptRunCount > reader.remaining() / sMinScriptRunBytes)
            return std::nullopt;
        batch.mScriptRuns.reserve(scriptRunCount);
        for (std::uint32_t i = 0; i < scriptRunCount; ++i)
        {
            ScriptRun run;
            std::uint8_t running = 0;
            if (!reader.readString(run.mScript) || !reader.read(running) || !reader.read(run.mTargetRef.mIndex)
                || !reader.read(run.mTargetRef.mContentFile) || !reader.readString(run.mTargetId)
                || !reader.read(run.mOrigin.mIndex) || !reader.read(run.mOrigin.mContentFile))
                return std::nullopt;
            run.mRunning = running != 0;
            batch.mScriptRuns.push_back(std::move(run));
        }

        std::uint32_t weatherCount = 0;
        if (!reader.read(weatherCount))
            return std::nullopt;
        if (weatherCount > reader.remaining() / sMinWeatherSyncBytes)
            return std::nullopt;
        batch.mWeatherSyncs.reserve(weatherCount);
        for (std::uint32_t i = 0; i < weatherCount; ++i)
        {
            WeatherSync weather;
            if (!reader.readString(weather.mRegion) || !reader.read(weather.mWeatherId)
                || !reader.read(weather.mOrigin.mIndex) || !reader.read(weather.mOrigin.mContentFile))
                return std::nullopt;
            batch.mWeatherSyncs.push_back(std::move(weather));
        }

        std::uint32_t doorMoveCount = 0;
        if (!reader.read(doorMoveCount))
            return std::nullopt;
        if (doorMoveCount > reader.remaining() / sMinDoorMoveBytes)
            return std::nullopt;
        batch.mDoorMoves.reserve(doorMoveCount);
        for (std::uint32_t i = 0; i < doorMoveCount; ++i)
        {
            DoorMove move;
            if (!reader.read(move.mRef.mIndex) || !reader.read(move.mRef.mContentFile) || !reader.read(move.mState)
                || !reader.read(move.mLockLevel) || !reader.read(move.mOrigin.mIndex)
                || !reader.read(move.mOrigin.mContentFile))
                return std::nullopt;
            batch.mDoorMoves.push_back(move);
        }

        std::uint32_t spellCastCount = 0;
        if (!reader.read(spellCastCount))
            return std::nullopt;
        if (spellCastCount > reader.remaining() / sMinSpellCastBytes)
            return std::nullopt;
        batch.mSpellCasts.reserve(spellCastCount);
        for (std::uint32_t i = 0; i < spellCastCount; ++i)
        {
            SpellCast cast;
            if (!reader.read(cast.mCaster.mIndex) || !reader.read(cast.mCaster.mContentFile)
                || !reader.read(cast.mTarget.mIndex) || !reader.read(cast.mTarget.mContentFile)
                || !reader.readString(cast.mSourceSpellId) || !reader.readString(cast.mDisplayName)
                || !reader.read(cast.mItem.mIndex) || !reader.read(cast.mItem.mContentFile) || !reader.read(cast.mFlags))
                return std::nullopt;
            std::uint32_t effectCount = 0;
            if (!reader.read(effectCount))
                return std::nullopt;
            if (effectCount > reader.remaining() / sMinSpellEffectBytes)
                return std::nullopt;
            cast.mEffects.reserve(effectCount);
            for (std::uint32_t j = 0; j < effectCount; ++j)
            {
                SpellEffect effect;
                if (!reader.readString(effect.mEffectId) || !reader.readString(effect.mArg)
                    || !reader.read(effect.mMinMagnitude) || !reader.read(effect.mMaxMagnitude)
                    || !reader.read(effect.mDuration) || !reader.read(effect.mEffectIndex)
                    || !reader.read(effect.mFlags))
                    return std::nullopt;
                cast.mEffects.push_back(std::move(effect));
            }
            if (!reader.read(cast.mOrigin.mIndex) || !reader.read(cast.mOrigin.mContentFile))
                return std::nullopt;
            batch.mSpellCasts.push_back(std::move(cast));
        }
        return batch;
    }
}
