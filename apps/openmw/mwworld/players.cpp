#include "players.hpp"

#include <stdexcept>
#include <string>

#include <components/esm/refid.hpp>

#include "player.hpp"
#include "ptr.hpp"

namespace MWWorld
{
    namespace
    {
        ESM::RefId makePlayerRefId(std::size_t index)
        {
            if (index == 0)
                return Player::getPrimaryRefId();
            return ESM::RefId::stringRefId("Player" + std::to_string(index));
        }
    }

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
            append(record, sPrimaryIndex);
        return primary();
    }

    Player& Players::loadExtra(std::size_t index, const ESM::NPC* record)
    {
        while (mPlayers.size() <= index)
            append(record, mPlayers.size());
        return get(index);
    }

    Player& Players::addPlayer(const ESM::NPC* record)
    {
        return append(record, mPlayers.size());
    }

    void Players::remove(std::size_t index)
    {
        if (index == sPrimaryIndex)
            throw std::out_of_range("Players::remove: the primary player cannot be removed");
        if (index >= mPlayers.size())
            throw std::out_of_range("Players::remove: no player with the requested index");
        mPlayerRefs.erase(mPlayers[index]->getConstPlayer().mRef);
        mPlayers.erase(mPlayers.begin() + index);
    }

    void Players::keepOnlyPrimary()
    {
        while (mPlayers.size() > 1)
            remove(mPlayers.size() - 1);
    }

    Player& Players::append(const ESM::NPC* record, std::size_t index)
    {
        auto& player = mPlayers.emplace_back(std::make_unique<Player>(record, makePlayerRefId(index)));
        // The player's LiveCellRef address is stable for the lifetime of the Player object,
        // so it is safe to record it once here for the isPlayer() membership test.
        mPlayerRefs.insert(player->getConstPlayer().mRef);
        return *player;
    }
}
