-- Client-side, on every ghost. Drives a ghost from what the source reports:
--  * LOCOMOTION (default once MP_Anim arrives): play the source's active LOWER-body group
--    FULL-BODY so gait + arm swing stay coordinated. We never pin the upper body to a separate
--    replicated group for the ready/idle pose -- doing that froze arms to 'idle' (hands on hips)
--    while walking.
--  * STANCE: replicate draw state so armed NPCs draw/sheathe their (already-equipped) weapon.
--  * ATTACK OVERLAY: the Torso group is a weapon group ONLY during an actual swing/cast (ready
--    and movement use the idle1h/walk1h variants). While that holds, the source streams the
--    playhead time (wt); we overlay that group on the UPPER body only, seeking to match, so the
--    swing plays out. It is scoped to the swing window, so it never disturbs the ready pose.
--  * VELOCITY fallback: until the first MP_Anim, infer a gait from position deltas.
local self = require('openmw.self')
local anim = require('openmw.animation')
local types = require('openmw.types')

local ATTACK = {
    weapononehand = true, weapontwohand = true, weapontwowide = true,
    bowandarrow = true, crossbow = true, throwweapon = true, handtohand = true, spellcast = true,
}

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

-- ---- attack overlay (upper body, scoped to the swing) ----
-- A weapon group packs slash/chop/thrust in distinct time ranges; the streamed playhead (wt)
-- says which type is mid-swing. When wt enters a type's range we play that type's segment ONCE
-- (start -> small follow stop) on the upper body, so the actual swing animation plays out.
local TYPES = { 'slash', 'chop', 'thrust' }
local swinging = nil   -- type currently being played, reset between swings
local function typeAt(group, wt)
    for _, t in ipairs(TYPES) do
        local s = anim.getTextKeyTime(self.object, group .. ': ' .. t .. ' start')
        local e = anim.getTextKeyTime(self.object, group .. ': ' .. t .. ' small follow stop')
            or anim.getTextKeyTime(self.object, group .. ': ' .. t .. ' large follow stop')
        if s and e and wt >= s and wt <= e then return t end
    end
end
local function swing(group, wt)
    local t = typeAt(group, wt)
    if not t or t == swinging then return end   -- not in a swing window yet, or already playing it
    swinging = t
    pcall(function()
        if anim.hasGroup(self.object, group) then
            anim.playBlended(self, group, { startKey = t .. ' start', stopKey = t .. ' small follow stop',
                loops = 0, priority = anim.PRIORITY.Weapon, blendMask = anim.BLEND_MASK.UpperBody, autoDisable = true })
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
        if e.wt and ATTACK[e.ua] then   -- mid-swing: play the matching attack on the upper body once
            swing(e.ua, e.wt)
        else
            swinging = nil              -- back to ready/locomotion; arm the next swing
        end
    end },
}
