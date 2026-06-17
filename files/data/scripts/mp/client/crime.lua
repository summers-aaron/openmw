-- PLAYER context. The server is authoritative for crime: it tracks this player's bounty and
-- pushes it down (PLAYER_BOUNTY -> terminal -> MP_SetBounty). Apply it to the real player so the
-- native bounty HUD shows it. setCrimeLevel modifies the player, so it needs the self handle.
local self  = require('openmw.self')
local types = require('openmw.types')
local ui    = require('openmw.ui')

return { eventHandlers = {
    MP_SetBounty = function(e)
        pcall(function() types.Player.setCrimeLevel(self, e.amount or 0) end)
    end,
    -- a guard reached the player to arrest. No dialogue/pay-off system yet, so just warn; the
    -- server keeps the guards in non-violent Follow until the player resists (strikes a guard).
    MP_Arrest = function()
        pcall(function() ui.showMessage('Stop, criminal! Surrender, or resist and face the guards.') end)
    end,
} }
