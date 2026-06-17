-- Server proxies + client avatars: equip the items a global script put in our inventory.
-- setEquipment is self-context; moveInto (global) is queued, so we retry for a couple seconds
-- until the items are actually in inventory, then stop.
local self = require('openmw.self')
local desired, tries = nil, 0
return {
    engineHandlers = { onUpdate = function()
        if not desired then return end
        pcall(function() self:setEquipment(desired) end)
        tries = tries - 1
        if tries <= 0 then desired = nil end
    end },
    eventHandlers = { MP_Equip = function(eq) desired = eq; tries = 60 end },
}
