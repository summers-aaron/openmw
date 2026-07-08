#ifndef OPENMW_MWPHYSICS_ACTORCONVEXCALLBACK_H
#define OPENMW_MWPHYSICS_ACTORCONVEXCALLBACK_H

#include <BulletCollision/CollisionDispatch/btCollisionWorld.h>

#include "worldspacetag.hpp"

class btCollisionObject;

namespace MWPhysics
{
    class ActorConvexCallback : public btCollisionWorld::ClosestConvexResultCallback
    {
    public:
        explicit ActorConvexCallback(const btCollisionObject* me, const btVector3& motion, btScalar minCollisionDot,
            const btCollisionWorld* world)
            : btCollisionWorld::ClosestConvexResultCallback(btVector3(0.0, 0.0, 0.0), btVector3(0.0, 0.0, 0.0))
            , mMe(me)
            , mMotion(motion)
            , mMinCollisionDot(minCollisionDot)
            , mWorld(world)
            , mWorldspaceTag(worldspaceTag(me))
        {
        }

        bool needsCollision(btBroadphaseProxy* proxy0) const override
        {
            if (!sameWorldspace(mWorldspaceTag, *proxy0))
                return false;
            return btCollisionWorld::ClosestConvexResultCallback::needsCollision(proxy0);
        }

        btScalar addSingleResult(btCollisionWorld::LocalConvexResult& convexResult, bool normalInWorldSpace) override;

    protected:
        const btCollisionObject* mMe;
        const btVector3 mMotion;
        const btScalar mMinCollisionDot;
        const btCollisionWorld* mWorld;
        const int mWorldspaceTag;
    };
}

#endif
