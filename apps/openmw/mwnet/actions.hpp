#ifndef OPENMW_MWNET_ACTIONS_H
#define OPENMW_MWNET_ACTIONS_H

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

#include <cstdint>
#include <string>

#include <osg/Vec3f>

#include <components/esm3/refnum.hpp>

namespace MWNet
{
    /// A melee hit a peer's player landed on a host-owned actor. Clients don't own the
    /// world, so they can't resolve a hit on an NPC themselves — they report it and the
    /// host applies it authoritatively. mAttacker is the attacking player's network id (so
    /// the host can target its avatar); mVictim is the struck actor's world RefNum. mDamage
    /// is the real damage the client computed with the full hit formula (weapon, strength,
    /// skill, resist, block); mHealthDamage selects health (weapons) vs fatigue (a
    /// non-knockout hand-to-hand hit). The host trusts the client's number for now;
    /// re-validating it host-side is a later hardening step.
    struct CombatHit
    {
        ESM::RefNum mAttacker;
        ESM::RefNum mVictim;
        float mDamage = 0.f;
        bool mHealthDamage = true;

        friend bool operator==(const CombatHit&, const CombatHit&) = default;
    };

    /// Damage the host dealt to a remote player's avatar, flowing the other way: host -> the
    /// owning client, which applies it to its real player. mTarget is that player's network id.
    /// This is what makes combat bidirectional — host NPCs (or another player) can hurt you.
    struct PlayerDamage
    {
        ESM::RefNum mTarget;
        float mDamage = 0.f;
        bool mHealthDamage = true;

        friend bool operator==(const PlayerDamage&, const PlayerDamage&) = default;
    };

    /// A client's request to drop an item into the shared world. Clients don't own the world, so
    /// they don't place the dropped reference themselves — they ask the host, which places it
    /// authoritatively (assigning a world RefNum) and replicates it back to everyone, the dropper
    /// included. mRefId is the item record to instantiate, mCount the stack size, mPosition where
    /// to drop it, and mCellId the cell to drop it in.
    struct ItemDrop
    {
        std::string mRefId;
        std::int32_t mCount = 1;
        osg::Vec3f mPosition;
        std::string mCellId;

        friend bool operator==(const ItemDrop&, const ItemDrop&) = default;
    };

    /// One item stack inside a container/corpse: the record to instantiate and the stack size.
    /// (First slice: per-instance state — condition, enchant charge, soul — is not carried, so a
    /// synced container's items reset to default condition. Fine for the common loot; a later pass
    /// can carry full ObjectState.)
    struct ContainerItem
    {
        std::string mRefId;
        std::int32_t mCount = 1;

        friend bool operator==(const ContainerItem&, const ContainerItem&) = default;
    };

    /// The full contents of one shared lootable inventory (a world container, or a corpse's
    /// inventory), keyed by the object's world RefNum. Sent whenever the contents change: a client
    /// reports the change to the host, the host applies it to the authoritative store and relays the
    /// new contents to every other peer, so all peers loot from the same shelves.
    struct ContainerState
    {
        ESM::RefNum mId;
        std::vector<ContainerItem> mItems;

        friend bool operator==(const ContainerState&, const ContainerState&) = default;
    };

    /// One frame's worth of reported actions crossing the transport (Reliable channel).
    /// mHits / mDrops / mItemsTaken flow client -> host (resolve my action); mPlayerDamages flow
    /// host -> client (you were hit). mContainers flow BOTH ways (a changed lootable inventory). A
    /// given batch is populated by one side and consumed by the other.
    struct ActionBatch
    {
        std::vector<CombatHit> mHits;
        std::vector<PlayerDamage> mPlayerDamages;
        // client -> host: items the peer dropped on the floor, for the host to place authoritatively.
        std::vector<ItemDrop> mDrops;
        // client -> host: RefNums of host-owned loose items the peer picked up, for the host to
        // delete from the shared world (it then replicates the removal to every other peer).
        std::vector<ESM::RefNum> mItemsTaken;
        // both ways: lootable inventories whose contents changed and must be synced.
        std::vector<ContainerState> mContainers;

        bool empty() const
        {
            return mHits.empty() && mPlayerDamages.empty() && mDrops.empty() && mItemsTaken.empty()
                && mContainers.empty();
        }

        friend bool operator==(const ActionBatch&, const ActionBatch&) = default;
    };

    std::vector<std::byte> serializeActions(const ActionBatch& batch);

    /// Parse an action batch from arbitrary bytes; std::nullopt on malformed input. Counts
    /// are validated against the remaining buffer, so it never over-reads or over-allocates
    /// on hostile data (it is fuzzed).
    std::optional<ActionBatch> deserializeActions(std::span<const std::byte> data);
}

#endif
