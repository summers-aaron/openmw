#ifndef OPENMW_MWPHYSICS_CLOSESTNOTMERAYRESULTCALLBACK_H
#define OPENMW_MWPHYSICS_CLOSESTNOTMERAYRESULTCALLBACK_H

#include <span>

#include <BulletCollision/CollisionDispatch/btCollisionWorld.h>

#include "worldspacetag.hpp"

class btCollisionObject;

namespace MWPhysics
{
    class Projectile;

    class ClosestNotMeRayResultCallback : public btCollisionWorld::ClosestRayResultCallback
    {
    public:
        explicit ClosestNotMeRayResultCallback(std::span<const btCollisionObject*> ignore,
            std::span<const btCollisionObject*> targets, const btVector3& from, const btVector3& to,
            int worldspaceTag)
            : btCollisionWorld::ClosestRayResultCallback(from, to)
            , mIgnoreList(ignore)
            , mTargets(targets)
            , mWorldspaceTag(worldspaceTag)
        {
        }

        bool needsCollision(btBroadphaseProxy* proxy0) const override
        {
            if (!sameWorldspace(mWorldspaceTag, *proxy0))
                return false;
            return btCollisionWorld::ClosestRayResultCallback::needsCollision(proxy0);
        }

        btScalar addSingleResult(btCollisionWorld::LocalRayResult& rayResult, bool normalInWorldSpace) override;

    private:
        const std::span<const btCollisionObject*> mIgnoreList;
        const std::span<const btCollisionObject*> mTargets;
        const int mWorldspaceTag;
    };
}

#endif
