#pragma once

#include <atomic>
#include <array>
#include <memory>
#include "engine/BufferPool.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace OpenYAMM::Game
{
// Worker-only cooperative callback; main-thread queries leave it null.
inline thread_local void (*pathCooperate)() = nullptr;
struct PathCooperationScope {
    void (*previous)();
    explicit PathCooperationScope(void (*callback)()) : previous(pathCooperate) { pathCooperate = callback; }
    ~PathCooperationScope() { pathCooperate = previous; }
};
// Cache the callback once per query, rather than looking up emulated TLS for every face.
// Only background plans install it; gameplay collision queries never sleep here.
class PathCooperationPoll
{
    void (*m_callback)() = pathCooperate;
    unsigned m_work = 0;
public:
    void poll()
    {
        if (m_callback && (++m_work & 63u) == 0)
        {
            m_callback();
        }
    }
};
struct PathPoint
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

inline std::shared_ptr<Engine::BufferPool<PathPoint>> pathBufferPool()
{
    static auto pool = std::make_shared<Engine::BufferPool<PathPoint>>(64*1024);
    return pool;
}

struct PathBounds
{
    float minX = 0.0f;
    float maxX = 0.0f;
    float minY = 0.0f;
    float maxY = 0.0f;
    float minZ = 0.0f;
    float maxZ = 0.0f;
};

enum class PathFacetKind
{
    Generic,
    Floor,
    Wall,
    Ceiling,
    Portal
};

struct PathFacetAttributes
{
    bool portal = false;
    bool invisible = false;
    bool untouchable = false;
};

struct PathFacet
{
    std::vector<PathPoint> vertices;
    PathFacetKind kind = PathFacetKind::Generic;
    PathFacetAttributes attributes;
    bool blocking = true;
    bool walkableFloor = false;
    bool dynamic = false;
    int32_t sourceId = -1;
};

struct PathFloorSample
{
    bool hasFloor = false;
    bool inVoid = false;
    float z = 0.0f;
    float normalZ = 0.0f;
    size_t facetIndex = 0;
};

struct PathFloorQueryDebug
{
    PathPoint position;
    PathFloorSample result;
    size_t candidateCount = 0;
    size_t invalidFacetCount = 0;
    size_t nonWalkableCount = 0;
    size_t boundsRejectCount = 0;
    size_t polygonRejectCount = 0;
    size_t belowCount = 0;
    size_t aboveCount = 0;
    PathFloorSample bestBelow;
    PathFloorSample bestAbove;
};

struct PathObject
{
    bool canFly = false;
    float radius = 0.0f;
    float stepLength = 24.0f;
    float stepHeight = 40.0f;
};

// A positive geometric proof, never a time-based assumption about a moving actor.
struct PathWalkCertificate
{
    PathPoint source;
    PathPoint waypoint;
    PathObject object;
    const void *map = nullptr;
    size_t revision = 0;
    float margin = 0.0f;
    bool valid = false;
};

enum class PathWalkRejectReason
{
    None,
    InvalidInput,
    StartNoFloor,
    StartVoid,
    SampleNoFloor,
    SampleVoid,
    StepHeight,
    Blocked
};

struct PathWalkSegmentDebug
{
    bool success = false;
    PathWalkRejectReason rejectReason = PathWalkRejectReason::None;
    PathPoint from;
    PathPoint to;
    PathPoint probe;
    PathPoint blockedPoint;
    PathFloorSample startFloor;
    PathFloorSample previousFloor;
    PathFloorSample failedFloor;
    size_t sampleIndex = 0;
    size_t sampleCount = 0;
    size_t blockedFacet = static_cast<size_t>(-1);
    float stepDeltaZ = 0.0f;
};

struct PathPlanRequest
{
    const std::atomic_bool *pCancelled = nullptr;
    // Optional worker-only scheduling hook. Does not change search order/results.
    void (*cooperate)() = nullptr;
    size_t actorIndex = 0;
    PathPoint source;
    PathPoint target;
    PathObject object;
    int32_t preferredSourceFacetSourceId = -1;
    size_t nodeLimit = 8000;
    uint32_t mapRevision = 0;
    float sourceSnapDistance = 0.0f;
    bool allowPartialPath = false;
    bool allowDirect = true;
};

struct PathPlanDebugInfo
{
    bool sourceValid = false;
    bool targetValid = false;
    bool directReachable = false;
    bool preferredSourceSnapUsed = false;
    PathPoint requestSource;
    PathPoint requestTarget;
    PathPoint snappedSource;
    PathPoint snappedTarget;
    PathPoint bestPoint;
    PathPoint maxStepRejectFrom;
    PathPoint maxStepRejectTo;
    size_t sourceFloorFacet = static_cast<size_t>(-1);
    size_t targetFloorFacet = static_cast<size_t>(-1);
    size_t bestFloorFacet = static_cast<size_t>(-1);
    int32_t preferredSourceFacetSourceId = -1;
    float bestDistance2d = 0.0f;
    float bestDistance3d = 0.0f;
    float maxRejectedStepDeltaZ = 0.0f;
    size_t generatedCandidates = 0;
    size_t acceptedCandidates = 0;
    size_t rejectedNoFloor = 0;
    size_t rejectedStepHeight = 0;
    size_t rejectedWalkSegment = 0;
    size_t rejectedFlyingInvalid = 0;
    size_t rejectedDuplicate = 0;
    size_t reopenedCandidates = 0;
    size_t skippedClosedNodes = 0;
};

enum class PathPlanStatus
{
    NotRequested,
    Success,
    Partial,
    NoRoute,
    NodeLimitExceeded,
    StaleRevision,
    Cancelled
};

struct PathPlanResult
{
    PathPlanResult() = default;
    PathPlanResult(const PathPlanResult&) = default;
    PathPlanResult& operator=(const PathPlanResult&) = default;
    PathPlanResult(PathPlanResult&&) = default;
    PathPlanResult& operator=(PathPlanResult&&) = default;
    std::shared_ptr<Engine::BufferPool<PathPoint>> recycler = pathBufferPool();
    ~PathPlanResult() { if(recycler) recycler->give(std::move(waypoints)); }
    PathPlanStatus status = PathPlanStatus::NotRequested;
    std::vector<PathPoint> waypoints;
    PathPlanDebugInfo debug;
    size_t analyzedNodeCount = 0;
    uint32_t mapRevision = 0;
};

struct FailedWaypointCheck
{
    PathPoint source;
    PathPoint waypoint;
    PathPoint goal;
    PathObject object;
    double checkedSeconds = 0.0;
    bool valid = false;
};

struct ActorPathState
{
    ActorPathState() = default;
    ActorPathState(const ActorPathState&) = default;
    ActorPathState& operator=(const ActorPathState&) = default;
    ActorPathState(ActorPathState&&) = default;
    ActorPathState& operator=(ActorPathState&&) = default;
    std::shared_ptr<Engine::BufferPool<PathPoint>> recycler = pathBufferPool();
    ~ActorPathState() { if(recycler) recycler->give(std::move(waypoints)); }
    PathPoint targetSnapshot;
    std::array<PathWalkCertificate, 4> waypointCertificates{};
    unsigned nextWaypointCertificate = 0;
    unsigned shortcutProbeOffset = 0;
    PathPoint pendingTargetSnapshot;
    PathPoint sourceSnapshot;
    std::vector<PathPoint> waypoints;
    size_t waypointIndex = 0;
    uint64_t requestId = 0;
    bool inProgress = false;
    PathPoint directSource;
    PathPoint directTarget;
    PathObject directObject;
    bool directCheckValid = false;
    bool lastDirectReachable = false;
    bool recoverySourceWaypointActive = false;
    bool recoveryBestWaypointActive = false;
    PathPlanStatus planStatus = PathPlanStatus::NotRequested;
    double failedUntilSeconds = 0.0;
    double nextDirectCheckSeconds = 0.0;
    double nextPlanSeconds = 0.0;
    double nextShortcutCheckSeconds = 0.0;
    double lastWaypointProgressSeconds = 0.0;
    float bestWaypointDistance = 0.0f;
    bool waypointProgressObserved = false;
    float targetMovedThreshold = 1024.0f;
    size_t progressWaypointIndex = 0;
    PathPoint lastSourcePosition;
    uint32_t mapRevision = 0;
    // Bounded negative-only cache. Exact input matches; geometry/route replacement invalidates it.
    std::array<FailedWaypointCheck, 8> failedWaypointChecks{};
    unsigned nextFailedWaypointCheck = 0;
    FailedWaypointCheck safePendingPrefix;
    double nextPendingRefreshSeconds = 0.0;
};
}
