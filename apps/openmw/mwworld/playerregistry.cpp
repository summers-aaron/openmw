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

    void PlayerRegistry::registerRemotePlayer(const Ptr& avatar)
    {
        if (avatar.isEmpty())
            return;
        for (const Ptr& existing : mRemotePlayers)
            if (existing == avatar)
                return; // already registered
        mRemotePlayers.push_back(avatar);
    }

    void PlayerRegistry::forgetRemotePlayer(const Ptr& avatar)
    {
        std::erase(mRemotePlayers, avatar);
    }

    std::vector<Ptr> PlayerRegistry::getPlayers() const
    {
        std::vector<Ptr> players;
        players.reserve(mRemotePlayers.size() + 1);
        if (mLocalPlayer != nullptr)
            players.push_back(mLocalPlayer->getPlayer());
        for (const Ptr& remote : mRemotePlayers)
            if (!remote.isEmpty())
                players.push_back(remote);
        return players;
    }

    bool PlayerRegistry::isPlayer(const Ptr& ptr) const
    {
        if (ptr.isEmpty())
            return false;
        if (mLocalPlayer != nullptr && ptr == mLocalPlayer->getPlayer())
            return true;
        for (const Ptr& remote : mRemotePlayers)
            if (ptr == remote)
                return true;
        return false;
    }
}
