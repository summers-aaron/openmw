#ifndef OPENMW_MWNET_REPLICATOR_H
#define OPENMW_MWNET_REPLICATOR_H

#include <cstdint>
#include <map>

#include <components/esm3/refnum.hpp>

#include "../mwworld/ptr.hpp"

#include "snapshot.hpp"

namespace MWNet
{
    /// Bridges the running simulation to the snapshot/delta channel. Each tick it
    /// samples the authoritative transforms of active actors (plus this peer's own
    /// player under a network id) and emits only what changed since the last send;
    /// on the receive side it applies world deltas and shows other peers' players as
    /// avatars.
    ///
    /// In single-player / loopback nothing is applied (the round trip still exercises
    /// the whole sample -> serialize -> transport -> deserialize path), so SP stays
    /// byte-identical.
    class Replicator
    {
        std::uint32_t mTick = 0;
        std::map<ESM::RefNum, TransformState> mLastSent;
        // Avatars instantiated for other peers' players, keyed by their network id.
        std::map<ESM::RefNum, MWWorld::Ptr> mAvatars;
        // This peer's own player network id (role-based: host vs client), so we never
        // instantiate an avatar for our own player echoed back.
        ESM::RefNum mLocalPlayerNetId;

    public:
        /// Identify this peer's player on the wire (host and each client get distinct ids).
        void setLocalPlayerNetId(ESM::RefNum id) { mLocalPlayerNetId = id; }

        /// Read the world's active actors (and this peer's player) and build the delta.
        SnapshotDelta sampleDelta();

        /// Apply a received delta. Other peers' players are always shown as avatars
        /// (instantiated on first sight, then moved); ordinary world entities are moved
        /// only when applyWorldEntities is true (a client obeying its host) — never for a
        /// loopback echo, which would perturb the local simulation. Returns the number of
        /// entities updated.
        std::size_t applyDelta(const SnapshotDelta& delta, bool applyWorldEntities);
    };
}

#endif
