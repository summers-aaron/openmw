#ifndef OPENMW_MWMECHANICS_ACTORUTIL_H
#define OPENMW_MWMECHANICS_ACTORUTIL_H

#include <cstddef>

namespace MWWorld
{
    class Ptr;
    class ConstPtr;
}

namespace MWMechanics
{
    /// The primary (local) player, i.e. player index 0.
    MWWorld::Ptr getPlayer();
    /// An arbitrary player by index. getPlayer(0) is equivalent to getPlayer().
    MWWorld::Ptr getPlayer(std::size_t index);
    std::size_t getPlayerCount();
    /// Generic player identity test (does not rely on the hardcoded "player" RefId).
    bool isPlayer(const MWWorld::ConstPtr& ptr);
    bool isPlayerInCombat();
    bool canActorMoveByZAxis(const MWWorld::Ptr& actor);
    bool hasWaterWalking(const MWWorld::Ptr& actor);
    bool isTargetMagicallyHidden(const MWWorld::Ptr& actor);
}

#endif
