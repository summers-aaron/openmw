#ifndef GAME_MWWORLD_PLAYERS_H
#define GAME_MWWORLD_PLAYERS_H

#include <cstddef>
#include <memory>
#include <unordered_set>
#include <vector>

namespace ESM
{
    class NPC;
}

namespace MWWorld
{
    class Player;
    class ConstPtr;
    struct LiveCellRefBase;

    /// \brief Owns every Player instance in the world.
    ///
    /// Player index 0 is the "primary" (local) player and behaves exactly as the historical
    /// singleton. Additional players may be created by later multiplayer phases; the rest of the
    /// engine that has not yet been generalised keeps operating on the primary player.
    ///
    /// Storage note: outstanding MWWorld::Ptr objects hold the address of a Player's internal
    /// LiveCellRef (see Player::getPlayer), so Player instances must never move in memory. The
    /// owning container therefore boxes them in unique_ptr; do NOT switch to a bare
    /// std::vector<Player>, which would reallocate and invalidate those Ptrs.
    class Players
    {
        std::vector<std::unique_ptr<Player>> mPlayers;
        // Stable LiveCellRefBase addresses of every player, used for the isPlayer() membership
        // test. These addresses only change when a Player is added to / removed from mPlayers
        // (Player::clear()/readRecord() rebuild the ref in place, keeping its address stable).
        std::unordered_set<const LiveCellRefBase*> mPlayerRefs;

    public:
        static constexpr std::size_t sPrimaryIndex = 0;

        Players();
        ~Players();

        bool empty() const { return mPlayers.empty(); }
        std::size_t size() const { return mPlayers.size(); }

        // These are const-qualified yet return a mutable Player&, mirroring the historical
        // std::unique_ptr<Player> member (unique_ptr::operator-> is const but yields a mutable
        // pointer). This preserves the previous behaviour where const World methods could still
        // obtain a mutable player; the Players container's logical state is unchanged either way.
        Player& primary() const { return get(sPrimaryIndex); }
        Player& get(std::size_t index) const;

        /// Is the given object one of the players? Identity test that does not rely on the
        /// hardcoded "player" RefId, so it remains correct once multiple players exist.
        bool isPlayer(const ConstPtr& ptr) const;

        /// Create (or, if one already exists, leave in place) the primary player from an NPC
        /// record and return it. Use Player::set() to re-point an existing primary player.
        Player& setupPrimary(const ESM::NPC* record);
    };
}

#endif
