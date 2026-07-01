#ifndef OPENMW_COMPONENTS_DETOURNAVIGATOR_NAVIGATORMULTIWORLDSPACE_H
#define OPENMW_COMPONENTS_DETOURNAVIGATOR_NAVIGATORMULTIWORLDSPACE_H

#include "agentbounds.hpp"
#include "navigator.hpp"
#include "settings.hpp"
#include "updateguard.hpp"

#include <components/esm/refid.hpp>

#include <osg/Vec2i>

#include <filesystem>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace DetourNavigator
{
    // Routing facade holding one real NavigatorImpl per active worldspace, so a dedicated server can
    // keep navmesh for several worldspaces (interiors + exterior) alive at once. Single-player only
    // ever activates one worldspace, so it routes through a single sub-navigator and behaves identically.
    //
    // Geometry mutations are routed to mCurrentWorldspace (the scene always calls updateBounds before
    // loading a cell's geometry). Each key (object id / water cell / heightfield cell / pathgrid) is
    // recorded against the worldspace that owns it so later update/remove calls route correctly.
    class NavigatorMultiWorldspace final : public Navigator
    {
    public:
        explicit NavigatorMultiWorldspace(const Settings& settings, const std::filesystem::path& userDataPath);

        ScopedUpdateGuard makeUpdateGuard() override;

        bool addAgent(const AgentBounds& agentBounds) override;

        void removeAgent(const AgentBounds& agentBounds) override;

        void updateBounds(ESM::RefId worldspace, const std::optional<CellGridBounds>& cellGridBounds,
            const osg::Vec3f& playerPosition, const UpdateGuard* guard) override;

        void addObject(const ObjectId id, const ObjectShapes& shapes, const btTransform& transform,
            const UpdateGuard* guard) override;

        void addObject(const ObjectId id, const DoorShapes& shapes, const btTransform& transform,
            const UpdateGuard* guard) override;

        void updateObject(const ObjectId id, const ObjectShapes& shapes, const btTransform& transform,
            const UpdateGuard* guard) override;

        void updateObject(const ObjectId id, const DoorShapes& shapes, const btTransform& transform,
            const UpdateGuard* guard) override;

        void removeObject(const ObjectId id, const UpdateGuard* guard) override;

        void addWater(const osg::Vec2i& cellPosition, int cellSize, float level, const UpdateGuard* guard) override;

        void removeWater(const osg::Vec2i& cellPosition, const UpdateGuard* guard) override;

        void addHeightfield(const osg::Vec2i& cellPosition, int cellSize, const HeightfieldShape& shape,
            const UpdateGuard* guard) override;

        void removeHeightfield(const osg::Vec2i& cellPosition, const UpdateGuard* guard) override;

        void addPathgrid(const ESM::Cell& cell, const ESM::Pathgrid& pathgrid) override;

        void removePathgrid(const ESM::Pathgrid& pathgrid) override;

        void update(std::span<const PlayerPosition> playerPositions, const UpdateGuard* guard) override;

        void wait(WaitConditionType waitConditionType, Loading::Listener* listener) override;

        SharedNavMeshCacheItem getNavMesh(const AgentBounds& agentBounds, ESM::RefId worldspace) const override;

        std::map<AgentBounds, SharedNavMeshCacheItem> getNavMeshes() const override;

        const Settings& getSettings() const override;

        Stats getStats() const override;

        RecastMeshTiles getRecastMeshTiles() const override;

        float getMaxNavmeshAreaRealRadius() const override;

    private:
        Settings mSettings;
        std::filesystem::path mUserDataPath;
        ESM::RefId mCurrentWorldspace;
        std::map<ESM::RefId, std::unique_ptr<Navigator>> mNavigators;
        // Live agents, ref-counted by bounds: many actors (and avatars) share the same pathfinding
        // bounds, so a plain set would drop the shared agent the moment ONE of them leaves — and a
        // sub-navigator later (re)seeded from it would build a navmesh with no agent (an empty navmesh).
        std::map<AgentBounds, std::size_t> mAgents;

        // Key -> worldspace mappings so update/remove route to the worldspace that owns the geometry.
        std::unordered_map<ObjectId, ESM::RefId> mObjectWorldspace;
        std::map<osg::Vec2i, ESM::RefId> mWaterWorldspace;
        std::map<osg::Vec2i, ESM::RefId> mHeightfieldWorldspace;
        std::unordered_map<const ESM::Pathgrid*, ESM::RefId> mPathgridWorldspace;

        // Per-worldspace count of live geometry (objects + water + heightfields). When it drops to
        // zero the sub-navigator is retired: kept alive (its in-memory tile cache stays warm, so
        // re-entering the cell is a cache hit rather than a full recast rebuild) but eligible for
        // eviction once too many are idle.
        std::map<ESM::RefId, std::size_t> mLiveGeometry;

        // Retired (zero-geometry) worldspaces whose sub-navigators are kept warm, least-recently-idled
        // first. Bounded by sMaxIdleNavigators so a session that visits many interiors doesn't
        // accumulate updater threads without limit.
        std::list<ESM::RefId> mIdleWorldspaces;

        // makeUpdateGuard returns a no-op guard; the facade ignores it and lets each sub-call self-lock.
        std::mutex mDummyMutex;
        UpdateGuard mDummyGuard{ mDummyMutex };

        Navigator* getOrCreateNavigator(ESM::RefId worldspace);
        Navigator* findNavigator(ESM::RefId worldspace) const;
        void incrementLiveGeometry(ESM::RefId worldspace);
        void decrementLiveGeometry(ESM::RefId worldspace);
        void retireNavigator(ESM::RefId worldspace);
        void activateNavigator(ESM::RefId worldspace);
    };
}

#endif
