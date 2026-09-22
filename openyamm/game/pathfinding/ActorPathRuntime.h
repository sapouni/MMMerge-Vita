#pragma once

#include "game/pathfinding/PathPlanner.h"
#include "game/pathfinding/PathfindingTypes.h"

#include <condition_variable>
#include <chrono>
#include <cstddef>
#include <optional>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace OpenYAMM::Game
{
enum class ActorPathDiscardReason
{
    None,
    StaleGeneration,
    StaleTarget,
    StaleSource,
    StaleObject
};

struct ActorPathResolveRequest
{
    size_t actorIndex = 0;
    PathPoint source;
    PathPoint target;
    PathObject object;
    int32_t preferredSourceFacetSourceId = -1;
    size_t nodeLimit = 8000;
    size_t mapRevision = 0;
    float sourceSnapDistance = 0.0f;
    float planningRange = 12000.0f;
    float waypointReachDistance = 0.0f;
    double nowSeconds = 0.0;
    double failedRetrySeconds = 3.0;
    double directCheckIntervalSeconds = 0.25;
    double minReplanIntervalSeconds = 1.0;
    double shortcutCheckIntervalSeconds = 0.5;
    // Zero preserves the unrestricted policy. Longer ground validations use worker plans.
    float maxSynchronousWalkDistance = 0.0f;
    bool allowPlan = true;
    bool allowDirect = true;
    bool allowPartialPath = false;
    bool progressiveDelivery = false;
};

struct ActorPathResolveResult
{
    unsigned directChecks = 0;
    unsigned directCacheHits = 0;
    unsigned longChecksDeferred = 0;
    unsigned prefixUsed = 0, prefixBlocked = 0;
    unsigned waypointChecks = 0;
    unsigned waypointCacheHits = 0;
    unsigned waypointLongDeferred = 0;
    unsigned waypointCertificateHits = 0, partialRejected = 0, shortcutsDeferred = 0;
    bool directReachable = false;
    bool pathActive = false;
    bool planned = false;
    bool failed = false;
    bool cooldown = false;
    bool deferred = false;
    bool outOfRange = false;
    bool queued = false;
    bool discarded = false;
    ActorPathDiscardReason discardReason = ActorPathDiscardReason::None;
    PathPlanStatus planStatus = PathPlanStatus::NotRequested;
    PathPlanDebugInfo planDebug;
    size_t analyzedNodeCount = 0;
    size_t waypointCount = 0;
    size_t waypointIndex = 0;
    size_t reachedWaypointCount = 0;
    size_t shortcutWaypointCount = 0;
    size_t stalledWaypointCount = 0;
    PathPoint waypoint;
};

class ActorPathRuntime
{
public:
    ActorPathRuntime() = default;
    ActorPathRuntime(const ActorPathRuntime &other) = delete;
    ActorPathRuntime &operator=(const ActorPathRuntime &other) = delete;
    ~ActorPathRuntime();

    struct QueueStats
    {
        unsigned pending = 0, running = 0;
        uint64_t completed = 0, nodes = 0, maxDeliveryMs = 0, refreshed = 0, cancelled = 0, oldestPendingMs = 0;
        uint64_t partial = 0, success = 0, budget = 0, refinement = 0, maxQueueMs = 0, maxSearchMs = 0;
    };
    QueueStats queueStats() const;
    void setWorkerCount(size_t workerCount);
    void clear();
    void resetActor(size_t actorIndex);
    bool actorHasPendingPlan(size_t actorIndex) const;
    bool actorHasActivePath(size_t actorIndex) const;
    ActorPathResolveResult resolveWaypoint(const PathMap &pathMap, const ActorPathResolveRequest &request);
    ActorPathResolveResult resolveWaypoint(
        std::shared_ptr<const PathMap> pathMap,
        const ActorPathResolveRequest &request);

private:
    struct PendingPlanJob
    {
        uint64_t jobId = 0;
        uint64_t generation = 0;
        ActorPathResolveRequest request;
        std::shared_ptr<const PathMap> pathMap;
        std::shared_ptr<std::atomic_bool> cancelled;
        std::chrono::steady_clock::time_point queuedAt;
    };

    struct CompletedPlanJob
    {
        uint64_t jobId = 0;
        uint64_t generation = 0;
        ActorPathResolveRequest request;
        PathPlanResult result;
    };

    ActorPathResolveResult resolveWaypointInternal(
        const PathMap &pathMap,
        std::shared_ptr<const PathMap> pathMapSnapshot,
        const ActorPathResolveRequest &request
    );
    ActorPathState &stateForActor(size_t actorIndex);
    bool pathIsStale(const ActorPathState &state, const ActorPathResolveRequest &request) const;
    bool pathCanStillBeFollowed(const ActorPathState &state) const;
    void resetWaypointProgress(ActorPathState &state, const ActorPathResolveRequest &request) const;
    bool waypointReachable(
        const PathMap &pathMap,
        ActorPathState &state,
        const ActorPathResolveRequest &request,
        const PathPoint &waypoint,
        ActorPathResolveResult &result
    ) const;
    size_t advanceReachedWaypoints(
        const PathMap &pathMap,
        ActorPathState &state,
        const ActorPathResolveRequest &request,
        ActorPathResolveResult &result
    ) const;
    size_t advanceShortcutWaypoints(
        const PathMap &pathMap,
        ActorPathState &state,
        const ActorPathResolveRequest &request,
        ActorPathResolveResult &result
    ) const;
    size_t advanceStalledWaypoint(
        const PathMap &pathMap,
        ActorPathState &state,
        const ActorPathResolveRequest &request,
        ActorPathResolveResult &result
    ) const;
    bool consumeCompletedPlan(
        ActorPathState &state,
        const ActorPathResolveRequest &request,
        ActorPathResolveResult &result
    );
    bool installPlanResult(
        ActorPathState &state,
        const ActorPathResolveRequest &request,
        PathPlanResult &planResult,
        ActorPathResolveResult &result
    );
    bool queuePlan(
        const PathMap &pathMap,
        const std::shared_ptr<const PathMap> &pathMapSnapshot,
        ActorPathState &state,
        const ActorPathResolveRequest &request,
        ActorPathResolveResult &result
    );
    void discardCompletedPlans(size_t actorIndex);
    void cancelActorJobsLocked(size_t actorIndex);
    void cancelAllJobsLocked();
    void stopWorkers();
    void workerLoop();

    const PathMap *m_boundMap = nullptr;
    size_t m_boundMapRevision = 0;
    std::shared_ptr<const PathMap> m_boundMapSnapshot;
    PathPlanner m_planner;
    std::vector<ActorPathState> m_actorStates;
    mutable std::mutex m_jobMutex;
    std::condition_variable m_jobCondition;
    std::deque<PendingPlanJob> m_pendingPlanJobs;
    std::deque<CompletedPlanJob> m_completedPlanJobs;
    std::vector<std::thread> m_workers;
    std::vector<std::shared_ptr<std::atomic_bool>> m_actorJobCancellation;
    uint64_t m_nextJobId = 1;
    uint64_t m_generation = 1;
    bool m_stopWorkers = false;
    unsigned m_runningJobs = 0;
    uint64_t m_completedJobs = 0, m_analyzedNodes = 0, m_maxDeliveryMs = 0, m_refreshedJobs = 0, m_cancelledJobs = 0;
    uint64_t m_partialJobs = 0, m_successJobs = 0, m_budgetJobs = 0, m_refinementJobs = 0;
    uint64_t m_maxQueueMs = 0, m_maxSearchMs = 0;
};
}
