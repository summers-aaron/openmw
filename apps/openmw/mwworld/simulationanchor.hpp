#ifndef GAME_MWWORLD_SIMULATIONANCHOR_H
#define GAME_MWWORLD_SIMULATIONANCHOR_H

#include <components/esm/refid.hpp>

#include <osg/Vec3f>

#include "ptr.hpp"

namespace MWWorld
{
    /// A point the world must stay fully simulated around: an active, present player — the local
    /// one or a network peer's avatar. Cell keep-alive, actor processing range, navmesh generation
    /// and nearest-player reactions all consume these (Players::getSimulationAnchors) instead of
    /// re-deriving their own player filters. Each hand-rolled filter historically missed a case:
    /// parked slots stole the navmesh focus, the dedicated placeholder anchored AI around an
    /// invisible ghost, and every new consumer risked a new variant of the same bug.
    struct SimulationAnchor
    {
        Ptr mPtr; // the player object; always standing in a cell
        ESM::RefId mWorldspace;
        osg::Vec3f mPosition;
    };
}

#endif
