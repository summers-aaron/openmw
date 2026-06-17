-- PLAYER-context: apply damage the server's world dealt to this player (relayed via our
-- server-side proxy) and reproduce the hit feedback locally. The hit lands on the server
-- (the proxy), so the client only gets a number -- writing health.current directly skips
-- the engine's hit reaction. Re-create it here: recoil animation, impact sound, red flash.
local self    = require('openmw.self')
local types   = require('openmw.types')
local anim    = require('openmw.animation')
local ambient = require('openmw.ambient')
local ui      = require('openmw.ui')
local util    = require('openmw.util')

-- red full-screen flash, faded out in onUpdate (the engine's hit fader isn't Lua-exposed)
local flashTex = ui.texture{ path = 'scripts/mp/mp_white.png' }
local flash, flashEl = 0, nil
local function showFlash()
    flash = 0.5
    if not flashEl then
        flashEl = ui.create{
            layer = 'HUD',
            type = ui.TYPE.Image,
            props = {
                relativeSize = util.vector2(1, 1),
                resource = flashTex,
                color = util.color.rgb(0.7, 0, 0),
                alpha = flash,
            },
        }
    else
        flashEl.layout.props.alpha = flash; flashEl:update()
    end
end

-- brief full-body hit recovery (the engine picks a random "hitN"; mirror that)
local function recoil()
    local groups = {}
    for i = 1, 9 do if anim.hasGroup(self.object, 'hit' .. i) then groups[#groups + 1] = 'hit' .. i end end
    if #groups == 0 then return end
    local g = groups[math.random(#groups)]
    pcall(function()
        anim.playBlended(self, g, { priority = anim.PRIORITY.Hit, blendMask = anim.BLEND_MASK.All,
            loops = 0, startKey = 'start', stopKey = 'stop' })
    end)
end

return {
    engineHandlers = { onUpdate = function(dt)
        if flash > 0 and flashEl then
            flash = flash - dt * 1.8
            if flash < 0 then flash = 0 end
            flashEl.layout.props.alpha = flash; flashEl:update()
        end
    end },
    eventHandlers = { MP_PlayerDamage = function(e)
        local h = types.Actor.stats.dynamic.health(self)
        h.current = h.current - (e.dmg or 0)
        recoil()
        pcall(function() ambient.playSound('Health Damage') end)
        showFlash()
        print(string.format('[MP] player took %.0f damage -> hp %.0f', e.dmg or 0, h.current))
    end } }
