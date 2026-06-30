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

    /// Host -> the owning client: the player's new total crime bounty, the result of the host
    /// resolving a crime for that player's avatar (e.g. assaulting an NPC). mTarget is the player's
    /// network id; mBounty is the absolute new bounty (not a delta) so it is idempotent and a late
    /// joiner / resync converges to the right value rather than accumulating.
    struct PlayerBounty
    {
        ESM::RefNum mTarget;
        std::int32_t mBounty = 0;

        friend bool operator==(const PlayerBounty&, const PlayerBounty&) = default;
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

    /// One item stack inside a container/corpse: the record to instantiate, the stack size, and the
    /// per-instance state that distinguishes otherwise-identical items — condition (mCharge, -1 =
    /// full/unset), enchantment charge (mEnchantCharge, -1 = full/unset), and a bound soul (mSoul,
    /// empty = none). Items differing in any of these don't stack, so each is its own ContainerItem.
    struct ContainerItem
    {
        std::string mRefId;
        std::int32_t mCount = 1;
        std::int32_t mCharge = -1;
        float mEnchantCharge = -1.f;
        std::string mSoul;

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

    /// A client's request to move an item between a lootable inventory and its own inventory, for
    /// the host to resolve authoritatively. mActor is the requesting player's network id (so an
    /// over-take can be corrected back to it); mContainer the lootable; mItem the stack moved (id +
    /// count + condition/charge/soul). mTake true = take (container -> player, granted only up to
    /// what's actually there); false = put (player -> container, always applied).
    struct ContainerChange
    {
        ESM::RefNum mActor;
        ESM::RefNum mContainer;
        ContainerItem mItem;
        bool mTake = true;

        friend bool operator==(const ContainerChange&, const ContainerChange&) = default;
    };

    /// Host -> the over-taking client: it claimed mItem.mCount more than the container actually held
    /// (another peer beat it to those items), so it must remove that many from its own inventory.
    struct ContainerRevoke
    {
        ESM::RefNum mTarget;
        ContainerItem mItem;

        friend bool operator==(const ContainerRevoke&, const ContainerRevoke&) = default;
    };

    /// A client's summon, routed to the host so the summoned creature is host-authoritative (owned and
    /// simulated by the host, like any world NPC, so its AI and combat ride the normal paths). mSummoner
    /// is the casting player's network id — the host spawns the creature bound to that player's avatar.
    /// mEffectId is the summon magic effect's serialized RefId (the host maps it to the creature record).
    /// mEnd false = spawn (the player cast the summon); true = despawn (the player's summon effect ended).
    /// (netId, effectId) keys the host's registry that links the despawn back to the spawned creature.
    struct SummonAction
    {
        ESM::RefNum mSummoner;
        std::string mEffectId;
        bool mEnd = false;

        friend bool operator==(const SummonAction&, const SummonAction&) = default;
    };

    /// Host -> clients: a host-owned actor spoke a voiced line (a combat taunt, a greeting, a hit
    /// grunt, a scripted Say). The host owns and simulates the world's NPCs, so all their say()
    /// calls happen host-side; clients run no AI for those actors and would otherwise stay silent.
    /// mActor is the speaking actor's world RefNum; mSound is the already-corrected voice file path
    /// the host resolved (replicating the resolved file, not the dialogue topic, keeps it
    /// deterministic — the client just plays it, no re-filtering). mText is the line's subtitle (empty
    /// if the speech carried none); it is sent regardless of the host's subtitle setting, and each
    /// client decides whether to show it from its OWN setting. Cosmetic only: the audio/subtitle play
    /// on whichever peers have that actor's cell loaded; gameplay stays authoritative on the host.
    struct NpcSpeech
    {
        ESM::RefNum mActor;
        std::string mSound;
        std::string mText;

        friend bool operator==(const NpcSpeech&, const NpcSpeech&) = default;
    };

    /// Host -> the owning client: a host guard pursuing that client's avatar for a crime has caught it,
    /// so the client should open the arrest dialogue. The host can't show the client's UI (and opening
    /// it on the host would pull the host's own player into the conversation), so it routes the arrest
    /// to the avatar's owner. mTarget is that player's network id; mGuard is the arresting guard's world
    /// RefNum, which the client resolves to its local copy of the guard to talk to.
    struct ArrestRequest
    {
        ESM::RefNum mTarget;
        ESM::RefNum mGuard;

        friend bool operator==(const ArrestRequest&, const ArrestRequest&) = default;
    };

    /// A client -> host request to make a host-owned actor fight this client's player: the client can't
    /// drive a host actor (its local copy is a suppressed puppet), so when something tells that actor to
    /// attack the local player — the "resist arrest" dialogue's StartCombat, or any scripted aggression —
    /// it routes the order to the host. mInstigator is the host actor's world RefNum (the guard); mTarget
    /// is the requesting player's network id, whose avatar the host puts that actor into combat with.
    struct CombatRequest
    {
        ESM::RefNum mInstigator;
        ESM::RefNum mTarget;

        friend bool operator==(const CombatRequest&, const CombatRequest&) = default;
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
        // host -> clients: the authoritative full contents of a lootable that changed.
        std::vector<ContainerState> mContainers;
        // client -> host: take/put requests for the host to resolve against its authoritative record.
        std::vector<ContainerChange> mContainerChanges;
        // host -> the over-taking client: items it must drop from its inventory (lost a take race).
        std::vector<ContainerRevoke> mContainerRevokes;
        // client -> host: spawn/despawn a host-authoritative summoned creature for the casting player.
        std::vector<SummonAction> mSummons;
        // host -> clients: a player's new total crime bounty after the host resolved a crime for it.
        std::vector<PlayerBounty> mBounties;
        // host -> clients: voiced lines a host-owned actor spoke, for clients to replay on that actor.
        std::vector<NpcSpeech> mSpeech;
        // host -> the owning client: a guard caught its avatar, so it should open the arrest dialogue.
        std::vector<ArrestRequest> mArrests;
        // client -> host: make a host-owned actor fight my avatar (resist arrest / scripted aggression).
        std::vector<CombatRequest> mCombatRequests;

        bool empty() const
        {
            return mHits.empty() && mPlayerDamages.empty() && mDrops.empty() && mItemsTaken.empty()
                && mContainers.empty() && mContainerChanges.empty() && mContainerRevokes.empty()
                && mSummons.empty() && mBounties.empty() && mSpeech.empty() && mArrests.empty()
                && mCombatRequests.empty();
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
