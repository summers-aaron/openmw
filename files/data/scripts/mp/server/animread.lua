-- Server-side, on every actor. Reads the actor's active animation groups (lower + upper body)
-- and reports them to the authority on change. getActiveGroup is self-context, so this local
-- script does the read; the authority puts them in ACTOR_STATE and clients replay them.
local self = require('openmw.self')
local anim = require('openmw.animation')
local core = require('openmw.core')
local last
return { engineHandlers = { onUpdate = function(dt)
    local lo = anim.getActiveGroup(self.object, anim.BONE_GROUP.LowerBody)
    local up = anim.getActiveGroup(self.object, anim.BONE_GROUP.Torso)
    local key = lo .. '|' .. up
    if key ~= last then
        last = key
        core.sendGlobalEvent('MP_ActorAnim', { id = tostring(self.object.id), la = lo, ua = up })
    end
end } }
