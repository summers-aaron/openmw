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

    /// A one-shot 3D world sound game logic played — a weapon or armor impact, a spell's
    /// cast/fail/hit sound, an NPC opening a door, drowning. Host -> clients for the world the
    /// host owns; a client also reports sounds of its OWN making (its player's casts, swishes,
    /// area explosions), which the host replays and relays on to the other clients. Animation-
    /// driven sounds — footsteps, jump/land, anything emitted from animation text keys — are
    /// deliberately NOT replicated: every peer animates loaded actors itself from replicated
    /// state and produces those locally (see Replicator::LocalSoundScope). Anchored on a world
    /// object (mObject set: a world RefNum, or a player's wire id that each peer resolves to its
    /// local copy of that player/avatar) or at a raw position (mObject unset). mOrigin is the
    /// reporting peer's wire id: a receiver skips sounds it originated (the host rebroadcasts to
    /// everyone, so your own report comes back to you). Loops never cross (stateful). Cosmetic
    /// only: plays on whichever peers can resolve the anchor.
    struct WorldSound
    {
        ESM::RefNum mObject; // unset => positional sound at mPosition
        float mPosition[3] = { 0.f, 0.f, 0.f };
        std::string mSound; // ESM::Sound record id (serialized RefId)
        float mVolume = 1.f;
        float mPitch = 1.f;
        ESM::RefNum mOrigin; // reporting peer's wire id, for echo suppression

        friend bool operator==(const WorldSound&, const WorldSound&) = default;
    };

    /// A shared-journal change. The journal is co-op world state: any player's quest progress
    /// advances the one world journal for everyone, and content scripts that gate world changes
    /// on GetJournalIndex converge on every peer. Flows client -> host (a dialogue result script
    /// or console advanced the quest locally) and host -> clients (authoritative apply, relayed
    /// onward with the origin preserved so the reporting peer skips its own echo — though the
    /// journal's (topic, infoId) dedup makes echoes harmless no-ops anyway).
    /// mInfoId empty => index-only (SetJournalIndex); otherwise the RENDERED entry crosses —
    /// text is substituted against the acting NPC's locals at add time and that NPC is usually
    /// unresolvable on other machines, so the receiver stores the text as-is, exactly like a
    /// save's REC_JOUR record. The originator stamps the in-game date.
    struct JournalDelta
    {
        std::string mTopic; // quest/dialogue RefId, serialized
        std::int32_t mIndex = 0;
        std::string mInfoId; // serialized RefId; empty => index-only
        std::string mText; // pre-rendered entry text
        std::string mActorName;
        std::int32_t mDay = 0; // originator's in-game date stamp
        std::int32_t mMonth = 0;
        std::int32_t mDayOfMonth = 0;
        ESM::RefNum mOrigin; // reporting peer's wire id, for echo suppression

        friend bool operator==(const JournalDelta&, const JournalDelta&) = default;
    };

    /// A shared global-variable change. Globals are co-op world state alongside the journal:
    /// quest flags scripts branch on must agree on every peer. Flows client -> host (a dialogue
    /// result or local script wrote it) and host -> clients (authoritative apply + relay, origin
    /// preserved). The unsynced families — game time (its own TimeSync channel), chargenstate
    /// (gates each client's private chargen bubble) and the per-player pc*/crime globals — never
    /// cross; see isUnsyncedGlobal. mType is the SETTER used ('i' via setGlobalInt, 'f' via
    /// setGlobalFloat); the receiving variant converts to its own storage type either way.
    struct GlobalDelta
    {
        std::string mName; // lowercased global name
        std::uint8_t mType = 'f';
        std::int32_t mIntValue = 0;
        float mFloatValue = 0.f;
        ESM::RefNum mOrigin;

        friend bool operator==(const GlobalDelta&, const GlobalDelta&) = default;
    };

    /// Host -> clients: the authoritative game clock. Emitted at a low cadence (each peer's clock
    /// advances locally between syncs; this corrects drift) and immediately after a discontinuous
    /// advance (rest, jail, travel), which clients themselves never perform locally — they route a
    /// TimeRequest instead. Applied through the same setGlobal* path the DateTimeManager listens
    /// to, so sun position, weather timing and NPC schedules follow.
    struct TimeSync
    {
        float mGameHour = 0.f;
        std::int32_t mDay = 0;
        std::int32_t mMonth = 0;
        std::int32_t mYear = 0;
        std::int32_t mDaysPassed = 0;
        float mTimeScale = 1.f;

        friend bool operator==(const TimeSync&, const TimeSync&) = default;
    };

    /// Client -> host: advance the shared world clock by this many hours (a rest step, a jail
    /// term, fast travel). The world has ONE clock in co-op — any player's rest advances it for
    /// everyone — so the client skips its local advance and waits for the discontinuity TimeSync.
    struct TimeRequest
    {
        float mHours = 0.f;
        ESM::RefNum mOrigin;

        friend bool operator==(const TimeRequest&, const TimeRequest&) = default;
    };

    /// Should this global stay off the wire? The time family has its own TimeSync channel;
    /// chargenstate gates each client's private chargen bubble and must never leak between
    /// peers; the pc*/crime family reflects ONE player's dialogue state (crime gold owed, race
    /// checks) and is re-derived per machine. Everything else — including mod globals — syncs.
    bool isUnsyncedGlobal(std::string_view name);

    /// A scripted ref enable/disable — the world mutation quest scripts actually perform (Dreamers
    /// appearing after a quest stage). With the journal, globals and clock synced, every peer's
    /// own scripts usually converge on the same change; this channel is the authoritative
    /// correction that also covers divergence (Random, per-player GetDistance) and peers whose
    /// cell was unloaded when the script ran (re-applied at cell load and periodically
    /// re-asserted). Only content refs cross — dynamic refs ride the item/summon channels.
    /// Idempotent on both ends (World::enable/disable no-op on an unchanged flag).
    struct RefEnable
    {
        ESM::RefNum mRef;
        bool mEnabled = true;
        ESM::RefNum mOrigin; // reporting peer's wire id, for echo suppression

        friend bool operator==(const RefEnable&, const RefEnable&) = default;
    };

    /// An interactable door's commanded state change: a player/NPC/script swinging it open or
    /// closed (World::activateDoor), or a script's Lock snapping it shut. Only the command
    /// crosses, not the angle — every peer plays the fixed 90°/s swing locally, so the terminal
    /// pose converges. Flows client -> host (my player pushed a door) and host -> clients
    /// (authoritative apply + relay, origin preserved; the host's record is re-asserted
    /// periodically and at cell load, change-guarded on the receiver, so late joiners find doors
    /// standing the way the world left them). Only content refs cross; teleport ("load") doors
    /// never swing. mState carries MWWorld::DoorState.
    struct DoorMove
    {
        ESM::RefNum mRef;
        std::uint8_t mState = 0; // MWWorld::DoorState: 0 idle (snapped shut), 1 opening, 2 closing
        // The door's CellRef lock level (signed: > 0 locked at that pick difficulty; <= 0 unlocked,
        // the magnitude remembered). Carried on every move and on a bare lock/unlock so a client's
        // lockpick, a key, or a scripted Lock/Unlock reaches every peer — otherwise a shared door a
        // client opened stays locked for the host and everyone else. Applied change-guarded.
        std::int32_t mLockLevel = 0;
        // The door's CellRef trap spell (ESM::RefId serialized text; empty = untrapped/disarmed).
        // Rides alongside the lock so a client's disarmed trap, or a trap consumed when the door was
        // opened, reaches every peer — otherwise another peer re-triggers a trap that's already gone.
        std::string mTrap;
        ESM::RefNum mOrigin; // reporting peer's wire id, for echo suppression

        friend bool operator==(const DoorMove&, const DoorMove&) = default;
    };

    /// One magic effect of a routed spell/enchant cast, mirroring ESM::ActiveEffect but only the
    /// fields needed to reconstruct it on the owner. Magnitude is sent as min/max, not rolled — the
    /// owner rolls, staying authoritative over its own actor's RNG, resistance and reflection.
    struct SpellEffect
    {
        std::string mEffectId; // ESM::RefId serialized text (the magic effect)
        std::string mArg; // skill/attribute RefId serialized text (empty when the effect has none)
        float mMinMagnitude = 0.f;
        float mMaxMagnitude = 0.f;
        float mDuration = 0.f;
        std::int32_t mEffectIndex = 0;
        std::int32_t mFlags = 0; // ESM::ActiveEffect::Flags

        friend bool operator==(const SpellEffect&, const SpellEffect&) = default;
    };

    /// client -> host: a player cast a spell/enchant on a host-owned actor. A magic effect is not
    /// applied where it's cast — it's added to the target's ActiveSpells and ticked every frame by
    /// the actor's owner. So a client can't apply it to the host's replica (the host would overwrite
    /// it); instead it routes the cast here and the host adds it to the authoritative actor, whose
    /// stat/position snapshots then carry the outcome (damage, paralyze, death) back to every peer.
    struct SpellCast
    {
        ESM::RefNum mCaster; // casting player's wire id
        ESM::RefNum mTarget; // host-owned actor's shared RefNum (or a peer's wire id for PvP magic)
        std::string mSourceSpellId; // ESM::RefId serialized text (recast/dispel/UI key)
        std::string mDisplayName; // spell/source display name
        ESM::RefNum mItem; // enchant source item, unset for a plain spell
        std::int32_t mFlags = 0; // ESM::ActiveSpells::Flags
        std::vector<SpellEffect> mEffects;
        ESM::RefNum mOrigin; // reporting peer's wire id, for echo suppression

        friend bool operator==(const SpellCast&, const SpellCast&) = default;
    };

    /// host -> clients: a magic effect's hit VFX (the coloured flash/particles on the model, from
    /// playEffects) just played on a host-owned actor. The effect itself rides the actor's stat
    /// snapshots; this is purely cosmetic, so a witness sees the target react when a spell lands
    /// rather than just its health tick down. The player's own hits are visualized locally (the
    /// effect lives in its own ActiveSpells), so only host NPCs/creatures cross here.
    struct SpellVfx
    {
        ESM::RefNum mActor; // the struck host-owned actor's shared RefNum
        std::string mEffectId; // ESM::RefId serialized text of the magic effect whose VFX to play

        friend bool operator==(const SpellVfx&, const SpellVfx&) = default;
    };

    /// A global script started or stopped outside the content defaults (StartScript/StopScript —
    /// quest machinery like escort timers and staged events). Only entries differing from the
    /// default running set ("main" + the ESM::StartScript store, which addStartup starts on every
    /// machine identically) are seeded; live transitions cross as they happen. Targeted starts
    /// carry the target's RefNum and record id; a receiver that can resolve neither starts the
    /// script untargeted (rare — vanilla StartScript is almost always untargeted). Script LOCALS
    /// deliberately do not cross: they converge because every machine runs the same scripts over
    /// the now-synced journal/globals/clock.
    struct ScriptRun
    {
        std::string mScript; // script RefId, serialized
        bool mRunning = true;
        ESM::RefNum mTargetRef; // unset for untargeted scripts
        std::string mTargetId; // serialized RefId; empty for untargeted
        ESM::RefNum mOrigin; // reporting peer's wire id, for echo suppression

        friend bool operator==(const ScriptRun&, const ScriptRun&) = default;
    };

    /// Host -> clients: the authoritative weather for one region. Weather is per-region and normally
    /// rolled locally (random, so every machine would pick differently even with a synced clock), so
    /// the host owns it: it rolls each occupied region's weather from the region's static
    /// probabilities on the shared clock and broadcasts the result; clients apply it and suppress
    /// their own rolls. Scripted ChangeWeather on the host rides the same channel. mWeatherId is the
    /// weather index (0..N-1 into the weather settings). Re-asserted periodically for late joiners.
    struct WeatherSync
    {
        std::string mRegion; // region RefId, serialized
        std::int32_t mWeatherId = 0;
        ESM::RefNum mOrigin; // host's wire id, for echo suppression

        friend bool operator==(const WeatherSync&, const WeatherSync&) = default;
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
        // host -> clients AND client -> host (own-player sounds, relayed onward): one-shot world
        // sounds game logic played, for the other peers to replay.
        std::vector<WorldSound> mSounds;
        // host -> the owning client: a guard caught its avatar, so it should open the arrest dialogue.
        std::vector<ArrestRequest> mArrests;
        // client -> host: make a host-owned actor fight my avatar (resist arrest / scripted aggression).
        std::vector<CombatRequest> mCombatRequests;
        // both ways: shared-journal changes (client reports its quest progress up; the host
        // applies authoritatively and relays to every peer).
        std::vector<JournalDelta> mJournalDeltas;
        // both ways: shared global-variable changes (same flow as journal deltas).
        std::vector<GlobalDelta> mGlobalDeltas;
        // host -> clients: the authoritative game clock (at most one meaningful per batch).
        std::vector<TimeSync> mTimeSyncs;
        // client -> host: discontinuous time advances (rest/jail/travel) to resolve centrally.
        std::vector<TimeRequest> mTimeRequests;
        // both ways: scripted ref enable/disable state (same flow as journal/global deltas).
        std::vector<RefEnable> mRefEnables;
        // both ways: global script start/stop transitions (same flow as journal/global deltas).
        std::vector<ScriptRun> mScriptRuns;
        // host -> clients: authoritative per-region weather (rolled on the host from region chances).
        std::vector<WeatherSync> mWeatherSyncs;
        // both ways: interactable door swings (same flow as ref enables).
        std::vector<DoorMove> mDoorMoves;
        // client -> host: a player's spell/enchant cast on a host-owned actor, for the host to apply
        // to the authoritative actor (the outcome returns via the actor's stat/position snapshots).
        std::vector<SpellCast> mSpellCasts;
        // host -> clients: cosmetic magic-effect hit VFX to replay on a host-owned actor.
        std::vector<SpellVfx> mSpellVfx;
        // client -> host ONLY (never relayed): the peer's full player inventory — equipped items
        // included — keyed by its net id (mId is the sNetPlayerContentFile net id, not a world
        // RefNum). Unlike equipment (which every peer needs to dress the avatar), the backpack is
        // uploaded solely so the host's avatar carries it and persists it with the character record.
        std::vector<ContainerState> mAvatarInventory;

        bool empty() const
        {
            return mHits.empty() && mPlayerDamages.empty() && mDrops.empty() && mItemsTaken.empty()
                && mContainers.empty() && mContainerChanges.empty() && mContainerRevokes.empty()
                && mSummons.empty() && mBounties.empty() && mSpeech.empty() && mSounds.empty() && mArrests.empty()
                && mCombatRequests.empty() && mJournalDeltas.empty() && mGlobalDeltas.empty() && mTimeSyncs.empty()
                && mTimeRequests.empty() && mRefEnables.empty() && mScriptRuns.empty() && mWeatherSyncs.empty()
                && mDoorMoves.empty() && mSpellCasts.empty() && mSpellVfx.empty() && mAvatarInventory.empty();
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
