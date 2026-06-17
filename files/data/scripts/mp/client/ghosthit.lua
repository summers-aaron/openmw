-- Attached to every actor on the client. On a successful hit by the LOCAL player, report
-- the damage up to the global terminal, which forwards it to the authoritative server.
-- (Ghosts are server-owned; the server resolves real damage/death.)
--
-- Ghosts/avatars are PUPPETS: their real death comes from the server (hp<=0 -> the terminal
-- removes them). Locally they must never die, or you get spurious corpses (e.g. swinging at
-- another player's avatar kills the local copy, so further hits miss the body on the floor).
-- So we pin local health to full every frame; the server stays authoritative.
local self  = require('openmw.self')
local core  = require('openmw.core')
local types = require('openmw.types')
return {
    engineHandlers = { onUpdate = function()
        local h = types.Actor.stats.dynamic.health(self)
        if h.current < h.base then h.current = h.base end
    end },
    eventHandlers = { Hit = function(data)
        if not data or data.successful == false then return end
        local atk = data.attacker
        if not (atk and types.Player.objectIsInstance(atk)) then return end
        -- Report every successful hit, even 0 health damage: hand-to-hand drains fatigue first
        -- (health stays full), but the blow still lands -> it must count as a crime / contact.
        local dmg = (data.damage and data.damage.health) or 0
        core.sendGlobalEvent('MP_GhostHit', { ghost = self.object, dmg = dmg })
    end },
}
