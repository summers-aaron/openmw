-- PLAYER-context: apply damage the server's world dealt to this player (relayed via our
-- server-side proxy). Health writes require self-context, so the global terminal forwards
-- PLAYER_DAMAGE here as an MP_PlayerDamage event.
local self  = require('openmw.self')
local types = require('openmw.types')
return { eventHandlers = { MP_PlayerDamage = function(e)
    local h = types.Actor.stats.dynamic.health(self)
    h.current = h.current - (e.dmg or 0)
    print(string.format('[MP] player took %.0f damage -> hp %.0f', e.dmg or 0, h.current))
end } }
