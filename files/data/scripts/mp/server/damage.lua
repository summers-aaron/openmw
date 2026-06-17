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
        -- isolate each group: a failure in one must not skip the others
        if s.health then pcall(function() local h = types.Actor.stats.dynamic.health(self); h.base = s.health; h.current = s.health end) end
        if s.level then pcall(function() types.Actor.stats.level(self).current = s.level end) end
        local na, nk = 0, 0
        for id, v in pairs(s.attr or {}) do if pcall(function() types.Actor.stats.attributes[id](self).base = v end) then na = na + 1 end end
        for id, v in pairs(s.skill or {}) do if pcall(function() types.NPC.stats.skills[id](self).base = v end) then nk = nk + 1 end end
        print(string.format('[MP] %s stats applied: hp=%s lvl=%s attrs=%d skills=%d', self.object.recordId, tostring(s.health), tostring(s.level), na, nk))
    end,
} }
