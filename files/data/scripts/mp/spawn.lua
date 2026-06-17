-- Teleport this role's player to the configured spawn (middle of Seyda Neen) once on boot.
-- Used by BOTH server and client: the server's placeholder player must be in the same area
-- as the clients so the authoritative sim loads the cells the players occupy. godmode covers
-- the small settling drop.
local world = require('openmw.world')
local util  = require('openmw.util')
local cfg   = require('scripts.mp.config')
local n, done = 0, false
local function gp() for _, p in ipairs(world.players) do return p end end
return { engineHandlers = { onUpdate = function(dt)
    if done then return end
    n = n + 1
    if n < 20 then return end                 -- let the world finish loading first
    local pl = gp(); if not pl then return end
    pcall(function() pl:teleport(cfg.spawnCell, util.vector3(cfg.spawn.x, cfg.spawn.y, cfg.spawn.z)) end)
    done = true
    print(string.format('[MP] spawned player at %.0f,%.0f,%.0f (Seyda Neen)', cfg.spawn.x, cfg.spawn.y, cfg.spawn.z))
end } }
