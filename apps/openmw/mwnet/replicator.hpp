#ifndef OPENMW_MWNET_REPLICATOR_H
#define OPENMW_MWNET_REPLICATOR_H

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

#include <osg/Vec3f>

#include <components/esm3/refnum.hpp>

#include "../mwworld/ptr.hpp"

#include "actions.hpp"
#include "snapshot.hpp"

namespace ESM
{
    class RefId;
}

namespace MWRender
{
    class Animation;
}

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
            std::optional<SwingState> mSwing;
            std::optional<float> mSpeed;

            friend bool operator==(const SentState&, const SentState&) = default;
        };
        std::map<ESM::RefNum, SentState> mLastSent;
        // Avatars instantiated for other peers' players, keyed by their network id.
        std::map<ESM::RefNum, MWWorld::Ptr> mAvatars;
        // Each peer's last-advertised body identity, so its avatar can be built to match
        // it (received occasionally; an avatar is only instantiated once it's known).
        std::map<ESM::RefNum, AppearanceState> mAppearances;
        // Sampling side: each actor's attacking-or-spell state last tick, and whether a swing for
        // the current attack pulse is still waiting to be captured. On the rising edge a capture is
        // armed; the swing counter is bumped once at the first tick of that pulse where the weapon
        // group is actually the active Torso group (so a same-tick weapon draw or hand-to-hand
        // doesn't slip past an exact-edge check). We stream that counter rather than guessing swings
        // from a free-running playhead. mSampledSwing holds the latest {group, type, seq} to emit.
        std::map<ESM::RefNum, bool> mWasAttacking;
        std::map<ESM::RefNum, bool> mPendingSwing;
        // Sampling side: a weapon wind-up has been emitted (phase 1) and is waiting for its owner to let
        // go, at which point a phase-2 release is emitted on the same channel.
        std::map<ESM::RefNum, bool> mWindupPendingRelease;
        // Sampling side: each actor's block state last tick (the shield animation playing on the left
        // arm), so a block emits on the swing channel once per rising edge — like a swing.
        std::map<ESM::RefNum, bool> mWasBlocking;
        // Sampling side: each actor's spell-cast state last tick (the spellcast animation playing on
        // the torso). A cast can't be read from getAttackingOrSpell() — the controller clears that flag
        // the same frame the cast begins, and sampleDelta runs at frame start — so, like a block, the
        // cast is detected from its animation and emitted on the swing channel once per rising edge.
        std::map<ESM::RefNum, bool> mWasCasting;
        std::map<ESM::RefNum, SwingState> mSampledSwing;
        // Applying side: the swing counter last played on each actor, so a received swing fires its
        // segment exactly once — when the counter changes. The first counter seen for an actor is
        // recorded without firing (it's a stale latest-swing, not a fresh one to replay on sight).
        std::map<ESM::RefNum, std::uint32_t> mAppliedSwingSeq;
        // Applying side: the authoritative airborne state of each driven actor, recorded from its
        // moveflags. driveRemoteActors forces the puppet's physics grounded state from it each frame,
        // so the puppet's own controller plays jump/land and gates locomotion natively.
        std::map<ESM::RefNum, bool> mWasAirborne;
        // Applying side: a cast whose cosmetic bolt is waiting to be launched. The spell/type ride in a
        // SwingState; driveRemoteActors fires the bolt when the avatar's spellcast animation reaches the
        // "<type> release" key, so the bolt leaves the hands in step with the cast rather than at its start.
        std::map<ESM::RefNum, SwingState> mPendingCastBolt;
        // Applying side: a weapon strike whose follow-through recovery is waiting to play. The strike
        // (phase 2) is held at its impact key; driveRemoteActors swaps in the follow-through segment
        // once the playhead lands there, so the weapon recovers smoothly without spanning all three
        // strength follow-throughs at once.
        std::map<ESM::RefNum, SwingState> mPendingFollow;
        // The last swing received for each avatar, so the host relays the peer's ORIGINAL swing
        // (its own counter) to other clients rather than re-deriving one from its brief overlay.
        std::map<ESM::RefNum, std::optional<SwingState>> mAvatarSwing;
        // The last locomotion (speed + gait flags) received for each avatar, relayed VERBATIM for the
        // same reason as the swing: the avatar is a remote-driven puppet on the host, so re-sampling
        // its own getCurrentSpeed/stance is unreliable — a stopped owner left the host puppet's
        // speed factor non-zero, so downstream peers kept playing its walk cycle in place.
        std::map<ESM::RefNum, std::optional<float>> mAvatarSpeed;
        std::map<ESM::RefNum, std::optional<std::uint8_t>> mAvatarMoveFlags;
        // Per actor, the health last applied and the tick we last played a hit reaction, so a drop
        // in replicated health makes the victim flinch + grunt once per hit on this client (the
        // authoritative damage is applied directly, bypassing the onHit that would normally react).
        // The tick gate keeps a damage-over-time effect from re-flinching every single tick.
        std::map<ESM::RefNum, float> mLastHealth;
        std::map<ESM::RefNum, std::uint32_t> mLastHitReactionTick;
        // Per remote-owned actor's locomotion intent, re-asserted EVERY frame (driveRemoteActors)
        // rather than only on snapshot-receipt frames. The mechanics pass consumes and zeroes an
        // actor's movement vector every frame; for a remote avatar (AI skipped) only we write it,
        // so driving it solely on snapshot frames left it idle between snapshots — the avatar slid
        // (position corrected discretely under a frozen idle pose) and its walk cycle restarted on
        // each snapshot. Re-asserting it every frame keeps the cycle continuous and lets the actor
        // dead-reckon between snapshots, with the snapshot moveObject as authoritative correction.
        struct RemoteMotion
        {
            MWWorld::Ptr mActor;          // the applied actor to drive each frame
            osg::Vec3f mPrevTarget;       // last authoritative position, to derive the per-snapshot step
            float mDirX = 0.f;            // movement direction in the actor's local frame: X right,
            float mDirY = 0.f;            // Y forward (matching Movement::mPosition[0]/[1])
            float mFraction = 0.f;        // speed as a fraction of the actor's max speed; 0 = idle
            bool mHasPrev = false;        // false until the first snapshot establishes mPrevTarget
        };
        std::map<ESM::RefNum, RemoteMotion> mRemoteMotion;
        // This peer's own player network id (role-based: host vs client), so we never
        // instantiate an avatar for our own player echoed back.
        ESM::RefNum mLocalPlayerNetId;
        // Hits this peer's player landed on host-owned actors, awaiting send to the host.
        std::vector<CombatHit> mOutgoingHits;
        // Host only: damage dealt to remote players' avatars, awaiting send to their owners.
        std::vector<PlayerDamage> mOutgoingPlayerDamages;
        // Client only: items this peer dropped / picked up, awaiting send to the host to resolve.
        std::vector<ItemDrop> mOutgoingDrops;
        std::vector<ESM::RefNum> mOutgoingTakes;
        // Client only: take/put requests against lootable inventories, awaiting host resolution.
        std::vector<ContainerChange> mOutgoingContainerChanges;
        // Host only: over-take corrections, awaiting send to the clients that lost a take race.
        std::vector<ContainerRevoke> mOutgoingRevokes;
        // Lootable inventories (world containers / corpses) whose contents changed and must be
        // synced. Filled by the loot UI on any peer; drained each tick into the outgoing action
        // batch. On the host, also re-filled when a client's change is applied, to relay it on.
        std::set<ESM::RefNum> mDirtyContainers;
        // Host only: the authoritative contents of every lootable that has changed from its
        // deterministic default this session. The persistent record behind late-join (a peer that
        // arrives after a loot gets the real contents on the periodic re-broadcast) and re-resolve
        // recovery (if the host's cell unloads and rolls the container back to default, the record
        // restores it). Survives the live store; never trusts a re-rolled store.
        std::map<ESM::RefNum, ContainerState> mAuthoritativeContainers;
        // Host only: loose items created during the session (a peer's drop), which must be
        // replicated for existence — unlike items already in the shared save, which every peer
        // loads identically and so needs no syncing. Sampled each tick and dropped when deleted.
        std::set<ESM::RefNum> mNetworkItems;
        // Host only: RefNums of loose items deleted on the authority this tick (a pickup, or a
        // script/NPC delete), to broadcast as removals. Covers save items too — every peer holds a
        // save item under the same RefNum, so the removal deletes their copy as well.
        std::vector<ESM::RefNum> mPendingItemRemovals;
        // Client only: loose items spawned from the host's replication, keyed by their (host) RefNum.
        // Used to tell a host-owned floor item apart from anything else when this peer deletes one
        // (picks it up), so only those are reported back to the host.
        std::set<ESM::RefNum> mReplicatedItems;
        // Host only: re-broadcast clients' players (avatars) so clients see each other.
        bool mRelayAvatars = false;
        // True on the host (the authority that resolves combat for the shared world).
        bool mIsAuthority = false;
        // Set transiently while the world deletes a just-dropped item being handed to the host, so
        // that deletion isn't reported back as a pickup (see setHandingOffDrop).
        bool mHandingOffDrop = false;

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

        /// True only on an actual networked client — a peer that connected to a host, so it has a
        /// network id but is not the authority. False on the host AND in single-player / loopback
        /// (no id assigned), so gating client-side world interception on this keeps SP unchanged.
        bool isNetworkClient() const { return mLocalPlayerNetId.isSet() && !mIsAuthority; }

        /// True in any networked session — host OR client (a network id is assigned). Single-player
        /// and loopback leave it false. Used where the host and every client must make the SAME
        /// shared-world choice deterministically (e.g. seeding a container's leveled-list roll from
        /// its RefNum so every peer rolls identical loot without replicating the contents).
        bool isNetworked() const { return mLocalPlayerNetId.isSet(); }

        /// Guard a deletion the world is doing to hand a just-dropped item to the host, so that
        /// deletion is NOT mistaken for a pickup and reported back (the drop is already reported,
        /// and the local ref's RefNum is meaningless — or worse, colliding — on the host).
        void setHandingOffDrop(bool value) { mHandingOffDrop = value; }
        bool isHandingOffDrop() const { return mHandingOffDrop; }

        /// Report (from combat code on a client) that our player struck a host-resolved actor for a
        /// computed amount of damage, queued for the host to apply authoritatively. The victim is
        /// identified on the wire by its shared world RefNum if it is a host-owned actor, or — if it
        /// is another peer's player avatar (a local-only ref the host can't resolve) — by that peer's
        /// network id, so the host routes the damage to that player (PvP). healthDamage selects
        /// health vs fatigue.
        void reportHit(const MWWorld::Ptr& victim, float damage, bool healthDamage);

        /// Report (host only) that a host-owned actor dealt damage to a remote player's avatar,
        /// so the owning client can apply it to its real player. A no-op off the authority or if
        /// the struck Ptr isn't one of our avatars.
        void reportRemotePlayerHit(const MWWorld::Ptr& avatar, float damage, bool healthDamage);

        /// Report (from a client) that this peer dropped an item into the shared world, for the
        /// host to place authoritatively and replicate back to everyone. cellId is the serialized
        /// text RefId of the cell to drop it in.
        void reportDrop(std::string refId, int count, const osg::Vec3f& position, std::string cellId)
        {
            mOutgoingDrops.push_back({ std::move(refId), count, position, std::move(cellId) });
        }

        /// Report (from a client) that this peer picked a host-owned loose item up, for the host to
        /// delete it from the shared world (and replicate the removal to every other peer).
        void reportItemTaken(ESM::RefNum item) { mOutgoingTakes.push_back(item); }

        /// Is this RefNum a loose item this peer mirrors from the host? Lets the world tell a
        /// picked-up host item apart from any other deletion before reporting it back.
        bool isReplicatedItem(ESM::RefNum item) const { return mReplicatedItems.count(item) != 0; }

        /// Report (host only) that a loose item was deleted from the shared world (a pickup, or a
        /// script/NPC delete), to broadcast as a removal so every peer drops its copy.
        void reportItemRemoved(ESM::RefNum item)
        {
            mNetworkItems.erase(item);
            mPendingItemRemovals.push_back(item);
        }

        /// Mark a lootable inventory (a world container or a corpse) as changed, so this tick's
        /// action batch carries its new contents. Called by the loot UI on whichever peer made the
        /// change. No-op outside a networked session, so single-player is unaffected.
        void markContainerDirty(ESM::RefNum id)
        {
            if (isNetworked())
                mDirtyContainers.insert(id);
        }

        /// Apply received lootable-inventory contents, overwriting each local store. relay=true (on
        /// the host) also re-marks them dirty so the change is relayed to every other peer.
        void applyContainers(const ActionBatch& batch, bool relay);

        /// Report (from a client) that this peer took an item out of / put an item into a lootable
        /// inventory, for the host to resolve authoritatively. The local change happens optimistically;
        /// the host validates a take against its authoritative record and corrects an over-take.
        void reportContainerChange(ESM::RefNum container, const MWWorld::Ptr& item, int count, bool take);

        /// Apply received take/put requests (host only): resolve each against the authoritative
        /// container record — grant a take only up to what's actually there (queuing a revoke for the
        /// rest), always apply a put — then update the live store and broadcast the new contents.
        void applyContainerChanges(const ActionBatch& batch);

        /// Apply received over-take corrections (client only): drop from this peer's own inventory the
        /// items the host says it claimed but the container didn't have.
        void applyContainerRevokes(const ActionBatch& batch);

        /// Drain this tick's reported actions for sending (including the contents of any container
        /// that changed since the last tick).
        ActionBatch takeOutgoingActions();

        /// Apply received actions authoritatively (host only): make each struck actor aggro
        /// onto the reporting peer's avatar and take the reported damage.
        void applyActions(const ActionBatch& batch);

        /// Apply received player-damage reports (client only): subtract from this peer's real
        /// player whatever the host says host-owned actors dealt to its avatar.
        void applyIncomingPlayerDamage(const ActionBatch& batch);

        /// Read the world's active actors (and this peer's player) and build the delta.
        SnapshotDelta sampleDelta();

        /// Re-assert every remote-owned actor's locomotion intent for THIS frame, so its walk
        /// cycle plays continuously and it dead-reckons between snapshots. Must be called every
        /// frame (the mechanics pass zeroes the movement vector each frame), before mechanics
        /// update — applyDelta only records the intent (recordMotion) on snapshot frames.
        void driveRemoteActors();

    private:
        /// Sample an actor's discrete swing state: bump its per-swing counter on the rising edge
        /// of attacking-or-spell (capturing the weapon group and attack type then), and return the
        /// latest {group, type, seq}. nullopt until the actor has swung at least once. Stateful
        /// (keyed by id), so it must be called once per tick per actor.
        std::optional<SwingState> sampleSwing(const MWWorld::Ptr& actor, const ESM::RefNum& id);

        /// Overlay a received swing onto an applied actor: when its counter differs from the one
        /// last played, play the attack segment once on the actor's upper body. No-op while the
        /// counter is unchanged, so a continuously-active weapon animation never re-fires.
        void applySwing(const MWWorld::Ptr& actor, const ESM::RefNum& id, const SwingState& swing);
        // Reproduce a remote caster's cosmetic spell visuals (cast aura, glowing hands, and a non-damaging
        // bolt for target spells) without applying any gameplay effect.
        void applyCastEffects(const MWWorld::Ptr& actor, MWRender::Animation* animation, const ESM::RefId& spellId);
        void applyJump(const MWWorld::Ptr& actor, const ESM::RefNum& id, bool airborne);

        /// React visibly to a drop in an actor's replicated health: make it flinch (hit-recovery
        /// animation, played by its own controller) and play the pain sound, once per hit. localPlayer
        /// also flashes the screen hit overlay. No-op when health didn't fall or the actor is dead.
        void applyHitReaction(const MWWorld::Ptr& actor, const ESM::RefNum& id, float newHealth, bool localPlayer);

        /// Spawn a blood splatter on a struck actor, mirroring omw/combat/local.lua spawnBloodEffect
        /// (random of three blood meshes, texture by the actor's blood type). The receiver runs no
        /// onHit, so the Lua handler that would normally spawn it never fires for a remote victim.
        void spawnBloodEffect(const MWWorld::Ptr& actor);

        /// Record a remote actor's locomotion intent from an applied snapshot: derive the
        /// movement direction (in the actor's local frame) and speed fraction from the
        /// authoritative step since the previous snapshot, for driveRemoteActors to replay
        /// each frame.
        void recordMotion(const ESM::RefNum& id, const MWWorld::Ptr& actor, const osg::Vec3f& target,
            const osg::Vec3f& rotation, std::optional<float> speed);

        /// Read a lootable inventory's current contents into a ContainerState. nullopt if the object
        /// isn't a syncable lootable (a world container or a corpse) currently loaded in a cell.
        std::optional<ContainerState> buildContainerState(ESM::RefNum id);

        /// Overwrite a lootable inventory's local contents to match a received ContainerState, and
        /// keep it from being re-rolled by a later lazy resolve.
        void applyContainerState(const ContainerState& state);

    public:
        /// Apply a received delta. Other peers' players are always shown as avatars
        /// (instantiated on first sight, then moved); ordinary world entities are moved
        /// only when applyWorldEntities is true (a client obeying its host) — never for a
        /// loopback echo, which would perturb the local simulation. Returns the number of
        /// entities updated.
        std::size_t applyDelta(const SnapshotDelta& delta, bool applyWorldEntities);
    };
}

#endif
