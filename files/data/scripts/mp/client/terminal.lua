-- Client terminal: not authoritative. Sends own player transform + appearance, renders
-- server-owned actors and remote players as ghosts. Incoming transforms are buffered and
-- the ghost is rendered cfg.interpDelay in the past, lerping between updates so motion is
-- smooth every frame instead of snapping at the network rate. Native twins are disabled;
-- ghosts that stop refreshing are dropped. AI is frozen by client/suppress.lua.
local world = require('openmw.world')
local net   = require('openmw.network')
local types = require('openmw.types')
local core  = require('openmw.core')          -- game time
local euti  = require('openmw.util')          -- engine util (vector3)
local cfg   = require('scripts.mp.config')
local P     = require('scripts.mp.protocol')
local util  = require('scripts.mp.util')
local n, clock = 0, 0
local ghosts, glist, seen, buf = {}, {}, {}, {}   -- buf[key] = ordered list of {t,x,y,z}
local avatarRec, remotePos = {}, {}
local avatarEquip, avatarEquipped = {}, {}   -- pid -> equipment table / whether applied to the avatar
local ghostAnim = {}                          -- key -> last "la|ua" sent to the ghost
local myAnim = {}                             -- our player's own active groups (from playeranim.lua)
local function isGhost(o) for _, g in ipairs(glist) do if g == o then return true end end return false end
local function forget(o) for i, g in ipairs(glist) do if g == o then table.remove(glist, i); return end end end
local function keyOf(o) for k, g in pairs(ghosts) do if g == o then return k end end end
local function removeGhost(key)
    local g = ghosts[key]; if not g then return end
    pcall(function() g.enabled = false end)
    forget(g); ghosts[key] = nil; seen[key] = nil; buf[key] = nil
end

-- ensure a ghost object exists for key, then append a transform sample to its buffer
local function sample(key, rec, x, y, z, yaw)
    if not ghosts[key] then
        local ok, o = pcall(function() return world.createObject(rec, 1) end)
        if ok then ghosts[key] = o; glist[#glist + 1] = o; buf[key] = {} else return end
    end
    local b = buf[key]
    b[#b + 1] = { t = clock, x = x, y = y, z = z, yaw = yaw or 0 }
    seen[key] = n
end

-- interpolate an angle along the shortest arc (handles +/-pi wraparound)
local function lerpAngle(a0, a1, t)
    local d = (a1 - a0) % (2 * math.pi)
    if d > math.pi then d = d - 2 * math.pi end
    return a0 + d * t
end

-- render every ghost at (clock - interpDelay), lerping position + facing between samples
local function interpolate()
    local renderT = clock - cfg.interpDelay
    for key, g in pairs(ghosts) do
        local b = buf[key]
        if b and #b > 0 then
            while #b > 2 and b[2].t <= renderT do table.remove(b, 1) end   -- prune consumed samples
            local s0, s1
            for i = 1, #b do
                if b[i].t <= renderT then s0 = b[i] else s1 = b[i]; break end
            end
            local x, y, z, yaw
            if s0 and s1 then
                local a = (renderT - s0.t) / math.max(s1.t - s0.t, 1e-5)
                x = s0.x + (s1.x - s0.x) * a; y = s0.y + (s1.y - s0.y) * a; z = s0.z + (s1.z - s0.z) * a
                yaw = lerpAngle(s0.yaw, s1.yaw, a)
            else
                local s = s0 or s1 or b[#b]; x, y, z, yaw = s.x, s.y, s.z, s.yaw
            end
            pcall(function() g:teleport('', euti.vector3(x, y, z), { rotation = euti.transform.rotateZ(yaw) }) end)
        end
    end
end

-- Build a visual avatar that LOOKS like the remote player (race/head/hair/sex) but is ALIVE.
-- Trick: clone the LOCAL player's own record as a template (so it inherits valid health/stats
-- -> not a 0-hp corpse), then override the appearance fields. The avatar's stats are
-- irrelevant (it's a health-pinned visual puppet; combat goes through the server proxy).
local function buildAvatar(d)
    local ok, recId = pcall(function()
        local base = types.NPC.record(util.getPlayer())   -- a known-valid (alive) NPC record
        local draft = types.NPC.createRecordDraft({
            template = base,
            name = d.name, race = d.race, head = d.head, hair = d.hair, isMale = d.isMale })
        return world.createRecord(draft).id
    end)
    if ok then avatarRec[d.pid] = recId; print('[MP] built avatar for peer ' .. d.pid .. ' race=' .. tostring(d.race))
    else print('[MP] avatar build FAILED for peer ' .. d.pid .. ': ' .. tostring(recId)) end
end

return { engineHandlers = { onUpdate = function(dt)
    n = n + 1; clock = clock + dt
    if #net.peers() == 0 and n % 60 == 1 then net.connect(cfg.host, cfg.port) end
    local pl = util.getPlayer(); if not pl then return end
    local p = pl.position
    -- myAnim is fed by client/playeranim.lua (PLAYER ctx; openmw.animation is local-only)
    net.send(0, P.PLAYER_STATE, { x = p.x, y = p.y, z = p.z, yaw = util.yaw(pl), la = myAnim.la, ua = myAnim.ua })
    if n % 90 == 1 then local a = util.appearance(pl); if a then net.send(0, P.PLAYER_INFO, a) end end
    if n % 90 == 5 then local s = util.playerStats(pl); if s then net.send(0, P.PLAYER_STATS, s) end end

    for _, m in ipairs(net.poll()) do
        local d = m.data
        if m.event == P.ACTOR_STATE then
            if d.hp and d.hp <= 0 then removeGhost('n:' .. d.id)   -- server says this actor is dead
            else
                local key = 'n:' .. d.id
                sample(key, d.rec, d.x, d.y, d.z, d.yaw)
                local ak = (d.la or '') .. '|' .. (d.ua or '') .. '|' .. tostring(d.st)   -- groups + draw stance
                local g = ghosts[key]
                -- forward on change, or every frame while a swing playhead (wt) is streaming
                if g and d.la and (ak ~= ghostAnim[key] or d.wt) then ghostAnim[key] = ak; pcall(function() g:sendEvent('MP_Anim', { la = d.la, ua = d.ua, st = d.st, wt = d.wt }) end) end
            end
        elseif m.event == P.REMOTE_PLAYER then remotePos[d.pid] = { x = d.x, y = d.y, z = d.z, yaw = d.yaw, la = d.la, ua = d.ua }
        elseif m.event == P.REMOTE_INFO then
            if not avatarRec[d.pid] then
                buildAvatar(d)
                -- the ghost may already exist as the fargoth fallback (REMOTE_PLAYER arrived
                -- before this INFO); drop it so it respawns with the real appearance + gear
                if avatarRec[d.pid] and ghosts['p:' .. d.pid] then
                    removeGhost('p:' .. d.pid); avatarEquipped[d.pid] = nil
                end
            end
            avatarEquip[d.pid] = d.equip
        elseif m.event == P.PLAYER_DAMAGE then pl:sendEvent('MP_PlayerDamage', { dmg = d.dmg })   -- health write is self-context
        elseif m.event == P.PLAYER_BOUNTY then pl:sendEvent('MP_SetBounty', { amount = d.amount })  -- setCrimeLevel is self-context
        elseif m.event == P.ARREST then pl:sendEvent('MP_Arrest', {})
        elseif m.event == P.WORLD_TIME then
            local delta = (d.t - core.getGameTime()) / 3600   -- advanceTime takes hours
            if math.abs(delta) > 1 / 60 then pcall(function() world.advanceTime(delta) end) end   -- correct drift > 1 game-min
        end
    end
    for pid, rp in pairs(remotePos) do
        -- appearance avatar (cloned from our own record + their race/head/hair) once built;
        -- fargoth fallback until their PLAYER_INFO arrives
        local akey = 'p:' .. pid
        sample(akey, avatarRec[pid] or cfg.playerAvatar, rp.x, rp.y, rp.z, rp.yaw)
        local g = ghosts[akey]   -- once spawned, dress it in their gear (once)
        if g and avatarEquip[pid] and not avatarEquipped[pid] then util.giveEquipment(g, avatarEquip[pid]); avatarEquipped[pid] = true end
        local ak = (rp.la or '') .. '|' .. (rp.ua or '')   -- replicate the remote player's own anim groups
        if g and rp.la and ak ~= ghostAnim[akey] then ghostAnim[akey] = ak; pcall(function() g:sendEvent('MP_Anim', { la = rp.la, ua = rp.ua }) end) end
        remotePos[pid] = nil      -- consumed into the buffer
    end

    interpolate()

    if n % 30 == 0 then
        for key, g in pairs(ghosts) do   -- drop ghosts gone stale (out of the server's interest set)
            if n - (seen[key] or 0) > cfg.ghostTimeoutFrames then removeGhost(key) end
        end
    end
    if n % cfg.hideNativeEvery == 0 then
        for _, a in ipairs(world.activeActors) do
            if a ~= pl and not isGhost(a) and a.enabled then pcall(function() a.enabled = false end) end
        end
    end
    if n % 60 == 0 then
        local npc, ply = 0, 0
        for k, _ in pairs(ghosts) do if k:sub(1,2) == 'n:' then npc = npc + 1 else ply = ply + 1 end end
        print(string.format('[MP] terminal: npcGhosts=%d playerGhosts=%d peers=%d', npc, ply, #net.peers()))
    end
end },
-- the player hit a ghost (reported by client/ghosthit.lua) -> tell the server which
-- server-owned actor was hit and for how much; the server applies authoritative damage.
eventHandlers = {
    MP_GhostHit = function(e)
        local key = keyOf(e.ghost)
        if not key then return end
        if key:sub(1, 2) == 'n:' then net.send(0, P.ATTACK, { id = key:sub(3), dmg = e.dmg })          -- hit an NPC
        elseif key:sub(1, 2) == 'p:' then net.send(0, P.PVP_ATTACK, { pid = tonumber(key:sub(3)), dmg = e.dmg }) end  -- hit another player
    end,
    -- playeranim.lua (player ctx) reports our own active anim groups; relayed up in PLAYER_STATE
    MP_MyAnim = function(e) myAnim = e end,
    -- death.lua (player ctx) revived us; relocate to the respawn point (teleport is global)
    MP_Respawn = function()
        local p = util.getPlayer()
        if p then pcall(function() p:teleport(cfg.spawnCell, euti.vector3(cfg.spawn.x, cfg.spawn.y, cfg.spawn.z)) end) end
    end,
} }
