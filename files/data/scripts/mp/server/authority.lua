-- Server authority: the sim runs here (native + scripted AI). Each tick we send each
-- client only the actors near ITS player (interest), only those that changed (delta) with
-- a periodic keepalive, plus each actor's health. Clients report attacks; the server
-- applies authoritative damage/death. Player transforms are relayed peer-to-peer.
local world = require('openmw.world')
local net   = require('openmw.network')
local types = require('openmw.types')
local core  = require('openmw.core')          -- game time
local euti  = require('openmw.util')          -- vector3 / transform
local cfg   = require('scripts.mp.config')
local P     = require('scripts.mp.protocol')
local util  = require('scripts.mp.util')
local n, started, clock = 0, false, 0
local lastSeen  = {}   -- peer -> clock (s) of its last PLAYER_STATE; stale => disconnected
local playerPos = {}   -- peer -> last known {x, y, z}
local lastSent  = {}   -- peer -> { actorId -> {x, y, z, yaw, hp} }
local proxies   = {}   -- peer -> proxy actor (a server-side body for the connected player)
local proxyHp   = {}   -- peer -> proxy's last health (drop => relay damage to that client)
local pstats    = {}   -- peer -> last player stats (applied to the proxy so combat is accurate)
local pstatted  = {}   -- peer -> whether the live proxy already had stats applied (apply once/life)
local pequip    = {}   -- peer -> last equipment ({slot->recordId}) to put on the proxy
local pinfo     = {}   -- peer -> last REMOTE_INFO payload (cached so new joiners get it at once)
local aanim     = {}   -- actorId -> {la, ua} active animation groups (reported by animread.lua)
local aggroed   = {}   -- actorId -> true: guard already sent into combat (don't re-issue every tick)
local function hpOf(a) local ok, h = pcall(function() return types.Actor.stats.dynamic.health(a).current end); return ok and h or 0 end
local function findActor(id)
    for _, a in ipairs(world.activeActors) do if tostring(a.id) == id then return a end end
end
local function isProxy(o) for _, px in pairs(proxies) do if px == o then return true end end return false end
-- keep a server-side proxy actor at the client's reported player transform, and relay any
-- health it loses (NPCs hitting it) back to that client. Players never see proxies (they're
-- excluded from broadcasts); the proxy exists so the server's world can target/hit the player.
local function updateProxy(peer, x, y, z, yaw)
    local px, created = proxies[peer], false
    if not px or hpOf(px) <= 0 then
        if px then pcall(function() px.enabled = false end) end
        local ok, o = pcall(function() return world.createObject(cfg.playerAvatar, 1) end)
        if not ok then return end
        proxies[peer] = o; px = o; proxyHp[peer] = nil; created = true; pstatted[peer] = false
    end
    -- A fresh proxy starts with the placeholder body's (low) health; pin its durable health pool
    -- immediately (MP_SetStats forces a large pool) so it can't be killed before the player's real
    -- stats arrive. Only mark stats "applied" once the real stats are actually present.
    if created then
        px:sendEvent('MP_SetStats', pstats[peer] or {})
        if pstats[peer] then pstatted[peer] = true end
        if pequip[peer] then util.giveEquipment(px, pequip[peer]) end
    end
    pcall(function() px:teleport('', euti.vector3(x, y, z), { rotation = euti.transform.rotateZ(yaw or 0) }) end)
    local hp = hpOf(px)
    if proxyHp[peer] and hp < proxyHp[peer] then
        net.send(peer, P.PLAYER_DAMAGE, { dmg = proxyHp[peer] - hp })
        print(string.format('[MP] proxy(peer %d) took %.0f -> relay to client', peer, proxyHp[peer] - hp))
    end
    proxyHp[peer] = hp
end
local function invalidate(id) for _, c in pairs(lastSent) do c[id] = nil end end   -- force resend (e.g. after damage)
local function changed(prev, x, y, z, yaw, hp)
    if not prev then return true end
    local dx, dy, dz = x - prev.x, y - prev.y, z - prev.z
    if dx * dx + dy * dy + dz * dz > cfg.moveThreshold * cfg.moveThreshold then return true end
    if math.abs(yaw - prev.yaw) > 0.05 then return true end
    return hp ~= prev.hp
end
return { engineHandlers = { onUpdate = function(dt)
    n = n + 1; clock = clock + dt
    if not started then net.listen(cfg.port); started = true; print('[MP] server listening :' .. cfg.port) end
    local pl = util.getPlayer()

    if n % 150 == 0 then net.broadcast(P.WORLD_TIME, { t = core.getGameTime() }) end   -- sync day/night

    -- The placeholder player is the active-cell grid center. Keep it at the centroid of all
    -- connected players so the simulated area (a big grid, see cfg/settings) follows them as
    -- they roam. Only teleport when it drifts past a threshold to avoid cell-reload churn.
    if pl and n % 30 == 0 then
        local cx, cy, cz, cnt = 0, 0, 0, 0
        for _, pp in pairs(playerPos) do cx = cx + pp.x; cy = cy + pp.y; cz = cz + pp.z; cnt = cnt + 1 end
        if cnt > 0 then
            cx, cy, cz = cx / cnt, cy / cnt, cz / cnt
            local pp = pl.position
            if (euti.vector3(cx, cy, cz) - euti.vector3(pp.x, pp.y, pp.z)):length() > 4096 then
                pcall(function() pl:teleport('', euti.vector3(cx, cy, cz)) end)
            end
        end
    end

    -- Reap disconnected peers: free the proxy body + every per-peer table. The transport does
    -- NOT reliably drop a peer when its client dies (a half-open TCP socket lingers in
    -- net.peers()), so we key off liveness instead: clients send PLAYER_STATE every frame, so
    -- no update for cfg-many seconds means gone. Without this a dropped client leaves an
    -- invisible proxy actor in the world (NPCs keep targeting it) and the peer tables leak.
    if n % 30 == 0 then
        for peer, t in pairs(lastSeen) do
            if clock - t > 5 then
                local px = proxies[peer]
                if px then pcall(function() px.enabled = false end) end
                proxies[peer], proxyHp[peer], playerPos[peer] = nil, nil, nil
                lastSent[peer], pstats[peer], pstatted[peer] = nil, nil, nil
                pequip[peer], pinfo[peer], lastSeen[peer] = nil, nil, nil
                print('[MP] reaped disconnected peer ' .. peer)
            end
        end
    end

    for _, m in ipairs(net.poll()) do
        if m.event == P.PLAYER_STATE then
            local isNew = playerPos[m.peer] == nil
            lastSeen[m.peer] = clock
            playerPos[m.peer] = { x = m.data.x, y = m.data.y, z = m.data.z }
            updateProxy(m.peer, m.data.x, m.data.y, m.data.z, m.data.yaw)
            if isNew then   -- a fresh joiner: hand it everyone's cached appearance up front (no fargoth flicker)
                for opeer, info in pairs(pinfo) do if opeer ~= m.peer then net.send(m.peer, P.REMOTE_INFO, info) end end
            end
            for _, peer in ipairs(net.peers()) do
                if peer ~= m.peer then
                    net.send(peer, P.REMOTE_PLAYER, { pid = m.peer, x = m.data.x, y = m.data.y, z = m.data.z, yaw = m.data.yaw, la = m.data.la, ua = m.data.ua })
                end
            end
        elseif m.event == P.PLAYER_INFO then
            local d = m.data
            pequip[m.peer] = d.equip                            -- store for the proxy
            local px = proxies[m.peer]; if px then util.giveEquipment(px, d.equip) end
            local info = { pid = m.peer, name = d.name, race = d.race, class = d.class, head = d.head, hair = d.hair, isMale = d.isMale, equip = d.equip }
            pinfo[m.peer] = info                                -- cache for new joiners
            for _, peer in ipairs(net.peers()) do
                if peer ~= m.peer then net.send(peer, P.REMOTE_INFO, info) end
            end
        elseif m.event == P.ATTACK then
            -- health writes are self-context only; route damage to the actor's local script
            local a = findActor(m.data.id)
            if a then
                a:sendEvent('MP_Damage', { dmg = m.data.dmg or 0 })
                invalidate(m.data.id)
                local px = proxies[m.peer]                 -- make it fight back at the attacker's proxy
                if px then a:sendEvent('StartAIPackage', { type = 'Combat', target = px }) end
                print(string.format('[MP] ATTACK -> %s for %.0f%s', a.recordId, m.data.dmg or 0, px and ' (retaliating)' or ''))
            end
        elseif m.event == P.PLAYER_STATS then
            pstats[m.peer] = m.data                       -- store for the (next) proxy
            local px = proxies[m.peer]                    -- apply to the live proxy once (don't re-reset its health)
            if px and not pstatted[m.peer] then px:sendEvent('MP_SetStats', m.data); pstatted[m.peer] = true end
        elseif m.event == P.PVP_ATTACK then
            -- damage the target player's proxy; updateProxy's health-watch relays it to that client
            local px = proxies[m.data.pid]
            if px then
                px:sendEvent('MP_Damage', { dmg = m.data.dmg or 0 })
                print(string.format('[MP] PVP: peer %d hit peer %d for %.0f', m.peer, m.data.pid, m.data.dmg or 0))
            end
        end
    end

    -- TEST hook (no crime system yet): force nearby guards to attack each player's proxy, so the
    -- NPC->player combat path (pursue, hit proxy, relay PLAYER_DAMAGE) can be exercised in the open.
    if cfg.aggroGuards and n % 60 == 0 then
        for peer, px in pairs(proxies) do
            if px and hpOf(px) > 0 then
                local pp = px.position
                for _, a in ipairs(world.activeActors) do
                    if a ~= pl and not isProxy(a) and not aggroed[tostring(a.id)] then
                        local ok, cls = pcall(function() return types.NPC.record(a).class end)
                        if ok and cls and tostring(cls):lower():find('guard') then
                            if (a.position - pp):length() < cfg.aggroRadius then
                                a:sendEvent('StartAIPackage', { type = 'Combat', target = px })
                                aggroed[tostring(a.id)] = true
                                print('[MP] guard ' .. a.recordId .. ' aggro -> peer ' .. peer)
                            end
                        end
                    end
                end
            end
        end
    end

    if n % cfg.broadcastEvery == 0 then
        local r2 = cfg.interestRadius * cfg.interestRadius
        local keepalive = (n % cfg.keepaliveEvery == 0)
        for _, peer in ipairs(net.peers()) do
            local pp = playerPos[peer]
            if pp then
                lastSent[peer] = lastSent[peer] or {}
                local cache = lastSent[peer]
                local inRange, sent = 0, 0
                for _, a in ipairs(world.activeActors) do
                    if a ~= pl and not isProxy(a) then
                        local p = a.position
                        local dx, dy = p.x - pp.x, p.y - pp.y
                        if dx * dx + dy * dy <= r2 then
                            inRange = inRange + 1
                            local id = tostring(a.id)
                            local yaw, hp = util.yaw(a), hpOf(a)
                            if keepalive or changed(cache[id], p.x, p.y, p.z, yaw, hp) then
                                local ag = aanim[id]
                                net.send(peer, P.ACTOR_STATE, { id = id, rec = a.recordId, x = p.x, y = p.y, z = p.z, yaw = yaw, hp = hp, la = ag and ag.la, ua = ag and ag.ua, st = ag and ag.st, wt = ag and ag.wt })
                                cache[id] = { x = p.x, y = p.y, z = p.z, yaw = yaw, hp = hp }
                                sent = sent + 1
                            end
                        end
                    end
                end
                if n % 30 == 0 then print(string.format('[MP] -> peer %d: sent %d / %d in range%s', peer, sent, inRange, keepalive and ' (keepalive)' or '')) end
            end
        end
    end
end },
-- actor animation groups, reported by server/animread.lua (getActiveGroup is self-context)
eventHandlers = {
    MP_ActorAnim = function(e)
        aanim[e.id] = { la = e.la, ua = e.ua, st = e.st, wt = e.wt }
        invalidate(e.id)   -- force the next ACTOR_STATE to carry the new groups/stance (wt streams mid-swing)
    end,
} }
