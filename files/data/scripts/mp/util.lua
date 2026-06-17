local world = require('openmw.world')
local types = require('openmw.types')
local core  = require('openmw.core')
local M = {}
function M.getPlayer() for _, p in ipairs(world.players) do return p end end
function M.yaw(a) local ok, y = pcall(function() return a.rotation:getYaw() end); return ok and y or 0 end
-- Snapshot an actor's NPC appearance (all string-serialized, network-safe).
function M.appearance(p)
    local ok, r = pcall(function() return types.NPC.record(p) end)
    if not ok or not r then return nil end
    return { name = r.name, race = r.race, class = r.class, head = r.head, hair = r.hair, isMale = r.isMale,
             equip = M.playerEquipment(p) }
end
-- Snapshot a player's equipped items as { slotInt -> recordId } for replication.
function M.playerEquipment(p)
    local ok, eq = pcall(function()
        local out = {}
        for slot, item in pairs(types.Actor.getEquipment(p)) do out[slot] = item.recordId end
        return out
    end)
    return ok and eq or nil
end
-- GLOBAL helper: ensure an actor carries `eq` ({slot->recordId}) in its inventory, then ask
-- it (self-context) to equip them. createObject/moveInto are global; setEquipment is self, so
-- the actor's local equip.lua retries setEquipment until the queued moveInto lands.
function M.giveEquipment(actor, eq)
    if not eq then return end
    pcall(function()
        local inv = types.Actor.inventory(actor)
        for _, recId in pairs(eq) do
            if not inv:find(recId) then world.createObject(recId, 1):moveInto(actor) end
        end
        actor:sendEvent('MP_Equip', eq)
    end)
end
-- Snapshot a player's combat stats (base values) for replication onto its server proxy.
function M.playerStats(p)
    local ok, s = pcall(function()
        local st = { health = types.Actor.stats.dynamic.health(p).base,
                     level = types.Actor.stats.level(p).current, attr = {}, skill = {} }
        for _, a in ipairs(core.stats.Attribute.records) do st.attr[a.id] = types.Actor.stats.attributes[a.id](p).base end
        for _, sk in ipairs(core.stats.Skill.records) do st.skill[sk.id] = types.NPC.stats.skills[sk.id](p).base end
        return st
    end)
    return ok and s or nil
end
return M
