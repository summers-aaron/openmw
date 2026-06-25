#ifndef OPENMW_MWRENDER_RENDERINGMANAGERINTERFACE_H
#define OPENMW_MWRENDER_RENDERINGMANAGERINTERFACE_H

#include "rayresult.hpp"
#include "renderinginterface.hpp"
#include "rendermode.hpp"

#include <components/settings/settings.hpp>
#include <components/vfs/pathutil.hpp>

#include <osg/BoundingBox>
#include <osg/Vec2f>
#include <osg/Vec3f>
#include <osg/Vec4f>
#include <osg/Vec4i>
#include <osg/ref_ptr>

#include <cstddef>
#include <deque>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace osg
{
    class Image;
    class Quat;
}

namespace osgUtil
{
    class IncrementalCompileOperation;
}

namespace Resource
{
    class ResourceSystem;
}

namespace ESM
{
    struct FormId;
    using RefNum = FormId;
    class RefId;
}

namespace Terrain
{
    class World;
}

namespace SceneUtil
{
    class WorkQueue;
    class LightManager;
}

namespace DetourNavigator
{
    struct AgentBounds;
}

namespace MWWorld
{
    class Cell;
    class CellStore;
    class Ptr;
    class ConstPtr;
}

namespace Debug
{
    struct DebugDrawer;
}

namespace MWRender
{
    class SkyManager;
    class Camera;
    class Animation;
    class LandManager;
    class PostProcessor;

    class RenderingManagerInterface : public RenderingInterface
    {
    public:
        virtual ~RenderingManagerInterface() = default;

        virtual osgUtil::IncrementalCompileOperation* getIncrementalCompileOperation() = 0;

        virtual Resource::ResourceSystem* getResourceSystem() = 0;

        virtual SceneUtil::WorkQueue* getWorkQueue() = 0;
        virtual Terrain::World* getTerrain() = 0;

        virtual void preloadCommonAssets() = 0;

        virtual double getReferenceTime() const = 0;

        virtual SceneUtil::LightManager* getLightRoot() = 0;

        virtual void setNightEyeFactor(float factor) = 0;

        virtual void setAmbientColour(const osg::Vec4f& colour) = 0;

        virtual int skyGetMasserPhase() const = 0;
        virtual int skyGetSecundaPhase() const = 0;
        virtual void skySetMoonColour(bool red) = 0;

        virtual const osg::Vec4f& getSunLightPosition() const = 0;
        virtual void setSunDirection(const osg::Vec3f& direction) = 0;
        virtual void setSunColour(const osg::Vec4f& diffuse, const osg::Vec4f& specular, float sunVis) = 0;
        virtual void setNight(bool isNight) = 0;

        virtual void configureAmbient(const MWWorld::Cell& cell) = 0;
        virtual void configureFog(const MWWorld::Cell& cell) = 0;
        virtual void configureFog(
            float fogDepth, float underwaterFog, float dlFactor, float dlOffset, const osg::Vec4f& colour)
            = 0;

        virtual void addCell(const MWWorld::CellStore* store) = 0;
        virtual void removeCell(const MWWorld::CellStore* store) = 0;

        virtual void enableTerrain(bool enable, ESM::RefId worldspace) = 0;

        virtual void updatePtr(const MWWorld::Ptr& old, const MWWorld::Ptr& updated) = 0;

        virtual void rotateObject(const MWWorld::Ptr& ptr, const osg::Quat& rot) = 0;
        virtual void moveObject(const MWWorld::Ptr& ptr, const osg::Vec3f& pos) = 0;
        virtual void scaleObject(const MWWorld::Ptr& ptr, const osg::Vec3f& scale) = 0;

        virtual void removeObject(const MWWorld::Ptr& ptr) = 0;

        virtual void setWaterEnabled(bool enabled) = 0;
        virtual void setWaterHeight(float level) = 0;

        virtual void screenshot(osg::Image* image, int w, int h) = 0;

        virtual RayResult castRay(const osg::Vec3f& origin, const osg::Vec3f& dest, bool ignorePlayer,
            bool ignoreActors, std::span<const MWWorld::Ptr> ignoreList)
            = 0;

        virtual RayResult castCameraToViewportRay(
            const float nX, const float nY, float maxDistance, bool ignorePlayer, bool ignoreActors)
            = 0;

        virtual osg::Vec2f getScreenCoords(const osg::BoundingBox& bb) = 0;

        virtual void setSkyEnabled(bool enabled) = 0;

        virtual bool toggleRenderMode(RenderMode mode) = 0;

        virtual SkyManager* getSkyManager() = 0;

        virtual void spawnEffect(VFS::Path::NormalizedView model, std::string_view texture,
            const osg::Vec3f& worldPosition, float scale, bool isMagicVFX, bool useAmbientLight,
            std::string_view effectId, bool loop)
            = 0;

        virtual void removeEffect(std::string_view effectId) = 0;

        virtual void clear() = 0;

        virtual void notifyWorldSpaceChanged() = 0;

        virtual void update(float dt, bool paused) = 0;

        virtual Animation* getAnimation(const MWWorld::Ptr& ptr) = 0;
        virtual const Animation* getAnimation(const MWWorld::ConstPtr& ptr) const = 0;

        virtual PostProcessor* getPostProcessor() = 0;

        virtual void addWaterRippleEmitter(const MWWorld::Ptr& ptr) = 0;
        virtual void removeWaterRippleEmitter(const MWWorld::Ptr& ptr) = 0;
        virtual void emitWaterRipple(const osg::Vec3f& pos) = 0;

        virtual void updatePlayerPtr(const MWWorld::Ptr& ptr) = 0;

        virtual void removePlayer(const MWWorld::Ptr& player) = 0;
        virtual void setupPlayer(const MWWorld::Ptr& player) = 0;
        virtual void renderPlayer(const MWWorld::Ptr& player) = 0;

        virtual void rebuildPtr(const MWWorld::Ptr& ptr) = 0;

        virtual void processChangedSettings(const Settings::CategorySettingVector& settings) = 0;

        virtual float getNearClipDistance() const = 0;
        virtual float getViewDistance() const = 0;

        virtual void setViewDistance(float distance, bool delay) = 0;

        virtual float getTerrainHeightAt(const osg::Vec3f& pos, ESM::RefId worldspace) = 0;

        virtual Camera* getCamera() = 0;

        virtual void overrideFieldOfView(float val) = 0;
        virtual void setFieldOfView(float val) = 0;
        virtual float getFieldOfView() const = 0;
        virtual void resetFieldOfView() = 0;

        virtual osg::Vec3f getHalfExtents(const MWWorld::ConstPtr& object) const = 0;

        virtual osg::BoundingBox getCullSafeBoundingBox(const MWWorld::Ptr& ptr) const = 0;

        virtual void exportSceneGraph(
            const MWWorld::Ptr& ptr, const std::filesystem::path& filename, const std::string& format)
            = 0;

        virtual Debug::DebugDrawer& getDebugDrawer() const = 0;

        virtual LandManager* getLandManager() const = 0;

        virtual bool toggleBorders() = 0;

        virtual void updateActorPath(const MWWorld::ConstPtr& actor, const std::deque<osg::Vec3f>& path,
            const DetourNavigator::AgentBounds& agentBounds, const osg::Vec3f& start, const osg::Vec3f& end) const
            = 0;

        virtual void removeActorPath(const MWWorld::ConstPtr& actor) const = 0;

        virtual void setNavMeshNumber(const std::size_t value) = 0;

        virtual void setActiveGrid(const osg::Vec4i& grid) = 0;

        virtual bool pagingEnableObject(int type, const MWWorld::ConstPtr& ptr, bool enabled) = 0;
        virtual void pagingBlacklistObject(int type, const MWWorld::ConstPtr& ptr) = 0;
        virtual bool pagingUnlockCache() = 0;
        virtual void getPagedRefnums(const osg::Vec4i& activeGrid, std::vector<ESM::RefNum>& out) = 0;

        virtual void updateProjectionMatrix() = 0;

        virtual void setScreenRes(int width, int height) = 0;

        virtual void setNavMeshMode(Settings::NavMeshRenderMode value) = 0;

        virtual void setProjectionOffset(const osg::Vec2f& offset) = 0;
        virtual osg::Vec2f getProjectionOffset() const = 0;
    };
}

#endif
