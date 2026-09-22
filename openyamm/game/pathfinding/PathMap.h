#pragma once

#include "game/pathfinding/PathfindingTypes.h"
#include "game/pathfinding/PathSnapshotStorage.h"

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>
#include <span>
#include <array>
#include <memory>

namespace OpenYAMM::Game
{
struct PathTraceResult
{
    bool blocked = false;
    size_t facetIndex = 0;
    PathPoint point;
};

class PathMap
{
public:
    // One bounded memo per resumable search. A new request or map revision invalidates it.
    class QueryCache
    {
    public:
        struct State;
        QueryCache();
        ~QueryCache();
        void reset();
        std::unique_ptr<State> state;
    };
    class QueryCacheScope
    {
    public:
        explicit QueryCacheScope(const PathMap &map, QueryCache *cache = nullptr);
        ~QueryCacheScope();
        QueryCacheScope(const QueryCacheScope &) = delete;
        QueryCacheScope &operator=(const QueryCacheScope &) = delete;
    private:
        const PathMap *m_previous = nullptr;
        QueryCache::State *m_previousCache = nullptr;
    };
    void clear();
    void setFacets(std::vector<PathFacet> facets);
    size_t addFacet(const PathFacet &facet);
    // One source face per replacement; an empty polygon removes its geometry.
    void replaceSourceFacet(PathFacet facet);
    void buildSpatialGrid(float cellSize);

    const PathSnapshotVector<PathFacet> &facets() const;
    size_t revision() const;
    size_t gridMemberships() const { return m_gridMemberships; }
    size_t overflowFacetCount() const { return m_overflowFacets.size(); }

    PathFloorSample floorAt(const PathPoint &position) const;
    PathPoint snapToWalkableSourceFacet(
        int32_t sourceId,
        const PathPoint &position,
        float maxDistance,
        bool &valid,
        PathFloorSample *pFloorSample = nullptr
    ) const;
    PathPoint snapToNearestWalkableFloor(
        const PathPoint &position,
        float maxDistance,
        bool &valid,
        PathFloorSample *pFloorSample = nullptr
    ) const;
    PathFloorQueryDebug debugFloorQuery(const PathPoint &position) const;
    PathTraceResult traceLine(
        const PathPoint &from,
        const PathPoint &to,
        float radius = 0.0f,
        bool checkBody = false
    ) const;
    bool traceWalkSegment(const PathPoint &from, const PathPoint &to, const PathObject &object,
        const std::atomic_bool *pCancelled = nullptr) const;
    PathWalkSegmentDebug debugTraceWalkSegment(
        const PathPoint &from,
        const PathPoint &to,
        const PathObject &object
    ) const;
    bool canReachDirectly(const PathPoint &from, const PathPoint &to, const PathObject &object,
        const std::atomic_bool *pCancelled = nullptr) const;
    bool certifyWalkCorridor(const PathPoint &from, const PathPoint &to, const PathObject &object,
        PathWalkCertificate &certificate) const;

private:
    struct TraceQueryCache
    {
        struct Entry
        {
            std::vector<size_t> candidates;
            std::array<int32_t, 6> cells{};
            PathBounds envelope{};
            bool valid = false;
        };
        std::array<Entry, 16> entries;
        size_t next = 0;
        void clear() { for (Entry &entry : entries) { entry.valid = false; } next = 0; }
    };
    PathTraceResult traceLineInternal(const PathPoint &from, const PathPoint &to,
        float radius, bool checkBody, TraceQueryCache &cache) const;
    void candidateFloorFacetsForArea(float minX, float minY, float maxX, float maxY,
        std::vector<size_t> &candidates) const;
    bool certifyFlatWalk(const PathPoint &from, const PathPoint &to, const PathObject &object,
        const PathFloorSample &floor, float supportMargin = 0.0f) const;
    struct FacetGeometry
    {
        PathBounds bounds;
        PathPoint normal;
        bool valid = false;
        bool nonBlocking = false;
    };

    struct GridCellKey
    {
        int32_t x = 0;
        int32_t y = 0;
        int32_t z = 0;

        bool operator==(const GridCellKey &other) const;
    };

    struct GridCellKeyHash
    {
        size_t operator()(const GridCellKey &key) const;
    };

    struct FloorCellKey
    {
        int32_t x = 0;
        int32_t y = 0;

        bool operator==(const FloorCellKey &other) const;
    };

    struct FloorCellKeyHash
    {
        size_t operator()(const FloorCellKey &key) const;
    };

    void candidateFacetsForBounds(const PathBounds &bounds, std::vector<size_t> &candidates, bool filterBounds = true) const;
    std::span<const size_t> candidateFloorFacetsForPoint(float x, float y) const;
    bool traceWalkSegmentInternal(
        const PathPoint &from,
        const PathPoint &to,
        const PathObject &object,
        PathWalkSegmentDebug *pDebug,
        const std::atomic_bool *pCancelled = nullptr
    ) const;
    void rebuildGeometry();
    void rebuildSpatialGrid();
    void insertSpatialFacet(size_t index);
    void removeSpatialFacet(size_t index);
    int32_t gridCoordinate(float value) const;

    PathSnapshotVector<PathFacet> m_facets;
    PathSnapshotVector<FacetGeometry> m_geometry;
    PathSnapshotMap<int32_t, size_t> m_sourceFacetIndex;
    std::vector<size_t> m_floorFacetIndices;
    PathSnapshotMap<int32_t, std::vector<size_t>> m_facetsBySourceId;
    PathSnapshotMap<GridCellKey, std::vector<size_t>, GridCellKeyHash> m_spatialGrid;
    PathSnapshotMap<FloorCellKey, std::vector<size_t>, FloorCellKeyHash> m_floorGrid;
    std::vector<size_t> m_overflowFacets;
    std::vector<size_t> m_overflowFloorFacets;
    size_t m_gridMemberships = 0;
    size_t m_revision = 0;
    float m_gridCellSize = 0.0f;
};
}
