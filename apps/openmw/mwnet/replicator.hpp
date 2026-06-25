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

        /// Apply an authoritative delta: move each entity already present locally to the
        /// transform the authority reported. Entities not present yet are skipped (remote
        /// instantiation is a later step). Returns how many entities were moved. The caller
        /// must only invoke this for genuinely remote/authoritative deltas — never for a
        /// loopback echo of our own state, which would perturb the local simulation.
        std::size_t applyDelta(const SnapshotDelta& delta);
    };
}

#endif
