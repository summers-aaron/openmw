#ifndef OPENMW_MWNET_REPLICATOR_H
#define OPENMW_MWNET_REPLICATOR_H

#include <cstdint>
#include <map>

#include <components/esm3/refnum.hpp>

#include "snapshot.hpp"

namespace MWNet
{
    /// Bridges the running simulation to the snapshot/delta channel. Each tick it
    /// samples the authoritative transforms of active actors and emits only what
    /// changed since the last send (a delta); on the receive side it applies
    /// deltas for remotely-owned entities.
    ///
    /// In single-player / loopback every entity is locally authoritative, so
    /// applyDelta is a no-op and SP stays byte-identical — yet the round trip still
    /// exercises the whole sample -> serialize -> transport -> deserialize path.
    class Replicator
    {
        std::uint32_t mTick = 0;
        std::map<ESM::RefNum, TransformState> mLastSent;

    public:
        /// Read the world's active actors and build the delta of changed transforms.
        SnapshotDelta sampleDelta();

        /// Apply a received delta to remotely-owned entities (none in SP/loopback).
        void applyDelta(const SnapshotDelta& delta);
    };
}

#endif
