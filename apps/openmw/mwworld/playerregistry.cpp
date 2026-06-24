#include "playerregistry.hpp"

#include <components/esm/refid.hpp>

#include "player.hpp"
#include "ptr.hpp"

namespace MWWorld
{
    PlayerRegistry::PlayerRegistry() = default;

    PlayerRegistry::~PlayerRegistry() = default;

    const ESM::RefId& PlayerRegistry::localPlayerId()
    {
        static const ESM::RefId sId = ESM::RefId::stringRefId("Player");
        return sId;
    }

    void PlayerRegistry::createLocalPlayer(const ESM::NPC* record)
    {
        mLocalPlayer = std::make_unique<Player>(record);
    }

    Player& PlayerRegistry::getLocalPlayer() const
    {
        return *mLocalPlayer;
    }

    Ptr PlayerRegistry::getLocalPlayerPtr() const
    {
        return mLocalPlayer->getPlayer();
    }

    ConstPtr PlayerRegistry::getLocalPlayerConstPtr() const
    {
        return mLocalPlayer->getConstPlayer();
    }
}
