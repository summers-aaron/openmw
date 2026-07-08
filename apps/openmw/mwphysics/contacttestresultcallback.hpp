#ifndef OPENMW_MWPHYSICS_CONTACTTESTRESULTCALLBACK_H
#define OPENMW_MWPHYSICS_CONTACTTESTRESULTCALLBACK_H

#include <vector>

#include <BulletCollision/CollisionDispatch/btCollisionWorld.h>

#include "physicssystem.hpp"
#include "worldspacetag.hpp"

class btCollisionObject;
struct btCollisionObjectWrapper;

namespace MWPhysics
{
    class ContactTestResultCallback : public btCollisionWorld::ContactResultCallback
    {
    public:
        explicit ContactTestResultCallback(const btCollisionObject* testedAgainst)
            : mTestedAgainst(testedAgainst)
            , mWorldspaceTag(worldspaceTag(testedAgainst))
        {
        }

        bool needsCollision(btBroadphaseProxy* proxy0) const override
        {
            if (!sameWorldspace(mWorldspaceTag, *proxy0))
                return false;
            return btCollisionWorld::ContactResultCallback::needsCollision(proxy0);
        }

        btScalar addSingleResult(btManifoldPoint& cp, const btCollisionObjectWrapper* col0Wrap, int partId0, int index0,
            const btCollisionObjectWrapper* col1Wrap, int partId1, int index1) override;

        std::vector<ContactPoint> mResult;

    private:
        const btCollisionObject* mTestedAgainst;
        const int mWorldspaceTag;
    };
}

#endif
