#ifndef GAME_MWBASE_WORLDRENDERING_H
#define GAME_MWBASE_WORLDRENDERING_H

#include "../mwrender/rendermode.hpp"

namespace osg
{
    class Image;
}

namespace MWRender
{
    class Camera;
    class RenderingManager;
    class PostProcessor;
}

namespace MWBase
{
    /// \brief The rendering facet of the world.
    ///
    /// Split out of MWBase::World (the simulation interface) so that simulation code
    /// — and, in time, the OSG-free server build — does not see MWRender / scene-graph
    /// types in the world vtable. The same concrete World implements both interfaces;
    /// clients reach this one via MWBase::Environment::getWorldRendering(). A headless
    /// server simply never registers a rendering facet.
    ///
    /// Note: getAnimation() deliberately stays on MWBase::World for now — gameplay reads
    /// animation text-key timing through it, so it can only move once a server-side
    /// animation-state model exists (M6).
    class WorldRendering
    {
    public:
        virtual ~WorldRendering() = default;

        virtual bool toggleRenderMode(MWRender::RenderMode mode) = 0;

        virtual MWRender::Camera* getCamera() = 0;
        virtual MWRender::RenderingManager* getRenderingManager() = 0;
        virtual MWRender::PostProcessor* getPostProcessor() = 0;

        virtual void screenshot(osg::Image* image, int w, int h) = 0;
    };
}

#endif
