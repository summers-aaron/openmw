-- Server-side, on every actor. Reads the actor's active animation groups (lower + upper body)
-- and draw stance, reporting them to the authority on change. getActiveGroup/getStance are
-- self-context, so this local script does the read; the authority puts them in ACTOR_STATE and
-- clients replay them (stance draws the weapon so armed NPCs look armed).
local self  = require('openmw.self')
local anim  = require('openmw.animation')
local core  = require('openmw.core')
local types = require('openmw.types')
-- Weapon/attack groups: the Torso group is one of these ONLY during an actual swing/cast (the
-- ready stance and movement use the idle1h/walkforward1h variants instead). So when the upper
-- group is one of these we stream the playhead time (wt) and the ghost overlays the swing.
local ATTACK = {
    weapononehand = true, weapontwohand = true, weapontwowide = true,
    bowandarrow = true, crossbow = true, throwweapon = true, handtohand = true, spellcast = true,
}
local last
return { engineHandlers = { onUpdate = function(dt)
    local lo = anim.getActiveGroup(self.object, anim.BONE_GROUP.LowerBody)
    local up = anim.getActiveGroup(self.object, anim.BONE_GROUP.Torso)
    local st = types.Actor.getStance(self.object)
    local wt
    if ATTACK[up] then wt = anim.getCurrentTime(self.object, up) end   -- mid-swing: stream the playhead
    local key = lo .. '|' .. up .. '|' .. st
    if key ~= last or wt then
        last = key
        core.sendGlobalEvent('MP_ActorAnim', { id = tostring(self.object.id), la = lo, ua = up, st = st, wt = wt })
    end
end } }
