# OpenMW multiplayer (`mwnet`) — status & plan

Experimental, authoritative-server multiplayer built directly into the engine (no
separate server binary, no Lua bridge). This document tracks where the work stands on
the `merge/mp-de-singleton` branch: the design, what works, and what's still broken and
why.

> This is a moving target and a research branch. Treat everything here as
> work-in-progress, not a stable protocol. The wire format (`snapshot.cpp`,
> `sVersion = 10`) is not compatibility-locked.

## Topology

The intended deployment is **one headless dedicated server + N rendering clients**:

- The server runs `openmw --dedicated --listen <port>`. It owns and arbitrates the
  shared world. Its own "anchor" player is invisible and not replicated to clients; it
  exists only so the engine has a player to anchor cell/AI simulation on.
- Each client runs `openmw --connect <host:port>`. Each is a real player. Clients see
  each other as **avatars** and the server resolves their shared-world interactions
  (combat, damage).

Single-player and loopback are unaffected: with no network id assigned and no authority
set, the whole sample → serialize → transport → deserialize path still runs but nothing
is applied, so SP stays byte-identical.

## Architecture

| File | Responsibility |
|------|----------------|
| `session.*` | `HostSession` / `ClientSession` / `LoopbackSession` — peer roster, broadcast, poll. Defines who is the authority and who obeys remote state. |
| `networktransport.*`, `loopbacktransport.*`, `sessiontransport.hpp` | Byte transport over the wire (Reliable / Unreliable channels). |
| `snapshot.*` | The per-tick world delta. `EntityState` carries transform, dynamic stats, draw state, move flags, swing, speed, appearance, equipment, and **cell id**. Versioned, fully bounds-checked / fuzzed deserialization. |
| `replicator.*` | The bridge between the running simulation and the snapshot/action channel. Samples owned actors, applies remote deltas, instantiates and drives avatars, resolves combat. The bulk of the gameplay logic lives here. |
| `actions.*` | `CombatHit` (client → host: "I struck X") and `PlayerDamage` (host → client: "you were hit"). The combat round-trip. |
| `events.*` | Replicated Lua events. |

Wiring lives in `apps/openmw/engine.cpp` (`pumpTransport`): each frame it samples a delta
and broadcasts it, drains/sends reported actions, applies received deltas/actions, then
re-asserts remote actors' locomotion (`driveRemoteActors`).

### The player & cell model (the "de-singleton" dependency)

This branch removes the hard-coded single-player assumption from the engine. The world
now holds a `MWWorld::Players` collection: index 0 is the primary (local) player; **each
connected peer is represented on the host as a non-primary player** (`World::addPlayer`).
Being a real player is what lets the host:

- **keep the peer's cell loaded** — `Scene::isCellOccupiedByNonPrimaryPlayer` stops those
  cells unloading, and `Scene::addExtraPlayer` loads the full exterior **grid** around
  each avatar (not just its single sub-cell), so everything the peer can interact with is
  simulated host-side; and
- **drive AI near the peer** — the mechanics AI/animation loops gate on the distance to
  the *nearest* player, so NPCs around a remote peer are simulated even when the host's
  own anchor player is far away.

Non-primary players are minted a unique, stable `RefNum` under a reserved content file
(`-2000`) so AI target tracking (which keys purely on `RefNum`) can tell avatars apart.
They are added to the scene (`addObjectToScene` → collision body + `mActors` slot) so NPCs
can perceive, target and melee them, and they migrate between cells **directly** (re-point
the `Player` wrapper + re-key the registry), never through `moveObject`/`CellStore::moveTo`
(whose moved-ref bookkeeping is built for placed refs and strands a player ref on its
second move).

## What works

- Two clients connect to a headless dedicated server and load the shared save.
- Clients **see each other as avatars** with the correct race/sex/head/hair and worn
  equipment.
- A client's **full inventory** (not just worn equipment) is uploaded to the host on the
  container channel, keyed by its net id and applied to its avatar (never relayed to other
  peers, who only need the visible equipment). The host carries it on the avatar and persists
  it with the character record (`REC_PLAYER_EXTRA`), so items a client picked up, bought, or
  was given survive a server save/restart and are served back when it rejoins.
- Remote avatars **animate** across the player animation state space:
  - **locomotion** — walk/idle at the right speed, run/sneak gait, strafe, swimming, and
    jump/fall/land (an airborne flag on the wire; the puppet's own controller plays the arc);
  - **turn-in-place** — the `turnleft`/`turnright` foot-shuffle, observed from the lower body on the
    authority and carried as two spare move-flag bits; the receiver loops the group explicitly (the
    puppet has no rotation rate to re-derive it from) while the authoritative facing is unchanged;
  - **weapon draw / sheathe** and the combat stance;
  - discrete **melee swings** (per-type: chop/slash/thrust) and **spell casts**, replicated
    as an edge-counter (not a streamed playhead) so a free-running NPC weapon loop can't spawn
    phantom swings. A weapon swing is sent in two slices — a **wind-up** (the avatar holds its
    drawn-back charge pose for exactly as long as its owner holds the attack button) and a
    **release** (the strike on let-go, played as a strike arc plus a strength-scaled
    follow-through) — so a charged attack reads as hold-then-strike at the size it was charged to,
    not an instant swing. A cast reproduces its **cosmetic VFX** — the body aura, glowing hands, and a
    non-damaging bolt for target spells that leaves the hand at the animation's release key and
    bursts on impact (gameplay stays authoritative on the caster);
  - **shield blocks**, carried on the same discrete channel (detected from the block animation,
    since the `getBlock()` flag is set and cleared within one mechanics pass);
  - **hit reactions** — flinch on damage, **knockdown** and fatigue **knockout** (the
    knocked-down flag rides a spare move-flag bit; knockout additionally falls out of replicated
    fatigue), and **death** (from replicated health <= 0, picking the matching
    death-knockdown/knockout variant because the knockdown state is replicated too);
  - **idle fidgets** (`idle2`–`idle9`) — the random standing-idle variations an actor's AI plays
    via the animation queue. A host-owned actor's AI is suppressed on the receiver, so it would
    never fidget on its own; the authority's choice is detected and replayed through the same queue.
- Avatars **track their owner across cells**, interiors included (cell id on the wire +
  direct avatar migration).
- **World NPCs are simulated by the host** and replicated to clients. A client stops
  running local AI on a host-owned actor (`isRemoteOwned` → cease-remote-sim), so the two
  sides don't fight over the same actor.
- **NPC/creature inventories and equipment are host-authoritative.** An actor's
  leveled-list loot is resolved on the host and broadcast on the container channel the
  first time the actor is replicated (then re-asserted periodically, which also covers
  late joiners), so the weapon a guard wields, what a pickpocket window shows, and what
  its corpse holds are the same on every peer regardless of host-save history or player
  level. The worn slots additionally ride the full-refresh snapshot (like an avatar's),
  pinning the visible dress to exactly what the host equipped. As a first line of
  defense every peer also *rolls* identically: in a networked session, leveled-list
  fills (NPCs, creatures, containers) are seeded from the ref's shared RefNum
  (`levelledListSeed`) from process start, so a store resolved before the broadcast
  arrives is already almost always right. Single-player keeps the historical random
  roll.
- **Summoned creatures** are host-authoritative. A host NPC's summon spawns host-side and
  replicates like any NPC; a **player's** summon is intercepted on the client and routed to the
  host (a `SummonAction`), which spawns the creature bound to that player's avatar. Because the
  creature isn't in the shared save, the host carries a **spawn descriptor** (its creature RefId
  + cell) on the wire so receivers instantiate it the first time they see it, then drive it like
  any host-owned actor; its despawn (effect ended, or it died) is broadcast as a removal. Being
  host-owned, its AI and combat ride the normal NPC / cross-peer-hit paths.
- **Combat both ways:**
  - **PvP** — a client can damage another client; the hit is routed to the victim by
    network id and applied to their real player (with flinch / hit overlay).
  - **AI retaliation** — striking a host-owned NPC makes it fight back, for **any**
    client, in **any** cell (interior or exterior).
  - **Melee and ranged** — bow/crossbow/thrown hits route through the same host-authoritative
    path as melee (`projectileHit` mirrors `Npc::hit`), and witnesses see the arrow/bolt fly: at
    the shooter's release the avatar looses a **cosmetic** projectile from its own replicated bow +
    ammo (no hit resolved, no ammo consumed — the real shot stays authoritative on the shooter).
- **Loose-item world persistence (floor items)** — a player's dropped item is placed
  authoritatively by the host and replicated to every peer under one shared RefNum; a
  pickup (of a dropped *or* a save item) deletes it for everyone. Host-authoritative,
  wait-for-echo. Items already in the save aren't re-replicated (they load identically),
  and single-player is unaffected. *Not yet covered:* NPC death drops (only player drops
  are tracked), and the two-clients-grab-the-same-item race can duplicate.
- **Interactable doors** — a door any player/NPC/script swings open or shut (or a script
  `Lock` snaps shut) moves on every peer. Only the commanded edge crosses (`DoorMove`);
  each peer plays the fixed 90°/s swing locally, so the terminal pose converges. The
  host's per-door record is re-asserted periodically and at cell load (change-guarded on
  receivers) so late joiners find doors standing the way the world left them, and it
  persists in the server save. Teleport ("load") doors don't swing and never cross.
- **Cell-state protocol (per-cell baseline download)** — as a client loads a cell it asks
  the host (`CellStateRequest`) and the host answers with the cell's *current* state as the
  same `REC_CSTA` record a save writes (`cellstatecodec`), applied through the save-load
  read path. This replaces the client's legacy load-time re-derivation wholesale: ghost
  items the host's save consumed, moved/changed content refs, container contents,
  scripted enable/disable, door poses, and save-restored dynamic spawns all arrive in one
  blob with their RefNums verbatim — closing the generated-RefNum aliasing bug class at
  the root instead of one category at a time. Mechanics:
  - the client's own generated RefNums allocate from a disjoint range
    (`sClientGeneratedRefNumBase`), so blob refs can never collide with (and silently
    re-key) a local allocation, and host- vs client-origin refs are distinguishable;
  - re-applying is safe: the client clears the cell's host-origin runtime refs before
    each apply (the save-read path always *adds* non-content refs), so every reload
    re-requests and converges — no per-category caches needed while a cell is unloaded;
  - a blob for a scene-ACTIVE cell applies inside `Scene::reloadCellWith` (unload → apply
    → load): the save-format read path replaces each touched ref's RefData wholesale —
    base node, custom data, mwscript locals — which is only safe against bare ref lists,
    the one regime a real save-load exercises. Mutating a live cell instead leaves
    still-registered scripts running on unconfigured locals and mechanics/rendering
    holding freed state (heap corruption, found the hard way);
  - the host serves only once the cell is **scene-active** on its side (the requester's
    avatar migration is loading it), so the blob is cut *after* load-time reconciliation
    (leveled rolls, respawn, save-restored-spawn RefNum migration); parked requests retry
    each pump with a deadline, and an unknown/oversized cell gets an empty-blob denial —
    on which the client falls back to the legacy per-category machinery (kept compiled);
  - summons and avatars are filtered out of blobs (the snapshot channel owns them);
    reserved `-3001` spawns are included, and the descriptor path adopts instead of
    duplicating. Live channels (snapshot, actions, container broadcasts) are untouched —
    the blob is the *baseline*, the live channels are the *diffs*.
  - the host also PUSHES a cell's state unsolicited to every peer the moment the cell
    becomes scene-active on its side (a resuming player's slot unparking at character
    select, an avatar's grid growing into new cells). A client STAGES a push into a cell
    it hasn't loaded (a prefetched baseline: the cell's own load then builds straight
    from host state — no ghost window, no reload, no request round-trip, as long as the
    staging is fresh) and drops it for a cell it has loaded (the live channels keep
    active cells right; reloading them would be pure churn). Blob application is held
    only during ACTUAL chargen — the pre-adopt select screen applies staged baselines,
    which is what makes a resumed character's spawn area arrive pre-built.
  - *Accepted transient:* for a cell the client loads before any blob arrives (walking
    into fresh territory ahead of the host's avatar migration), content-default state
    shows for ~1-2 pumps until the requested blob lands and the cell reloads. The legacy
    retention (`mRefStates`/`mDoorStates`/`mRemovedWorldItems` re-asserts) deliberately
    stays: it is the denial fallback's data.
- A test harness (`mp-server.sh`, see below) spins up the server + two pre-kitted clients.

## What's broken / known limitations

### 1. NPCs retaliate but don't chase (headless navmesh) — environmental

On the **headless dedicated server**, the Detour/Recast navmesh does not generate (no
working GPU/GL pipeline under `SDL_VIDEODRIVER=offscreen` + software GL). With no navmesh,
NPC pathfinding produces nothing, so a provoked NPC **fights only when its target is
already in melee/attack range** — it can't path toward or pursue a player. This is a
property of the headless environment, **not** the sync code: on a host that renders
(non-headless), the navmesh builds and NPCs chase normally. Confirmed by the standalone
navmesh tests failing under the same headless software-GL setup.

### 2. Exterior cell accumulation on an idle-anchor host

The host loads a grid around each avatar but, on a dedicated server, the anchor player
never moves, so `Scene::changeCellGrid` (which is what normally unloads far cells) never
runs. Exterior cells an avatar walks away from are therefore **not unloaded** — a slow
memory creep over a long session. The right fix is a multi-anchor cell manager that
ref-counts cells per player and unloads them when no player (primary or peer) is near.

### 3. Non-primary-player lifecycle is custom and partial

- **Disconnect cleanup** is not wired up: when a client leaves, its avatar / non-primary
  player (and the cells it was keeping alive) persist. `World::removePlayer` exists but is
  not called on disconnect.
- Live cross-cell migration of non-primary players is new territory for the de-singleton
  work; the avatar path bypasses `moveObject` deliberately, which means some `moveObject`
  side effects (e.g. cell-scoped script hooks) don't run for avatars.

### 4. Combat is trust-the-client

The host applies the damage number the client computed with its full hit formula. There
is **no host-side re-validation** yet — a malicious client could report arbitrary damage.
Hardening (recompute or sanity-bound the damage host-side) is a later step.

### 5. Diagnostics still in the hot path

`Replicator::applyActions` and avatar instantiation emit per-event diagnostics
(`applyActions: …`, avatar `refNum`/cell). These now log at `Debug::Verbose` so they stay
silent at the default log level, but they haven't been curated down — before any merge the
survivors should be reviewed and the truly noisy ones removed rather than just gated.

### 6. Tuned for the dedicated-host topology

The avatar-as-non-primary-player approach is exercised against a headless host. A
"host-that-also-plays" (one window is both authority and a rendered player) needs the
avatar rendered on the host and is less tested.

### 7. A couple of animation states are still not replicated

The player animation state machine is covered (locomotion, turn-in-place, jump, draw, swing,
cast, block, hit/knockdown/knockout/death, idle fidgets). Deliberately left out:

- **`idlestorm`** (shielding from ash storms) is not replicated, but it's weather-driven and the
  avatar's own controller plays it locally when it stands in the same storm, so it needs no wire
  field. (The random `idle2`–`idle9` fidgets *are* replicated — see "What works".)

## Roadmap (rough, next-first)

1. Strip/gate the debug logging; promote the stable parts out of "research" state.
2. Multi-anchor cell ref-counting → fix exterior accumulation (limitation #2).
3. Disconnect handling: `removePlayer` + release kept-alive cells + drop the avatar.
4. Host-side combat validation (limitation #4).
5. Broaden replicated world state: world script state next. Container/corpse contents,
   NPC/creature inventories, take/put arbitration, loose floor items and interactable
   door swings already cross the wire (doors as `DoorMove`: the commanded
   open/close/snap-shut edge, animated locally on each peer, re-asserted for late
   joiners and persisted in the server save). Remaining item gap: host-side runtime
   floor drops beyond player drops.
6. Networked navmesh strategy for headless servers, or document the host-renders
   requirement (limitation #1).

## Testing

There is no in-repo automated MP integration test yet (the live path needs two real
client processes + a GPU for the clients). Unit coverage exists for the wire format
(`apps/openmw_tests/mwnet/` — snapshot/actions round-trip + fuzz).

Manual harness (not in-repo; lives in the developer's scratch scripts): `mp-server.sh`
launches a headless `--dedicated` server plus two `--connect` clients in containers, each
loading the same save, each pre-kitted via `--script-run` (console loadout: glass vs
daedric armour, set attributes/skills). It tails the server log for the combat
diagnostics above. Build the engine in the container per the project's build notes (Qt
disabled, `-DBUILD_OPENMW_TESTS=ON` for the unit tests).
