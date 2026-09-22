#include "engine/VitaFrameProfile.h"
#include "game/pathfinding/PathPlanner.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <queue>
#include <atomic>

namespace OpenYAMM::Game
{
namespace
{
constexpr float PlannerEpsilon = 0.0001f;
constexpr size_t InvalidNodeIndex = std::numeric_limits<size_t>::max();

struct NodeKey
{
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;

    bool operator==(const NodeKey &other) const
    {
        return x == other.x && y == other.y && z == other.z;
    }
};

struct NodeKeyHash
{
    size_t operator()(const NodeKey &key) const
    {
        const uint32_t x = static_cast<uint32_t>(key.x) * 73856093u;
        const uint32_t y = static_cast<uint32_t>(key.y) * 19349663u;
        const uint32_t z = static_cast<uint32_t>(key.z) * 83492791u;
        return static_cast<size_t>(x ^ y ^ z);
    }
};

struct SearchNode
{
    NodeKey key;
    PathPoint point;
    float cost = 0.0f;
    float estimate = 0.0f;
    size_t parentIndex = InvalidNodeIndex;
    size_t floorFacetIndex = static_cast<size_t>(-1);
    bool closed = false;
};

// Indices reference the existing node vector; insertion never copies or allocates a node.
class NodeIndex
{
public:
    void clear() { std::fill(m_slots.begin(), m_slots.end(), 0); m_count = 0; }
    void trim() { if (m_slots.capacity() > 131072) { std::vector<uint32_t>().swap(m_slots); m_count = 0; } }
    size_t find(const NodeKey &key, const std::vector<SearchNode> &nodes) const
    {
        if (m_slots.empty()) { return InvalidNodeIndex; }
        size_t slot = NodeKeyHash{}(key) & (m_slots.size() - 1);
        while (m_slots[slot] != 0)
        {
            const size_t index = m_slots[slot] - 1;
            if (nodes[index].key == key) { return index; }
            slot = (slot + 1) & (m_slots.size() - 1);
        }
        return InvalidNodeIndex;
    }
    void insert(size_t index, const std::vector<SearchNode> &nodes)
    {
        if ((m_count + 1) * 2 >= m_slots.size())
        {
            std::vector<uint32_t> old = std::move(m_slots);
            m_slots.assign(std::max<size_t>(256, old.size() * 2), 0);
            for (uint32_t value : old) { if (value) { place(value - 1, nodes); } }
        }
        place(index, nodes);
        ++m_count;
    }
private:
    void place(size_t index, const std::vector<SearchNode> &nodes)
    {
        size_t slot = NodeKeyHash{}(nodes[index].key) & (m_slots.size() - 1);
        while (m_slots[slot]) { slot = (slot + 1) & (m_slots.size() - 1); }
        m_slots[slot] = static_cast<uint32_t>(index + 1);
    }
    std::vector<uint32_t> m_slots;
    size_t m_count = 0;
};

struct OpenNode
{
    float priority = 0.0f;
    size_t nodeIndex = 0;

    bool operator<(const OpenNode &other) const
    {
        return priority > other.priority;
    }
};

float distanceSquared2d(const PathPoint &from, const PathPoint &to)
{
    const float dx = to.x - from.x;
    const float dy = to.y - from.y;
    return dx * dx + dy * dy;
}

float distanceSquared3d(const PathPoint &from, const PathPoint &to)
{
    const float dx = to.x - from.x;
    const float dy = to.y - from.y;
    const float dz = to.z - from.z;
    return dx * dx + dy * dy + dz * dz;
}

float distance2d(const PathPoint &from, const PathPoint &to)
{
    return std::sqrt(distanceSquared2d(from, to));
}

float distance3d(const PathPoint &from, const PathPoint &to)
{
    return std::sqrt(distanceSquared3d(from, to));
}

float maxWalkDropHeight(const PathObject &object)
{
    return std::max(object.stepHeight, std::min(object.radius, object.stepHeight * 2.0f));
}

bool walkStepDeltaAllowed(float fromZ, float toZ, const PathObject &object)
{
    const float deltaZ = toZ - fromZ;

    if (deltaZ > object.stepHeight + PlannerEpsilon)
    {
        return false;
    }

    if (-deltaZ > maxWalkDropHeight(object) + PlannerEpsilon)
    {
        return false;
    }

    return true;
}

void updateBestDebugPoint(PathPlanDebugInfo &debug, const SearchNode &node, const PathPoint &target)
{
    debug.bestPoint = node.point;
    debug.bestFloorFacet = node.floorFacetIndex;
    debug.bestDistance2d = distance2d(node.point, target);
    debug.bestDistance3d = distance3d(node.point, target);
}

NodeKey makeNodeKey(const PathPoint &point, float stepSize)
{
    return {
        static_cast<int32_t>(std::lround(point.x / stepSize)),
        static_cast<int32_t>(std::lround(point.y / stepSize)),
        static_cast<int32_t>(std::lround(point.z / stepSize))
    };
}

float heuristic(const PathPoint &from, const PathPoint &to, bool canFly)
{
    if (canFly)
    {
        return distanceSquared3d(from, to);
    }

    const float uphill = std::max(0.0f, to.z - from.z);
    return distanceSquared2d(from, to) + uphill * uphill * 4.0f;
}

float travelCost(const PathPoint &from, const PathPoint &to, bool canFly)
{
    if (canFly)
    {
        return distance3d(from, to);
    }

    const float uphill = std::max(0.0f, to.z - from.z);
    return distance2d(from, to) + uphill * 2.0f;
}

float partialProgressScore(
    const PathPoint &start,
    const PathPoint &point,
    const PathPoint &target,
    const PathObject &object
)
{
    if (object.canFly)
    {
        return heuristic(point, target, true);
    }

    const float targetRise = target.z - start.z;

    if (targetRise <= object.stepHeight)
    {
        return heuristic(point, target, false);
    }

    const float remainingRise = std::max(0.0f, target.z - point.z);
    const float droppedBelowStart = std::max(0.0f, start.z - point.z - object.stepHeight);
    return distanceSquared2d(point, target)
        + remainingRise * remainingRise * 16.0f
        + droppedBelowStart * droppedBelowStart * 64.0f;
}

PathPoint snapGroundPoint(
    const PathMap &map,
    const PathPoint &point,
    const PathObject &object,
    bool &valid,
    PathFloorSample *pFloorSample = nullptr,
    int32_t preferredSourceFacetSourceId = -1,
    bool *pPreferredSnapUsed = nullptr,
    float requestedNearbySnapDistance = 0.0f
)
{
    const PathFloorSample floor = map.floorAt({point.x, point.y, point.z + object.stepHeight});

    if (pFloorSample != nullptr)
    {
        *pFloorSample = floor;
    }

    if (!floor.hasFloor || floor.inVoid)
    {
        const float sourceSnapDistance = std::max(4.0f, object.radius + 4.0f);

        if (preferredSourceFacetSourceId >= 0)
        {
            PathFloorSample preferredFloor = {};
            bool preferredValid = false;
            const PathPoint snapped =
                map.snapToWalkableSourceFacet(
                    preferredSourceFacetSourceId,
                    {point.x, point.y, point.z + object.stepHeight},
                    sourceSnapDistance,
                    preferredValid,
                    &preferredFloor);

            if (preferredValid)
            {
                valid = true;

                if (pFloorSample != nullptr)
                {
                    *pFloorSample = preferredFloor;
                }

                if (pPreferredSnapUsed != nullptr)
                {
                    *pPreferredSnapUsed = true;
                }

                return snapped;
            }
        }

        PathFloorSample nearbyFloor = {};
        bool nearbyValid = false;
        const float nearbySnapDistance =
            std::max({sourceSnapDistance, object.radius + object.stepLength, requestedNearbySnapDistance});
        const PathPoint nearbySnapped =
            map.snapToNearestWalkableFloor(
                {point.x, point.y, point.z + object.stepHeight},
                nearbySnapDistance,
                nearbyValid,
                &nearbyFloor);

        if (nearbyValid)
        {
            valid = true;

            if (pFloorSample != nullptr)
            {
                *pFloorSample = nearbyFloor;
            }

            if (pPreferredSnapUsed != nullptr)
            {
                *pPreferredSnapUsed = true;
            }

            return nearbySnapped;
        }

        valid = false;
        return point;
    }

    valid = true;
    return {point.x, point.y, floor.z};
}

bool flyingPointIsValid(const PathMap &map, const PathPoint &point)
{
    const PathFloorSample floor = map.floorAt(point);
    return floor.hasFloor && !floor.inVoid;
}

std::vector<PathPoint> rebuildPathToNode(const std::vector<SearchNode> &nodes, size_t nodeIndex)
{
    static thread_local std::vector<PathPoint> reversed;
    reversed.clear();
    while (nodeIndex != InvalidNodeIndex)
    {
        reversed.push_back(nodes[nodeIndex].point);
        nodeIndex = nodes[nodeIndex].parentIndex;
    }
    std::vector<PathPoint> waypoints = pathBufferPool()->take(reversed.size()+1);
    waypoints.clear();
    for (size_t index = reversed.size(); index > 1; --index)
        waypoints.push_back(reversed[index - 2]);

    return waypoints;
}

std::vector<PathPoint> rebuildPath(const std::vector<SearchNode> &nodes, size_t nodeIndex, const PathPoint &target)
{
    std::vector<PathPoint> waypoints = rebuildPathToNode(nodes, nodeIndex);
    waypoints.push_back(target);
    return waypoints;
}
}

struct PathSearch::State
{
    struct OpenQueue : std::priority_queue<OpenNode>
    {
        void clear() { c.clear(); }
        void trim() { if (c.capacity() > 65536) { std::vector<OpenNode>().swap(c); } }
    };
    const PathMap *pMap = nullptr;
    PathPlanRequest request{};
    PathPlanResult result{};
    std::vector<SearchNode> nodes;
    NodeIndex nodeIndexByKey;
    OpenQueue openSet;
    PathMap::QueryCache queryCache;
    PathFloorSample startFloor{}, targetFloor{};
    bool startValid = true;
    unsigned initializationPhase = 0;
    PathPoint start{}, target{};
    float stepSize = 0, bestPartialScore = 0;
    size_t bestPartialNodeIndex = 0;
    bool targetValid = true, initialized = false, finished = false;
    bool advance(size_t nodeBudget);
};

PathSearch::PathSearch() : m_state(std::make_unique<State>()) {}
PathSearch::~PathSearch() = default;
PathSearch::PathSearch(PathSearch &&) noexcept = default;
PathSearch &PathSearch::operator=(PathSearch &&) noexcept = default;
void PathSearch::reset(const PathMap &map, const PathPlanRequest &request)
{
    State &state = *m_state;
    state.pMap = &map; state.request = request; state.result = {};
    state.result.mapRevision = request.mapRevision;
    state.initialized = state.finished = false;
    state.initializationPhase = 0; state.queryCache.reset();
    if (state.nodes.capacity() > 65536) { std::vector<SearchNode>().swap(state.nodes); }
    state.nodeIndexByKey.trim(); state.openSet.trim();
    state.nodes.clear(); state.nodeIndexByKey.clear(); state.openSet.clear();
    state.nodes.reserve(std::min<size_t>(request.nodeLimit, 256));
}
bool PathSearch::advance(size_t nodeBudget) { return m_state->advance(std::max<size_t>(1, nodeBudget)); }
PathPlanResult PathSearch::takeResult() { return std::move(m_state->result); }
size_t PathSearch::analyzedNodes() const { return m_state->result.analyzedNodeCount; }
bool PathSearch::finishProgress(float minimumDistance)
{
    State &s = *m_state;
    if (s.finished || !s.initialized || !s.request.allowPartialPath || s.bestPartialNodeIndex == 0
        || (s.request.pCancelled && s.request.pCancelled->load(std::memory_order_relaxed))) { return false; }
    const PathPoint &end = s.nodes[s.bestPartialNodeIndex].point;
    const float distance = s.request.object.canFly ? distance3d(s.start, end) : distance2d(s.start, end);
    if (distance < minimumDistance) { return false; } // Keep this search's frontier; do not deliver tiny prefixes.
    s.result.waypoints = rebuildPathToNode(s.nodes, s.bestPartialNodeIndex);
    if (s.result.waypoints.empty()) { return false; }
    s.result.status = PathPlanStatus::Partial;
    s.finished = true;
    return true;
}
void PathSearch::finishBudget()
{
    State &s = *m_state;
    if (s.finished) { return; }
    s.result.status = s.request.pCancelled && s.request.pCancelled->load(std::memory_order_relaxed)
        ? PathPlanStatus::Cancelled : PathPlanStatus::NodeLimitExceeded;
    s.finished = true;
}

bool PathSearch::State::advance(size_t nodeBudget)
{
    if (finished) { return true; }
    const PathMap &map = *pMap;
    PathCooperationScope cooperation(request.cooperate);
    PathMap::QueryCacheScope queryCacheScope(map, &queryCache);
    const auto cancelled = [&]()
    {
        return request.pCancelled != nullptr && request.pCancelled->load(std::memory_order_relaxed);
    };
    if (cancelled()) { result.status = PathPlanStatus::Cancelled; finished = true; return true; }
    if (!initialized)
    {
        if (initializationPhase == 0)
        {
            result.debug.requestSource = request.source;
            result.debug.requestTarget = request.target;
            result.debug.preferredSourceFacetSourceId = request.preferredSourceFacetSourceId;
            stepSize = std::max(request.object.stepLength, 24.0f);
            start = request.source; target = request.target;
            startValid = targetValid = true;
            startFloor = {}; targetFloor = {};
            if (!request.object.canFly)
            {
                bool preferred = false;
                start = snapGroundPoint(map, request.source, request.object, startValid, &startFloor,
                    request.preferredSourceFacetSourceId, &preferred, request.sourceSnapDistance);
                result.debug.preferredSourceSnapUsed = preferred;
            }
            else
            {
                startFloor = map.floorAt(request.source);
                startValid = startFloor.hasFloor && !startFloor.inVoid;
            }
            initializationPhase = 1;
            return false;
        }
        if (initializationPhase == 1)
        {
            if (!request.object.canFly)
            {
                target = snapGroundPoint(map, request.target, request.object, targetValid, &targetFloor);
            }
            else
            {
                targetFloor = map.floorAt(request.target);
                targetValid = targetFloor.hasFloor && !targetFloor.inVoid;
            }
            initializationPhase = 2;
            return false;
        }
    result.debug.sourceValid = startValid;
    result.debug.targetValid = targetValid;
    result.debug.snappedSource = start;
    result.debug.snappedTarget = target;

    if (startFloor.hasFloor)
    {
        result.debug.sourceFloorFacet = startFloor.facetIndex;
    }

    if (targetFloor.hasFloor)
    {
        result.debug.targetFloorFacet = targetFloor.facetIndex;
    }

    if (!startValid || (!targetValid && !request.allowPartialPath))
    {
        result.debug.bestPoint = start;
        result.debug.bestDistance2d = distance2d(start, target);
        result.debug.bestDistance3d = distance3d(start, target);
        result.status = PathPlanStatus::NoRoute;
        finished = true; return true;
    }

    if (request.allowDirect && targetValid && map.canReachDirectly(start, target, request.object, request.pCancelled))
    {
        result.status = PathPlanStatus::Success;
        result.debug.directReachable = true;
        result.debug.bestPoint = target;
        result.debug.bestFloorFacet = targetFloor.hasFloor ? targetFloor.facetIndex : static_cast<size_t>(-1);
        result.debug.bestDistance2d = 0.0f;
        result.debug.bestDistance3d = 0.0f;
        result.waypoints.push_back(target);
        finished = true; return true;
    }

    SearchNode startNode = {};
    startNode.key = makeNodeKey(start, stepSize);
    startNode.point = start;
    startNode.estimate = heuristic(start, target, request.object.canFly);
    startNode.floorFacetIndex = startFloor.hasFloor ? startFloor.facetIndex : static_cast<size_t>(-1);
    nodes.push_back(startNode);
    nodeIndexByKey.insert(0, nodes);
    openSet.push({startNode.estimate, 0});
    bestPartialNodeIndex = 0;
    bestPartialScore = partialProgressScore(start, start, target, request.object);
    initialized = true;
    updateBestDebugPoint(result.debug, startNode, target);
    return false; // Direct test and setup do not also consume an A* slice.
    }
    const int directions2d[8][2] = {
        {-1, -1},
        {-1, 0},
        {-1, 1},
        {0, -1},
        {0, 1},
        {1, -1},
        {1, 0},
        {1, 1}
    };

    const int directions3d[3] = {-1, 0, 1};

    size_t expanded = 0;
    while (!openSet.empty())
    {
        if (cancelled()) { result.status = PathPlanStatus::Cancelled; finished = true; return true; }
        if (expanded >= nodeBudget) { return false; }
        if (request.cooperate) { request.cooperate(); }
        const OpenNode openNode = openSet.top();
        openSet.pop();
        SearchNode &currentNode = nodes[openNode.nodeIndex];

        if (currentNode.closed)
        {
            ++result.debug.skippedClosedNodes;
            continue;
        }

        currentNode.closed = true;
        const PathPoint currentPoint = currentNode.point;
        const float currentCost = currentNode.cost;
        ++result.analyzedNodeCount;
        ++expanded;

        const float currentPartialScore = partialProgressScore(start, currentPoint, target, request.object);

        if (currentPartialScore < bestPartialScore)
        {
            bestPartialScore = currentPartialScore;
            bestPartialNodeIndex = openNode.nodeIndex;
            updateBestDebugPoint(result.debug, currentNode, target);
        }

        if (result.analyzedNodeCount > request.nodeLimit)
        {
            if (request.allowPartialPath && bestPartialNodeIndex != 0)
            {
                result.waypoints = rebuildPathToNode(nodes, bestPartialNodeIndex);

                if (!result.waypoints.empty())
                {
                    result.status = PathPlanStatus::Partial;
                    finished = true; return true;
                }
            }

            result.status = PathPlanStatus::NodeLimitExceeded;
            finished = true; return true;
        }

        const float closeEnoughDistance = stepSize * 4.0f;

        if (targetValid
            && (request.allowDirect || openNode.nodeIndex != 0)
            && distance3d(currentPoint, target) <= closeEnoughDistance
            && map.canReachDirectly(currentPoint, target, request.object, request.pCancelled))
        {
            result.status = PathPlanStatus::Success;
            result.waypoints = rebuildPath(nodes, openNode.nodeIndex, target);
            finished = true; return true;
        }

        auto visitCandidate =
            [&](PathPoint candidate) -> void
        {
            if (request.cooperate) { request.cooperate(); }
            ++result.debug.generatedCandidates;
            PathFloorSample candidateFloor = {};

            if (request.object.canFly)
            {
                if (!flyingPointIsValid(map, candidate))
                {
                    ++result.debug.rejectedFlyingInvalid;
                    return;
                }
            }
            else
            {
                bool candidateValid = false;
                candidate = snapGroundPoint(map, candidate, request.object, candidateValid, &candidateFloor);

                if (!candidateValid)
                {
                    ++result.debug.rejectedNoFloor;
                    return;
                }

                const float stepDeltaZ = std::fabs(candidate.z - currentPoint.z);

                if (!walkStepDeltaAllowed(currentPoint.z, candidate.z, request.object))
                {
                    ++result.debug.rejectedStepHeight;

                    if (stepDeltaZ > result.debug.maxRejectedStepDeltaZ)
                    {
                        result.debug.maxRejectedStepDeltaZ = stepDeltaZ;
                        result.debug.maxStepRejectFrom = currentPoint;
                        result.debug.maxStepRejectTo = candidate;
                    }

                    return;
                }
            }

            const NodeKey key = makeNodeKey(candidate, stepSize);
            const float newCost =
                currentCost + travelCost(currentPoint, candidate, request.object.canFly);
            const auto found = nodeIndexByKey.find(key, nodes);

            if (found != InvalidNodeIndex)
            {
                SearchNode &knownNode = nodes[found];

                if (knownNode.closed || newCost >= knownNode.cost)
                {
                    ++result.debug.rejectedDuplicate;
                    return;
                }
            }

            if (request.object.canFly)
            {
                if (map.traceLine(currentPoint, candidate, request.object.radius, true).blocked)
                {
                    ++result.debug.rejectedWalkSegment;
                    return;
                }
            }
            else if (!map.traceWalkSegment(currentPoint, candidate, request.object, request.pCancelled))
            {
                ++result.debug.rejectedWalkSegment;
                return;
            }

            if (found != InvalidNodeIndex)
            {
                SearchNode &knownNode = nodes[found];

                knownNode.point = candidate;
                knownNode.cost = newCost;
                knownNode.estimate = newCost + heuristic(candidate, target, request.object.canFly);
                knownNode.parentIndex = openNode.nodeIndex;
                knownNode.floorFacetIndex = request.object.canFly
                    ? static_cast<size_t>(-1)
                    : candidateFloor.facetIndex;
                openSet.push({knownNode.estimate, found});
                ++result.debug.acceptedCandidates;
                ++result.debug.reopenedCandidates;
                return;
            }

            SearchNode nextNode = {};
            nextNode.key = key;
            nextNode.point = candidate;
            nextNode.cost = newCost;
            nextNode.estimate = newCost + heuristic(candidate, target, request.object.canFly);
            nextNode.parentIndex = openNode.nodeIndex;
            nextNode.floorFacetIndex = request.object.canFly
                ? static_cast<size_t>(-1)
                : candidateFloor.facetIndex;
            const size_t nextIndex = nodes.size();
            nodes.push_back(nextNode);
            nodeIndexByKey.insert(nextIndex, nodes);
            openSet.push({nextNode.estimate, nextIndex});
            ++result.debug.acceptedCandidates;
        };

        if (request.object.canFly)
        {
            for (int dx : directions3d)
            {
                for (int dy : directions3d)
                {
                    for (int dz : directions3d)
                    {
                        if (dx == 0 && dy == 0 && dz == 0)
                        {
                            continue;
                        }

                        visitCandidate({
                            currentPoint.x + static_cast<float>(dx) * stepSize,
                            currentPoint.y + static_cast<float>(dy) * stepSize,
                            currentPoint.z + static_cast<float>(dz) * stepSize
                        });
                    }
                }
            }
        }
        else
        {
            for (const int *direction : directions2d)
            {
                visitCandidate({
                    currentPoint.x + static_cast<float>(direction[0]) * stepSize,
                    currentPoint.y + static_cast<float>(direction[1]) * stepSize,
                    currentPoint.z
                });
            }
        }
    }

    if (request.allowPartialPath && bestPartialNodeIndex != 0)
    {
        result.waypoints = rebuildPathToNode(nodes, bestPartialNodeIndex);

        if (!result.waypoints.empty())
        {
            result.status = PathPlanStatus::Partial;
            finished = true; return true;
        }
    }

    result.status = PathPlanStatus::NoRoute;
    finished = true; return true;
}

PathPlanResult PathPlanner::plan(const PathMap &map, const PathPlanRequest &request)
{
    Engine::VitaProfile::Scope profilePathfinding(Engine::VitaProfile::Pathfinding);
    static thread_local PathSearch search;
    search.reset(map, request);
    while (!search.advance(std::numeric_limits<size_t>::max())) {}
    return search.takeResult();
}
}
