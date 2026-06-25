#ifndef OPENMW_MWNET_SNAPSHOT_H
#define OPENMW_MWNET_SNAPSHOT_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include <osg/Vec3f>

#include <components/esm3/refnum.hpp>

namespace MWNet
{
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

    /// One entity's contribution to a delta. mId is the persistent global
    /// reference identity (the same RefNum used for save games and the cell
    /// graph). A field is present iff it changed since the last sent snapshot.
    struct EntityState
    {
        ESM::RefNum mId;
        std::optional<TransformState> mTransform;

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
