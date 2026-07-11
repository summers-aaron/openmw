#include "actorutil.hpp"

#include "../mwbase/environment.hpp"
#include "../mwbase/world.hpp"

#include "../mwnet/replicator.hpp"

#include "../mwworld/class.hpp"
#include "../mwworld/player.hpp"
#include "../mwworld/refdata.hpp"

#include "../mwmechanics/creaturestats.hpp"
#include "../mwmechanics/magiceffects.hpp"

#include <components/esm3/loadmgef.hpp>

namespace MWMechanics
{
    MWWorld::Ptr getPlayer()
    {
        return MWBase::Environment::get().getWorld()->getPlayerPtr();
    }

    MWWorld::Ptr getPlayer(std::size_t index)
    {
        return MWBase::Environment::get().getWorld()->getPlayerPtr(index);
    }

    std::size_t getPlayerCount()
    {
        return MWBase::Environment::get().getWorld()->getPlayerCount();
    }

    bool isPlayer(const MWWorld::ConstPtr& ptr)
    {
        return MWBase::Environment::get().getWorld()->isPlayer(ptr);
    }

    bool isNetworkRemoteActor(const MWWorld::Ptr& actor)
    {
        if (actor.isEmpty())
            return false;
        if (actor.getRefData().isRemoteOwned())
            return true; // already claimed by the host (also the only path on the host / in SP)
        const MWNet::Replicator* replicator = MWBase::Environment::get().getReplicator();
        // On a client every world actor is the host's; the local player is this client's own. Peer
        // avatars are players too but are already flagged remote-owned above, so excluding all players
        // here still leaves them remote — it only spares the local player from cease-remote-sim.
        return replicator != nullptr && replicator->isNetworkClient() && !isPlayer(actor);
    }

    bool isPlayerInCombat()
    {
        return MWBase::Environment::get().getWorld()->getPlayer().isInCombat();
    }

    bool canActorMoveByZAxis(const MWWorld::Ptr& actor)
    {
        MWBase::World* world = MWBase::Environment::get().getWorld();
        return (actor.getClass().canSwim(actor) && world->isSwimming(actor)) || world->isFlying(actor);
    }

    bool hasWaterWalking(const MWWorld::Ptr& actor)
    {
        const MWMechanics::MagicEffects& effects = actor.getClass().getCreatureStats(actor).getMagicEffects();
        return effects.getOrDefault(ESM::MagicEffect::WaterWalking).getMagnitude() > 0;
    }

    bool isTargetMagicallyHidden(const MWWorld::Ptr& actor)
    {
        const MagicEffects& magicEffects = actor.getClass().getCreatureStats(actor).getMagicEffects();
        return (magicEffects.getOrDefault(ESM::MagicEffect::Invisibility).getMagnitude() > 0)
            || (magicEffects.getOrDefault(ESM::MagicEffect::Chameleon).getMagnitude() >= 75);
    }
}
