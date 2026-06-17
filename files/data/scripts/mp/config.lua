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
    -- Crime: attacking a lawful NPC adds a bounty to the attacker; guards within aggroRadius of a
    -- player's proxy hunt that player while their bounty > 0 (the engine crime system tracks only
    -- one player, so the server drives this for the per-peer proxies).
    crimeEnabled  = true,      -- master switch for the crime/guard-enforcement system
    aggroRadius   = 3000,      -- units: guards within this range of a wanted player's proxy pursue it
    arrestRadius  = 450,       -- units: a pursuing guard this close has "reached" the player (arrest prompt; AiFollow stops ~300u out)
    assaultBounty = 40,        -- bounty added for hitting a lawful NPC
    murderBounty  = 1000,      -- bounty added when the hit kills a lawful NPC
    -- Spawn point applied on boot to every role's player (server placeholder + clients), so
    -- the server simulates the same area the players are in. Middle of Seyda Neen.
    spawnCell = '',                              -- '' = exterior worldspace
    spawn = { x = -11500, y = -68500, z = 280 }, -- just above town ground (~z259): minimal settling fall
}
