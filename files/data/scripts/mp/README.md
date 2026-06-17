# Morrowind MP (OpenMW 0.52 addon)

Server-authoritative multiplayer on OpenMW's Lua + `openmw.network`. The dedicated
server runs the full game simulation; clients are terminals that render server state
as ghost objects and stream their own player transform up.

## Layout

```
scripts/mp/
  config.lua            tunables (host, port, broadcast rate, avatar record)
  protocol.lua          network event names (the wire vocabulary)
  util.lua              shared helpers (getPlayer, yaw)
  server/authority.lua  GLOBAL  — the authority: broadcast actor state, relay players
  client/terminal.lua   GLOBAL  — ghost server actors + remote players, hide native twins
  client/suppress.lua   PLAYER  — freeze native AI (debug pkg is player-context)
```

Role is chosen by content file:
- `mp-server.omwscripts` → server/authority.lua
- `mp-client.omwscripts` → client/terminal.lua + client/suppress.lua

## Protocol

| Event | Dir | Payload |
|-------|-----|---------|
| `apos` (ACTOR_STATE) | server→clients | `{id, rec, x, y, z, yaw}` per server-owned actor |
| `playerpos` (PLAYER_STATE) | client→server | `{x, y, z, yaw}` this client's player |
| `otherplayer` (REMOTE_PLAYER) | server→clients | `{pid, x, y, z, yaw}` another client's player |

Client ghost keys: `n:<actorId>` for server actors, `p:<peerId>` for remote players.

## Run (headless, GPU/display-less)

```sh
env -u DISPLAY -u WAYLAND_DISPLAY SDL_VIDEODRIVER=offscreen \
  LD_LIBRARY_PATH=<native-lib> openmw --config <cfg> \
  --data <vfs-mw> --data <Morrowind Data Files> --data <this data dir> \
  --content Morrowind.esm --content mp-server.omwscripts \
  --headless --skip-menu --no-sound --no-grab
```

Clients use `--content mp-client.omwscripts` (add `godmode.omwscripts` for dev).
`vfs-mw` (esmfallbacks GMSTs) is REQUIRED or the built-in control scripts crash.

## Verified

3 instances (server + 2 clients) offscreen: each client sees all server-owned actors
and the other player live; native client AI frozen; native twins hidden. Position
error ~0 (one network tick while moving).

## Scaling / netcode

- **Interest management**: server sends each client only actors within `interestRadius`
  of that client's player; clients drop ghosts that stop refreshing.
- **Delta + keepalive**: server sends an actor only when it moved past `moveThreshold`;
  every `keepaliveEvery` frames it force-sends all in-range actors so idle ghosts survive
  the client's stale-cleanup.
- **Appearance replication**: the avatar clones the LOCAL player's own NPC record as a
  `createRecordDraft` *template* (so it inherits valid health/stats and is alive), then
  overrides race/head/hair/sex from the remote player's `PLAYER_INFO`. Looks like them, alive.
  (A from-scratch `createRecordDraft` has 0 hp → spawns dead; the template avoids that.)
- **Player stats on proxy**: client sends `PLAYER_STATS` (health/level/attributes/skills);
  the server applies them to that player's proxy (self-context, once per proxy life) so NPC/PvP
  combat uses the player's real stats.
- **Equipment**: `PLAYER_INFO` carries `{slot->recordId}`. `util.giveEquipment` (global) creates
  the items + `moveInto` the actor's inventory, then `equip.lua` (self) retries `setEquipment`
  until the queued move lands. Applied to BOTH the server proxy (armor mitigation in combat)
  and the avatar on other clients (visible gear).
- **Interpolation**: incoming transforms are buffered; each ghost is rendered
  `interpDelay` seconds in the past, lerping position *and facing* (yaw, shortest-arc) between
  the bracketing samples, applied via `teleport(pos, {rotation=...})`. Motion is smooth at
  full framerate (verified ~1 unit/frame) and ghosts face their travel direction.
  Broadcast rate is `broadcastEvery` frames (=1 → ~30 Hz, the server fps cap); `interpDelay`
  (0.07 s ≈ 2 ticks) sets how far behind ghosts render. Idle actors are still delta-skipped,
  so 30 Hz costs bandwidth only for things that move.
- **Locomotion animation** (`client/ghostanim.lua`): ghosts are teleported with no movement
  state, so the CharacterController would just idle them. The ghost script infers speed from
  position deltas and drives a scripted `walkforward`/`runforward` (via `playQueued`, which
  overrides the CC); below the walk threshold it clears the queue and lets the CC idle.
  Verified: cliff racer (sp 579) → runforward, scamp (sp 24-67) → walkforward, `isPlaying`
  true. (Tier 1 = locomotion only; attacks/spells/hit-reactions are Tier 2 — replicate the
  server actor's `getActiveGroup` per bone group.)

## Combat authority (PvE)

Co-op trust model (client reports its own hits):
1. `client/ghosthit.lua` (local, all NPC/CREATURE) handles the engine `Hit` event; on a
   successful hit by the local player it `sendGlobalEvent('MP_GhostHit', {ghost, dmg})`.
2. `client/terminal.lua` maps that ghost to its server id and sends `ATTACK {id, dmg}`.
3. `server/authority.lua` routes it to the actor via `MP_Damage` (health writes are
   self-context only, so the global script can't apply damage directly).
4. `server/damage.lua` (local, all NPC/CREATURE) subtracts from `self` health; `<= 0`
   lets the engine resolve death.
5. Health rides along in `ACTOR_STATE.hp`; the client removes a ghost when `hp <= 0`.

Verified (synthetic-hit harness): scamp 45→15→-15 (DEAD), client ghosts 7→4→0.
NPCs don't yet fight the attacking player back (the player isn't a server-side actor).

## Player proxy (bidirectional combat)

The server keeps a hidden **proxy actor** per connected client at that client's player
transform (`updateProxy` in `authority.lua`), so the world has a real body to target/hit:
- Proxies are **excluded from broadcasts** (`isProxy`) — players never see them.
- On `ATTACK`, the hit NPC is sent `AiCombat(proxy)` so it **fights back** (fighters melee
  the proxy; commoners flee — vanilla AI).
- The server **watches each proxy's health**; any drop is relayed as `PLAYER_DAMAGE` to that
  client, where `client/playerdamage.lua` (player ctx) applies it to real player health.
- Proxy that dies is respawned on the next player update.

Verified: proxy excluded from broadcast (no self-ghost); damaging the proxy relayed to the
client and dropped player hp 35→25→15→5→-5. Avatar record for the proxy is the generic
`playerAvatar` (players don't see it, so its looks don't matter).

**PvP** reuses the proxy path: hitting another player's avatar (a `p:`+pid ghost) sends
`PVP_ATTACK {pid, dmg}`; the server damages that player's proxy, and the proxy health-watch
relays it back as `PLAYER_DAMAGE`. Verified server-routed both directions; victim's hp dropped.

**Ghosts/avatars are invulnerable locally** (`ghosthit.lua` pins health to full each frame).
Their real death comes from the server (`hp<=0` → terminal removes them). Without this,
swinging at another player's avatar kills the *local* copy → a corpse on the floor that later
swings miss, while the real player (proxy) is unharmed.

## Shared world

- **Time of day**: server broadcasts `WORLD_TIME` (its `core.getGameTime()`); clients correct
  drift via `world.advanceTime` so day/night is shared + server-authoritative.
- **Roaming (Phase A)**: server keeps a wide cell grid centered on the players' centroid (the
  placeholder player is moved to it), so the world is simulated wherever the group goes. Tunables
  in `config-server/settings.cfg`: `[Cells] exterior grid radius`, `[Game] actors processing range`.
  Players must stay within ~grid-radius cells of each other.

## Known limits / next

- Avatars carry race/sex/head/hair but not **equipment/armor** yet.
- Retaliation depends on the NPC being a fighter; no PvP or crime yet. Proxy doesn't carry
  the player's real stats/equipment, so NPC damage to it isn't player-accurate.
- AI pathing stalls on steep terrain — same as vanilla OpenMW, navmesh-limited.
