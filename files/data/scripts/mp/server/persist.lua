-- Server-only, MENU context. The dedicated server is a real OpenMW playthrough, so the
-- whole simulated world (NPC deaths, container loot, door states, time of day, item
-- positions) is native engine state that the save system already serialises. We just need
-- to drive it headlessly: periodically autosave to ONE fixed slot (overwritten each time,
-- so no disk bloat). Loading on boot is done at the shell level via --load-savegame
-- (mp-common.sh), which is cleaner than racing the char-gen new-game from Lua.
--
-- openmw.menu is the only context that exposes saveGame/loadGame, and it runs in-game too
-- (MENU scripts are always active), so onFrame here fires every server frame.
local menu = require('openmw.menu')
local cfg  = require('scripts.mp.config')

-- NB: the save system sanitises the description into the filename (non-alphanumerics -> '_'),
-- so DESC must already be underscore-clean or SLOT won't match it and every save makes a new
-- file ('mp_autosave - 1.omwsave', ...) instead of overwriting.
local DESC = 'mp_autosave'              -- description -> filename 'mp_autosave.omwsave'
local SLOT = 'mp_autosave.omwsave'      -- stable slot: created on first save, overwritten after
local EVERY = cfg.saveEvery or 120      -- seconds of real time between autosaves
local acc = 0

return { engineHandlers = {
    onFrame = function(dt)
        if menu.getState() ~= menu.STATE.Running then return end   -- no world loaded yet
        acc = acc + dt
        if acc < EVERY then return end
        acc = 0
        -- pass SLOT so an existing autosave is overwritten in place (nil match on the first
        -- save -> a new slot is created from DESC, yielding exactly that filename)
        local ok, err = pcall(function() menu.saveGame(DESC, SLOT) end)
        if ok then print('[MP] persist: autosaved world') else print('[MP] persist: save FAILED: ' .. tostring(err)) end
    end,
} }
