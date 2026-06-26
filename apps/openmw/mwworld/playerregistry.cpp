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

    void PlayerRegistry::registerPlayer(const Ptr& player)
    {
        if (player.isEmpty())
            return;
        for (const RegisteredPlayer& existing : mRemotePlayers)
            if (existing.mActor == player)
                return; // already registered
        mRemotePlayers.push_back(RegisteredPlayer{ player, PlayerData{} }); // its own fresh record
    }

    void PlayerRegistry::forgetPlayer(const Ptr& player)
    {
        std::erase_if(mRemotePlayers, [&](const RegisteredPlayer& rp) { return rp.mActor == player; });
    }

    std::vector<Ptr> PlayerRegistry::getPlayers() const
    {
        std::vector<Ptr> players;
        players.reserve(mRemotePlayers.size() + 1);
        if (mLocalPlayer != nullptr)
            players.push_back(mLocalPlayer->getPlayer());
        for (const RegisteredPlayer& remote : mRemotePlayers)
            if (!remote.mActor.isEmpty())
                players.push_back(remote.mActor);
        return players;
    }

    bool PlayerRegistry::isPlayer(const Ptr& ptr) const
    {
        if (ptr.isEmpty())
            return false;
        if (mLocalPlayer != nullptr && ptr == mLocalPlayer->getPlayer())
            return true;
        for (const RegisteredPlayer& remote : mRemotePlayers)
            if (ptr == remote.mActor)
                return true;
        return false;
    }

    PlayerData* PlayerRegistry::getPlayerData(const Ptr& player)
    {
        if (player.isEmpty())
            return nullptr;
        if (mLocalPlayer != nullptr && player == mLocalPlayer->getPlayer())
            return &mLocalPlayer->getData();
        for (RegisteredPlayer& remote : mRemotePlayers)
            if (player == remote.mActor)
                return &remote.mData;
        return nullptr;
    }
}
