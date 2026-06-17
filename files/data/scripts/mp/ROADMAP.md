# Morrowind Multiplayer — Roadmap & Onboarding

Onboarding doc for anyone (human or AI) picking this up. Read this first, then
[`README.md`](README.md) for how the current code works.

## Vision

A **Morrowind multiplayer mod for OpenMW 0.52**, built "the right way": almost entirely in
the Lua scripting layer (`openmw.*`) plus a tiny engine fork, rather than weaving a thin shim
through the C++ like the retired TES3MP approach.

Model is **server-authoritative**. A headless OpenMW instance is the dedicated server and the
single source of truth for the world (NPC AI, combat, world state). Clients are mostly
**terminals**: they render the server's world as "ghost" objects, send their own player's
input/transform up, and apply what the server tells them.

## Architecture in one screen

- **Engine fork** (`~/ai-morrowind/openmw`, branch `morrowind-mp`): two committed C++ changes —
  `--headless` (dedicated-server mode, hidden SDL window) and `openmw.network` (framed-TCP
  transport primitive: listen/connect/send/broadcast/poll/peers). Everything else is Lua.
- **The addon** (`openmw-run/data/scripts/mp/`): the multiplayer logic. Role chosen by content
  file — `mp-server.omwscripts` vs `mp-client.omwscripts`.
- **Server** runs the real sim + an authority script that broadcasts actor state, relays player
  state, applies combat. Keeps a hidden **proxy** actor per connected player so the world has a
  real body to target/hit.
- **Clients** render server actors + other players as ghosts (interpolated, animated, faced),
  freeze native AI, hide native twins, and stream their own player up.
- **Headless + GPU/display-less**: runs with `SDL_VIDEODRIVER=offscreen` and no `DISPLAY`
  (software GL via llvmpipe) — cloud-deployable, no GPU. Launch with `mp.sh` / `mp-server.sh` /
  `mp-client.sh` (WSL) or the Windows `.bat`s.

## Status — DONE (verified headless)

- Headless dedicated server; GPU/display-less cloud run.
- `openmw.network` transport; connect-retry; interest management; delta + keepalive; 30 Hz.
- Server-authoritative AI (real engine `AiPursue`/`AiTravel`/etc. drive actors).
- Actor + player replication; client **interpolation** (smooth) + **facing** (yaw).
- **Locomotion animation** (walk/run/idle, Tier 1) with spike/hysteresis smoothing.
- **Combat authority** (PvE): client reports hit → server applies authoritative damage/death →
  replicates → client removes the corpse.
- **Player proxy** → bidirectional combat: NPCs retaliate against the proxy; damage to the
  proxy relays back to the real player.
- **PvP** (hit another player → server damages their proxy → relays).
- Full **player representation replicates to both the visual avatar and the combat proxy**:
  **appearance** (race/sex/head/hair via template-clone trick), **stats** (health/level/
  attributes/skills), **equipment** (armor/clothes/weapons). Symmetric regardless of join order.
- Local **health-pin** so puppets never die locally (no phantom corpses).
- **Faithful animation** on every ghost (NPCs + player avatars) via active-group replication.
- **Player death/respawn**, **time-of-day sync**, **per-client cell loading** (centroid grid).
- **Server world persistence** (autosave + resume; world survives restart).
- Spawn in middle of Seyda Neen.
- Windows-container builder (`~/ai-morrowind/openmw-windows-builder`) + `.bat` launchers.

Detailed verification notes + every gotcha live in the auto-memory:
`~/.claude/projects/-home-aaron-ai-morrowind-TES3MP/memory/phase2-puppet-verified.md`.

## Roadmap

Ordered by recommendation. Each phase notes effort/risk.

### Phase A — World architecture (KEYSTONE, do next)
**Per-client server cell loading.** Today the server only simulates cells around ONE placeholder
player (parked in Seyda Neen), so all players are confined there.

PROVEN (cellprobe.lua): active cells follow ONLY the player. A test actor teleported to Balmora
while the player stayed put → `active=false`, 0 nearby NPCs (cell not simulated). Move the
*player* there → `active=true`, 19 NPCs load. No Lua hook exists (`getCell(forceLoad=false)`
loads cell *data*, not simulation). So this needs a C++ change. TWO gates, both player-centric:
1. **Scene** (`scene.cpp changeCellGrid`) loads a `CellGridRadius`(=1, a hard const → 3×3) grid
   around the player's cell; far cells unload. Also drives navmesh bounds (`DetourNavigator`),
   terrain, render grid — single-center.
2. **Actors** AI processing gated on distance from the player (`Settings::game().mActorsProcessingRange`
   = 7168, a setting) — even a loaded far cell won't run AI.

Two implementation options:
- **DONE — Centroid + bigger single grid (co-op).** Park the placeholder player at the centroid
  of all connected players; widen the grid + actor-processing range. ONE center → no navmesh
  refactor. Engine changes (committed to the fork): (1) `Cells/exterior grid radius` is now a
  setting (was the hard `CellGridRadius=1`), used in `scene.cpp`; (2) `Game/actors processing
  range` clamp raised from 7168 to 65536. Lua: `authority.lua` teleports the placeholder to the
  players' centroid (throttled, >4096 drift). Server config: `config-server/settings.cfg` sets
  `exterior grid radius = 3`, `actors processing range = 24576`. VERIFIED: radius 4 simulated 206
  actors at Balmora (vs 27); a client roaming Seyda Neen→Balmora made the server follow and stream
  Balmora's NPCs. CONSTRAINT: all players must stay within ~grid-radius cells of the centroid
  (tunable; bigger grid = more spread at server-CPU cost). Bug fixed in passing: `openmw.network`
  `connect()` reassigned a joinable `std::thread` on retry → `std::terminate`; the slower
  big-grid boot exposed it (client's 1st connect failed → retry crashed). Now starts the IO
  thread once.
- **(Later, unconstrained) Full multi-center** — `changeCellGrid` loads the union of grids around
  player + every proxy; `DetourNavigator` gets multi-bounds; Actors range uses nearest center.
  Deep, multi-subsystem; the eventual real fix for fully-independent (far-apart) roaming.

### Phase B — Shared world state
Make the world itself consistent, not just actors. Mostly Lua, reusing the replication pattern.
- **Time of day — DONE.** Server broadcasts `core.getGameTime()` (`WORLD_TIME`, every 150 frames);
  clients correct drift > 1 game-min via `world.advanceTime(delta)`. Verified: a late-booting
  client (clock behind) snapped to the server's time then stayed synced (delta ~0).
- **Doors / containers / items / activators — TODO, needs a GPU client to verify.** Two snags:
  (1) cross-instance identity — a door's `id` (e.g. `0x103762f`) does NOT resolve via
  `getObjectByFormId` (same as actors). Need a stable key: cell name + deterministic
  `cell:getAll()` enumeration index (same ESM → same order in both instances), or cell+record+
  position match. (2) Activation-driven (global `onActivate(obj, actor)` fires on a real click) —
  can't trigger headless, so verification needs an in-engine client. API ready: `types.Door.isOpen`
  / `activateDoor(obj, open)`, `onActivate`.
- Weather: same pattern as time (broadcast + apply), fully testable headless.

### Phase C — Combat & death completeness
- **Player death/respawn — DONE.** `client/death.lua` (player ctx): on health <= 0, heal to full
  (prevents the game-over screen) + `sendGlobalEvent('MP_Respawn')`; terminal teleports to
  `cfg.spawn`. Verified: player at Balmora, health→0, respawned at Seyda Neen hp 35/35, no
  game-over. TODO: configurable respawn point / nearest temple; death penalty; death anim/delay.
- **Spells / ranged damage — ALREADY WORKS.** The engine fires the `Hit` Lua event for melee,
  ranged (combat.cpp) AND magic (spelleffects.cpp → class onHit → luaManager onHit), and
  `ghosthit.lua` reports `damage.health` regardless of `sourceType`. So player→target spell/ranged
  damage flows through the existing path; NPC→player via the proxy health-watch (catches any
  health drop). TODO: non-damage magic effects (paralyze/drain/buffs) + projectile/spell VISUALS.
- **Animation — DONE (faithful replication, NPCs AND player avatars).** Replaced the
  velocity-inference hack with real active-group replication on every ghost:
  - NPCs: `server/animread.lua` (per actor, self-ctx) reads `getActiveGroup(LowerBody/Torso)`,
    reports on change; authority caches + ships `la`/`ua` in `ACTOR_STATE`.
  - Avatars: `client/playeranim.lua` (PLAYER ctx — `openmw.animation` is local-only, so global
    `terminal.lua` can't read it) reads the player's own groups, `sendGlobalEvent('MP_MyAnim')`
    → terminal relays them up in `PLAYER_STATE` → server forwards in `REMOTE_PLAYER`.
  Either way the client forwards `MP_Anim` to the ghost. `ghostanim.lua` plays the LOCOMOTION
  group (`e.la`) FULL-BODY so the gait + arm swing stay coordinated. It deliberately does NOT pin
  the torso to a separate replicated group — that froze the arms to the torso's group (usually
  `idle` = hands on hips) even while walking ("arms at hips" regression). Velocity inference covers
  the brief pre-first-packet window. `e.ua` is still sent but ignored by the client.
  - **Combat anims (windup/swing) — STILL TODO; a playhead-sync attempt was REVERTED.** A drawn
    weapon keeps ONE group active (`handtohand`/`weapononehand`/...) the whole time; the attack is
    3 phases of `playBlendedAnimation(group, Priority_Weapon)` differentiated by TEXT KEYS + playback
    time (`<type> start`→`max attack`→`hit`→follow), NOT a group change (character.cpp:1704/1785/1810).
    So group-name replication can't see swings. A first attempt streamed `getCurrentTime` and
    re-seated the ghost's playhead via `playBlended{startPoint=frac, priority=Weapon}` — but it
    BROKE the upper body for every weapon-drawn actor (guards froze hands-down, likely seated to
    frac=0 / overrode the ready pose), so it was reverted. Needs a non-destructive approach (e.g.
    only overlay during an actual swing window, or drive a one-shot attack event, not continuous
    re-seat) and must not disturb the weapon-ready/locomotion pose.
- Crime — guards react to a player-proxy's crimes (proxy makes this possible). Needs GPU client.
  Interim TEST hook (`cfg.aggroGuards`, default ON): `authority.lua` forces guards (NPC class
  contains "guard") within `cfg.aggroRadius` to `StartAIPackage Combat` on each player's proxy, so
  NPC→player combat can be exercised in the open with no crime system. Verified headless: "imperial
  guard aggro -> peer 1" + proxy took damage relayed to the client. Set `aggroGuards=false` to stop.

### Phase D — Social
- Chat, player trading, grouping/party. Net layer is ready; mostly new message types + UI.

### Phase E — Persistence & production
- **Server world persistence — DONE (no engine change).** The dedicated server is a real
  OpenMW playthrough, so the whole world (NPC deaths, container loot, door states, time, item
  positions) is native save state. `server/persist.lua` (MENU ctx — `openmw.menu` is the only
  context exposing `saveGame`/`loadGame`, and MENU scripts run in-game) autosaves every
  `cfg.saveEvery` (120 s) to ONE fixed slot, overwriting in place. Boot-time load is shell-level:
  `mp-common.sh run_server` keeps the server's saves by default and, if `mp_autosave.omwsave`
  exists, boots with `--load-savegame <path>` instead of `--skip-menu` (a fresh new-game).
  `RESET_WORLD=1 ./mp-server.sh` wipes for a clean start. Verified: boot 1 saved (single file,
  overwritten — NOT 'foo - 1.omwsave'); boot 2 logged "resuming saved world", reached Running,
  re-listened :7000, kept autosaving. GOTCHA: the save system sanitises the description into the
  filename (`-` → `_`), so DESC/SLOT/the shell glob must all use `mp_autosave`, or SLOT never
  matches and every save spawns a new file. TODO (needs GPU client to fully confirm): prove a
  specific world mutation (killed NPC / looted container) survives the round-trip.
- Player persistence: each player's character (stats/inventory) lives in that CLIENT's own
  OpenMW save — the server owns the world, not the player bodies. So it's already persistent
  client-side; nothing to do unless we move to server-owned characters.
- **Disconnect/reconnect cleanup — DONE.** `authority.lua` reaps a peer that goes quiet:
  clients stream `PLAYER_STATE` every frame, so >5 s without one ⇒ gone. On reap it disables the
  server-side proxy body and clears every per-peer table (proxies/proxyHp/playerPos/lastSent/
  pstats/pstatted/pequip/pinfo/lastSeen). Liveness-based on purpose: the framed-TCP transport does
  NOT reliably drop a half-open peer (`kill -9` of the client left it in `net.peers()` streaming
  stale state), so we can't trust `net.peers()` alone. Verified: killed client → age climbed
  1→5 s → "reaped disconnected peer 1"; proxy + state freed, no leak across reconnects.
- TODO: server config (player slots, password, admin/kick).

### Phase F — Hard problems (deliberately deferred; need design before code)
- **Dialogue & quests** — the real MP wall: shared vs instanced quest state, who "owns" an NPC
  mid-conversation. TES3MP largely punted here. Design pass required.
- **Anti-cheat** — current model fully trusts clients (fine for co-op with friends, not public).
- **UDP transport** — TCP works but head-of-line blocks under loss; revisit for internet play.

## Hard-won constraints (don't re-discover these)

- **Self-context wall**: writing an actor's `health`/`stats`/`equipment`/`enableAI`, and playing
  animations, are allowed ONLY in a *local* (self) script on that actor — not from global. The
  pattern throughout: a global script creates/positions things; a per-actor local script
  (`damage.lua`, `equip.lua`, `ghosthit.lua`, `ghostanim.lua`) does the self-context writes,
  triggered by a `sendEvent`. *Reads* are global-OK.
- **`createRecordDraft` has no stats** → a from-scratch NPC spawns at 0 hp = a corpse. Use the
  `template` field (clone a valid NPC record) to get a live one. That's how avatars work.
- **`teleport` is async/queued** and resets animation — hence the per-frame teleport + scripted
  locomotion anim, and why `moveInto` then equip needs a retry.
- **`vfs-mw` overlay is REQUIRED** (`--data .../resources/vfs-mw`) or built-in control scripts
  crash and the player can't move.
- **Lua `0` is truthy** — `if not x` where x can be 0 never re-inits; use `x == nil`.
- **Death latches**: restoring health can't revive an actor whose death already processed — pin
  health BEFORE it can hit 0.
- Server simulates only where its placeholder player is → spawn the placeholder where the
  players are (current stopgap; Phase A is the real fix).

## File map

```
openmw-run/data/scripts/mp/
  config.lua            tunables (host/port/rates/avatar/spawn/interest)
  protocol.lua          network event names
  util.lua              shared global helpers (player snapshot, giveEquipment)
  spawn.lua             teleport player to spawn on boot (both roles)
  equip.lua             self: retry setEquipment (proxy + avatar)
  server/authority.lua  GLOBAL: broadcast state, relay players, combat, proxies, push-on-join
  server/persist.lua    MENU: periodic autosave of the world to a fixed slot (persistence)
  server/animread.lua   self (NPC/CREATURE): read active anim groups -> report to authority
  server/damage.lua     self (NPC/CREATURE): apply damage + stats to self
  client/terminal.lua   GLOBAL: ghosts, interpolation, avatars, attack reporting
  client/suppress.lua   PLAYER: freeze native AI
  client/playerdamage.lua  PLAYER: apply relayed damage to self
  client/ghosthit.lua   self: report player's hits up + pin puppet health
  client/playeranim.lua PLAYER: read own active anim groups -> relay to terminal (replicate avatar anim)
  client/ghostanim.lua  self: play replicated anim groups (MP_Anim); velocity fallback pre-first-packet
mp-server.omwscripts / mp-client.omwscripts   role content files
```

Engine fork: `~/ai-morrowind/openmw` (branch `morrowind-mp`).
Windows builder: `~/ai-morrowind/openmw-windows-builder`.
Run dir / configs / launch scripts: `~/ai-morrowind/openmw-run`.
