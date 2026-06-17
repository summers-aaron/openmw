-- Server-side, on every actor. Reads the actor's active animation groups (lower + upper body)
-- and draw stance, reporting them to the authority on change. getActiveGroup/getStance are
-- self-context, so this local script does the read; the authority puts them in ACTOR_STATE and
-- clients replay them (stance draws the weapon so armed NPCs look armed).
local self  = require('openmw.self')
local anim  = require('openmw.animation')
local core  = require('openmw.core')
local types = require('openmw.types')
local last
return { engineHandlers = { onUpdate = function(dt)
    local lo = anim.getActiveGroup(self.object, anim.BONE_GROUP.LowerBody)
    local up = anim.getActiveGroup(self.object, anim.BONE_GROUP.Torso)
    local st = types.Actor.getStance(self.object)
    local key = lo .. '|' .. up .. '|' .. st
    if key ~= last then
        last = key
        core.sendGlobalEvent('MP_ActorAnim', { id = tostring(self.object.id), la = lo, ua = up, st = st })
    end
end } }
