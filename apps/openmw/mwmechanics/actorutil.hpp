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
    /// Multiplayer: true when \a actor must be driven purely from the network on this peer, never
    /// locally simulated — either the host has already claimed it (RefData::isRemoteOwned), or this is
    /// a networked CLIENT and \a actor is a host-owned world actor (anything but the local player;
    /// peer avatars are already flagged). The client branch closes the window between an actor loading
    /// and the host's first snapshot, during which it would otherwise run a frame of divergent local
    /// AI / combat / magic. Equal to the raw flag on the host and in single-player.
    bool isNetworkRemoteActor(const MWWorld::Ptr& actor);
    bool isPlayerInCombat();
    bool canActorMoveByZAxis(const MWWorld::Ptr& actor);
    bool hasWaterWalking(const MWWorld::Ptr& actor);
    bool isTargetMagicallyHidden(const MWWorld::Ptr& actor);
}

#endif
