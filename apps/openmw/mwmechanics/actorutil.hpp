#ifndef OPENMW_MWMECHANICS_ACTORUTIL_H
#define OPENMW_MWMECHANICS_ACTORUTIL_H

namespace MWWorld
{
    class Ptr;
    class ConstPtr;
}

namespace MWMechanics
{
    MWWorld::Ptr getPlayer();
    /// Generic player identity test (does not rely on the hardcoded "player" RefId).
    bool isPlayer(const MWWorld::ConstPtr& ptr);
    bool isPlayerInCombat();
    bool canActorMoveByZAxis(const MWWorld::Ptr& actor);
    bool hasWaterWalking(const MWWorld::Ptr& actor);
    bool isTargetMagicallyHidden(const MWWorld::Ptr& actor);
}

#endif
