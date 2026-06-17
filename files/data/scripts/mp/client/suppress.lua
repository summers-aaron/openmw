local debug = require('openmw.debug')
local n = 0
return { engineHandlers = { onUpdate = function(dt)
    n = n + 1
    if n == 15 then
        if debug.isAIEnabled() then debug.toggleAI() end
        print('[MP] client native AI enabled=' .. tostring(debug.isAIEnabled()))
    end
end } }
