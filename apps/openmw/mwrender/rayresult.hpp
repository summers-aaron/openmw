#ifndef OPENMW_MWRENDER_RAYRESULT_H
#define OPENMW_MWRENDER_RAYRESULT_H

#include <osg/Vec3f>

#include <components/esm3/refnum.hpp>

#include "../mwworld/ptr.hpp"

namespace MWRender
{
    struct RayResult
    {
        bool mHit;
        osg::Vec3f mHitNormalWorld;
        osg::Vec3f mHitPointWorld;
        MWWorld::Ptr mHitObject;
        ESM::RefNum mHitRefnum;
        float mRatio;
    };
}

#endif
