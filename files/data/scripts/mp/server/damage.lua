-- Server-side, attached to every actor. Applies authoritative damage to SELF when the
-- authority script routes a hit here (health/stat writes require self-context). Also applies
-- a player's replicated stats to its proxy so combat against it is accurate.
local self  = require('openmw.self')
local types = require('openmw.types')
return { eventHandlers = {
    MP_Damage = function(e)
        local h = types.Actor.stats.dynamic.health(self)
        local before = h.current
        h.current = before - (e.dmg or 0)
        print(string.format('[MP] %s hp %.0f -> %.0f%s', self.object.recordId, before, h.current, h.current <= 0 and ' (DEAD)' or ''))
    end,
    MP_SetStats = function(s)
        -- The proxy is a durable damage-relay target, not a real combatant: give it a large
        -- health pool so NPC damage relays to the real player (client-side) without the proxy
        -- dying and dropping the attackers' aggro. Other stats stay real so the engine's hit
        -- chance / damage against the proxy match the real player.
        pcall(function() local h = types.Actor.stats.dynamic.health(self); h.base = 100000; h.current = 100000 end)
        if s.level then pcall(function() types.Actor.stats.level(self).current = s.level end) end
        local na, nk = 0, 0
        for id, v in pairs(s.attr or {}) do if pcall(function() types.Actor.stats.attributes[id](self).base = v end) then na = na + 1 end end
        for id, v in pairs(s.skill or {}) do if pcall(function() types.NPC.stats.skills[id](self).base = v end) then nk = nk + 1 end end
        print(string.format('[MP] %s stats applied: hp=%s lvl=%s attrs=%d skills=%d', self.object.recordId, tostring(s.health), tostring(s.level), na, nk))
    end,
} }
