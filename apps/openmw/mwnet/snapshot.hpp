#ifndef OPENMW_MWNET_SNAPSHOT_H
#define OPENMW_MWNET_SNAPSHOT_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <osg/Vec3f>

#include <components/esm3/refnum.hpp>

namespace MWNet
{
    /// Reserved RefNum content-file value marking a "player" entity in a snapshot.
    /// A real world reference never uses it, so an EntityState whose mId.mContentFile
    /// equals this is a peer's player (its mIndex is the peer's id), to be shown as an
    /// avatar rather than matched against a local world reference.
    inline constexpr std::int32_t sNetPlayerContentFile = -1000;

    inline bool isNetPlayer(const ESM::RefNum& id)
    {
        return id.mContentFile == sNetPlayerContentFile;
    }

    /// Replicated rigid-body state for one entity: world position and Euler
    /// rotation (matching ESM::Position's pos/rot). The canonical high-frequency
    /// field; other replicated fields (CreatureStats, equipment, active anim,
    /// active spells) are added alongside this as std::optional members on
    /// EntityState so a delta only carries what changed.
    struct TransformState
    {
        osg::Vec3f mPosition;
        osg::Vec3f mRotation;

        friend bool operator==(const TransformState&, const TransformState&) = default;
    };

    /// Current values of an actor's three dynamic stats (health, magicka, fatigue). Lets
    /// the world's combat state — damage and death (health <= 0) — replicate, not just
    /// where actors are.
    struct DynamicStats
    {
        float mHealth;
        float mMagicka;
        float mFatigue;

        friend bool operator==(const DynamicStats&, const DynamicStats&) = default;
    };

    /// A peer player's character appearance: the records that define how its
    /// avatar looks, carried as stable serialized-text RefIds. Those records
    /// (race, head/hair body parts, class) come from the shared content files,
    /// so the strings resolve to the same records on every peer — only the
    /// identities need to cross the wire, never the meshes. Sent rarely (it
    /// barely changes), so a fresh peer can synthesize a matching NPC body
    /// instead of the placeholder creature.
    struct AppearanceState
    {
        std::string mRace;
        std::string mHead;
        std::string mHair;
        std::string mClass;
        std::string mName;
        bool mIsMale = true;

        friend bool operator==(const AppearanceState&, const AppearanceState&) = default;
    };

    /// One worn item: the inventory slot it occupies (MWWorld::InventoryStore slot
    /// index) and the item record's stable serialized-text RefId. Like appearance,
    /// the item records are shared content, so only their identities cross the wire.
    struct EquipmentSlot
    {
        std::uint8_t mSlot;
        std::string mItem;

        friend bool operator==(const EquipmentSlot&, const EquipmentSlot&) = default;
    };

    /// A discrete melee swing / spell cast, replicated as an EDGE rather than a streamed
    /// playhead. mSeq is a per-actor counter that increments once each time the actor begins
    /// a new swing (the rising edge of its attacking-or-spell state on the authority); the
    /// receiver plays the segment exactly once whenever mSeq changes. mGroup is the weapon/spell
    /// animation group to play it on and mType the attack segment ("chop"/"slash"/"thrust"/
    /// "shoot"; empty = play the whole group, e.g. a spell cast).
    ///
    /// An earlier design streamed the playhead time and inferred the segment from which window
    /// it fell in. That broke for AI NPCs, whose weapon group plays continuously in sustained
    /// combat: the ever-advancing playhead swept through the slash/chop/thrust windows in turn,
    /// firing three phantom attacks per real one. An explicit per-swing counter can't be fooled
    /// by a free-running playhead.
    struct SwingState
    {
        std::string mGroup;
        std::string mType;
        std::uint32_t mSeq = 0;

        friend bool operator==(const SwingState&, const SwingState&) = default;
    };

    /// One entity's contribution to a delta. mId is the persistent global
    /// reference identity (the same RefNum used for save games and the cell
    /// graph). A field is present iff it changed since the last sent snapshot.
    struct EntityState
    {
        ESM::RefNum mId;
        std::optional<TransformState> mTransform;
        std::optional<DynamicStats> mStats;
        // The actor's weapon/spell draw stance (MWMechanics::DrawState as a byte): 0 nothing,
        // 1 weapon, 2 spell. Replicated so a remote actor visibly draws its weapon and adopts a
        // combat stance — the first slice of animation-state replication.
        std::optional<std::uint8_t> mDrawState;
        // The actor's run/sneak stance as a bit set (bit 0 run, bit 1 sneak). A
        // high-frequency field like the transform: it selects the run/sneak vs walk
        // animation variants, so a remote avatar moves in the gait its owner chose.
        std::optional<std::uint8_t> mMoveFlags;
        // The actor's latest discrete swing/cast (group + attack type + a per-swing counter).
        // Present once the actor has swung at least once; the receiver plays the segment each
        // time the counter changes, so a free-running NPC weapon animation can't spawn phantom
        // swings the way an inferred-from-playhead scheme did.
        std::optional<SwingState> mSwing;
        // The actor's current world movement speed (units/sec). Used to set the avatar's
        // animation playback rate so its feet match its replicated translation instead of
        // sliding (it always animated at full speed before). High-frequency.
        std::optional<float> mSpeed;
        // The avatar's body identity (race/sex/head/hair/class/name). Present only on
        // the occasional ticks that re-advertise it (it rarely changes), and only for
        // a peer's own player entity — a receiver needs it once to build the avatar.
        std::optional<AppearanceState> mAppearance;
        // The avatar's worn items. Like appearance, re-advertised on the occasional
        // refresh tick. Present (even if empty — meaning "wearing nothing") is an
        // authoritative full list the receiver reconciles its avatar's equipment to;
        // absent means "no update, leave equipment as is".
        std::optional<std::vector<EquipmentSlot>> mEquipment;
        // The cell the entity occupies, as a stable serialized-text cell RefId (an
        // interior's id or an exterior worldspace id). Present only for a peer's player
        // entity, sent every tick alongside its transform: the receiver needs it to place
        // and move the avatar in the SAME cell its owner is in (a raw position is ambiguous
        // across interiors, which share coordinate spaces), and the host needs it to load
        // that cell so the NPCs there are simulated and replicated back to every peer.
        std::optional<std::string> mCellId;

        friend bool operator==(const EntityState&, const EntityState&) = default;
    };

    /// A post-tick delta: the set of entities whose replicated state changed on
    /// the tick mTick. This is what crosses the session transport each frame.
    struct SnapshotDelta
    {
        std::uint32_t mTick = 0;
        std::vector<EntityState> mEntities;

        friend bool operator==(const SnapshotDelta&, const SnapshotDelta&) = default;
    };

    /// Serialize a delta to a self-describing byte buffer (versioned).
    std::vector<std::byte> serializeSnapshot(const SnapshotDelta& delta);

    /// Parse a delta from arbitrary bytes. Returns std::nullopt on any malformed
    /// input — every read is bounds-checked and the entity count is validated
    /// against the remaining buffer, so this never crashes, over-reads, or
    /// over-allocates on hostile/corrupt data (it is fuzzed).
    std::optional<SnapshotDelta> deserializeSnapshot(std::span<const std::byte> data);
}

#endif
