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
        // The avatar's body identity (race/sex/head/hair/class/name). Present only on
        // the occasional ticks that re-advertise it (it rarely changes), and only for
        // a peer's own player entity — a receiver needs it once to build the avatar.
        std::optional<AppearanceState> mAppearance;

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
