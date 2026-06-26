#ifndef OPENMW_MWMECHANICS_ACTORUTIL_H
#define OPENMW_MWMECHANICS_ACTORUTIL_H

#include <vector>

namespace MWWorld
{
    class Ptr;
}

namespace MWMechanics
{
    MWWorld::Ptr getPlayer();
    /// Every player in the world (local + any remote peers' avatars); may be empty. World logic
    /// that should react to all players iterates this instead of using getPlayer().
    std::vector<MWWorld::Ptr> getPlayers();
    /// Is this actor one of the players (local or a remote peer's avatar)?
    bool isPlayer(const MWWorld::Ptr& ptr);
    bool isPlayerInCombat();
    bool canActorMoveByZAxis(const MWWorld::Ptr& actor);
    bool hasWaterWalking(const MWWorld::Ptr& actor);
    bool isTargetMagicallyHidden(const MWWorld::Ptr& actor);
}

#endif
