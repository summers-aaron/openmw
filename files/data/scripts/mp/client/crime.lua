-- PLAYER context. The server is authoritative for crime: it tracks this player's bounty and
-- pushes it down (PLAYER_BOUNTY -> terminal -> MP_SetBounty). Apply it to the real player so the
-- native bounty HUD shows it. setCrimeLevel modifies the player, so it needs the self handle.
local self  = require('openmw.self')
local types = require('openmw.types')

return { eventHandlers = {
    MP_SetBounty = function(e)
        pcall(function() types.Player.setCrimeLevel(self, e.amount or 0) end)
    end,
} }
