#ifndef OPENMW_MWPHYSICS_WORLDSPACETAG_H
#define OPENMW_MWPHYSICS_WORLDSPACETAG_H

#include <BulletCollision/BroadphaseCollision/btBroadphaseProxy.h>
#include <BulletCollision/CollisionDispatch/btCollisionObject.h>

namespace MWPhysics
{
    // All active cells share one btCollisionWorld, but with multiple simulation anchors
    // (multiplayer) cells from several worldspaces can be active at once — and interior
    // worldspaces all use overlapping local coordinates near the origin, so their geometry
    // occupies the same region of the shared collision space. Every collision object
    // therefore carries its worldspace as a bullet user index (interned per worldspace by
    // PhysicsSystem), and every query/sweep callback rejects candidates from a different
    // worldspace, so an actor can never stand on (or get pushed around by) a floor that
    // belongs to a cell it is not in.
    //
    // A tag <= 0 means "no worldspace": bullet's default user index (-1), the global water
    // plane, and subject-less queries (camera and UI picks). Those keep the old
    // collide-with-everything behavior, which also keeps single-player byte-identical.

    inline bool sameWorldspace(int tagA, int tagB)
    {
        return tagA <= 0 || tagB <= 0 || tagA == tagB;
    }

    inline bool sameWorldspace(int tag, const btBroadphaseProxy& proxy)
    {
        const auto* object = static_cast<const btCollisionObject*>(proxy.m_clientObject);
        return sameWorldspace(tag, object->getUserIndex());
    }

    inline int worldspaceTag(const btCollisionObject* object)
    {
        return object != nullptr ? object->getUserIndex() : 0;
    }
}

#endif
