#ifndef OPENMW_MWRENDER_RENDERINGMANAGER_H
#define OPENMW_MWRENDER_RENDERINGMANAGER_H

#include "objects.hpp"
#include "rayresult.hpp"
#include "renderinginterface.hpp"
#include "renderingmanagerinterface.hpp"
#include "rendermode.hpp"

#include <components/settings/settings.hpp>
#include <components/vfs/pathutil.hpp>

#include <osg/Light>
#include <osg/ref_ptr>

#include <osgUtil/IncrementalCompileOperation>

#include <deque>
#include <memory>
#include <span>
#include <unordered_map>

namespace osg
{
    class Group;
    class PositionAttitudeTransform;
}

namespace osgUtil
{
    class IntersectionVisitor;
    class Intersector;
}

namespace Resource
{
    class ResourceSystem;
}

namespace osgViewer
{
    class Viewer;
}

namespace ESM
{
    struct Cell;
    struct FormId;
    using RefNum = FormId;
}

namespace Terrain
{
    class World;
}

namespace Fallback
{
    class Map;
}

namespace SceneUtil
{
    class ShadowManager;
    class WorkQueue;
    class LightManager;
    class UnrefQueue;
    class PerViewUniformStateUpdater;
    class SharedUniformStateUpdater;
    class StateUpdater;
}

namespace DetourNavigator
{
    struct Navigator;
    struct Settings;
    struct AgentBounds;
}

namespace MWWorld
{
    class GroundcoverStore;
    class Cell;
}

namespace Debug
{
    struct DebugDrawer;
}

namespace MWRender
{
    class IntersectionVisitorWithIgnoreList;

    class EffectManager;
    class ScreenshotManager;
    class FogManager;
    class SkyManager;
    class NpcAnimation;
    class Pathgrid;
    class Camera;
    class Water;
    class TerrainStorage;
    class LandManager;
    class NavMesh;
    class ActorsPaths;
    class RecastMesh;
    class ObjectPaging;
    class Groundcover;
    class PostProcessor;

    class RenderingManager final : public MWRender::RenderingManagerInterface
    {
    public:
        using RayResult = MWRender::RayResult;

        RenderingManager(osgViewer::Viewer* viewer, osg::ref_ptr<osg::Group> rootNode,
            Resource::ResourceSystem* resourceSystem, SceneUtil::WorkQueue* workQueue,
            DetourNavigator::Navigator& navigator, const MWWorld::GroundcoverStore& groundcoverStore,
            SceneUtil::UnrefQueue& unrefQueue);
        ~RenderingManager();

        osgUtil::IncrementalCompileOperation* getIncrementalCompileOperation() override;

        MWRender::Objects& getObjects() override;

        Resource::ResourceSystem* getResourceSystem() override;

        SceneUtil::WorkQueue* getWorkQueue() override;
        Terrain::World* getTerrain() override;

        void preloadCommonAssets() override;

        double getReferenceTime() const override;

        SceneUtil::LightManager* getLightRoot() override;

        void setNightEyeFactor(float factor) override;

        void setAmbientColour(const osg::Vec4f& colour) override;

        int skyGetMasserPhase() const override;
        int skyGetSecundaPhase() const override;
        void skySetMoonColour(bool red) override;

        const osg::Vec4f& getSunLightPosition() const override { return mSunLight->getPosition(); }
        void setSunDirection(const osg::Vec3f& direction) override;
        void setSunColour(const osg::Vec4f& diffuse, const osg::Vec4f& specular, float sunVis) override;
        void setNight(bool isNight) override { mNight = isNight; }

        void configureAmbient(const MWWorld::Cell& cell) override;
        void configureFog(const MWWorld::Cell& cell) override;
        void configureFog(
            float fogDepth, float underwaterFog, float dlFactor, float dlOffset, const osg::Vec4f& colour) override;

        void addCell(const MWWorld::CellStore* store) override;
        void removeCell(const MWWorld::CellStore* store) override;

        void enableTerrain(bool enable, ESM::RefId worldspace) override;

        void updatePtr(const MWWorld::Ptr& old, const MWWorld::Ptr& updated) override;

        void rotateObject(const MWWorld::Ptr& ptr, const osg::Quat& rot) override;
        void moveObject(const MWWorld::Ptr& ptr, const osg::Vec3f& pos) override;
        void scaleObject(const MWWorld::Ptr& ptr, const osg::Vec3f& scale) override;

        void removeObject(const MWWorld::Ptr& ptr) override;

        void setWaterEnabled(bool enabled) override;
        void setWaterHeight(float level) override;

        /// Take a screenshot of w*h onto the given image, not including the GUI.
        void screenshot(osg::Image* image, int w, int h) override;

        RayResult castRay(const osg::Vec3f& origin, const osg::Vec3f& dest, bool ignorePlayer,
            bool ignoreActors = false, std::span<const MWWorld::Ptr> ignoreList = {}) override;

        /// Return the object under the mouse cursor / crosshair position, given by nX and nY normalized screen
        /// coordinates, where (0,0) is the top left corner.
        RayResult castCameraToViewportRay(
            const float nX, const float nY, float maxDistance, bool ignorePlayer, bool ignoreActors = false) override;

        /// Get normalized screen coordinates of the bounding box's summit, where (0,0) is the top left corner
        osg::Vec2f getScreenCoords(const osg::BoundingBox& bb) override;

        void setSkyEnabled(bool enabled) override;

        bool toggleRenderMode(RenderMode mode) override;

        SkyManager* getSkyManager() override;

        void spawnEffect(VFS::Path::NormalizedView model, std::string_view texture, const osg::Vec3f& worldPosition,
            float scale = 1.f, bool isMagicVFX = true, bool useAmbientLight = true, std::string_view effectId = {},
            bool loop = false) override;

        void removeEffect(std::string_view effectId) override;

        /// Clear all savegame-specific data
        void clear() override;

        /// Clear all worldspace-specific data
        void notifyWorldSpaceChanged() override;

        void update(float dt, bool paused) override;

        Animation* getAnimation(const MWWorld::Ptr& ptr) override;
        const Animation* getAnimation(const MWWorld::ConstPtr& ptr) const override;

        PostProcessor* getPostProcessor() override;

        void addWaterRippleEmitter(const MWWorld::Ptr& ptr) override;
        void removeWaterRippleEmitter(const MWWorld::Ptr& ptr) override;
        void emitWaterRipple(const osg::Vec3f& pos) override;

        void updatePlayerPtr(const MWWorld::Ptr& ptr) override;

        void removePlayer(const MWWorld::Ptr& player) override;
        void setupPlayer(const MWWorld::Ptr& player) override;
        void renderPlayer(const MWWorld::Ptr& player) override;

        void rebuildPtr(const MWWorld::Ptr& ptr) override;

        void processChangedSettings(const Settings::CategorySettingVector& settings) override;

        float getNearClipDistance() const override { return mNearClip; }
        float getViewDistance() const override { return mViewDistance; }

        void setViewDistance(float distance, bool delay = false) override;

        float getTerrainHeightAt(const osg::Vec3f& pos, ESM::RefId worldspace) override;

        // camera stuff
        Camera* getCamera() override { return mCamera.get(); }

        /// temporarily override the field of view with given value.
        void overrideFieldOfView(float val) override;
        void setFieldOfView(float val) override;
        float getFieldOfView() const override;
        /// reset a previous overrideFieldOfView() call, i.e. revert to field of view specified in the settings file.
        void resetFieldOfView() override;

        osg::Vec3f getHalfExtents(const MWWorld::ConstPtr& object) const override;

        // Return local bounding box. Safe to be called in parallel with cull thread.
        osg::BoundingBox getCullSafeBoundingBox(const MWWorld::Ptr& ptr) const override;

        void exportSceneGraph(
            const MWWorld::Ptr& ptr, const std::filesystem::path& filename, const std::string& format) override;

        Debug::DebugDrawer& getDebugDrawer() const override { return *mDebugDraw; }

        LandManager* getLandManager() const override;

        bool toggleBorders() override;

        void updateActorPath(const MWWorld::ConstPtr& actor, const std::deque<osg::Vec3f>& path,
            const DetourNavigator::AgentBounds& agentBounds, const osg::Vec3f& start, const osg::Vec3f& end)
            const override;

        void removeActorPath(const MWWorld::ConstPtr& actor) const override;

        void setNavMeshNumber(const std::size_t value) override;

        void setActiveGrid(const osg::Vec4i& grid) override;

        bool pagingEnableObject(int type, const MWWorld::ConstPtr& ptr, bool enabled) override;
        void pagingBlacklistObject(int type, const MWWorld::ConstPtr& ptr) override;
        bool pagingUnlockCache() override;
        void getPagedRefnums(const osg::Vec4i& activeGrid, std::vector<ESM::RefNum>& out) override;

        void updateProjectionMatrix() override;

        void setScreenRes(int width, int height) override;

        void setNavMeshMode(Settings::NavMeshRenderMode value) override;

        void setProjectionOffset(const osg::Vec2f& offset) override
        {
            mProjectionOffset = offset;
            mUpdateProjectionMatrix = true;
        }
        osg::Vec2f getProjectionOffset() const override { return mProjectionOffset; }

    private:
        void updateTextureFiltering();
        void updateAmbient();
        void setFogColor(const osg::Vec4f& color);

        struct WorldspaceChunkMgr
        {
            std::unique_ptr<Terrain::World> mTerrain;
            std::unique_ptr<ObjectPaging> mObjectPaging;
            std::unique_ptr<Groundcover> mGroundcover;
        };

        WorldspaceChunkMgr& getWorldspaceChunkMgr(ESM::RefId worldspace);

        void reportStats() const;

        void updateNavMesh();

        void updateRecastMesh();

        const bool mSkyBlending;

        osg::ref_ptr<osgUtil::IntersectionVisitor> getIntersectionVisitor(osgUtil::Intersector* intersector,
            bool ignorePlayer, bool ignoreActors, std::span<const MWWorld::Ptr> ignoreList = {});

        osg::ref_ptr<IntersectionVisitorWithIgnoreList> mIntersectionVisitor;

        osg::ref_ptr<osgViewer::Viewer> mViewer;
        osg::ref_ptr<osg::Group> mRootNode;
        osg::ref_ptr<SceneUtil::LightManager> mSceneRoot;
        Resource::ResourceSystem* mResourceSystem;

        osg::ref_ptr<SceneUtil::WorkQueue> mWorkQueue;

        osg::ref_ptr<osg::Light> mSunLight;

        DetourNavigator::Navigator& mNavigator;
        std::unique_ptr<NavMesh> mNavMesh;
        std::size_t mNavMeshNumber = 0;
        std::unique_ptr<ActorsPaths> mActorsPaths;
        std::unique_ptr<RecastMesh> mRecastMesh;
        std::unique_ptr<Pathgrid> mPathgrid;
        std::unique_ptr<Objects> mObjects;
        std::unique_ptr<Water> mWater;
        std::unordered_map<ESM::RefId, WorldspaceChunkMgr> mWorldspaceChunks;
        Terrain::World* mTerrain;
        std::unique_ptr<TerrainStorage> mTerrainStorage;
        ObjectPaging* mObjectPaging;
        Groundcover* mGroundcover;
        std::unique_ptr<SkyManager> mSky;
        std::unique_ptr<FogManager> mFog;
        std::unique_ptr<ScreenshotManager> mScreenshotManager;
        std::unique_ptr<EffectManager> mEffectManager;
        std::unique_ptr<SceneUtil::ShadowManager> mShadowManager;
        osg::ref_ptr<PostProcessor> mPostProcessor;
        osg::ref_ptr<NpcAnimation> mPlayerAnimation;
        osg::ref_ptr<SceneUtil::PositionAttitudeTransform> mPlayerNode;
        std::unique_ptr<Camera> mCamera;
        osg::ref_ptr<Debug::DebugDrawer> mDebugDraw;

        osg::ref_ptr<SceneUtil::StateUpdater> mStateUpdater;
        osg::ref_ptr<SceneUtil::SharedUniformStateUpdater> mSharedUniformStateUpdater;
        osg::ref_ptr<SceneUtil::PerViewUniformStateUpdater> mPerViewUniformStateUpdater;

        osg::Vec4f mAmbientColor;
        float mNightEyeFactor;

        float mNearClip;
        float mViewDistance;
        bool mFieldOfViewOverridden;
        float mFieldOfViewOverride;
        float mFieldOfView;
        float mFirstPersonFieldOfView;
        bool mUpdateProjectionMatrix = false;
        bool mNight = false;
        osg::Vec2f mProjectionOffset;
        const MWWorld::GroundcoverStore& mGroundCoverStore;

        void operator=(const RenderingManager&);
        RenderingManager(const RenderingManager&);
    };

}

#endif
