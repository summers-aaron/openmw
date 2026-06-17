-- Network event vocabulary shared by server + client.
return {
    ACTOR_STATE   = 'apos',         -- server -> clients: one server-owned actor's transform
    PLAYER_STATE  = 'playerpos',    -- client -> server: this client's own player transform
    REMOTE_PLAYER = 'otherplayer',  -- server -> clients: another client's player transform
    PLAYER_INFO   = 'pinfo',        -- client -> server: this client's appearance (race/head/hair/...)
    REMOTE_INFO   = 'rinfo',        -- server -> clients: another client's appearance
    ATTACK        = 'atk',          -- client -> server: this client's player hit actor <id> for <dmg> health
    PLAYER_DAMAGE = 'pdmg',         -- server -> client: your player took <dmg> (from the world, via your proxy)
    PVP_ATTACK    = 'pvp',          -- client -> server: this client's player hit player <pid> for <dmg>
    PLAYER_STATS  = 'pstats',       -- client -> server: this client's stats (health/level/attributes/skills) for its proxy
    WORLD_TIME    = 'wtime',        -- server -> clients: authoritative game time (seconds) to sync day/night
    PLAYER_BOUNTY = 'pbounty',      -- server -> client: your current crime bounty (server tracks it; client shows the HUD)
    ARREST        = 'arrest',       -- server -> client: a guard reached you to arrest (surrender or resist)
}
