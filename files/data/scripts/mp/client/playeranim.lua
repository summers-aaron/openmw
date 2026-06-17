-- PLAYER context. openmw.animation is local-only, so the global terminal can't read the
-- player's own active groups. Read them here and forward to terminal.lua (global), which
-- ships them up in PLAYER_STATE so remote clients animate our avatar faithfully.
local self = require('openmw.self')
local anim = require('openmw.animation')
local core = require('openmw.core')
local last

return { engineHandlers = { onUpdate = function()
    local lo = anim.getActiveGroup(self.object, anim.BONE_GROUP.LowerBody)
    local up = anim.getActiveGroup(self.object, anim.BONE_GROUP.Torso)
    local key = (lo or '') .. '|' .. (up or '')
    if key ~= last then
        last = key
        core.sendGlobalEvent('MP_MyAnim', { la = lo, ua = up })
    end
end } }
