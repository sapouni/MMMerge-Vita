#include "game/pathfinding/PathMap.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>
#include <bit>
#include <memory>

namespace OpenYAMM::Game
{
struct PathMap::QueryCache::State
{
    struct FloorEntry
    {
        std::array<uint32_t, 3> key{};
        PathFloorSample result;
        uint32_t epoch = 0;
    };
    struct WalkEntry
    {
        std::array<uint32_t, 10> key{};
        bool result = false;
        uint32_t epoch = 0;
    };
    std::array<FloorEntry, 2048> floors;
    std::array<WalkEntry, 1024> walks;
    uint32_t epoch = 0;
    const PathMap *map = nullptr;
    size_t revision = 0;
    void reset()
    {
        if (++epoch == 0)
        {
            for (auto &entry : floors) { entry.epoch = 0; }
            for (auto &entry : walks) { entry.epoch = 0; }
            epoch = 1;
        }
    }
};
PathMap::QueryCache::QueryCache() : state(std::make_unique<State>()) {}
PathMap::QueryCache::~QueryCache() = default;
void PathMap::QueryCache::reset() { state->reset(); state->map = nullptr; }
namespace
{
thread_local const PathMap *cachedQueryMap = nullptr;
thread_local std::unique_ptr<PathMap::QueryCache> fallbackQueryCache;
thread_local PathMap::QueryCache::State *plannerQueryCache = nullptr;
template <size_t N> size_t queryHash(const std::array<uint32_t, N> &key)
{
    uint32_t hash = 2166136261u;
    for (uint32_t value : key) { hash = (hash ^ value) * 16777619u; hash ^= hash >> 16; }
    return hash;
}

constexpr float PathEpsilon = 0.0001f;
constexpr uint64_t MaxPathGridCellsPerQuery = 4096;
constexpr uint64_t MaxPathGridMemberships = 262144;
bool boundedGridVolume(int32_t minX, int32_t maxX, int32_t minY, int32_t maxY,
    int32_t minZ, int32_t maxZ, uint64_t &cells)
{
    cells = 1;
    for (int64_t span : {int64_t(maxX) - minX + 1, int64_t(maxY) - minY + 1, int64_t(maxZ) - minZ + 1})
    {
        if (span <= 0 || uint64_t(span) > MaxPathGridCellsPerQuery / cells) { return false; }
        cells *= uint64_t(span);
    }
    return true;
}

PathPoint pointAdd(const PathPoint &left, const PathPoint &right)
{
    return {left.x + right.x, left.y + right.y, left.z + right.z};
}

PathPoint pointSubtract(const PathPoint &left, const PathPoint &right)
{
    return {left.x - right.x, left.y - right.y, left.z - right.z};
}

PathPoint pointScale(const PathPoint &point, float scale)
{
    return {point.x * scale, point.y * scale, point.z * scale};
}

float pointDot(const PathPoint &left, const PathPoint &right)
{
    return left.x * right.x + left.y * right.y + left.z * right.z;
}

PathPoint pointCross(const PathPoint &left, const PathPoint &right)
{
    return {
        left.y * right.z - left.z * right.y,
        left.z * right.x - left.x * right.z,
        left.x * right.y - left.y * right.x
    };
}

float pointLengthSquared(const PathPoint &point)
{
    return pointDot(point, point);
}

float pointLength(const PathPoint &point)
{
    return std::sqrt(pointLengthSquared(point));
}

PathPoint pointNormalize(const PathPoint &point)
{
    const float length = pointLength(point);

    if (length <= PathEpsilon)
    {
        return {};
    }

    return {point.x / length, point.y / length, point.z / length};
}

float xyDistance(const PathPoint &from, const PathPoint &to)
{
    const float dx = to.x - from.x;
    const float dy = to.y - from.y;
    return std::sqrt(dx * dx + dy * dy);
}

float maxWalkDropHeight(const PathObject &object)
{
    return std::max(object.stepHeight, std::min(object.radius, object.stepHeight * 2.0f));
}

bool walkStepDeltaAllowed(float fromZ, float toZ, const PathObject &object)
{
    const float deltaZ = toZ - fromZ;

    if (deltaZ > object.stepHeight + PathEpsilon)
    {
        return false;
    }

    if (-deltaZ > maxWalkDropHeight(object) + PathEpsilon)
    {
        return false;
    }

    return true;
}

float distanceSquared2d(const PathPoint &from, const PathPoint &to)
{
    const float dx = to.x - from.x;
    const float dy = to.y - from.y;
    return dx * dx + dy * dy;
}

PathPoint closestPointOnSegment2d(const PathPoint &point, const PathPoint &from, const PathPoint &to)
{
    const float edgeX = to.x - from.x;
    const float edgeY = to.y - from.y;
    const float edgeLengthSquared = edgeX * edgeX + edgeY * edgeY;

    if (edgeLengthSquared <= PathEpsilon)
    {
        return {from.x, from.y, point.z};
    }

    const float t =
        std::clamp(((point.x - from.x) * edgeX + (point.y - from.y) * edgeY) / edgeLengthSquared, 0.0f, 1.0f);
    return {from.x + edgeX * t, from.y + edgeY * t, point.z};
}

PathPoint interpolatePoint(const PathPoint &from, const PathPoint &to, float t)
{
    return {
        from.x + (to.x - from.x) * t,
        from.y + (to.y - from.y) * t,
        from.z + (to.z - from.z) * t
    };
}

PathBounds facetBounds(const PathFacet &facet)
{
    PathBounds bounds = {};

    if (facet.vertices.empty())
    {
        return bounds;
    }

    bounds.minX = facet.vertices.front().x;
    bounds.maxX = facet.vertices.front().x;
    bounds.minY = facet.vertices.front().y;
    bounds.maxY = facet.vertices.front().y;
    bounds.minZ = facet.vertices.front().z;
    bounds.maxZ = facet.vertices.front().z;

    for (const PathPoint &vertex : facet.vertices)
    {
        bounds.minX = std::min(bounds.minX, vertex.x);
        bounds.maxX = std::max(bounds.maxX, vertex.x);
        bounds.minY = std::min(bounds.minY, vertex.y);
        bounds.maxY = std::max(bounds.maxY, vertex.y);
        bounds.minZ = std::min(bounds.minZ, vertex.z);
        bounds.maxZ = std::max(bounds.maxZ, vertex.z);
    }

    return bounds;
}

PathBounds segmentBounds(const PathPoint &from, const PathPoint &to, float padding)
{
    PathBounds bounds = {};
    bounds.minX = std::min(from.x, to.x) - padding;
    bounds.maxX = std::max(from.x, to.x) + padding;
    bounds.minY = std::min(from.y, to.y) - padding;
    bounds.maxY = std::max(from.y, to.y) + padding;
    bounds.minZ = std::min(from.z, to.z) - padding;
    bounds.maxZ = std::max(from.z, to.z) + padding;
    return bounds;
}

bool boundsOverlap(const PathBounds &left, const PathBounds &right)
{
    return left.maxX + PathEpsilon >= right.minX
        && left.minX - PathEpsilon <= right.maxX
        && left.maxY + PathEpsilon >= right.minY
        && left.minY - PathEpsilon <= right.maxY
        && left.maxZ + PathEpsilon >= right.minZ
        && left.minZ - PathEpsilon <= right.maxZ;
}

bool pointIsFinite(const PathPoint &point)
{
    return std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z);
}

bool boundsAreFinite(const PathBounds &bounds)
{
    return std::isfinite(bounds.minX)
        && std::isfinite(bounds.maxX)
        && std::isfinite(bounds.minY)
        && std::isfinite(bounds.maxY)
        && std::isfinite(bounds.minZ)
        && std::isfinite(bounds.maxZ);
}

PathPoint computeFacetNormal(const PathFacet &facet)
{
    if (facet.vertices.size() < 3)
    {
        return {};
    }

    const PathPoint &origin = facet.vertices.front();

    for (size_t index = 1; index + 1 < facet.vertices.size(); ++index)
    {
        const PathPoint edgeA = pointSubtract(facet.vertices[index], origin);
        const PathPoint edgeB = pointSubtract(facet.vertices[index + 1], origin);
        const PathPoint normal = pointNormalize(pointCross(edgeA, edgeB));

        if (pointLengthSquared(normal) > PathEpsilon)
        {
            return normal;
        }
    }

    return {};
}

int dominantAxisToDrop(const PathPoint &normal)
{
    const float absX = std::fabs(normal.x);
    const float absY = std::fabs(normal.y);
    const float absZ = std::fabs(normal.z);

    if (absX >= absY && absX >= absZ)
    {
        return 0;
    }

    if (absY >= absZ)
    {
        return 1;
    }

    return 2;
}

void projectPoint(const PathPoint &point, int dropAxis, float &outX, float &outY)
{
    if (dropAxis == 0)
    {
        outX = point.y;
        outY = point.z;
        return;
    }

    if (dropAxis == 1)
    {
        outX = point.x;
        outY = point.z;
        return;
    }

    outX = point.x;
    outY = point.y;
}

bool projectedPointOnSegment(float px, float py, float ax, float ay, float bx, float by)
{
    const float cross = (px - ax) * (by - ay) - (py - ay) * (bx - ax);

    if (std::fabs(cross) > PathEpsilon)
    {
        return false;
    }

    return px >= std::min(ax, bx) - PathEpsilon
        && px <= std::max(ax, bx) + PathEpsilon
        && py >= std::min(ay, by) - PathEpsilon
        && py <= std::max(ay, by) + PathEpsilon;
}

bool pointInsideFacet(const PathPoint &point, const PathFacet &facet, const PathPoint &normal)
{
    if (facet.vertices.size() < 3)
    {
        return false;
    }

    const int dropAxis = dominantAxisToDrop(normal);
    float px = 0.0f;
    float py = 0.0f;
    projectPoint(point, dropAxis, px, py);

    bool inside = false;

    for (size_t index = 0; index < facet.vertices.size(); ++index)
    {
        const PathPoint &current = facet.vertices[index];
        const PathPoint &next = facet.vertices[(index + 1) % facet.vertices.size()];
        float currentX = 0.0f;
        float currentY = 0.0f;
        float nextX = 0.0f;
        float nextY = 0.0f;
        projectPoint(current, dropAxis, currentX, currentY);
        projectPoint(next, dropAxis, nextX, nextY);

        if (projectedPointOnSegment(px, py, currentX, currentY, nextX, nextY))
        {
            return true;
        }

        if ((currentY > py) == (nextY > py))
        {
            continue;
        }

        const float intersectionX = (nextX - currentX) * (py - currentY) / (nextY - currentY) + currentX;

        if (px < intersectionX)
        {
            inside = !inside;
        }
    }

    return inside;
}

bool segmentIntersectsFacet(
    const PathPoint &from,
    const PathPoint &to,
    const PathFacet &facet,
    const PathPoint &normal,
    PathPoint &outPoint
)
{
    if (facet.vertices.size() < 3 || pointLengthSquared(normal) <= PathEpsilon)
    {
        return false;
    }

    const PathPoint segment = pointSubtract(to, from);
    const float denominator = pointDot(normal, segment);

    if (std::fabs(denominator) <= PathEpsilon)
    {
        return false;
    }

    const float t = pointDot(normal, pointSubtract(facet.vertices.front(), from)) / denominator;

    if (t < -PathEpsilon || t > 1.0f + PathEpsilon)
    {
        return false;
    }

    outPoint = interpolatePoint(from, to, std::clamp(t, 0.0f, 1.0f));
    return pointInsideFacet(outPoint, facet, normal);
}

bool facetBlocksBodyOverlap(const PathFacet &facet, const PathPoint &normal)
{
    if (facet.kind == PathFacetKind::Floor
        || facet.kind == PathFacetKind::Ceiling
        || facet.kind == PathFacetKind::Portal
        || facet.walkableFloor)
    {
        return false;
    }

    if (facet.kind != PathFacetKind::Wall && std::fabs(normal.z) >= 0.5f)
    {
        return false;
    }

    return true;
}

bool segmentMovesIntoFacetBodyOverlap(
    const PathPoint &from,
    const PathPoint &to,
    const PathFacet &facet,
    const PathPoint &normal,
    float radius,
    PathPoint &outPoint
)
{
    if (radius <= PathEpsilon || !facetBlocksBodyOverlap(facet, normal))
    {
        return false;
    }

    const float fromSignedDistance = pointDot(normal, pointSubtract(from, facet.vertices.front()));
    const float toSignedDistance = pointDot(normal, pointSubtract(to, facet.vertices.front()));
    const float fromDistance = std::fabs(fromSignedDistance);
    const float toDistance = std::fabs(toSignedDistance);

    if (toDistance > radius + PathEpsilon)
    {
        return false;
    }

    const PathPoint projectedPoint = pointSubtract(to, pointScale(normal, toSignedDistance));

    if (!pointInsideFacet(projectedPoint, facet, normal))
    {
        return false;
    }

    const bool startedInsideWallRadius = fromDistance <= radius + 0.5f;
    const bool stayedOnSameSide =
        fromSignedDistance == 0.0f
        || toSignedDistance == 0.0f
        || (fromSignedDistance > 0.0f) == (toSignedDistance > 0.0f);
    const bool didNotMoveTowardWall = toDistance >= fromDistance - 0.5f;

    if (startedInsideWallRadius && stayedOnSameSide && didNotMoveTowardWall)
    {
        return false;
    }

    outPoint = projectedPoint;
    return true;
}

bool facetIsNonBlocking(const PathFacet &facet)
{
    return !facet.blocking
        || facet.attributes.portal
        || facet.kind == PathFacetKind::Portal
        || (facet.attributes.invisible && facet.attributes.untouchable);
}
}

void PathMap::clear()
{
    m_facets.clear();
    m_geometry.clear();
    m_sourceFacetIndex.clear();
    m_floorFacetIndices.clear();
    m_facetsBySourceId.clear();
    m_spatialGrid.clear();
    m_floorGrid.clear();
    m_overflowFacets.clear();
    m_overflowFloorFacets.clear();
    m_gridMemberships = 0;
    ++m_revision;
}

void PathMap::setFacets(std::vector<PathFacet> facets)
{
    m_facets.assign(std::move(facets));
    rebuildGeometry();
    ++m_revision;
}

size_t PathMap::addFacet(const PathFacet &facet)
{
    m_facets.push_back(facet);
    rebuildGeometry();
    ++m_revision;
    return m_facets.size() - 1;
}

void PathMap::buildSpatialGrid(float cellSize)
{
    m_gridCellSize = std::max(0.0f, cellSize);
    rebuildSpatialGrid();
}

const PathSnapshotVector<PathFacet> &PathMap::facets() const
{
    return m_facets;
}

size_t PathMap::revision() const
{
    return m_revision;
}

PathMap::QueryCacheScope::QueryCacheScope(const PathMap &map, QueryCache *cache)
    : m_previous(cachedQueryMap), m_previousCache(plannerQueryCache)
{
    if (!cache)
    {
        if (!fallbackQueryCache) { fallbackQueryCache = std::make_unique<QueryCache>(); }
        cache = fallbackQueryCache.get();
        cache->reset(); // Legacy one-shot scopes cannot retain a reused map address.
    }
    QueryCache::State &state = *cache->state;
    if (state.map != &map || state.revision != map.revision())
    {
        state.reset(); state.map = &map; state.revision = map.revision();
    }
    plannerQueryCache = &state;
    cachedQueryMap = &map;
}

PathMap::QueryCacheScope::~QueryCacheScope()
{
    if (plannerQueryCache == m_previousCache && m_previousCache)
    {
        // A nested legacy scope reused the fallback storage: outer-map entries are gone.
        m_previousCache->reset();
        m_previousCache->map = m_previous;
        m_previousCache->revision = m_previous ? m_previous->revision() : 0;
    }
    cachedQueryMap = m_previous;
    plannerQueryCache = m_previousCache;
}

PathFloorSample PathMap::floorAt(const PathPoint &position) const
{
    if (cachedQueryMap != this) { return debugFloorQuery(position).result; }
    auto &cache = *plannerQueryCache;
    const std::array<uint32_t, 3> key = {std::bit_cast<uint32_t>(position.x),
        std::bit_cast<uint32_t>(position.y), std::bit_cast<uint32_t>(position.z)};
    auto &entry = cache.floors[queryHash(key) & (cache.floors.size() - 1)];
    if (entry.epoch == cache.epoch && entry.key == key) { return entry.result; }
    const PathFloorSample result = debugFloorQuery(position).result;
    entry = {key, result, cache.epoch};
    return result;
}

PathPoint PathMap::snapToWalkableSourceFacet(
    int32_t sourceId,
    const PathPoint &position,
    float maxDistance,
    bool &valid,
    PathFloorSample *pFloorSample) const
{
    valid = false;

    if (sourceId < 0 || !pointIsFinite(position) || !std::isfinite(maxDistance) || maxDistance < 0.0f)
    {
        return position;
    }

    const auto sampleFacet = [&](size_t facetIndex, const PathPoint &probe, PathFloorSample &sample) -> bool
    {
        if (facetIndex >= m_facets.size() || facetIndex >= m_geometry.size())
        {
            return false;
        }

        const PathFacet &facet = m_facets[facetIndex];
        const FacetGeometry &geometry = m_geometry[facetIndex];

        if (!facet.walkableFloor || !geometry.valid || std::fabs(geometry.normal.z) <= PathEpsilon)
        {
            return false;
        }

        if (probe.x < geometry.bounds.minX - PathEpsilon
            || probe.x > geometry.bounds.maxX + PathEpsilon
            || probe.y < geometry.bounds.minY - PathEpsilon
            || probe.y > geometry.bounds.maxY + PathEpsilon)
        {
            return false;
        }

        const float z =
            facet.vertices.front().z
            - (geometry.normal.x * (probe.x - facet.vertices.front().x)
                + geometry.normal.y * (probe.y - facet.vertices.front().y)) / geometry.normal.z;
        const PathPoint floorPoint = {probe.x, probe.y, z};

        if (!pointInsideFacet(floorPoint, facet, geometry.normal))
        {
            return false;
        }

        sample.hasFloor = true;
        sample.inVoid = z > probe.z + PathEpsilon;
        sample.z = z;
        sample.normalZ = geometry.normal.z;
        sample.facetIndex = facetIndex;
        return true;
    };

    const auto *source = m_facetsBySourceId.get(sourceId);
    if (source == nullptr) return position;
    for (size_t facetIndex : *source)
    {
        const PathFacet &facet = m_facets[facetIndex];

        if (facet.sourceId != sourceId || !facet.walkableFloor || facet.vertices.empty())
        {
            continue;
        }

        PathFloorSample directSample = {};

        if (sampleFacet(facetIndex, position, directSample) && !directSample.inVoid)
        {
            valid = true;

            if (pFloorSample != nullptr)
            {
                *pFloorSample = directSample;
            }

            return {position.x, position.y, directSample.z};
        }

        const PathBounds &bounds = m_geometry[facetIndex].bounds;
        if (position.x < bounds.minX - maxDistance - PathEpsilon
            || position.x > bounds.maxX + maxDistance + PathEpsilon
            || position.y < bounds.minY - maxDistance - PathEpsilon
            || position.y > bounds.maxY + maxDistance + PathEpsilon) continue;

        PathPoint closestPoint = {facet.vertices.front().x, facet.vertices.front().y, position.z};
        float closestDistanceSquared = distanceSquared2d(position, closestPoint);
        PathPoint centroid = {};

        for (const PathPoint &vertex : facet.vertices)
        {
            centroid.x += vertex.x;
            centroid.y += vertex.y;
        }

        centroid.x /= static_cast<float>(facet.vertices.size());
        centroid.y /= static_cast<float>(facet.vertices.size());
        centroid.z = position.z;

        for (size_t vertexIndex = 0; vertexIndex < facet.vertices.size(); ++vertexIndex)
        {
            const PathPoint &from = facet.vertices[vertexIndex];
            const PathPoint &to = facet.vertices[(vertexIndex + 1) % facet.vertices.size()];
            const PathPoint edgePoint = closestPointOnSegment2d(position, from, to);
            const float edgeDistanceSquared = distanceSquared2d(position, edgePoint);

            if (edgeDistanceSquared < closestDistanceSquared)
            {
                closestDistanceSquared = edgeDistanceSquared;
                closestPoint = edgePoint;
            }
        }

        if (closestDistanceSquared > maxDistance * maxDistance)
        {
            continue;
        }

        const float inwardX = centroid.x - closestPoint.x;
        const float inwardY = centroid.y - closestPoint.y;
        const float inwardLength = std::sqrt(inwardX * inwardX + inwardY * inwardY);
        const std::array<float, 9> inwardDistances = {{
            128.0f,
            96.0f,
            64.0f,
            48.0f,
            32.0f,
            16.0f,
            8.0f,
            4.0f,
            0.0f
        }};

        for (float inwardDistance : inwardDistances)
        {
            PathPoint probe = closestPoint;

            if (inwardLength > PathEpsilon && inwardDistance > 0.0f)
            {
                probe.x += inwardX / inwardLength * inwardDistance;
                probe.y += inwardY / inwardLength * inwardDistance;
            }

            probe.z = position.z;
            PathFloorSample sample = {};

            if (!sampleFacet(facetIndex, probe, sample) || sample.inVoid)
            {
                continue;
            }

            valid = true;

            if (pFloorSample != nullptr)
            {
                *pFloorSample = sample;
            }

            return {probe.x, probe.y, sample.z};
        }
    }

    return position;
}

PathPoint PathMap::snapToNearestWalkableFloor(
    const PathPoint &position,
    float maxDistance,
    bool &valid,
    PathFloorSample *pFloorSample) const
{
    PathCooperationPoll cooperation;
    valid = false;

    if (!pointIsFinite(position) || !std::isfinite(maxDistance) || maxDistance < 0.0f)
    {
        return position;
    }

    PathPoint bestPoint = position;
    PathFloorSample bestSample = {};
    float bestDistanceSquared = std::numeric_limits<float>::max();

    static thread_local std::vector<size_t> nearbyFloors;
    candidateFloorFacetsForArea(position.x - maxDistance - PathEpsilon,
        position.y - maxDistance - PathEpsilon, position.x + maxDistance + PathEpsilon,
        position.y + maxDistance + PathEpsilon, nearbyFloors);
    for (size_t facetIndex : nearbyFloors)
    {
        cooperation.poll();
        if (facetIndex >= m_facets.size() || facetIndex >= m_geometry.size())
        {
            continue;
        }

        const PathFacet &facet = m_facets[facetIndex];
        const FacetGeometry &geometry = m_geometry[facetIndex];

        if (!facet.walkableFloor || !geometry.valid || std::fabs(geometry.normal.z) <= PathEpsilon)
        {
            continue;
        }

        const PathBounds &bounds = m_geometry[facetIndex].bounds;
        if (position.x < bounds.minX - maxDistance - PathEpsilon
            || position.x > bounds.maxX + maxDistance + PathEpsilon
            || position.y < bounds.minY - maxDistance - PathEpsilon
            || position.y > bounds.maxY + maxDistance + PathEpsilon) continue;

        PathPoint closestPoint = {facet.vertices.front().x, facet.vertices.front().y, position.z};
        float closestDistanceSquared = distanceSquared2d(position, closestPoint);
        PathPoint centroid = {};

        for (const PathPoint &vertex : facet.vertices)
        {
            cooperation.poll();
            centroid.x += vertex.x;
            centroid.y += vertex.y;
        }

        centroid.x /= static_cast<float>(facet.vertices.size());
        centroid.y /= static_cast<float>(facet.vertices.size());
        centroid.z = position.z;

        for (size_t vertexIndex = 0; vertexIndex < facet.vertices.size(); ++vertexIndex)
        {
            cooperation.poll();
            const PathPoint &from = facet.vertices[vertexIndex];
            const PathPoint &to = facet.vertices[(vertexIndex + 1) % facet.vertices.size()];
            const PathPoint edgePoint = closestPointOnSegment2d(position, from, to);
            const float edgeDistanceSquared = distanceSquared2d(position, edgePoint);

            if (edgeDistanceSquared < closestDistanceSquared)
            {
                closestDistanceSquared = edgeDistanceSquared;
                closestPoint = edgePoint;
            }
        }

        if (closestDistanceSquared > maxDistance * maxDistance
            || closestDistanceSquared >= bestDistanceSquared)
        {
            continue;
        }

        const float inwardX = centroid.x - closestPoint.x;
        const float inwardY = centroid.y - closestPoint.y;
        const float inwardLength = std::sqrt(inwardX * inwardX + inwardY * inwardY);
        const std::array<float, 9> inwardDistances = {{
            128.0f,
            96.0f,
            64.0f,
            48.0f,
            32.0f,
            16.0f,
            8.0f,
            4.0f,
            0.0f
        }};

        for (float inwardDistance : inwardDistances)
        {
            cooperation.poll();
            PathPoint probe = closestPoint;

            if (inwardLength > PathEpsilon && inwardDistance > 0.0f)
            {
                probe.x += inwardX / inwardLength * inwardDistance;
                probe.y += inwardY / inwardLength * inwardDistance;
            }

            probe.z = position.z;
            const PathFloorSample sample = floorAt(probe);

            if (!sample.hasFloor || sample.inVoid)
            {
                continue;
            }

            valid = true;
            bestDistanceSquared = closestDistanceSquared;
            bestSample = sample;
            bestPoint = {probe.x, probe.y, sample.z};
            break;
        }
    }

    if (!valid)
    {
        return position;
    }

    if (pFloorSample != nullptr)
    {
        *pFloorSample = bestSample;
    }

    return bestPoint;
}

PathFloorQueryDebug PathMap::debugFloorQuery(const PathPoint &position) const
{
    PathCooperationPoll cooperation;
    if (!pointIsFinite(position))
    {
        PathFloorQueryDebug debug = {};
        debug.position = position;
        debug.result.inVoid = true;
        return debug;
    }

    PathFloorQueryDebug debug = {};
    debug.position = position;
    float bestBelowDistance = std::numeric_limits<float>::max();
    float bestAboveDistance = std::numeric_limits<float>::max();

    const std::span<const size_t> candidates = candidateFloorFacetsForPoint(position.x, position.y);
    debug.candidateCount = candidates.size();

    for (size_t index : candidates)
    {
        cooperation.poll();
        if (index >= m_facets.size() || index >= m_geometry.size())
        {
            ++debug.invalidFacetCount;
            continue;
        }

        const PathFacet &facet = m_facets[index];
        const FacetGeometry &geometry = m_geometry[index];

        if (!facet.walkableFloor || !geometry.valid || std::fabs(geometry.normal.z) <= PathEpsilon)
        {
            ++debug.nonWalkableCount;
            continue;
        }

        if (position.x < geometry.bounds.minX - PathEpsilon
            || position.x > geometry.bounds.maxX + PathEpsilon
            || position.y < geometry.bounds.minY - PathEpsilon
            || position.y > geometry.bounds.maxY + PathEpsilon)
        {
            ++debug.boundsRejectCount;
            continue;
        }

        const float z =
            facet.vertices.front().z
            - (geometry.normal.x * (position.x - facet.vertices.front().x)
                + geometry.normal.y * (position.y - facet.vertices.front().y)) / geometry.normal.z;
        const PathPoint floorPoint = {position.x, position.y, z};

        if (!pointInsideFacet(floorPoint, facet, geometry.normal))
        {
            ++debug.polygonRejectCount;
            continue;
        }

        if (z <= position.z + PathEpsilon)
        {
            const float distance = position.z - z;
            ++debug.belowCount;

            if (distance < bestBelowDistance)
            {
                bestBelowDistance = distance;
                debug.bestBelow.hasFloor = true;
                debug.bestBelow.inVoid = false;
                debug.bestBelow.z = z;
                debug.bestBelow.normalZ = geometry.normal.z;
                debug.bestBelow.facetIndex = index;
            }
        }
        else
        {
            const float distance = z - position.z;
            ++debug.aboveCount;

            if (distance < bestAboveDistance)
            {
                bestAboveDistance = distance;
                debug.bestAbove.hasFloor = true;
                debug.bestAbove.inVoid = true;
                debug.bestAbove.z = z;
                debug.bestAbove.normalZ = geometry.normal.z;
                debug.bestAbove.facetIndex = index;
            }
        }
    }

    if (debug.bestBelow.hasFloor)
    {
        debug.result = debug.bestBelow;
        return debug;
    }

    if (debug.bestAbove.hasFloor)
    {
        debug.result = debug.bestAbove;
        return debug;
    }

    debug.result.inVoid = true;
    return debug;
}

PathTraceResult PathMap::traceLine(
    const PathPoint &from,
    const PathPoint &to,
    float radius,
    bool checkBody
) const
{
    if (!pointIsFinite(from) || !pointIsFinite(to) || !std::isfinite(radius))
    {
        PathTraceResult result = {};
        result.blocked = true;
        return result;
    }

    static thread_local TraceQueryCache cache;
    cache.clear();
    return traceLineInternal(from, to, radius, checkBody, cache);
}

PathTraceResult PathMap::traceLineInternal(const PathPoint &from, const PathPoint &to,
    float radius, bool checkBody, TraceQueryCache &cache) const
{
    PathCooperationPoll cooperation;
    std::array<std::pair<PathPoint, PathPoint>, 9> traces = {};
    size_t traceCount = 1;
    traces[0] = {from, to};

    const float xyLength = xyDistance(from, to);

    if (checkBody && radius > PathEpsilon && xyLength > PathEpsilon)
    {
        constexpr float DiagonalOffsetScale = 0.70710678f;
        const float forwardX = (to.x - from.x) / xyLength;
        const float forwardY = (to.y - from.y) / xyLength;
        const float sideX = -forwardY;
        const float sideY = forwardX;
        const std::array<PathPoint, 8> offsets = {{
            {sideX * radius, sideY * radius, 0.0f},
            {-sideX * radius, -sideY * radius, 0.0f},
            {forwardX * radius, forwardY * radius, 0.0f},
            {-forwardX * radius, -forwardY * radius, 0.0f},
            {(sideX + forwardX) * radius * DiagonalOffsetScale,
                (sideY + forwardY) * radius * DiagonalOffsetScale,
                0.0f},
            {(sideX - forwardX) * radius * DiagonalOffsetScale,
                (sideY - forwardY) * radius * DiagonalOffsetScale,
                0.0f},
            {(-sideX + forwardX) * radius * DiagonalOffsetScale,
                (-sideY + forwardY) * radius * DiagonalOffsetScale,
                0.0f},
            {(-sideX - forwardX) * radius * DiagonalOffsetScale,
                (-sideY - forwardY) * radius * DiagonalOffsetScale,
                0.0f}
        }};

        for (const PathPoint &offset : offsets)
        {
            cooperation.poll();
            traces[traceCount++] = {pointAdd(from, offset), pointAdd(to, offset)};
        }
    }

    PathTraceResult result = {};
    for (size_t traceIndex = 0; traceIndex < traceCount; ++traceIndex)
    {
        cooperation.poll();
        const std::pair<PathPoint, PathPoint> &trace = traces[traceIndex];
        const PathBounds bounds = segmentBounds(trace.first, trace.second, radius);
        const std::array<int32_t, 6> cells = {gridCoordinate(bounds.minX), gridCoordinate(bounds.maxX),
            gridCoordinate(bounds.minY), gridCoordinate(bounds.maxY),
            gridCoordinate(bounds.minZ), gridCoordinate(bounds.maxZ)};
        TraceQueryCache::Entry *pEntry = nullptr;
        for (auto &entry : cache.entries)
        {
            if (entry.valid && entry.cells == cells
                && bounds.minX >= entry.envelope.minX && bounds.maxX <= entry.envelope.maxX
                && bounds.minY >= entry.envelope.minY && bounds.maxY <= entry.envelope.maxY
                && bounds.minZ >= entry.envelope.minZ && bounds.maxZ <= entry.envelope.maxZ)
            { pEntry = &entry; break; }
        }
        if (pEntry == nullptr)
        {
            pEntry = &cache.entries[cache.next++ % cache.entries.size()];
            // Keep the original cell traversal order and exact per-ray bounds/polygon tests.
            candidateFacetsForBounds(bounds, pEntry->candidates, false);
            // Adjacent two-unit walk samples reuse this conservative local subset.
            // Preserve cell traversal order and the exact narrow-phase tests below.
            pEntry->envelope = bounds;
            pEntry->envelope.minX -= 32.0f; pEntry->envelope.maxX += 32.0f;
            pEntry->envelope.minY -= 32.0f; pEntry->envelope.maxY += 32.0f;
            pEntry->envelope.minZ -= 32.0f; pEntry->envelope.maxZ += 32.0f;
            std::erase_if(pEntry->candidates, [&](size_t index)
            {
                return index >= m_geometry.size() || !m_geometry[index].valid
                    || m_geometry[index].nonBlocking
                    || !boundsOverlap(pEntry->envelope, m_geometry[index].bounds);
            });
            pEntry->cells = cells;
            pEntry->valid = true;
        }

        for (size_t facetIndex : pEntry->candidates)
        {
            cooperation.poll();
            if (facetIndex >= m_facets.size() || facetIndex >= m_geometry.size())
            {
                continue;
            }

            const PathFacet &facet = m_facets[facetIndex];
            const FacetGeometry &geometry = m_geometry[facetIndex];

            if (!geometry.valid || geometry.nonBlocking || !boundsOverlap(bounds, geometry.bounds))
            {
                continue;
            }

            PathPoint hitPoint = {};

            if (segmentIntersectsFacet(trace.first, trace.second, facet, geometry.normal, hitPoint))
            {
                result.blocked = true;
                result.facetIndex = facetIndex;
                result.point = hitPoint;
                return result;
            }

            if (traceIndex == 0
                && checkBody
                && segmentMovesIntoFacetBodyOverlap(
                    trace.first,
                    trace.second,
                    facet,
                    geometry.normal,
                    radius,
                    hitPoint))
            {
                result.blocked = true;
                result.facetIndex = facetIndex;
                result.point = hitPoint;
                return result;
            }
        }
    }

    return result;
}

bool PathMap::traceWalkSegment(const PathPoint &from, const PathPoint &to, const PathObject &object,
    const std::atomic_bool *pCancelled) const
{
    if (pCancelled != nullptr && pCancelled->load(std::memory_order_relaxed)) { return false; }
    if (cachedQueryMap != this) { return traceWalkSegmentInternal(from, to, object, nullptr, pCancelled); }
    auto &cache = *plannerQueryCache;
    const std::array<uint32_t, 10> key = {std::bit_cast<uint32_t>(from.x), std::bit_cast<uint32_t>(from.y),
        std::bit_cast<uint32_t>(from.z), std::bit_cast<uint32_t>(to.x), std::bit_cast<uint32_t>(to.y),
        std::bit_cast<uint32_t>(to.z), std::bit_cast<uint32_t>(object.radius),
        std::bit_cast<uint32_t>(object.stepLength), std::bit_cast<uint32_t>(object.stepHeight), uint32_t(object.canFly)};
    auto &entry = cache.walks[queryHash(key) & (cache.walks.size() - 1)];
    if (entry.epoch == cache.epoch && entry.key == key) { return entry.result; }
    const bool result = traceWalkSegmentInternal(from, to, object, nullptr, pCancelled);
    if (pCancelled == nullptr || !pCancelled->load(std::memory_order_relaxed))
    { entry = {key, result, cache.epoch}; }
    return result;
}

PathWalkSegmentDebug PathMap::debugTraceWalkSegment(
    const PathPoint &from,
    const PathPoint &to,
    const PathObject &object
) const
{
    PathWalkSegmentDebug debug = {};
    debug.from = from;
    debug.to = to;

    traceWalkSegmentInternal(from, to, object, &debug);
    return debug;
}

bool PathMap::certifyFlatWalk(const PathPoint &from, const PathPoint &to, const PathObject &object,
    const PathFloorSample &floor, float supportMargin) const
{
    // A positive certificate only: ambiguous geometry uses the unchanged dense validator.
    // Do not infer support from endpoints on a concave floor or across a hole/stair/door.
    if (object.radius < 0 || object.stepHeight < 0 || floor.facetIndex >= m_geometry.size()) { return false; }
    const FacetGeometry &geometry = m_geometry[floor.facetIndex];
    const PathFacet &facet = m_facets[floor.facetIndex];
    if (!geometry.valid || facet.dynamic || geometry.bounds.minZ != geometry.bounds.maxZ
        || geometry.normal.x != 0 || geometry.normal.y != 0 || geometry.normal.z <= 0
        || !walkStepDeltaAllowed(floor.z, to.z, object)) { return false; }

    // Both endpoints must lie inside every edge half-plane. This proves the whole
    // straight segment has support even if a malformed polygon is not convex.
    float winding = 0;
    for (size_t i = 0; i < facet.vertices.size(); ++i)
    {
        const PathPoint &a = facet.vertices[i], &b = facet.vertices[(i + 1) % facet.vertices.size()];
        winding += a.x * b.y - b.x * a.y;
    }
    if (winding == 0) { return false; }
    const float sign = winding > 0 ? 1.0f : -1.0f;
    for (size_t i = 0; i < facet.vertices.size(); ++i)
    {
        const PathPoint &a = facet.vertices[i], &b = facet.vertices[(i + 1) % facet.vertices.size()];
        const float edgeLength = supportMargin > 0 ? xyDistance(a, b) : 0.0f;
        for (const PathPoint &point : {from, to})
        {
            if (sign * ((b.x - a.x) * (point.y - a.y) - (b.y - a.y) * (point.x - a.x))
                < PathEpsilon + supportMargin * edgeLength) { return false; }
        }
    }

    // Include the full nine-ray footprint plus its radius-expanded candidate bounds.
    // Any possible wall/ceiling/body contact or higher support disqualifies the shortcut.
    const float extent = object.radius * 2 + 1.0f;
    const PathBounds bounds = {std::min(from.x, to.x) - extent, std::max(from.x, to.x) + extent,
        std::min(from.y, to.y) - extent, std::max(from.y, to.y) + extent,
        floor.z - 1.0f, floor.z + std::max(object.radius * 2, object.stepHeight) + 1.0f};
    static thread_local std::vector<size_t> candidates;
    candidateFacetsForBounds(bounds, candidates);
    PathCooperationPoll cooperation;
    for (size_t index : candidates)
    {
        cooperation.poll();
        const FacetGeometry &other = m_geometry[index];
        const PathFacet &otherFacet = m_facets[index];
        if (!other.valid) { continue; }
        if (otherFacet.walkableFloor && other.bounds.minZ == floor.z && other.bounds.maxZ == floor.z)
        { continue; }
        if (otherFacet.walkableFloor || !other.nonBlocking) { return false; }
    }
    return true;
}

bool PathMap::certifyWalkCorridor(const PathPoint &from, const PathPoint &to, const PathObject &object,
    PathWalkCertificate &certificate) const
{
    certificate = {};
    constexpr float margin = 4.0f;
    if (object.canFly || !pointIsFinite(from) || !pointIsFinite(to)
        || !std::isfinite(object.radius) || !std::isfinite(object.stepHeight)
        || !std::isfinite(object.stepLength) || object.radius < 0 || object.stepHeight < 0)
        return false;
    const PathFloorSample floor = floorAt({from.x, from.y, from.z + object.stepHeight});
    if (!floor.hasFloor || floor.inVoid || from.z != floor.z || to.z != floor.z) return false;
    PathObject envelope = object;
    envelope.radius += margin;
    if (!certifyFlatWalk(from, to, envelope, floor, margin)) return false;
    certificate = {from, to, object, this, revision(), margin, true};
    return true;
}

bool PathMap::traceWalkSegmentInternal(
    const PathPoint &from,
    const PathPoint &to,
    const PathObject &object,
    PathWalkSegmentDebug *pDebug,
    const std::atomic_bool *pCancelled
) const
{
    if (!pointIsFinite(from)
        || !pointIsFinite(to)
        || !std::isfinite(object.radius)
        || !std::isfinite(object.stepLength)
        || !std::isfinite(object.stepHeight))
    {
        if (pDebug != nullptr)
        {
            pDebug->rejectReason = PathWalkRejectReason::InvalidInput;
        }

        return false;
    }

    const float distance = xyDistance(from, to);
    const float sampleStep = 2.0f;
    const size_t sampleCount = std::max<size_t>(1, static_cast<size_t>(std::ceil(distance / sampleStep)));
    const float stepHeight = std::max(0.0f, object.stepHeight);

    if (pDebug != nullptr)
    {
        pDebug->sampleCount = sampleCount;
    }

    static thread_local TraceQueryCache traceCache;
    traceCache.clear();
    PathPoint previousPoint = from;
    PathFloorSample previousFloor = floorAt({from.x, from.y, from.z + stepHeight});

    if (pDebug != nullptr)
    {
        pDebug->startFloor = previousFloor;
        pDebug->previousFloor = previousFloor;
    }

    if (!previousFloor.hasFloor)
    {
        if (pDebug != nullptr)
        {
            pDebug->rejectReason = PathWalkRejectReason::StartNoFloor;
        }

        return false;
    }

    if (previousFloor.inVoid)
    {
        if (pDebug != nullptr)
        {
            pDebug->rejectReason = PathWalkRejectReason::StartVoid;
        }

        return false;
    }

    if (distance >= 16.0f && certifyFlatWalk(from, to, object, previousFloor))
    {
        if (pCancelled != nullptr && pCancelled->load(std::memory_order_relaxed)) { return false; }
        if (pDebug != nullptr)
        {
            pDebug->success = true;
            pDebug->sampleCount = 0; // Certified continuous support; no distance samples needed.
            pDebug->previousFloor = previousFloor;
        }
        return true;
    }

    void (*const cooperate)() = pathCooperate;
    for (size_t sampleIndex = 1; sampleIndex <= sampleCount; ++sampleIndex)
    {
        // One A* expansion can itself contain thousands of samples. Yield outside all locks.
        if (cooperate && (sampleIndex & 15u) == 0) cooperate();
        if (pCancelled != nullptr && pCancelled->load(std::memory_order_relaxed)) { return false; }
        const float t = static_cast<float>(sampleIndex) / static_cast<float>(sampleCount);
        const PathPoint probe = interpolatePoint(from, to, t);
        const PathFloorSample floor = floorAt({probe.x, probe.y, previousFloor.z + stepHeight + PathEpsilon});

        if (pDebug != nullptr)
        {
            pDebug->sampleIndex = sampleIndex;
            pDebug->probe = probe;
            pDebug->failedFloor = floor;
            pDebug->previousFloor = previousFloor;
        }

        if (!floor.hasFloor)
        {
            if (pDebug != nullptr)
            {
                pDebug->rejectReason = PathWalkRejectReason::SampleNoFloor;
            }

            return false;
        }

        if (floor.inVoid)
        {
            if (pDebug != nullptr)
            {
                pDebug->rejectReason = PathWalkRejectReason::SampleVoid;
            }

            return false;
        }

        if (!walkStepDeltaAllowed(previousFloor.z, floor.z, object))
        {
            if (pDebug != nullptr)
            {
                pDebug->rejectReason = PathWalkRejectReason::StepHeight;
                pDebug->stepDeltaZ = std::fabs(floor.z - previousFloor.z);
            }

            return false;
        }

        const PathPoint segmentFrom = {previousPoint.x, previousPoint.y, previousFloor.z + object.radius};
        const PathPoint segmentTo = {probe.x, probe.y, floor.z + object.radius};
        const PathTraceResult traceResult = traceLineInternal(segmentFrom, segmentTo, object.radius, true, traceCache);

        if (traceResult.blocked)
        {
            if (pDebug != nullptr)
            {
                pDebug->rejectReason = PathWalkRejectReason::Blocked;
                pDebug->blockedFacet = traceResult.facetIndex;
                pDebug->blockedPoint = traceResult.point;
            }

            return false;
        }

        previousPoint = probe;
        previousFloor = floor;
    }

    if (!walkStepDeltaAllowed(previousFloor.z, to.z, object))
    {
        if (pDebug != nullptr)
        {
            pDebug->rejectReason = PathWalkRejectReason::StepHeight;
            pDebug->failedFloor = previousFloor;
            pDebug->stepDeltaZ = std::fabs(to.z - previousFloor.z);
        }

        return false;
    }

    if (pDebug != nullptr)
    {
        pDebug->success = true;
        pDebug->rejectReason = PathWalkRejectReason::None;
        pDebug->previousFloor = previousFloor;
    }

    return true;
}

bool PathMap::canReachDirectly(const PathPoint &from, const PathPoint &to, const PathObject &object,
    const std::atomic_bool *pCancelled) const
{
    if (pCancelled != nullptr && pCancelled->load(std::memory_order_relaxed)) { return false; }
    if (object.canFly)
    {
        return !traceLine(from, to, object.radius, true).blocked;
    }

    return traceWalkSegment(from, to, object, pCancelled);
}

void PathMap::candidateFacetsForBounds(const PathBounds &bounds, std::vector<size_t> &candidates, bool filterBounds) const
{
    PathCooperationPoll cooperation;
    candidates.clear();

    if (!boundsAreFinite(bounds))
    {
        return;
    }

    struct CandidateScratch
    {
        std::vector<uint32_t> marks;
        uint32_t markId = 0;
    };

    thread_local CandidateScratch scratch;

    if (m_gridCellSize <= PathEpsilon || m_spatialGrid.empty())
    {
        candidates.reserve(m_facets.size());

        for (size_t index = 0; index < m_geometry.size(); ++index)
        {
            cooperation.poll();
            if (!filterBounds || boundsOverlap(bounds, m_geometry[index].bounds))
            {
                candidates.push_back(index);
            }
        }

        return;
    }

    if (scratch.marks.size() != m_geometry.size())
    {
        scratch.marks.assign(m_geometry.size(), 0u);
        scratch.markId = 0;
    }

    ++scratch.markId;

    if (scratch.markId == 0)
    {
        std::fill(scratch.marks.begin(), scratch.marks.end(), 0u);
        ++scratch.markId;
    }

    const int32_t minCellX = gridCoordinate(bounds.minX);
    const int32_t maxCellX = gridCoordinate(bounds.maxX);
    const int32_t minCellY = gridCoordinate(bounds.minY);
    const int32_t maxCellY = gridCoordinate(bounds.maxY);
    const int32_t minCellZ = gridCoordinate(bounds.minZ);
    const int32_t maxCellZ = gridCoordinate(bounds.maxZ);

    uint64_t cells = 0;
    if (!boundedGridVolume(minCellX, maxCellX, minCellY, maxCellY, minCellZ, maxCellZ, cells))
    {
        candidates.reserve(m_facets.size());

        for (size_t index = 0; index < m_geometry.size(); ++index)
        {
            cooperation.poll();
            if (!filterBounds || boundsOverlap(bounds, m_geometry[index].bounds))
            {
                candidates.push_back(index);
            }
        }

        return;
    }

    for (int64_t cellX = minCellX; cellX <= maxCellX; ++cellX)
    {
        cooperation.poll();
        for (int64_t cellY = minCellY; cellY <= maxCellY; ++cellY)
        {
            cooperation.poll();
            for (int64_t cellZ = minCellZ; cellZ <= maxCellZ; ++cellZ)
            {
                cooperation.poll();
                const auto *cellIt = m_spatialGrid.get({int32_t(cellX), int32_t(cellY), int32_t(cellZ)});

                if (cellIt == nullptr)
                {
                    continue;
                }

                for (size_t facetIndex : *cellIt)
                {
                    cooperation.poll();
                    if (facetIndex >= m_geometry.size())
                    {
                        continue;
                    }

                    if (!filterBounds || boundsOverlap(bounds, m_geometry[facetIndex].bounds))
                    {
                        if (facetIndex < scratch.marks.size() && scratch.marks[facetIndex] != scratch.markId)
                        {
                            scratch.marks[facetIndex] = scratch.markId;
                            candidates.push_back(facetIndex);
                        }
                    }
                }
            }
        }
    }
    for (size_t index : m_overflowFacets)
    {
        cooperation.poll();
        if (!filterBounds || boundsOverlap(bounds, m_geometry[index].bounds)) { candidates.push_back(index); }
    }
}

void PathMap::candidateFloorFacetsForArea(float minX, float minY, float maxX, float maxY,
    std::vector<size_t> &candidates) const
{
    PathCooperationPoll cooperation;
    candidates.clear();
    const int32_t x0 = gridCoordinate(minX), x1 = gridCoordinate(maxX);
    const int32_t y0 = gridCoordinate(minY), y1 = gridCoordinate(maxY);
    uint64_t cells = 0;
    if (m_gridCellSize <= PathEpsilon || m_floorGrid.empty()
        || !std::isfinite(minX) || !std::isfinite(minY) || !std::isfinite(maxX) || !std::isfinite(maxY)
        || !boundedGridVolume(x0, x1, y0, y1, 0, 0, cells))
    {
        candidates.assign(m_floorFacetIndices.begin(), m_floorFacetIndices.end());
        return;
    }
    for (int64_t x = x0; x <= x1; ++x)
    {
        cooperation.poll();
        for (int64_t y = y0; y <= y1; ++y)
        {
            cooperation.poll();
            const auto *found = m_floorGrid.get({int32_t(x), int32_t(y)});
            if (found)
            { candidates.insert(candidates.end(), found->begin(), found->end()); }
        }
    }
    candidates.insert(candidates.end(), m_overflowFloorFacets.begin(), m_overflowFloorFacets.end());
    // Original floor order also preserves nearest-floor tie breaking.
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
}

std::span<const size_t> PathMap::candidateFloorFacetsForPoint(float x, float y) const
{
    if (!std::isfinite(x) || !std::isfinite(y))
    {
        return {};
    }

    if (m_gridCellSize <= PathEpsilon || m_floorGrid.empty())
    {
        return m_floorFacetIndices;
    }

    const auto *cellIt = m_floorGrid.get({gridCoordinate(x), gridCoordinate(y)});

    if (m_overflowFloorFacets.empty())
    {
        return cellIt == nullptr ? std::span<const size_t>{} : std::span<const size_t>{*cellIt};
    }
    static thread_local std::vector<size_t> merged;
    merged.clear();
    if (cellIt != nullptr) { merged.insert(merged.end(), cellIt->begin(), cellIt->end()); }
    for (size_t index : m_overflowFloorFacets)
    {
        const auto &b = m_geometry[index].bounds;
        if (x >= b.minX - PathEpsilon && x <= b.maxX + PathEpsilon
            && y >= b.minY - PathEpsilon && y <= b.maxY + PathEpsilon) { merged.push_back(index); }
    }
    std::sort(merged.begin(), merged.end());
    return merged;
}

void PathMap::rebuildGeometry()
{
    m_geometry.clear();
    m_sourceFacetIndex.clear();
    m_floorFacetIndices.clear();
    m_facetsBySourceId.clear();
    m_geometry.reserve(m_facets.size());
    m_floorFacetIndices.reserve(m_facets.size());

    for (size_t index = 0; index < m_facets.size(); ++index)
    {
        const PathFacet &facet = m_facets[index];
        m_sourceFacetIndex.insertIfAbsent(facet.sourceId,index);
        FacetGeometry geometry = {};
        geometry.bounds = facetBounds(facet);
        geometry.normal = computeFacetNormal(facet);
        geometry.valid = facet.vertices.size() >= 3 && pointLengthSquared(geometry.normal) > PathEpsilon;
        geometry.nonBlocking = facetIsNonBlocking(facet);

        if (facet.walkableFloor && geometry.valid)
        {
            m_floorFacetIndices.push_back(index);
            m_facetsBySourceId.edit(m_facets[index].sourceId).push_back(index);
        }

        m_geometry.push_back(geometry);
    }
    rebuildSpatialGrid();
}

void PathMap::rebuildSpatialGrid()
{
    m_spatialGrid.clear();
    m_floorGrid.clear();
    m_overflowFacets.clear();
    m_overflowFloorFacets.clear();
    m_gridMemberships = 0;

    if (m_gridCellSize <= PathEpsilon || m_geometry.empty())
    {
        return;
    }

    for (size_t index=0;index<m_geometry.size();++index) insertSpatialFacet(index);
}
void PathMap::insertSpatialFacet(size_t index)
{
    if (m_gridCellSize<=PathEpsilon) return;
        const FacetGeometry &geometry = m_geometry[index];

        if (!geometry.valid)
        {
            return;
        }

        if (!boundsAreFinite(geometry.bounds))
        {
            return;
        }

        const int32_t minCellX = gridCoordinate(geometry.bounds.minX);
        const int32_t maxCellX = gridCoordinate(geometry.bounds.maxX);
        const int32_t minCellY = gridCoordinate(geometry.bounds.minY);
        const int32_t maxCellY = gridCoordinate(geometry.bounds.maxY);
        const int32_t minCellZ = gridCoordinate(geometry.bounds.minZ);
        const int32_t maxCellZ = gridCoordinate(geometry.bounds.maxZ);

        uint64_t cells = 0;
        const uint64_t floorCells = uint64_t(int64_t(maxCellX) - minCellX + 1)
            * uint64_t(int64_t(maxCellY) - minCellY + 1);
        if (!boundedGridVolume(minCellX, maxCellX, minCellY, maxCellY, minCellZ, maxCellZ, cells)
            || cells + (m_facets[index].walkableFloor ? floorCells : 0) > MaxPathGridMemberships - m_gridMemberships)
        {
            m_overflowFacets.push_back(index);
            if (m_facets[index].walkableFloor) { m_overflowFloorFacets.push_back(index); }
            return;
        }
        m_gridMemberships += cells + (m_facets[index].walkableFloor ? floorCells : 0);

        for (int64_t cellX = minCellX; cellX <= maxCellX; ++cellX)
        {
            for (int64_t cellY = minCellY; cellY <= maxCellY; ++cellY)
            {
                for (int64_t cellZ = minCellZ; cellZ <= maxCellZ; ++cellZ)
                {
                    auto &members=m_spatialGrid.edit({int32_t(cellX),int32_t(cellY),int32_t(cellZ)});
                    members.insert(std::lower_bound(members.begin(),members.end(),index),index);
                }
            }
        }

        if (!m_facets[index].walkableFloor)
        {
            return;
        }

        for (int64_t cellX = minCellX; cellX <= maxCellX; ++cellX)
        {
            for (int64_t cellY = minCellY; cellY <= maxCellY; ++cellY)
            {
                auto &members=m_floorGrid.edit({int32_t(cellX),int32_t(cellY)});
                members.insert(std::lower_bound(members.begin(),members.end(),index),index);
            }
        }
 }

void PathMap::removeSpatialFacet(size_t index)
{
    if (m_gridCellSize<=PathEpsilon || !m_geometry[index].valid) return;
    auto overflow=std::find(m_overflowFacets.begin(),m_overflowFacets.end(),index);
    if (overflow!=m_overflowFacets.end()) {
        m_overflowFacets.erase(overflow); std::erase(m_overflowFloorFacets,index); return;
    }
    const auto &bounds=m_geometry[index].bounds;
    if (!boundsAreFinite(bounds)) return;
    const int32_t x0=gridCoordinate(bounds.minX), x1=gridCoordinate(bounds.maxX);
    const int32_t y0=gridCoordinate(bounds.minY), y1=gridCoordinate(bounds.maxY);
    const int32_t z0=gridCoordinate(bounds.minZ), z1=gridCoordinate(bounds.maxZ);
    uint64_t cells=0;
    if (!boundedGridVolume(x0,x1,y0,y1,z0,z1,cells)) return;
    const auto remove=[&](auto &grid, const auto &key) {
        if (!grid.get(key)) return;
        auto &members=grid.edit(key);
        auto pos=std::lower_bound(members.begin(),members.end(),index);
        if(pos!=members.end() && *pos==index) { members.erase(pos); --m_gridMemberships; }
        if(members.empty()) grid.erase(key);
    };
    for(int64_t x=x0;x<=x1;++x) for(int64_t y=y0;y<=y1;++y) {
        for(int64_t z=z0;z<=z1;++z) remove(m_spatialGrid,GridCellKey{int32_t(x),int32_t(y),int32_t(z)});
        if(m_facets[index].walkableFloor) remove(m_floorGrid,FloorCellKey{int32_t(x),int32_t(y)});
    }
}
void PathMap::replaceSourceFacet(PathFacet facet)
{
    const auto *found=m_sourceFacetIndex.get(facet.sourceId);
    size_t index;
    if(!found) {
        index=m_facets.size(); m_sourceFacetIndex.edit(facet.sourceId)=index;
        m_facets.emplace_back(); m_geometry.emplace_back();
    } else {
        index=*found; removeSpatialFacet(index);
        std::erase(m_floorFacetIndices,index);
        if(m_facetsBySourceId.get(facet.sourceId)) {
            auto &floors=m_facetsBySourceId.edit(facet.sourceId);
            std::erase(floors,index); if(floors.empty()) m_facetsBySourceId.erase(facet.sourceId);
        }
    }
    m_facets.edit(index)=std::move(facet);
    const auto &value=m_facets[index];
    auto &geometry=m_geometry.edit(index);
    geometry.bounds=facetBounds(value); geometry.normal=computeFacetNormal(value);
    geometry.valid=value.vertices.size()>=3 && pointLengthSquared(geometry.normal)>PathEpsilon;
    geometry.nonBlocking=facetIsNonBlocking(value);
    if(value.walkableFloor && geometry.valid) {
        m_floorFacetIndices.insert(std::lower_bound(m_floorFacetIndices.begin(),m_floorFacetIndices.end(),index),index);
        auto &floors=m_facetsBySourceId.edit(value.sourceId);
        floors.insert(std::lower_bound(floors.begin(),floors.end(),index),index);
    }
    insertSpatialFacet(index); ++m_revision;
}

int32_t PathMap::gridCoordinate(float value) const
{
    if (m_gridCellSize <= PathEpsilon || !std::isfinite(value))
    {
        return 0;
    }

    const float coordinate = std::floor(value / m_gridCellSize);

    if (!std::isfinite(coordinate))
    {
        return 0;
    }

    if (coordinate <= static_cast<float>(std::numeric_limits<int32_t>::min()))
    {
        return std::numeric_limits<int32_t>::min();
    }

    if (coordinate >= static_cast<float>(std::numeric_limits<int32_t>::max()))
    {
        return std::numeric_limits<int32_t>::max();
    }

    return static_cast<int32_t>(coordinate);
}

bool PathMap::GridCellKey::operator==(const GridCellKey &other) const
{
    return x == other.x && y == other.y && z == other.z;
}

size_t PathMap::GridCellKeyHash::operator()(const GridCellKey &key) const
{
    const uint32_t x = static_cast<uint32_t>(key.x) * 73856093u;
    const uint32_t y = static_cast<uint32_t>(key.y) * 19349663u;
    const uint32_t z = static_cast<uint32_t>(key.z) * 83492791u;
    return static_cast<size_t>(x ^ y ^ z);
}

bool PathMap::FloorCellKey::operator==(const FloorCellKey &other) const
{
    return x == other.x && y == other.y;
}

size_t PathMap::FloorCellKeyHash::operator()(const FloorCellKey &key) const
{
    const uint32_t x = static_cast<uint32_t>(key.x) * 73856093u;
    const uint32_t y = static_cast<uint32_t>(key.y) * 19349663u;
    return static_cast<size_t>(x ^ y);
}
}
