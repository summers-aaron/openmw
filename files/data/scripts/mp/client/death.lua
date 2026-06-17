-- PLAYER-context: on death (health <= 0), heal to full (prevents the engine game-over screen)
-- and ask the global script to teleport us to the respawn point. Health writes are self-context;
-- teleport is global, so we route the relocation through MP_Respawn.
local self = require('openmw.self')
local types = require('openmw.types')
local core  = require('openmw.core')
local dead = false
return { engineHandlers = { onUpdate = function(dt)
    local h = types.Actor.stats.dynamic.health(self)
    if h.current <= 0 and not dead then
        dead = true
        h.current = h.base               -- revive + full health (no game-over)
        core.sendGlobalEvent('MP_Respawn', {})
        print('[MP] player died -> respawning')
    elseif h.current > 0 then
        dead = false
    end
end } }
