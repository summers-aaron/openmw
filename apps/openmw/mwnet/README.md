# OpenMW multiplayer (`mwnet`) — status & plan

Experimental, authoritative-server multiplayer built directly into the engine (no
separate server binary, no Lua bridge). This document tracks where the work stands on
the `merge/mp-de-singleton` branch: the design, what works, and what's still broken and
why.

> This is a moving target and a research branch. Treat everything here as
> work-in-progress, not a stable protocol. The wire format (`snapshot.cpp`,
> `sVersion = 5`) is not compatibility-locked.

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
- Remote avatars **animate** across the player animation state space:
  - **locomotion** — walk/idle at the right speed, run/sneak gait, strafe, swimming, and
    jump/fall/land (an airborne flag on the wire; the puppet's own controller plays the arc);
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
    death-knockdown/knockout variant because the knockdown state is replicated too).
- Avatars **track their owner across cells**, interiors included (cell id on the wire +
  direct avatar migration).
- **World NPCs are simulated by the host** and replicated to clients. A client stops
  running local AI on a host-owned actor (`isRemoteOwned` → cease-remote-sim), so the two
  sides don't fight over the same actor.
- **Combat both ways:**
  - **PvP** — a client can damage another client; the hit is routed to the victim by
    network id and applied to their real player (with flinch / hit overlay).
  - **AI retaliation** — striking a host-owned NPC makes it fight back, for **any**
    client, in **any** cell (interior or exterior).
- **Loose-item world persistence (floor items)** — a player's dropped item is placed
  authoritatively by the host and replicated to every peer under one shared RefNum; a
  pickup (of a dropped *or* a save item) deletes it for everyone. Host-authoritative,
  wait-for-echo. Items already in the save aren't re-replicated (they load identically),
  and single-player is unaffected. *Not yet covered:* NPC death drops (only player drops
  are tracked), and the two-clients-grab-the-same-item race can duplicate.
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

`Replicator::applyActions` and avatar instantiation emit verbose `Debug::Info`
diagnostics (`applyActions: …`, avatar `refNum`/cell). These were for live debugging and
should be gated behind a verbose flag or removed before any merge.

### 6. Tuned for the dedicated-host topology

The avatar-as-non-primary-player approach is exercised against a headless host. A
"host-that-also-plays" (one window is both authority and a rendered player) needs the
avatar rendered on the host and is less tested.

### 7. A few animation states are still not replicated

The player animation state machine is covered (locomotion, jump, draw, swing, cast, block,
hit/knockdown/knockout/death). Deliberately left out, low-value or fiddly:

- **Turn-in-place** (`turnleft`/`turnright`, the foot-shuffle while pivoting without
  translating). The controller derives it from a per-frame rotation *rate* it reads from
  `Movement::mRotation`, but the avatar's rotation is set authoritatively each tick via
  `rotateObject`, so there is no rate to read. Feeding a synthetic `mRotation` would fight the
  authoritative rotation. Avatars still face the right way; they just don't shuffle their feet
  while turning on the spot.
- **Idle fidget variants** (`idle2`–`idle9`) and **`idlestorm`** (shielding from ash storms).
  The fidgets are random cosmetic flavour; `idlestorm` is weather-driven and plays on the
  avatar locally anyway when it stands in the same storm. Neither is worth a wire field.

## Roadmap (rough, next-first)

1. Strip/gate the debug logging; promote the stable parts out of "research" state.
2. Multi-anchor cell ref-counting → fix exterior accumulation (limitation #2).
3. Disconnect handling: `removePlayer` + release kept-alive cells + drop the avatar.
4. Host-side combat validation (limitation #4).
5. Broaden replicated world state: **container contents** (resolve leveled lists on the
   host and replicate the serialized store; route take/put through the host), then doors
   and world script state. Loose floor items already cross the wire; extend item coverage
   to NPC death drops and other host-side runtime drops.
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
