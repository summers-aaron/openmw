#include "navigatormultiworldspace.hpp"
#include "navigatorimpl.hpp"
#include "recastglobalallocator.hpp"
#include "settingsutils.hpp"
#include "stats.hpp"

#include <components/debug/debuglog.hpp>

#include <vector>

namespace DetourNavigator
{
    NavigatorMultiWorldspace::NavigatorMultiWorldspace(
        const Settings& settings, const std::filesystem::path& userDataPath)
        : mSettings(settings)
        , mUserDataPath(userDataPath)
    {
        RecastGlobalAllocator::init();
    }

    ScopedUpdateGuard NavigatorMultiWorldspace::makeUpdateGuard()
    {
        // The facade has no cross-worldspace lock: each sub-navigator self-locks per call. We hand back
        // a guard that does nothing, so callers can keep using the guarded API unchanged. We must lock
        // the dummy mutex because UnlockUpdateGuard's deleter unlocks it.
        mDummyMutex.lock();
        return ScopedUpdateGuard(&mDummyGuard);
    }

    Navigator* NavigatorMultiWorldspace::findNavigator(ESM::RefId worldspace) const
    {
        const auto it = mNavigators.find(worldspace);
        return it == mNavigators.end() ? nullptr : it->second.get();
    }

    Navigator* NavigatorMultiWorldspace::getOrCreateNavigator(ESM::RefId worldspace)
    {
        const auto it = mNavigators.find(worldspace);
        if (it != mNavigators.end())
            return it->second.get();

        // Sub-navigators use no disk cache to avoid file contention between worldspaces; the disk
        // cache is only an optimization and correctness comes first.
        static bool logged = false;
        if (!logged)
        {
            Log(Debug::Info) << "NavigatorMultiWorldspace: per-worldspace sub-navigators use no navmesh disk cache";
            logged = true;
        }

        auto navigator = std::make_unique<NavigatorImpl>(mSettings, nullptr);
        // A freshly created sub-navigator must learn about every known agent before use.
        for (const AgentBounds& agentBounds : mAgents)
            navigator->addAgent(agentBounds);

        Navigator* const result = navigator.get();
        mNavigators.emplace(worldspace, std::move(navigator));
        return result;
    }

    void NavigatorMultiWorldspace::incrementLiveGeometry(ESM::RefId worldspace)
    {
        ++mLiveGeometry[worldspace];
    }

    void NavigatorMultiWorldspace::decrementLiveGeometry(ESM::RefId worldspace)
    {
        const auto it = mLiveGeometry.find(worldspace);
        if (it == mLiveGeometry.end())
            return;
        if (it->second > 0)
            --it->second;
        // Destroy an empty worldspace's sub-navigator to join its updater threads. Never destroy the
        // worldspace currently being loaded into (mCurrentWorldspace) to avoid tearing down mid-load.
        if (it->second == 0 && worldspace != mCurrentWorldspace)
        {
            mNavigators.erase(worldspace);
            mLiveGeometry.erase(it);
        }
    }

    bool NavigatorMultiWorldspace::addAgent(const AgentBounds& agentBounds)
    {
        mAgents.insert(agentBounds);
        // Broadcast to every existing sub-navigator so all worldspaces gain this agent's navmesh.
        for (const auto& [worldspace, navigator] : mNavigators)
            navigator->addAgent(agentBounds);
        return true;
    }

    void NavigatorMultiWorldspace::removeAgent(const AgentBounds& agentBounds)
    {
        mAgents.erase(agentBounds);
        for (const auto& [worldspace, navigator] : mNavigators)
            navigator->removeAgent(agentBounds);
    }

    void NavigatorMultiWorldspace::updateBounds(ESM::RefId worldspace,
        const std::optional<CellGridBounds>& cellGridBounds, const osg::Vec3f& playerPosition, const UpdateGuard*)
    {
        mCurrentWorldspace = worldspace;
        Navigator* const navigator = getOrCreateNavigator(worldspace);
        navigator->updateBounds(worldspace, cellGridBounds, playerPosition, nullptr);
    }

    void NavigatorMultiWorldspace::addObject(
        const ObjectId id, const ObjectShapes& shapes, const btTransform& transform, const UpdateGuard*)
    {
        Navigator* const navigator = getOrCreateNavigator(mCurrentWorldspace);
        mObjectWorldspace[id] = mCurrentWorldspace;
        incrementLiveGeometry(mCurrentWorldspace);
        navigator->addObject(id, shapes, transform, nullptr);
    }

    void NavigatorMultiWorldspace::addObject(
        const ObjectId id, const DoorShapes& shapes, const btTransform& transform, const UpdateGuard*)
    {
        Navigator* const navigator = getOrCreateNavigator(mCurrentWorldspace);
        mObjectWorldspace[id] = mCurrentWorldspace;
        incrementLiveGeometry(mCurrentWorldspace);
        navigator->addObject(id, shapes, transform, nullptr);
    }

    void NavigatorMultiWorldspace::updateObject(
        const ObjectId id, const ObjectShapes& shapes, const btTransform& transform, const UpdateGuard*)
    {
        const auto it = mObjectWorldspace.find(id);
        if (it == mObjectWorldspace.end())
            return;
        if (Navigator* const navigator = findNavigator(it->second))
            navigator->updateObject(id, shapes, transform, nullptr);
    }

    void NavigatorMultiWorldspace::updateObject(
        const ObjectId id, const DoorShapes& shapes, const btTransform& transform, const UpdateGuard*)
    {
        const auto it = mObjectWorldspace.find(id);
        if (it == mObjectWorldspace.end())
            return;
        if (Navigator* const navigator = findNavigator(it->second))
            navigator->updateObject(id, shapes, transform, nullptr);
    }

    void NavigatorMultiWorldspace::removeObject(const ObjectId id, const UpdateGuard*)
    {
        const auto it = mObjectWorldspace.find(id);
        if (it == mObjectWorldspace.end())
            return;
        const ESM::RefId worldspace = it->second;
        if (Navigator* const navigator = findNavigator(worldspace))
            navigator->removeObject(id, nullptr);
        mObjectWorldspace.erase(it);
        decrementLiveGeometry(worldspace);
    }

    void NavigatorMultiWorldspace::addWater(
        const osg::Vec2i& cellPosition, int cellSize, float level, const UpdateGuard*)
    {
        Navigator* const navigator = getOrCreateNavigator(mCurrentWorldspace);
        mWaterWorldspace[cellPosition] = mCurrentWorldspace;
        incrementLiveGeometry(mCurrentWorldspace);
        navigator->addWater(cellPosition, cellSize, level, nullptr);
    }

    void NavigatorMultiWorldspace::removeWater(const osg::Vec2i& cellPosition, const UpdateGuard*)
    {
        const auto it = mWaterWorldspace.find(cellPosition);
        if (it == mWaterWorldspace.end())
            return;
        const ESM::RefId worldspace = it->second;
        if (Navigator* const navigator = findNavigator(worldspace))
            navigator->removeWater(cellPosition, nullptr);
        mWaterWorldspace.erase(it);
        decrementLiveGeometry(worldspace);
    }

    void NavigatorMultiWorldspace::addHeightfield(
        const osg::Vec2i& cellPosition, int cellSize, const HeightfieldShape& shape, const UpdateGuard*)
    {
        Navigator* const navigator = getOrCreateNavigator(mCurrentWorldspace);
        mHeightfieldWorldspace[cellPosition] = mCurrentWorldspace;
        incrementLiveGeometry(mCurrentWorldspace);
        navigator->addHeightfield(cellPosition, cellSize, shape, nullptr);
    }

    void NavigatorMultiWorldspace::removeHeightfield(const osg::Vec2i& cellPosition, const UpdateGuard*)
    {
        const auto it = mHeightfieldWorldspace.find(cellPosition);
        if (it == mHeightfieldWorldspace.end())
            return;
        const ESM::RefId worldspace = it->second;
        if (Navigator* const navigator = findNavigator(worldspace))
            navigator->removeHeightfield(cellPosition, nullptr);
        mHeightfieldWorldspace.erase(it);
        decrementLiveGeometry(worldspace);
    }

    void NavigatorMultiWorldspace::addPathgrid(const ESM::Cell& cell, const ESM::Pathgrid& pathgrid)
    {
        Navigator* const navigator = getOrCreateNavigator(mCurrentWorldspace);
        mPathgridWorldspace[&pathgrid] = mCurrentWorldspace;
        navigator->addPathgrid(cell, pathgrid);
    }

    void NavigatorMultiWorldspace::removePathgrid(const ESM::Pathgrid& pathgrid)
    {
        const auto it = mPathgridWorldspace.find(&pathgrid);
        if (it == mPathgridWorldspace.end())
            return;
        if (Navigator* const navigator = findNavigator(it->second))
            navigator->removePathgrid(pathgrid);
        mPathgridWorldspace.erase(it);
    }

    void NavigatorMultiWorldspace::update(std::span<const PlayerPosition> playerPositions, const UpdateGuard*)
    {
        // Group player focus points by worldspace and forward each subset to its sub-navigator. A
        // worldspace with no player this frame is skipped; its tiles persist until geometry is removed.
        std::map<ESM::RefId, std::vector<PlayerPosition>> byWorldspace;
        for (const PlayerPosition& position : playerPositions)
            byWorldspace[position.mWorldspace].push_back(position);

        for (const auto& [worldspace, positions] : byWorldspace)
            if (Navigator* const navigator = findNavigator(worldspace))
                navigator->update(positions, nullptr);
    }

    void NavigatorMultiWorldspace::wait(WaitConditionType waitConditionType, Loading::Listener* listener)
    {
        for (const auto& [worldspace, navigator] : mNavigators)
            navigator->wait(waitConditionType, listener);
    }

    SharedNavMeshCacheItem NavigatorMultiWorldspace::getNavMesh(
        const AgentBounds& agentBounds, ESM::RefId worldspace) const
    {
        if (Navigator* const navigator = findNavigator(worldspace))
            return navigator->getNavMesh(agentBounds, worldspace);
        return nullptr;
    }

    std::map<AgentBounds, SharedNavMeshCacheItem> NavigatorMultiWorldspace::getNavMeshes() const
    {
        // Diagnostics/debug-draw aggregation: later worldspaces overwrite earlier ones for shared
        // agent bounds. Good enough for debug rendering of the local player's worldspace.
        std::map<AgentBounds, SharedNavMeshCacheItem> result;
        for (const auto& [worldspace, navigator] : mNavigators)
            for (auto& [agentBounds, navMesh] : navigator->getNavMeshes())
                result[agentBounds] = std::move(navMesh);
        return result;
    }

    const Settings& NavigatorMultiWorldspace::getSettings() const
    {
        return mSettings;
    }

    Stats NavigatorMultiWorldspace::getStats() const
    {
        // Diagnostics only: return the first sub-navigator's stats, or an empty set when idle.
        if (!mNavigators.empty())
            return mNavigators.begin()->second->getStats();
        return Stats{};
    }

    RecastMeshTiles NavigatorMultiWorldspace::getRecastMeshTiles() const
    {
        RecastMeshTiles result;
        for (const auto& [worldspace, navigator] : mNavigators)
            for (auto& [tilePosition, mesh] : navigator->getRecastMeshTiles())
                result[tilePosition] = std::move(mesh);
        return result;
    }

    float NavigatorMultiWorldspace::getMaxNavmeshAreaRealRadius() const
    {
        if (!mNavigators.empty())
            return mNavigators.begin()->second->getMaxNavmeshAreaRealRadius();
        return getRealTileSize(mSettings.mRecast) * getMaxNavmeshAreaRadius(mSettings);
    }
}
