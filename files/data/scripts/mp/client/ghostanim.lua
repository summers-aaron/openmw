-- Client-side, on every ghost. Two modes:
--  * REPLICATION (default once MP_Anim arrives): the source reports its active LOWER-body
--    (locomotion) group; we play that group FULL-BODY so the gait + arm swing are coordinated.
--    We deliberately do NOT pin the upper body to a separate replicated group — doing that
--    froze the arms to whatever torso group was active (usually 'idle' = hands on hips) even
--    while walking. Faithful upper-body-only anims (casts/attacks) are intentionally dropped
--    here; they need a non-destructive overlay (TODO), not a forced upper-body pin.
--  * VELOCITY fallback: until the first MP_Anim, infer a gait from position deltas.
local self = require('openmw.self')
local anim = require('openmw.animation')
local types = require('openmw.types')

local replicated, rgroup, rst = false, nil, nil
local function play(group)
    if not group or group == '' or group == rgroup then return end
    rgroup = group
    pcall(function()
        if anim.hasGroup(self.object, group) then
            anim.playBlended(self, group,
                { loops = 1073741824, priority = anim.PRIORITY.Movement, forceLoop = true })   -- BlendMask_All
        end
    end)
end

-- ---- velocity fallback (pre-first-packet) ----
local prev, cur, smoothed = nil, nil, 0
local SPIKE, WALK_IN, WALK_OUT, RUN_IN, RUN_OUT = 600, 30, 12, 210, 120
local function gait(s, c)
    if c == 'runforward' then
        if s < RUN_OUT then return s > WALK_OUT and 'walkforward' or 'idle' end
        return 'runforward'
    elseif c == 'walkforward' then
        if s > RUN_IN then return 'runforward' end
        if s < WALK_OUT then return 'idle' end
        return 'walkforward'
    else
        if s > RUN_IN then return 'runforward' end
        if s > WALK_IN then return 'walkforward' end
        return 'idle'
    end
end
local function setGroup(g)
    if g == cur then return end
    pcall(function() anim.clearAnimationQueue(self, true) end)
    if g ~= 'idle' then
        local ok, has = pcall(function() return anim.hasGroup(self.object, g) end)
        if ok and has then pcall(function() anim.playQueued(self, g, { loops = 1000000, forceLoop = true }) end) end
    end
    cur = g
end

return {
    engineHandlers = { onUpdate = function(dt)
        if replicated or dt <= 0 then return end   -- driven by MP_Anim once it arrives
        local p = self.object.position
        if prev then
            local dx, dy = p.x - prev.x, p.y - prev.y
            local sp = math.sqrt(dx * dx + dy * dy) / dt
            if sp <= SPIKE then smoothed = smoothed * 0.8 + sp * 0.2 end
            setGroup(gait(smoothed, cur or 'idle'))
        end
        prev = p
    end },
    eventHandlers = { MP_Anim = function(e)
        replicated = true
        play(e.la)   -- la = locomotion group; play full-body so arms swing with the gait
        if e.st ~= nil and e.st ~= rst then   -- draw/sheathe the weapon so armed NPCs look armed
            rst = e.st
            pcall(function() types.Actor.setStance(self, e.st) end)
        end
    end },
}
