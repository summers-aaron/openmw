#include "players.hpp"

#include <stdexcept>

#include "player.hpp"
#include "ptr.hpp"

namespace MWWorld
{
    Players::Players() = default;

    Players::~Players() = default;

    Player& Players::get(std::size_t index) const
    {
        if (index >= mPlayers.size())
            throw std::out_of_range("Players::get: no player with the requested index");
        return *mPlayers[index];
    }

    bool Players::isPlayer(const ConstPtr& ptr) const
    {
        return ptr.mRef != nullptr && mPlayerRefs.find(ptr.mRef) != mPlayerRefs.end();
    }

    Player& Players::setupPrimary(const ESM::NPC* record)
    {
        if (mPlayers.empty())
        {
            auto& player = mPlayers.emplace_back(std::make_unique<Player>(record));
            // The player's LiveCellRef address is stable for the lifetime of the Player object,
            // so it is safe to record it once here for the isPlayer() membership test.
            mPlayerRefs.insert(player->getConstPlayer().mRef);
        }
        return primary();
    }
}
