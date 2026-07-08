#include "navmeshmanager.hpp"

#include "debug.hpp"
#include "gettilespositions.hpp"
#include "makenavmesh.hpp"
#include "navmeshcacheitem.hpp"
#include "settings.hpp"
#include "settingsutils.hpp"
#include "waitconditiontype.hpp"

#include <components/debug/debuglog.hpp>
#include <components/esm/util.hpp>

#include <osg/io_utils>

#include <DetourNavMesh.h>

#include <algorithm>

namespace
{
    /// Safely reset shared_ptr with definite underlying object destrutor call.
    /// Assuming there is another thread holding copy of this shared_ptr or weak_ptr to this shared_ptr.
    template <class T>
    bool resetIfUnique(std::shared_ptr<T>& ptr)
    {
        const std::weak_ptr<T> weak(ptr);
        ptr.reset();
        if (auto shared = weak.lock())
        {
            ptr = std::move(shared);
            return false;
        }
        return true;
    }
}

namespace DetourNavigator
{
    namespace
    {
        int getMaxRadius(int maxTiles)
        {
            return static_cast<int>(std::ceil(std::sqrt(static_cast<float>(maxTiles) / osg::PIf) + 1));
        }

        TilesPositionsRange makeRange(const TilePosition& center, int radius)
        {
            return TilesPositionsRange{
                .mBegin = center - TilePosition(radius, radius),
                .mEnd = center + TilePosition(radius + 1, radius + 1),
            };
        }

        osg::Vec2f getMinCellGridPosition(const osg::Vec2i& center, int offset, float cellSize)
        {
            const osg::Vec2i cell = center + osg::Vec2i(offset, offset);
            return osg::Vec2f(static_cast<float>(cell.x()) * cellSize, static_cast<float>(cell.y()) * cellSize);
        }

        TilesPositionsRange makeCellGridRange(
            const RecastSettings& settings, ESM::RefId worldspace, const CellGridBounds& bounds)
        {
            const float floatCellSize = static_cast<float>(ESM::getCellSize(worldspace));
            const osg::Vec2f min = getMinCellGridPosition(bounds.mCenter, -bounds.mHalfSize, floatCellSize);
            const osg::Vec2f max = getMinCellGridPosition(bounds.mCenter, bounds.mHalfSize + 1, floatCellSize);
            return TilesPositionsRange{
                .mBegin = getTilePosition(settings, toNavMeshCoordinates(settings, min)),
                .mEnd = getTilePosition(settings, toNavMeshCoordinates(settings, max)),
            };
        }

        TilesPositionsRange makeRange(const Settings& settings, ESM::RefId worldspace,
            const std::optional<CellGridBounds>& bounds, int radius, const TilePosition& center)
        {
            TilesPositionsRange result = makeRange(center, radius);
            if (bounds.has_value())
                result = getIntersection(result, makeCellGridRange(settings.mRecast, worldspace, *bounds));
            return result;
        }

        TilePosition toNavMeshTilePosition(const RecastSettings& settings, const osg::Vec3f& position)
        {
            return getTilePosition(settings, toNavMeshCoordinates(settings, position));
        }
    }

    NavMeshManager::NavMeshManager(const Settings& settings, std::unique_ptr<NavMeshDb>&& db)
        : mSettings(settings)
        , mMaxRadius(getMaxRadius(settings.mMaxTilesNumber))
        , mRecastMeshManager(settings.mRecast)
        , mOffMeshConnectionsManager(settings.mRecast)
        , mAsyncNavMeshUpdater(settings, mRecastMeshManager, mOffMeshConnectionsManager, std::move(db))
    {
    }

    void NavMeshManager::updateBounds(ESM::RefId worldspace, const std::optional<CellGridBounds>& cellGridBounds,
        const osg::Vec3f& playerPosition, const UpdateGuard* guard)
    {
        if (worldspace != mWorldspace)
        {
            mRecastMeshManager.setWorldspace(worldspace, guard);
            for (auto& [agent, cache] : mCache)
                cache = std::make_shared<GuardedNavMeshCacheItem>(++mGenerationCounter, mSettings);
            mWorldspace = worldspace;
        }

        const TilePosition playerTile = toNavMeshTilePosition(mSettings.mRecast, playerPosition);

        mRecastMeshManager.setRange(makeRange(mSettings, worldspace, cellGridBounds, mMaxRadius, playerTile), guard);
        mCellGridBounds = cellGridBounds;
    }

    bool NavMeshManager::addObject(const ObjectId id, const CollisionShape& shape, const btTransform& transform,
        const AreaType areaType, const UpdateGuard* guard)
    {
        return mRecastMeshManager.addObject(id, shape, transform, areaType, guard);
    }

    bool NavMeshManager::updateObject(
        const ObjectId id, const btTransform& transform, const AreaType areaType, const UpdateGuard* guard)
    {
        return mRecastMeshManager.updateObject(id, transform, areaType, guard);
    }

    void NavMeshManager::removeObject(const ObjectId id, const UpdateGuard* guard)
    {
        mRecastMeshManager.removeObject(id, guard);
    }

    void NavMeshManager::addWater(const osg::Vec2i& cellPosition, int cellSize, float level, const UpdateGuard* guard)
    {
        mRecastMeshManager.addWater(cellPosition, cellSize, level, guard);
    }

    void NavMeshManager::removeWater(const osg::Vec2i& cellPosition, const UpdateGuard* guard)
    {
        mRecastMeshManager.removeWater(cellPosition, guard);
    }

    void NavMeshManager::addHeightfield(
        const osg::Vec2i& cellPosition, int cellSize, const HeightfieldShape& shape, const UpdateGuard* guard)
    {
        mRecastMeshManager.addHeightfield(cellPosition, cellSize, shape, guard);
    }

    void NavMeshManager::removeHeightfield(const osg::Vec2i& cellPosition, const UpdateGuard* guard)
    {
        mRecastMeshManager.removeHeightfield(cellPosition, guard);
    }

    void NavMeshManager::addAgent(const AgentBounds& agentBounds)
    {
        auto cached = mCache.find(agentBounds);
        if (cached != mCache.end())
            return;
        mCache.emplace(agentBounds, std::make_shared<GuardedNavMeshCacheItem>(++mGenerationCounter, mSettings));
        mFoci.reset();
        Log(Debug::Debug) << "cache add for agent=" << agentBounds;
    }

    bool NavMeshManager::reset(const AgentBounds& agentBounds)
    {
        const auto it = mCache.find(agentBounds);
        if (it == mCache.end())
            return true;
        if (!resetIfUnique(it->second))
            return false;
        mCache.erase(agentBounds);
        mFoci.reset();
        return true;
    }

    void NavMeshManager::addOffMeshConnection(
        const ObjectId id, const osg::Vec3f& start, const osg::Vec3f& end, const AreaType areaType)
    {
        mOffMeshConnectionsManager.add(id, OffMeshConnection{ start, end, areaType });

        const auto startTilePosition = getTilePosition(mSettings.mRecast, start);
        const auto endTilePosition = getTilePosition(mSettings.mRecast, end);

        mRecastMeshManager.addChangedTile(startTilePosition, ChangeType::add);

        if (startTilePosition != endTilePosition)
            mRecastMeshManager.addChangedTile(endTilePosition, ChangeType::add);
    }

    void NavMeshManager::removeOffMeshConnections(const ObjectId id)
    {
        const auto changedTiles = mOffMeshConnectionsManager.remove(id);
        for (const auto& tile : changedTiles)
            mRecastMeshManager.addChangedTile(tile, ChangeType::update);
    }

    void NavMeshManager::update(std::span<const osg::Vec3f> playerPositions, const UpdateGuard* guard)
    {
        if (playerPositions.empty())
            return;
        std::vector<TilePosition> foci;
        foci.reserve(playerPositions.size());
        for (const osg::Vec3f& playerPosition : playerPositions)
            foci.push_back(toNavMeshTilePosition(mSettings.mRecast, playerPosition));
        std::sort(foci.begin(), foci.end());
        foci.erase(std::unique(foci.begin(), foci.end()), foci.end());
        if (mLastRecastMeshManagerRevision == mRecastMeshManager.getRevision() && mFoci.has_value()
            && *mFoci == foci)
            return;
        mLastRecastMeshManagerRevision = mRecastMeshManager.getRevision();
        mFoci = foci;
        // Split the fixed tile budget evenly across foci so several players in one worldspace each
        // keep tiles around them without overflowing the navmesh's tile capacity; a single player
        // gets the whole budget, matching the historical behavior.
        const int maxTilesPerFocus = std::max(1, mSettings.mMaxTilesNumber / static_cast<int>(foci.size()));
        const int radius = getMaxRadius(maxTilesPerFocus);
        TilesPositionsRange range = makeRange(mSettings, mWorldspace, mCellGridBounds, radius, foci.front());
        for (std::size_t i = 1; i < foci.size(); ++i)
            range = getUnion(range, makeRange(mSettings, mWorldspace, mCellGridBounds, radius, foci[i]));
        mRecastMeshManager.setRange(range, guard);
        const auto changedTiles = mRecastMeshManager.takeChangedTiles(guard);
        const TilesPositionsRange limitedRange = mRecastMeshManager.getLimitedObjectsRange();
        for (const auto& [agentBounds, cached] : mCache)
            update(agentBounds, foci, maxTilesPerFocus, limitedRange, cached, changedTiles);
    }

    void NavMeshManager::update(const AgentBounds& agentBounds, std::span<const TilePosition> foci,
        int maxTilesPerFocus, const TilesPositionsRange& range, const SharedNavMeshCacheItem& cached,
        const std::map<osg::Vec2i, ChangeType>& changedTiles)
    {
        std::map<osg::Vec2i, ChangeType> tilesToPost;
        for (const auto& [k, v] : changedTiles)
            if (shouldAddTile(k, foci, maxTilesPerFocus))
                tilesToPost.emplace(k, v);
        {
            const auto locked = cached->lockConst();
            const auto& navMesh = locked->getImpl();
            getTilesPositions(range, [&](const TilePosition& tile) {
                if (changedTiles.find(tile) != changedTiles.end() || locked->isEmptyTile(tile))
                    return;
                const bool shouldAdd = shouldAddTile(tile, foci, maxTilesPerFocus);
                const bool presentInNavMesh = navMesh.getTileAt(tile.x(), tile.y(), 0) != nullptr;
                if (shouldAdd && !presentInNavMesh)
                    tilesToPost.emplace(tile, ChangeType::add);
                else if (!shouldAdd && presentInNavMesh)
                    tilesToPost.emplace(tile, ChangeType::remove);
            });
            locked->forEachTilePosition([&](const TilePosition& tile) {
                if (!shouldAddTile(tile, foci, maxTilesPerFocus))
                    tilesToPost.emplace(tile, ChangeType::remove);
            });
        }
        mAsyncNavMeshUpdater.post(agentBounds, cached, foci, mWorldspace, tilesToPost);
        Log(Debug::Debug) << "Cache update posted for agent=" << agentBounds << " foci=" << foci.size()
                          << " firstFocus=" << foci.front()
                          << " recastMeshManagerRevision=" << mLastRecastMeshManagerRevision;
    }

    void NavMeshManager::wait(WaitConditionType waitConditionType, Loading::Listener* listener)
    {
        mAsyncNavMeshUpdater.wait(waitConditionType, listener);
    }

    SharedNavMeshCacheItem NavMeshManager::getNavMesh(const AgentBounds& agentBounds) const
    {
        return getCached(agentBounds);
    }

    std::map<AgentBounds, SharedNavMeshCacheItem> NavMeshManager::getNavMeshes() const
    {
        return mCache;
    }

    Stats NavMeshManager::getStats() const
    {
        return Stats{
            .mUpdater = mAsyncNavMeshUpdater.getStats(),
            .mRecast = mRecastMeshManager.getStats(),
        };
    }

    RecastMeshTiles NavMeshManager::getRecastMeshTiles() const
    {
        RecastMeshTiles result;
        getTilesPositions(mRecastMeshManager.getLimitedObjectsRange(), [&](const TilePosition& v) {
            if (auto mesh = mRecastMeshManager.getCachedMesh(mWorldspace, v))
                result.emplace(v, std::move(mesh));
        });
        return result;
    }

    SharedNavMeshCacheItem NavMeshManager::getCached(const AgentBounds& agentBounds) const
    {
        const auto cached = mCache.find(agentBounds);
        if (cached != mCache.end())
            return cached->second;
        return SharedNavMeshCacheItem();
    }
}
