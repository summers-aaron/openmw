#ifndef OPENMW_MWNET_REPLICATOR_H
#define OPENMW_MWNET_REPLICATOR_H

#include <cstdint>
#include <map>
#include <optional>
#include <utility>
#include <vector>

#include <components/esm3/refnum.hpp>

#include "../mwworld/ptr.hpp"

#include "actions.hpp"
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
        // The replicated state last sent per entity, so a change in ANY part triggers a resend
        // (a dying-but-stationary actor's health, or an actor drawing its weapon in place, must
        // sync immediately rather than wait for the periodic full refresh).
        struct SentState
        {
            TransformState mTransform;
            std::optional<DynamicStats> mStats;
            std::optional<std::uint8_t> mDrawState;
            std::optional<std::uint8_t> mMoveFlags;
            std::optional<std::uint8_t> mAttack;
            std::optional<float> mSpeed;

            friend bool operator==(const SentState&, const SentState&) = default;
        };
        std::map<ESM::RefNum, SentState> mLastSent;
        // Avatars instantiated for other peers' players, keyed by their network id.
        std::map<ESM::RefNum, MWWorld::Ptr> mAvatars;
        // Each peer's last-advertised body identity, so its avatar can be built to match
        // it (received occasionally; an avatar is only instantiated once it's known).
        std::map<ESM::RefNum, AppearanceState> mAppearances;
        // This peer's own player network id (role-based: host vs client), so we never
        // instantiate an avatar for our own player echoed back.
        ESM::RefNum mLocalPlayerNetId;
        // Hits this peer's player landed on host-owned actors, awaiting send to the host.
        std::vector<CombatHit> mOutgoingHits;
        // Host only: damage dealt to remote players' avatars, awaiting send to their owners.
        std::vector<PlayerDamage> mOutgoingPlayerDamages;
        // Host only: re-broadcast clients' players (avatars) so clients see each other.
        bool mRelayAvatars = false;
        // True on the host (the authority that resolves combat for the shared world).
        bool mIsAuthority = false;

    public:
        /// Identify this peer's player on the wire (host and each client get distinct ids).
        void setLocalPlayerNetId(ESM::RefNum id) { mLocalPlayerNetId = id; }

        /// Host only: relay other peers' players (the avatars we hold) back out under their
        /// network ids, so every client sees every other client's player, not just the host's.
        void setRelayAvatars(bool value) { mRelayAvatars = value; }

        /// Mark this peer as the authority (the host). The authority resolves combat for the
        /// shared world: it applies clients' reported hits and reports damage back to players.
        void setAuthority(bool value) { mIsAuthority = value; }
        bool isAuthority() const { return mIsAuthority; }

        /// Report (from combat code on a client) that our player struck a host-owned actor
        /// for a computed amount of damage. Queued for the host, which resolves it
        /// authoritatively. healthDamage selects health vs fatigue.
        void reportHit(ESM::RefNum victim, float damage, bool healthDamage)
        {
            mOutgoingHits.push_back({ mLocalPlayerNetId, victim, damage, healthDamage });
        }

        /// Report (host only) that a host-owned actor dealt damage to a remote player's avatar,
        /// so the owning client can apply it to its real player. A no-op off the authority or if
        /// the struck Ptr isn't one of our avatars.
        void reportRemotePlayerHit(const MWWorld::Ptr& avatar, float damage, bool healthDamage);

        /// Drain this tick's reported actions for sending.
        ActionBatch takeOutgoingActions()
        {
            ActionBatch batch;
            batch.mHits = std::move(mOutgoingHits);
            batch.mPlayerDamages = std::move(mOutgoingPlayerDamages);
            mOutgoingHits.clear();
            mOutgoingPlayerDamages.clear();
            return batch;
        }

        /// Apply received actions authoritatively (host only): make each struck actor aggro
        /// onto the reporting peer's avatar and take the reported damage.
        void applyActions(const ActionBatch& batch);

        /// Apply received player-damage reports (client only): subtract from this peer's real
        /// player whatever the host says host-owned actors dealt to its avatar.
        void applyIncomingPlayerDamage(const ActionBatch& batch);

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
