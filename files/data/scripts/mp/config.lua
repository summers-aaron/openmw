-- Multiplayer config. Single source of tunables.
return {
    host = '127.0.0.1',
    port = 7000,
    broadcastEvery = 1,        -- frames between world-state broadcasts (~30 Hz at 30 fps)
    hideNativeEvery = 10,      -- frames between native-twin suppression passes
    playerAvatar = 'fargoth',  -- placeholder body for remote players (until appearance replication)
    interestRadius = 6000,     -- server only sends a client actors within this XY range of its player
    ghostTimeoutFrames = 90,   -- client drops a ghost not refreshed within this many frames
    keepaliveEvery = 60,       -- frames: force-send every in-range actor (refresh idle ghosts)
    moveThreshold = 2.0,       -- units: below this delta an actor is treated as idle (not sent)
    interpDelay = 0.07,        -- seconds: render ghosts this far in the past, lerping between updates (~2 ticks at 30 Hz)
    saveEvery = 120,           -- seconds between server autosaves (persist.lua); world survives restart
    -- Spawn point applied on boot to every role's player (server placeholder + clients), so
    -- the server simulates the same area the players are in. Middle of Seyda Neen.
    spawnCell = '',                              -- '' = exterior worldspace
    spawn = { x = -11500, y = -68500, z = 280 }, -- just above town ground (~z259): minimal settling fall
}
