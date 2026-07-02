#ifndef OPENMW_MWNET_REPLICATOR_H
#define OPENMW_MWNET_REPLICATOR_H

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string_view>
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
        // Sampling side: each actor's idle-fidget state last tick (an idle2..idle9 group playing on the
        // lower body), so a fidget emits on the discrete channel once per rising edge — like a swing.
        std::map<ESM::RefNum, bool> mWasFidgeting;
        std::map<ESM::RefNum, SwingState> mSampledSwing;
        // Applying side: the swing counter last played on each actor, so a received swing fires its
        // segment exactly once — when the counter changes. The first counter seen for an actor is
        // recorded without firing (it's a stale latest-swing, not a fresh one to replay on sight).
        std::map<ESM::RefNum, std::uint32_t> mAppliedSwingSeq;
        // Applying side: the authoritative airborne state of each driven actor, recorded from its
        // moveflags. driveRemoteActors forces the puppet's physics grounded state from it each frame,
        // so the puppet's own controller plays jump/land and gates locomotion natively.
        std::map<ESM::RefNum, bool> mWasAirborne;
        // Applying side: the turn-in-place direction of each driven actor (0 none, 1 left, 2 right),
        // recorded from its move flags. driveRemoteActors loops the foot-shuffle on the lower body
        // while it is set and disables it when it clears; the authoritative rotation still sets facing.
        std::map<ESM::RefNum, std::uint8_t> mTurnState;
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
        // Whether the local player may be replicated. A connecting client clears this while it runs
        // chargen (the multiplayer start path) so its half-built avatar isn't broadcast, and restores
        // it once the character is finalized. Always true on the host and in single-player.
        bool mLocalPlayerReady = true;
        // Client only: this peer's player bounty last mirrored across the wire, so a client-side change
        // (paying a fine, going to jail — all of which clear the bounty on the local player) is reported
        // to the host to clear the avatar's bounty, while a host-driven change isn't echoed back. The
        // first sample only records a baseline (nullopt -> value) so a pre-existing bounty isn't resent.
        std::optional<std::int32_t> mLastLocalBounty;
        // Hits this peer's player landed on host-owned actors, awaiting send to the host.
        std::vector<CombatHit> mOutgoingHits;
        // Host only: damage dealt to remote players' avatars, awaiting send to their owners.
        std::vector<PlayerDamage> mOutgoingPlayerDamages;
        // Host only: new total bounties for players whose avatar committed a crime, awaiting send.
        std::vector<PlayerBounty> mOutgoingBounties;
        // Host only: voiced lines host-owned actors spoke this tick, awaiting broadcast to clients.
        std::vector<NpcSpeech> mOutgoingSpeech;
        // Host only: arrests (a guard caught a player's avatar) awaiting send to that player's client.
        std::vector<ArrestRequest> mOutgoingArrests;
        // Client only: requests for the host to put a host-owned actor into combat with our player
        // (resist arrest / scripted aggression), awaiting send to the host.
        std::vector<CombatRequest> mOutgoingCombatRequests;
        // Host only: the subtitle for the very next say() — set by the caller that knows the line's
        // text (dialogue/script/Lua) immediately before it speaks, since say() itself carries only the
        // sound file. Consumed (and cleared) by the next reportNpcSpeech, so it never crosses lines.
        std::optional<std::string> mPendingSpeechSubtitle;
        // Host only: a deferred assault on a host-owned actor, awaiting that actor's cell to finish
        // loading so its retaliation and the crime/witness reaction can take hold (see
        // driveRemoteActors). An avatar can act in a cell the host is still loading in the background,
        // when reacting would be discarded; we re-run the witness reaction every frame across the
        // settle window (it is idempotent — already-recruited witnesses are skipped) so bystanders are
        // pulled in as they load, and bound the avatar's bounty to a single crime's worth. Keyed by
        // the struck actor's world RefNum.
        struct PendingAggro
        {
            ESM::RefNum mAggressor; // the attacking avatar's network id
            std::uint32_t mExpireTick = 0; // stop re-asserting once mTick reaches this
            bool mDelivered = false; // has the avatar's bounty for this crime been applied + sent?
            std::int32_t mBounty = 0; // avatar's bounty after that one crime — re-pinned each frame
        };
        std::map<ESM::RefNum, PendingAggro> mPendingAggro;
        // Client only: items this peer dropped / picked up, awaiting send to the host to resolve.
        std::vector<ItemDrop> mOutgoingDrops;
        std::vector<ESM::RefNum> mOutgoingTakes;
        // Client only: this player's summon spawn/despawn requests, awaiting send to the host to resolve.
        std::vector<SummonAction> mOutgoingSummons;
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
        // Client only: the latest authoritative contents received for each changed lootable, kept even
        // while its cell is unloaded here. A broadcast for an unloaded container can't be applied at
        // receipt (the ref doesn't exist yet), so it is cached and re-applied the moment the container
        // materializes (syncContainerFromCache), instead of showing the deterministic default until the
        // host's next periodic re-broadcast. Closes the load-time / late-join window.
        std::map<ESM::RefNum, ContainerState> mCachedContainerStates;
        // Host only: loose items created during the session (a peer's drop), which must be
        // replicated for existence — unlike items already in the shared save, which every peer
        // loads identically and so needs no syncing. Sampled each tick and dropped when deleted.
        std::set<ESM::RefNum> mNetworkItems;
        // Host only: RefNums of loose items deleted on the authority this tick (a pickup, or a
        // script/NPC delete), to broadcast as removals. Covers save items too — every peer holds a
        // save item under the same RefNum, so the removal deletes their copy as well.
        std::vector<ESM::RefNum> mPendingItemRemovals;
        // The persistent record of every loose item removed from the shared world this session (a save
        // item picked up, or a session drop taken). On the host it is the authoritative removed set,
        // periodically re-broadcast so a late-joiner learns about earlier pickups; on a client it is the
        // set of removals received so far. Both purge these from a cell as it loads (purgeRemovedItems),
        // so an item taken while a peer's cell was unloaded doesn't reappear on the shelf when it loads.
        std::set<ESM::RefNum> mRemovedWorldItems;
        // Client only: loose items spawned from the host's replication, keyed by their (host) RefNum.
        // Used to tell a host-owned floor item apart from anything else when this peer deletes one
        // (picks it up), so only those are reported back to the host.
        std::set<ESM::RefNum> mReplicatedItems;
        // Client only: summoned creatures instantiated from the host's spawn descriptor, by their (host)
        // RefNum. Lets a despawn removal be told apart from any other removed actor (a disposed corpse),
        // so only an actual summon plays the summon-end VFX as it is deleted.
        std::set<ESM::RefNum> mInstantiatedSummons;
        // Host only: the summoned-creature RefNums replicated last tick, so a summon that vanished from
        // every summoner's map (its effect ended / it died) is detected and broadcast as a removal.
        std::set<ESM::RefNum> mReplicatedSummons;
        // Host only: a player's summon routed here via SummonAction, keyed by (summoner net id, effect
        // id), so the matching despawn (or the summoner leaving) can find and delete the spawned creature.
        std::map<std::pair<ESM::RefNum, std::string>, ESM::RefNum> mHostedSummons;
        // Host only: re-broadcast clients' players (avatars) so clients see each other.
        bool mRelayAvatars = false;
        // True on the host (the authority that resolves combat for the shared world).
        bool mIsAuthority = false;
        // True on a connecting network client from process start, before the login handshake has
        // assigned a network id (isNetworkClient() needs the id, which arrives only in LoginAccept).
        bool mIsClientSession = false;
        // Set transiently while the world deletes a just-dropped item being handed to the host, so
        // that deletion isn't reported back as a pickup (see setHandingOffDrop).
        bool mHandingOffDrop = false;

    public:
        /// Identify this peer's player on the wire (host and each client get distinct ids).
        void setLocalPlayerNetId(ESM::RefNum id) { mLocalPlayerNetId = id; }

        /// Whether this peer's own player may be replicated yet. Cleared on a connecting client while
        /// it runs character generation (the multiplayer start path) so a half-built avatar is never
        /// broadcast; set once chargen is finalized. Defaults true so single-player and the host —
        /// which never run client chargen — are unaffected.
        void setLocalPlayerReady(bool value) { mLocalPlayerReady = value; }
        bool isLocalPlayerReady() const { return mLocalPlayerReady; }

        /// Client multiplayer-start tick: while the local player isn't ready (still in chargen), watch
        /// for the chargen GUI to close and then finalize the new character (mark chargen done, re-enable
        /// the HUD, allow replication). Pumped every frame from the engine; a no-op once ready, so it
        /// costs nothing on the host or in single-player.
        void updateClientStart();

        /// Host only: relay other peers' players (the avatars we hold) back out under their
        /// network ids, so every client sees every other client's player, not just the host's.
        void setRelayAvatars(bool value) { mRelayAvatars = value; }

        /// Host: bind an existing (persisted) player slot to a connecting client's network id,
        /// making it that client's avatar puppet — driven by the client's replication instead of
        /// simulated as an independent world actor, and reused instead of instantiating a duplicate
        /// avatar on the client's first snapshot.
        void bindAvatar(const ESM::RefNum& netId, const MWWorld::Ptr& avatar);

        /// The avatar bound to a network id (present and in a cell), or an empty Ptr. Host-side: used
        /// to route a client's uploaded character sheet to its puppet slot.
        MWWorld::Ptr boundAvatar(const ESM::RefNum& netId) const { return findLiveAvatar(netId); }

        /// Host: a client disconnected — release its avatar binding, forget its per-entity
        /// replication state, and broadcast a despawn so every client deletes its cosmetic copy.
        /// Returns the avatar Ptr that was bound (empty if none), so the caller can park the
        /// world-side player slot (which stays as the character's last known state).
        MWWorld::Ptr unbindAvatar(const ESM::RefNum& netId);

        /// Mark this peer as a connecting network client from process start — BEFORE the login
        /// handshake has assigned its network id — so load-time paths (e.g. skipping a shared
        /// save's extra players) can already behave client-side. isNetworkClient() only turns true
        /// once the id arrives.
        void setClientSession(bool value) { mIsClientSession = value; }
        bool isClientSession() const { return mIsClientSession; }

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

        /// Client: route this player's summon to the host (which spawns it host-authoritatively) instead
        /// of spawning it locally. Returns true if it was routed (the caller then skips its local spawn);
        /// false if not handled (off the network, or not the local player), so the caller spawns normally.
        bool reportSummon(const ESM::RefId& effectId, const MWWorld::Ptr& summoner);

        /// Client: tell the host this player's summon effect ended, so it despawns the host-owned creature.
        void reportSummonEnd(const ESM::RefId& effectId, const MWWorld::Ptr& summoner);

        /// Report (host only) that a host-owned actor dealt damage to a remote player's avatar,
        /// so the owning client can apply it to its real player. A no-op off the authority or if
        /// the struck Ptr isn't one of our avatars.
        void reportRemotePlayerHit(const MWWorld::Ptr& avatar, float damage, bool healthDamage);

        /// Attach a subtitle to the very next reported speech. Called (host only) by the dialogue /
        /// script / Lua say sites that know the line's text, right before they speak — say() itself only
        /// carries the sound file. No-op off the authority. The next reportNpcSpeech consumes it.
        void setPendingSpeechSubtitle(std::string_view text)
        {
            if (mIsAuthority)
                mPendingSpeechSubtitle = std::string(text);
        }

        /// Report (host only) that a host-owned actor spoke a voiced line, so every client replays it
        /// on that actor. sound is the already-corrected voice file path the host resolved. A no-op off
        /// the authority, for a transient/unset RefNum, or for a player/avatar (only world NPCs cross).
        void reportNpcSpeech(const MWWorld::ConstPtr& actor, std::string_view sound);

        /// Report (host only) that a guard caught a player's avatar to arrest it, so the arrest dialogue
        /// opens on that player's client instead of on the host. Returns true if avatar was one of our
        /// avatars (the caller then suppresses the host-side dialogue); false otherwise (handle locally).
        bool reportArrest(const MWWorld::Ptr& avatar, const MWWorld::Ptr& guard);

        /// Apply received arrests (client only): if one names this peer's player, open the arrest
        /// dialogue with the guard it names (resolved to this peer's local copy by RefNum).
        void applyArrests(const ActionBatch& batch);

        /// Route (client only) an order for a host-owned actor to fight this peer's own player — the
        /// "resist arrest" StartCombat, or any scripted aggression a client can't drive itself — to the
        /// host. Returns true if it was routed (the caller skips the local, suppressed-puppet combat);
        /// false otherwise (off the network, the instigator isn't host-owned, or the target isn't us).
        bool reportCombatStart(const MWWorld::Ptr& instigator, const MWWorld::Ptr& target);

        /// Apply received combat requests (host only): put each named host-owned actor into combat with
        /// the requesting player's avatar, authoritatively (the actor and its retaliation are host-owned).
        void applyCombatRequests(const ActionBatch& batch);

        /// Apply received speech reports (client only): play each voiced line on the host-owned actor
        /// it names, if that actor's cell is loaded here (you only hear NPCs near you).
        void applyNpcSpeech(const ActionBatch& batch);

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
            mRemovedWorldItems.insert(item); // remember it, so late-joiners and cell reloads stay in sync
        }

        /// Called as a cell loads (on any peer): delete every loose item in it that has already been
        /// removed from the shared world, so a save item picked up while this cell was unloaded here
        /// doesn't reappear on the shelf. A no-op with nothing removed / in single-player.
        void purgeRemovedItems();

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

        /// Called (client only) when a container's store is first materialized — i.e. its cell just
        /// loaded here. If the host has already told us this container's authoritative contents (cached
        /// while its cell was unloaded), apply them now so a looted container never briefly shows its
        /// deterministic default. A no-op on the host, in single-player, or with nothing cached.
        void syncContainerFromCache(const MWWorld::Ptr& container);

        /// Drain this tick's reported actions for sending (including the contents of any container
        /// that changed since the last tick).
        ActionBatch takeOutgoingActions();

        /// Apply received actions authoritatively (host only): route combat hits, place dropped items,
        /// remove taken items, and spawn/despawn routed summons. Thin dispatcher over the helpers below.
        void applyActions(const ActionBatch& batch);

        /// Apply reported combat hits: route PvP damage to the victim's client, or apply an avatar's
        /// damage to a host-owned actor (charging a fatal blow / deferring the assault reaction).
        void applyHitActions(const ActionBatch& batch);
        /// Place each dropped item in the shared world authoritatively and track it for replication.
        void applyItemDropActions(const ActionBatch& batch);
        /// Delete each host-owned loose item a peer picked up (World::deleteObject broadcasts removal).
        void applyItemTakeActions(const ActionBatch& batch);
        /// Spawn or despawn a client-routed summon, bound to the summoner's avatar (host only).
        void applySummonActions(const ActionBatch& batch);

        /// Record the avatar as the hit-attempt actor of every host-owned actor already FIGHTING it, so
        /// their retaliation persists the way it does against the primary local player (an avatar's
        /// damage is applied host-side bypassing onHit, so they never record it otherwise). Pin only —
        /// it never starts combat, so an actor that reacted by arresting or staying peaceful is left
        /// alone. Safe to call repeatedly (the per-frame re-assert for mPendingAggro does).
        void pinAvatarAttacker(const MWWorld::Ptr& aggressor);

        /// Apply received player-damage reports (client only): subtract from this peer's real
        /// player whatever the host says host-owned actors dealt to its avatar.
        void applyIncomingPlayerDamage(const ActionBatch& batch);

        /// Apply received bounty reports (client only): set this peer's real player's crime bounty
        /// to whatever the host computed when its avatar committed a crime in the shared world.
        void applyIncomingPlayerBounty(const ActionBatch& batch);

        /// Apply received avatar-bounty reports (host only): a client cleared/changed its own player's
        /// bounty (e.g. resolved an arrest), so mirror it onto that player's avatar — guards reading the
        /// avatar's bounty then stop pursuing once it hits zero.
        void applyAvatarBounty(const ActionBatch& batch);

        /// Read the world's active actors (and this peer's player) and build the delta.
        SnapshotDelta sampleDelta();

        /// Re-assert every remote-owned actor's locomotion intent for THIS frame, so its walk
        /// cycle plays continuously and it dead-reckons between snapshots. Must be called every
        /// frame (the mechanics pass zeroes the movement vector each frame), before mechanics
        /// update — applyDelta only records the intent (recordMotion) on snapshot frames.
        void driveRemoteActors();

    private:
        /// Collect every live summoned-creature RefNum bound to any of the given actors (host only).
        std::set<ESM::RefNum> collectSummons(const std::vector<MWWorld::Ptr>& actors) const;

        /// Append this peer's own player to the delta under its network id, so other peers show it as
        /// an avatar. No-op until a network id is assigned and the local character is ready.
        void appendLocalPlayer(SnapshotDelta& delta, const MWWorld::Ptr& player, bool fullSnapshot);

        /// Look up a remote player's avatar, returning it only if it is live (present and placed in
        /// a cell). Returns an empty Ptr when the peer has no avatar yet or it isn't resolvable.
        MWWorld::Ptr findLiveAvatar(const ESM::RefNum& netId) const;

        /// Drop every piece of per-entity replication state held for the given id (sampling dedup,
        /// animation/motion bookkeeping, avatar relay caches). Used when an entity leaves the
        /// session (a client disconnected, or its avatar despawned here).
        void forgetEntity(const ESM::RefNum& id);

        /// Per-remote-actor frame drivers, called from driveRemoteActors: launch a deferred cosmetic
        /// cast bolt at its release key, swap a weapon strike into its follow-through at impact, and
        /// loop the turn-in-place foot-shuffle while the owner pivots in place (fraction is its speed).
        void driveCastBolt(const MWWorld::Ptr& actor, const ESM::RefNum& id);
        void driveFollowThrough(const MWWorld::Ptr& actor, const ESM::RefNum& id);
        void driveTurnInPlace(const MWWorld::Ptr& actor, const ESM::RefNum& id, float fraction);

        /// Host: deliver each pending avatar assault (crime + witness reaction, victim retaliation) once
        /// the victim's cell finishes loading, re-asserting until the victim is engaged. No-op off-host.
        void driveDeferredAssaults();

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
        void applyTurn(const ESM::RefNum& id, std::uint8_t flags);

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

        /// Client only: if this peer's player bounty changed since last mirrored (an arrest resolved,
        /// a fine paid), queue it for the host so the avatar's bounty follows. Records a silent baseline
        /// on the first sample so a pre-existing bounty isn't reported. No-op off a networked client.
        void sampleLocalBounty();

        /// Read a lootable inventory's current contents into a ContainerState. nullopt if the object
        /// isn't a syncable lootable (a world container or a corpse) currently loaded in a cell.
        std::optional<ContainerState> buildContainerState(ESM::RefNum id);

        /// Overwrite a lootable inventory's local contents to match a received ContainerState, and
        /// keep it from being re-rolled by a later lazy resolve.
        void applyContainerState(const ContainerState& state);

        /// Apply one entity from a delta. Each returns true when it actually updated an entity
        /// (so applyDelta can count it); false when it couldn't this tick (not placeable yet, our
        /// own echo, unresolved ref). applyAvatarEntity handles another peer's player (instantiate
        /// on first sight, then move/animate); applyWorldEntity handles a host-owned summon / loose
        /// item / driven actor; applyRemovedItems deletes local copies of items gone from the world.
        bool applyAvatarEntity(const EntityState& entity);
        bool applyWorldEntity(const EntityState& entity);
        void applyRemovedItems(const SnapshotDelta& delta);

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
