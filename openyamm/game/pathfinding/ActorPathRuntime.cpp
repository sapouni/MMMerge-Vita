#include "engine/VitaMovementProfile.h"
#include "engine/VitaStall.h"
#include "game/render/VitaPerformanceConfig.h"
#include "engine/VitaFrameProfile.h"
#include "game/pathfinding/ActorPathRuntime.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#if defined(OPENYAMM_VITA_LOADER)
#include "game/render/VitaPerformanceConfig.h"
#include <SDL3/SDL.h>
#endif

namespace OpenYAMM::Game
{
namespace
{
std::unique_lock<std::mutex> lockPathQueue(std::mutex &mutex)
{
    Engine::VitaMovementProfile::Scope waitTiming(Engine::VitaMovementProfile::PathQueueWait);
    return std::unique_lock<std::mutex>(mutex);
}

constexpr size_t MaxShortcutWaypointScan = 24;
constexpr size_t MaxGroundShortcutWaypointScan = 6;
constexpr float MaxGroundShortcutStepMultiplier = 6.0f;
constexpr double WaypointStallSeconds = 0.35;
constexpr float WaypointProgressEpsilon = 2.0f;
constexpr float RecoverySourceReachDistance = 4.0f;
constexpr float RecoveryBestReachDistance = 16.0f;

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

bool synchronousWalkAllowed(const ActorPathResolveRequest &request, const PathPoint &target)
{
    return request.object.canFly || request.maxSynchronousWalkDistance <= 0.0f
        || distanceSquared2d(request.source, target)
            <= request.maxSynchronousWalkDistance * request.maxSynchronousWalkDistance;
}

bool samePathObject(const PathObject &a, const PathObject &b)
{
    return a.canFly == b.canFly && a.radius == b.radius
        && a.stepHeight == b.stepHeight && a.stepLength == b.stepLength;
}

bool certificateContains(const PathWalkCertificate &proof, const PathMap &map,
    const ActorPathResolveRequest &request, const PathPoint &waypoint)
{
    if (!proof.valid || proof.map != &map || proof.revision != map.revision()
        || !samePathObject(proof.object, request.object)
        || distanceSquared3d(proof.waypoint, waypoint) != 0.0f) return false;
    if (distanceSquared3d(proof.source, request.source) == 0.0f) return true;
    if (proof.margin <= 0 || request.object.canFly || request.source.z != proof.source.z) return false;
    const float dx = waypoint.x - proof.source.x, dy = waypoint.y - proof.source.y;
    const float lengthSquared = dx * dx + dy * dy;
    if (lengthSquared <= 0) return false;
    const float t = ((request.source.x - proof.source.x) * dx + (request.source.y - proof.source.y) * dy)
        / lengthSquared;
    if (!(t >= 0.0f && t <= 1.0f)) return false;
    const float sideX = request.source.x - (proof.source.x + t * dx);
    const float sideY = request.source.y - (proof.source.y + t * dy);
    return sideX * sideX + sideY * sideY <= (proof.margin - 0.01f) * (proof.margin - 0.01f);
}

float distance2d(const PathPoint &from, const PathPoint &to)
{
    return std::sqrt(distanceSquared2d(from, to));
}

bool pathWaypointReached(const PathPoint &source, const PathPoint &waypoint, float reachDistance)
{
    return distance2d(source, waypoint) <= std::max(1.0f, reachDistance);
}

bool planHasReachableBestPoint(const PathPlanResult &planResult, const ActorPathResolveRequest &request)
{
    if (planResult.debug.acceptedCandidates == 0)
    {
        return false;
    }

    const float reachDistance = std::max(request.waypointReachDistance, RecoveryBestReachDistance);
    return distanceSquared2d(planResult.debug.bestPoint, request.source) > reachDistance * reachDistance;
}

float relaxedWaypointReachDistance(const ActorPathResolveRequest &request)
{
    const float reachDistance = std::max(1.0f, request.waypointReachDistance);
    const float stepSlack = std::max(8.0f, request.object.stepLength * 0.75f);
    return std::max(reachDistance * 1.25f, reachDistance + stepSlack);
}

size_t firstUnreachedWaypointIndex(
    const std::vector<PathPoint> &waypoints,
    const PathPoint &source,
    float reachDistance
)
{
    size_t index = 0;

    while (index < waypoints.size()
        && pathWaypointReached(source, waypoints[index], reachDistance))
    {
        ++index;
    }

    return index;
}

size_t closestUnreachedWaypointIndex(
    const std::vector<PathPoint> &waypoints,
    const PathPoint &source,
    float reachDistance
)
{
    size_t bestIndex = waypoints.size();
    float bestDistance = std::numeric_limits<float>::max();

    for (size_t index = 0; index < waypoints.size(); ++index)
    {
        if (pathWaypointReached(source, waypoints[index], reachDistance))
        {
            bestIndex = index + 1;
            bestDistance = 0.0f;
            continue;
        }

        const float distance = distanceSquared3d(source, waypoints[index]);

        if (distance < bestDistance)
        {
            bestDistance = distance;
            bestIndex = index;
        }
    }

    return bestIndex;
}

}

ActorPathRuntime::~ActorPathRuntime()
{
    stopWorkers();
}

void ActorPathRuntime::setWorkerCount(size_t workerCount)
{
#if defined(OPENYAMM_VITA_LOADER)
    if (workerCount) workerCount = std::min<size_t>(workerCount, vitaPerformanceSettings().pathWorkers);
#endif
    if (workerCount == m_workers.size())
    {
        return;
    }

    stopWorkers();

    if (workerCount == 0)
    {
        return;
    }

    {
        auto lock = lockPathQueue(m_jobMutex);
        m_stopWorkers = false;
    }

    m_workers.reserve(workerCount);

    for (size_t index = 0; index < workerCount; ++index)
    {
        m_workers.emplace_back(&ActorPathRuntime::workerLoop, this);
    }
}

ActorPathRuntime::QueueStats ActorPathRuntime::queueStats() const
{
    auto lock = lockPathQueue(m_jobMutex);
    const uint64_t oldestMs = m_pendingPlanJobs.empty() ? 0 : uint64_t(std::max<int64_t>(0,
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - m_pendingPlanJobs.front().queuedAt).count()));
    return {static_cast<unsigned>(m_pendingPlanJobs.size()), m_runningJobs,
        m_completedJobs, m_analyzedNodes, m_maxDeliveryMs, m_refreshedJobs, m_cancelledJobs, oldestMs,
        m_partialJobs, m_successJobs, m_budgetJobs, m_refinementJobs, m_maxQueueMs, m_maxSearchMs};
}

void ActorPathRuntime::clear()
{
    {
        auto lock = lockPathQueue(m_jobMutex);
        ++m_generation;
        cancelAllJobsLocked();
        m_completedJobs = m_analyzedNodes = m_maxDeliveryMs = m_refreshedJobs = m_cancelledJobs = 0;
        m_pendingPlanJobs.clear();
        m_completedPlanJobs.clear();
        m_partialJobs = m_successJobs = m_budgetJobs = m_refinementJobs = m_maxQueueMs = m_maxSearchMs = 0;
    }

    m_actorStates.clear();
    m_boundMap = nullptr;
    m_boundMapRevision = 0;
    m_boundMapSnapshot.reset();
}

void ActorPathRuntime::cancelActorJobsLocked(size_t actorIndex)
{
    if (actorIndex < m_actorJobCancellation.size() && m_actorJobCancellation[actorIndex])
    {
        if (!m_actorJobCancellation[actorIndex]->exchange(true, std::memory_order_relaxed)) { ++m_cancelledJobs; }
        m_actorJobCancellation[actorIndex].reset();
    }
    std::erase_if(m_pendingPlanJobs, [actorIndex](const auto &job) { return job.request.actorIndex == actorIndex; });
    std::erase_if(m_completedPlanJobs, [actorIndex](const auto &job) { return job.request.actorIndex == actorIndex; });
}

void ActorPathRuntime::cancelAllJobsLocked()
{
    for (const auto &token : m_actorJobCancellation)
    {
        if (token && !token->exchange(true, std::memory_order_relaxed)) { ++m_cancelledJobs; }
    }
    m_actorJobCancellation.clear();
}

void ActorPathRuntime::resetActor(size_t actorIndex)
{
    auto lock = lockPathQueue(m_jobMutex);
    cancelActorJobsLocked(actorIndex);
    if (actorIndex < m_actorStates.size())
    {
        m_actorStates[actorIndex] = {};
    }
}

bool ActorPathRuntime::actorHasPendingPlan(size_t actorIndex) const
{
    return actorIndex < m_actorStates.size() && m_actorStates[actorIndex].inProgress;
}

bool ActorPathRuntime::actorHasActivePath(size_t actorIndex) const
{
    return actorIndex < m_actorStates.size() && pathCanStillBeFollowed(m_actorStates[actorIndex]);
}

ActorPathResolveResult ActorPathRuntime::resolveWaypoint(
    const PathMap &pathMap,
    const ActorPathResolveRequest &request
)
{
    return resolveWaypointInternal(pathMap, nullptr, request);
}

ActorPathResolveResult ActorPathRuntime::resolveWaypoint(
    std::shared_ptr<const PathMap> pathMap,
    const ActorPathResolveRequest &request
)
{
    if (pathMap == nullptr)
    {
        return {};
    }

    std::shared_ptr<const PathMap> pathMapSnapshot = std::move(pathMap);
    const PathMap &pathMapReference = *pathMapSnapshot;
    return resolveWaypointInternal(pathMapReference, pathMapSnapshot, request);
}

ActorPathResolveResult ActorPathRuntime::resolveWaypointInternal(
    const PathMap &pathMap,
    std::shared_ptr<const PathMap> pathMapSnapshot,
    const ActorPathResolveRequest &request
)
{
    Engine::VitaProfile::Scope profilePathfinding(Engine::VitaProfile::Pathfinding);
    // Different immutable map snapshots can have the same local revision number.
    if (m_boundMap != &pathMap || m_boundMapRevision != request.mapRevision)
    {
        clear();
        m_boundMap = &pathMap;
        m_boundMapRevision = request.mapRevision;
        m_boundMapSnapshot = pathMapSnapshot;
    }
    ActorPathResolveResult result = {};
    ActorPathState &state = stateForActor(request.actorIndex);
    const float planningRangeSquared = request.planningRange * request.planningRange;

    if (!state.inProgress)
    {
        discardCompletedPlans(request.actorIndex);
    }

    if (distanceSquared3d(request.source, request.target) > planningRangeSquared)
    {
        resetActor(request.actorIndex);
        result.outOfRange = true;
        return result;
    }

    if (state.mapRevision != 0 && state.mapRevision != static_cast<uint32_t>(request.mapRevision))
    {
        state = {};
    }

    result.reachedWaypointCount += advanceReachedWaypoints(pathMap, state, request, result);

    const double directInterval = std::max(0.01, request.directCheckIntervalSeconds);
    // Reuse a successful direction only for small continuous movement. The movement controller
    // still sweeps every actual step. Teleports, changed body shape and geometry invalidate it.
    const float movementSlack = std::max(4.0f, std::min(16.0f, request.object.radius * 0.5f));
    if (state.directCheckValid && state.lastDirectReachable && (!samePathObject(state.directObject, request.object)
        || distanceSquared3d(state.directSource, request.source) > movementSlack * movementSlack
        || distanceSquared3d(state.directTarget, request.target) > movementSlack * movementSlack))
    {
        state.directCheckValid = false;
    }
    const bool directCheckDue = request.allowDirect
        && (!state.directCheckValid || request.nowSeconds >= state.nextDirectCheckSeconds);
    if (directCheckDue && synchronousWalkAllowed(request, request.target))
    {
        ++result.directChecks;
        const bool reachable = pathMap.canReachDirectly(request.source, request.target, request.object);
        if (reachable)
        {
            resetActor(request.actorIndex);
            state.mapRevision = static_cast<uint32_t>(request.mapRevision);
        }
        state.directCheckValid = true;
        state.lastDirectReachable = reachable;
        state.directSource = request.source;
        state.directTarget = request.target;
        state.directObject = request.object;
        state.nextDirectCheckSeconds = request.nowSeconds + directInterval;
        if (reachable) { result.directReachable = true; return result; }
    }
    else if (directCheckDue)
    {
        // Unknown is not treated as walkable. Queue the exact route on the existing planner.
        ++result.longChecksDeferred;
    }
    else if (request.allowDirect && state.directCheckValid && state.lastDirectReachable)
    {
        ++result.directCacheHits;
        result.directReachable = true;
        return result;
    }

    const float pendingTargetSlack = std::max({std::max(1.0f, state.targetMovedThreshold),
        request.object.stepLength * 4.0f, request.object.radius * 4.0f});
    if (state.inProgress && request.nowSeconds >= state.nextPlanSeconds
        && distanceSquared3d(state.pendingTargetSnapshot, request.target)
            > pendingTargetSlack * pendingTargetSlack)
    {
        auto lock = lockPathQueue(m_jobMutex);
        // A job still in the FIFO has done no geometric work. Retarget it in place
        // before the running-job cancellation test, preserving its age and queue slot.
        // Previously a moving target repeatedly sent waiting actors to the queue tail.
        bool refreshedQueued = false;
        for (PendingPlanJob &job : m_pendingPlanJobs)
        {
            if (job.jobId == state.requestId && job.request.actorIndex == request.actorIndex)
            {
                job.request = request;
                state.pendingTargetSnapshot = request.target;
                ++m_refreshedJobs;
                refreshedQueued = true;
                break;
            }
        }
        // Once started, finish the search. Repeated target motion used to cancel
        // expensive work before delivery, and could even erase an already completed
        // result before consumeCompletedPlan saw it. A stale result is only a prefix;
        // its next leg is checked against the current body/source/map below.
        (void)refreshedQueued;
    }
    consumeCompletedPlan(state, request, result);

    const bool stalePath = pathIsStale(state, request);
    const bool activePathBeforePlan = pathCanStillBeFollowed(state);

    if (stalePath)
    {
        const bool actorPlanReady = request.nowSeconds >= state.nextPlanSeconds;

        if (state.failedUntilSeconds > request.nowSeconds)
        {
            if (!activePathBeforePlan)
            {
                result.cooldown = true;
                return result;
            }
        }
        else if (state.inProgress)
        {
            result.deferred = true;
            if (!activePathBeforePlan) { return result; }
        }
        else if (!request.allowPlan || !actorPlanReady)
        {
            result.deferred = true; // Existing stale routes still require prefix validation.
            if (!activePathBeforePlan)
            {
                result.deferred = true;
                return result;
            }
        }
        else
        {
            queuePlan(pathMap, pathMapSnapshot, state, request, result);

            if (result.failed && !pathCanStillBeFollowed(state))
            {
                return result;
            }

            if (result.queued)
            {
                result.deferred = true;
                if (!activePathBeforePlan) { return result; }
            }
        }
    }

    if (stalePath && result.deferred && pathCanStillBeFollowed(state))
    {
        // A pending replacement must not hide an existing usable route. Validate only
        // its next leg, within the normal synchronous distance bound; never guess that
        // a wall/changed body/teleport is safe. Exact repeats reuse the validation.
        const PathPoint &next = state.waypoints[state.waypointIndex];
        auto &memo = state.safePendingPrefix;
        const bool memoHit = memo.valid && request.nowSeconds >= memo.checkedSeconds
            && request.nowSeconds - memo.checkedSeconds < 0.25
            && distanceSquared3d(memo.source, request.source) == 0.0f
            && distanceSquared3d(memo.waypoint, next) == 0.0f
            && samePathObject(memo.object, request.object);
        const bool safe = memoHit || waypointReachable(pathMap, state, request, next, result);
        if (safe)
        {
            if (!memoHit) { memo = {request.source, next, request.target, request.object, request.nowSeconds, true}; }
            ++result.prefixUsed;
            result.pathActive = true;
            result.waypointIndex = state.waypointIndex;
            result.waypoint = next;
        }
        else { ++result.prefixBlocked; memo.valid = false; }
        return result;
    }

    if (result.planned)
    {
        result.reachedWaypointCount += advanceReachedWaypoints(pathMap, state, request, result);
    }
    result.shortcutWaypointCount += advanceShortcutWaypoints(pathMap, state, request, result);

    if (!result.planned)
    {
        result.stalledWaypointCount += advanceStalledWaypoint(pathMap, state, request, result);
    }

    if (state.waypointIndex < state.waypoints.size())
    {
        result.pathActive = true;
        result.waypointIndex = state.waypointIndex;
        result.waypoint = state.waypoints[state.waypointIndex];
    }

    return result;
}

bool ActorPathRuntime::consumeCompletedPlan(
    ActorPathState &state,
    const ActorPathResolveRequest &request,
    ActorPathResolveResult &result
)
{
    if (!state.inProgress || state.requestId == 0)
    {
        return false;
    }

    CompletedPlanJob completedJob = {};
    uint64_t currentGeneration = 0;
    bool found = false;

    {
        auto lock = lockPathQueue(m_jobMutex);
        currentGeneration = m_generation;

        for (auto it = m_completedPlanJobs.begin(); it != m_completedPlanJobs.end(); ++it)
        {
            if (it->request.actorIndex != request.actorIndex || it->jobId != state.requestId)
            {
                continue;
            }

            completedJob = std::move(*it);
            m_completedPlanJobs.erase(it);
            found = true;
            break;
        }
    }

    if (!found)
    {
        if (request.nowSeconds >= state.nextPendingRefreshSeconds)
        {
            state.nextPendingRefreshSeconds = request.nowSeconds + 0.25;
            auto lock = lockPathQueue(m_jobMutex);
            for (PendingPlanJob &job : m_pendingPlanJobs)
            {
                if (job.jobId == state.requestId && job.request.actorIndex == request.actorIndex)
                {
                    // Keep FIFO position and cancellation token. A queued search should start
                    // from today's position, not the position recorded several seconds ago.
                    if (distanceSquared3d(job.request.source, request.source) > 1.0f
                        || distanceSquared3d(job.request.target, request.target) > 1.0f
                        || !samePathObject(job.request.object, request.object))
                    {
                        job.request = request;
                        state.pendingTargetSnapshot = request.target;
                        ++m_refreshedJobs;
                    }
                    break;
                }
            }
        }
        return false;
    }

    state.inProgress = false;

    result.planStatus = completedJob.result.status;
    result.planDebug = completedJob.result.debug;
    result.analyzedNodeCount = completedJob.result.analyzedNodeCount;
    result.waypointCount = completedJob.result.waypoints.size();

    if (completedJob.generation != currentGeneration
        || completedJob.request.mapRevision != request.mapRevision)
    {
        state = {};
        result.discarded = true;
        result.discardReason = ActorPathDiscardReason::StaleGeneration;
        return false;
    }

    const float targetMovedThreshold = std::max(1.0f, state.targetMovedThreshold);
    const float movedSquared = distanceSquared3d(completedJob.request.target, request.target);
    if (!samePathObject(completedJob.request.object, request.object))
    {
        state.requestId = 0;
        result.discarded = true;
        result.discardReason = ActorPathDiscardReason::StaleObject;
        return false;
    }
    if (movedSquared > targetMovedThreshold * targetMovedThreshold
        && (completedJob.result.waypoints.empty()
            || (completedJob.result.status != PathPlanStatus::Success
                && !(request.allowPartialPath && completedJob.result.status == PathPlanStatus::Partial))))
    {
        // A failed search's recovery point is not a validated route prefix.
        state.requestId = 0;
        // A stale failure is not a route, but it must not immediately retry an identical
        // failed search every frame. A changed goal gets the shorter normal replan interval.
        state.nextPlanSeconds = std::max(state.nextPlanSeconds,
            request.nowSeconds + std::max(0.1, request.minReplanIntervalSeconds));
        result.discarded = true;
        result.discardReason = ActorPathDiscardReason::StaleTarget;
        return false;
    }
    const bool installed = installPlanResult(state, request, completedJob.result, result);
    if (installed && movedSquared > targetMovedThreshold * targetMovedThreshold)
    {
        // This is a prefix toward the old target, never a route to the new target.
        // Existing current-source/waypoint checks and movement sweeps still validate it.
        state.targetSnapshot = completedJob.request.target;
        state.nextPlanSeconds = request.nowSeconds;
    }
    return installed;
}

bool ActorPathRuntime::installPlanResult(
    ActorPathState &state,
    const ActorPathResolveRequest &request,
    PathPlanResult &planResult,
    ActorPathResolveResult &result
)
{
    result.planned = true;
    result.planStatus = planResult.status;
    result.planDebug = planResult.debug;
    result.analyzedNodeCount = planResult.analyzedNodeCount;
    result.waypointCount = planResult.waypoints.size();
    const bool hadActivePath = pathCanStillBeFollowed(state);

    const float minimumRemaining = std::max(1.0f, request.waypointReachDistance) + request.object.stepLength;
    const bool rejectedPartial = request.progressiveDelivery && planResult.status == PathPlanStatus::Partial
        && (planResult.waypoints.empty()
            || (request.object.canFly ? distanceSquared3d(request.source, planResult.waypoints.back())
                                      : distanceSquared2d(request.source, planResult.waypoints.back()))
                <= minimumRemaining * minimumRemaining);
    if (rejectedPartial) ++result.partialRejected;
    const bool usablePlan = !rejectedPartial && (
        planResult.status == PathPlanStatus::Success
        || (request.allowPartialPath && planResult.status == PathPlanStatus::Partial));

    if (!usablePlan || planResult.waypoints.empty())
    {
        const bool useRecoveryBestWaypoint =
            !rejectedPartial && !hadActivePath
            && planHasReachableBestPoint(planResult, request);

        if (!hadActivePath)
        {
            state = {};
        }

        state.inProgress = false;
        state.requestId = 0;
        state.failedUntilSeconds = request.nowSeconds + std::max(0.1, request.failedRetrySeconds);
        state.nextPlanSeconds = state.failedUntilSeconds;
        result.failed = true;
        // The same existing collision-checked direct fallback applies during this cooldown.
        if (rejectedPartial) result.cooldown = true;

        if (useRecoveryBestWaypoint)
        {
            state.targetSnapshot = request.target;
            state.sourceSnapshot = request.source;
            state.lastSourcePosition = request.source;
            state.mapRevision = static_cast<uint32_t>(request.mapRevision);
            state.waypoints = {planResult.debug.bestPoint};
            state.waypointIndex = 0;
            state.recoveryBestWaypointActive = true;
            state.directCheckValid = true;
            state.lastDirectReachable = false;
            state.planStatus = planResult.status;
            state.nextDirectCheckSeconds =
                request.nowSeconds + std::max(0.01, request.directCheckIntervalSeconds);
            resetWaypointProgress(state, request);
            result.pathActive = true;
            result.waypointIndex = state.waypointIndex;
            result.waypoint = state.waypoints[state.waypointIndex];
        }

        return false;
    }

    std::vector<PathPoint> waypoints = std::move(planResult.waypoints);
    bool recoverySourceWaypointActive = false;

    if (planResult.debug.preferredSourceSnapUsed
        && distanceSquared2d(planResult.debug.snappedSource, request.source)
            > RecoverySourceReachDistance * RecoverySourceReachDistance)
    {
        waypoints.insert(waypoints.begin(), planResult.debug.snappedSource);
        recoverySourceWaypointActive = true;
    }

    size_t firstWaypointIndex =
        recoverySourceWaypointActive
            ? 0
            : firstUnreachedWaypointIndex(waypoints, request.source, request.waypointReachDistance);
    const float sourceMovedThreshold =
        std::max(request.waypointReachDistance * 2.0f, request.object.stepLength * 4.0f);

    if (!recoverySourceWaypointActive
        && distanceSquared2d(planResult.debug.requestSource, request.source)
        > sourceMovedThreshold * sourceMovedThreshold)
    {
        firstWaypointIndex =
            closestUnreachedWaypointIndex(waypoints, request.source, request.waypointReachDistance);
    }

    state.targetSnapshot = request.target;
    state.sourceSnapshot = request.source;
    state.lastSourcePosition = request.source;
    state.mapRevision = static_cast<uint32_t>(request.mapRevision);
    state.waypoints.swap(waypoints);
    state.failedWaypointChecks = {};
    state.waypointCertificates = {};
    state.nextWaypointCertificate = 0;
    state.shortcutProbeOffset = 0;
    state.safePendingPrefix = {};
    state.nextFailedWaypointCheck = 0;
    pathBufferPool()->give(std::move(waypoints));
    state.waypointIndex = firstWaypointIndex;
    state.recoverySourceWaypointActive = recoverySourceWaypointActive;
    state.inProgress = false;
    state.requestId = 0;
    state.failedUntilSeconds = 0.0;
    state.directCheckValid = true;
    state.lastDirectReachable = false;
    state.planStatus = planResult.status;
    state.nextDirectCheckSeconds =
        request.nowSeconds + std::max(0.01, request.directCheckIntervalSeconds);
    state.nextPlanSeconds = request.nowSeconds + std::max(0.0, request.minReplanIntervalSeconds);
    state.nextShortcutCheckSeconds = 0.0;
    resetWaypointProgress(state, request);

    return true;
}

bool ActorPathRuntime::queuePlan(
    const PathMap &pathMap,
    const std::shared_ptr<const PathMap> &pathMapSnapshot,
    ActorPathState &state,
    const ActorPathResolveRequest &request,
    ActorPathResolveResult &result
)
{
    PathPlanRequest planRequest = {};
    planRequest.actorIndex = request.actorIndex;
    planRequest.source = request.source;
    planRequest.target = request.target;
    planRequest.object = request.object;
    planRequest.preferredSourceFacetSourceId = request.preferredSourceFacetSourceId;
    planRequest.nodeLimit = request.nodeLimit;
    planRequest.mapRevision = static_cast<uint32_t>(request.mapRevision);
    planRequest.sourceSnapDistance = request.sourceSnapDistance;
    planRequest.allowDirect = request.allowDirect;
    planRequest.allowPartialPath = request.allowPartialPath;

    if (m_workers.empty())
    {
        PathPlanResult planResult = m_planner.plan(pathMap, planRequest);
        return installPlanResult(state, request, planResult, result);
    }

    PendingPlanJob job = {};
    job.request = request;
    job.queuedAt = std::chrono::steady_clock::now();
    job.pathMap =
        pathMapSnapshot != nullptr
            ? pathMapSnapshot
            : std::make_shared<PathMap>(pathMap);

    {
        auto lock = lockPathQueue(m_jobMutex);
        cancelActorJobsLocked(request.actorIndex);
        if (m_actorJobCancellation.size() <= request.actorIndex)
        {
            m_actorJobCancellation.resize(request.actorIndex + 1);
        }
        job.cancelled = std::make_shared<std::atomic_bool>(false);
        m_actorJobCancellation[request.actorIndex] = job.cancelled;
        job.jobId = m_nextJobId++;
        job.generation = m_generation;
        m_pendingPlanJobs.push_back(job);
    }

    state.requestId = job.jobId;
    state.pendingTargetSnapshot = request.target;
    state.inProgress = true;
    state.nextPlanSeconds = request.nowSeconds + std::max(0.0, request.minReplanIntervalSeconds);
    result.queued = true;

    m_jobCondition.notify_one();
    return true;
}

void ActorPathRuntime::discardCompletedPlans(size_t actorIndex)
{
    auto lock = lockPathQueue(m_jobMutex);

    for (auto it = m_completedPlanJobs.begin(); it != m_completedPlanJobs.end();)
    {
        if (it->request.actorIndex == actorIndex)
        {
            it = m_completedPlanJobs.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

ActorPathState &ActorPathRuntime::stateForActor(size_t actorIndex)
{
    if (actorIndex >= m_actorStates.size())
    {
        m_actorStates.resize(actorIndex + 1);
    }

    return m_actorStates[actorIndex];
}

bool ActorPathRuntime::pathIsStale(
    const ActorPathState &state,
    const ActorPathResolveRequest &request
) const
{
    if (state.waypoints.empty() || state.waypointIndex >= state.waypoints.size())
    {
        return true;
    }

    if (state.mapRevision != static_cast<uint32_t>(request.mapRevision))
    {
        return true;
    }

    const float targetMovedThreshold = std::max(1.0f, state.targetMovedThreshold);
    return distanceSquared3d(state.targetSnapshot, request.target) > targetMovedThreshold * targetMovedThreshold;
}

bool ActorPathRuntime::pathCanStillBeFollowed(const ActorPathState &state) const
{
    return !state.waypoints.empty() && state.waypointIndex < state.waypoints.size();
}

void ActorPathRuntime::resetWaypointProgress(
    ActorPathState &state,
    const ActorPathResolveRequest &request
) const
{
    state.progressWaypointIndex = state.waypointIndex;
    state.lastWaypointProgressSeconds = request.nowSeconds;
    state.waypointProgressObserved = false;

    if (state.waypointIndex < state.waypoints.size())
    {
        state.bestWaypointDistance = distance2d(request.source, state.waypoints[state.waypointIndex]);
    }
    else
    {
        state.bestWaypointDistance = 0.0f;
    }
}

bool ActorPathRuntime::waypointReachable(
    const PathMap &pathMap,
    ActorPathState &state,
    const ActorPathResolveRequest &request,
    const PathPoint &waypoint,
    ActorPathResolveResult &result
) const
{
    for (const PathWalkCertificate &proof : state.waypointCertificates)
    {
        if (certificateContains(proof, pathMap, request, waypoint))
        {
            ++result.waypointCertificateHits;
            return true;
        }
    }
    if (!synchronousWalkAllowed(request, waypoint))
    {
        ++result.waypointLongDeferred;
        return false;
    }

    const auto samePoint = [](const PathPoint &a, const PathPoint &b)
    {
        return a.x == b.x && a.y == b.y && a.z == b.z;
    };
    // Reached, shortcut and stalled checks can all ask about the same blocked leg.
    // Reuse only failures with identical inputs. Actual movement still uses exact sweeps.
    for (const FailedWaypointCheck &entry : state.failedWaypointChecks)
    {
        if (entry.valid && request.nowSeconds >= entry.checkedSeconds
            && request.nowSeconds - entry.checkedSeconds < 0.25
            && samePoint(entry.source, request.source) && samePoint(entry.waypoint, waypoint)
            && samePoint(entry.goal, request.target) && samePathObject(entry.object, request.object))
        {
            ++result.waypointCacheHits;
            return false;
        }
    }

    ++result.waypointChecks;
    PathWalkCertificate proof;
    if (pathMap.certifyWalkCorridor(request.source, waypoint, request.object, proof)
        || pathMap.canReachDirectly(request.source, waypoint, request.object))
    {
        if (!proof.valid)
            proof = {request.source, waypoint, request.object, &pathMap, pathMap.revision(), 0.0f, true};
        state.waypointCertificates[state.nextWaypointCertificate++ % state.waypointCertificates.size()] = proof;
        return true;
    }
    FailedWaypointCheck &entry = state.failedWaypointChecks[
        state.nextFailedWaypointCheck++ % state.failedWaypointChecks.size()];
    entry = {request.source, waypoint, request.target, request.object, request.nowSeconds, true};
    return false;
}

size_t ActorPathRuntime::advanceReachedWaypoints(
    const PathMap &pathMap,
    ActorPathState &state,
    const ActorPathResolveRequest &request,
    ActorPathResolveResult &result
) const
{
    const size_t originalIndex = state.waypointIndex;

    while (state.waypointIndex < state.waypoints.size()
        && pathWaypointReached(
            request.source,
            state.waypoints[state.waypointIndex],
            state.recoverySourceWaypointActive && state.waypointIndex == 0
                ? RecoverySourceReachDistance
                : (state.recoveryBestWaypointActive && state.waypointIndex == 0
                    ? RecoveryBestReachDistance
                    : request.waypointReachDistance)))
    {
        if (state.waypointIndex + 1 < state.waypoints.size()
            && !waypointReachable(pathMap, state, request, state.waypoints[state.waypointIndex + 1], result))
        {
            break;
        }

        ++state.waypointIndex;

        if (state.recoverySourceWaypointActive && state.waypointIndex > 0)
        {
            state.recoverySourceWaypointActive = false;
        }

        if (state.recoveryBestWaypointActive && state.waypointIndex > 0)
        {
            state.recoveryBestWaypointActive = false;
        }
    }

    if (state.waypointIndex != originalIndex)
    {
        resetWaypointProgress(state, request);
    }

    return state.waypointIndex - originalIndex;
}

size_t ActorPathRuntime::advanceShortcutWaypoints(
    const PathMap &pathMap,
    ActorPathState &state,
    const ActorPathResolveRequest &request,
    ActorPathResolveResult &result
) const
{
    if (!pathCanStillBeFollowed(state)
        || !request.allowDirect
        || request.nowSeconds < state.nextShortcutCheckSeconds)
    {
        return 0;
    }

    if (state.recoverySourceWaypointActive || state.recoveryBestWaypointActive)
    {
        return 0;
    }

    const double intervalSeconds = std::max(0.01, request.shortcutCheckIntervalSeconds);
    state.nextShortcutCheckSeconds = request.nowSeconds + intervalSeconds;

    const size_t originalIndex = state.waypointIndex;
    const size_t maxShortcutWaypointScan =
        request.object.canFly ? MaxShortcutWaypointScan : MaxGroundShortcutWaypointScan;
    const size_t lastCandidateIndex =
        std::min(state.waypoints.size() - 1, originalIndex + maxShortcutWaypointScan);
    const float maxGroundShortcutDistance =
        std::max(request.object.stepLength, 1.0f) * MaxGroundShortcutStepMultiplier;
    const float maxGroundShortcutDistanceSquared =
        maxGroundShortcutDistance * maxGroundShortcutDistance;

    const bool boundedGroundChecks = !request.object.canFly && request.maxSynchronousWalkDistance > 0;
    if (boundedGroundChecks && result.waypointChecks != 0)
    {
        ++result.shortcutsDeferred;
        return 0; // Reached/pending movement work has priority over an optional shortcut.
    }
    if (boundedGroundChecks && lastCandidateIndex > originalIndex)
    {
        const size_t count = lastCandidateIndex - originalIndex;
        const size_t index = lastCandidateIndex - state.shortcutProbeOffset++ % count;
        if (distanceSquared2d(request.source, state.waypoints[index]) <= maxGroundShortcutDistanceSquared
            && waypointReachable(pathMap, state, request, state.waypoints[index], result))
        {
            state.waypointIndex = index;
            state.shortcutProbeOffset = 0;
            resetWaypointProgress(state, request);
            return state.waypointIndex - originalIndex;
        }
        return 0;
    }

    for (size_t index = lastCandidateIndex; index > originalIndex; --index)
    {
        if (!request.object.canFly
            && distanceSquared2d(request.source, state.waypoints[index]) > maxGroundShortcutDistanceSquared)
        {
            continue;
        }

        if (!waypointReachable(pathMap, state, request, state.waypoints[index], result))
        {
            continue;
        }

        state.waypointIndex = index;
        resetWaypointProgress(state, request);
        return state.waypointIndex - originalIndex;
    }

    return 0;
}

size_t ActorPathRuntime::advanceStalledWaypoint(
    const PathMap &pathMap,
    ActorPathState &state,
    const ActorPathResolveRequest &request,
    ActorPathResolveResult &result
) const
{
    if (!pathCanStillBeFollowed(state))
    {
        return 0;
    }

    const float waypointDistance = distance2d(request.source, state.waypoints[state.waypointIndex]);

    if (state.progressWaypointIndex != state.waypointIndex)
    {
        resetWaypointProgress(state, request);
        return 0;
    }

    if (waypointDistance + WaypointProgressEpsilon < state.bestWaypointDistance)
    {
        state.bestWaypointDistance = waypointDistance;
        state.lastWaypointProgressSeconds = request.nowSeconds;
        state.waypointProgressObserved = true;
        return 0;
    }

    if (!state.waypointProgressObserved)
    {
        state.lastWaypointProgressSeconds = request.nowSeconds;
        state.waypointProgressObserved = true;
        return 0;
    }

    const bool nearWaypoint = waypointDistance <= relaxedWaypointReachDistance(request);
    const bool stalled = request.nowSeconds - state.lastWaypointProgressSeconds >= WaypointStallSeconds;

    if (!stalled)
    {
        return 0;
    }

    if (state.waypointIndex + 1 < state.waypoints.size()
        && !waypointReachable(pathMap, state, request, state.waypoints[state.waypointIndex + 1], result))
    {
        if (nearWaypoint)
        {
            return 0;
        }

        state.waypointIndex = state.waypoints.size();
    }
    else
    {
        ++state.waypointIndex;
    }

    if (state.recoveryBestWaypointActive && state.waypointIndex >= state.waypoints.size())
    {
        state.recoveryBestWaypointActive = false;
    }

    resetWaypointProgress(state, request);
    return 1;
}

void ActorPathRuntime::stopWorkers()
{
    {
        auto lock = lockPathQueue(m_jobMutex);
        m_stopWorkers = true;
        cancelAllJobsLocked();
        m_pendingPlanJobs.clear();
    }

    m_jobCondition.notify_all();

    for (std::thread &worker : m_workers)
    {
        if (worker.joinable())
        {
            worker.join();
        }
    }

    {
        auto lock = lockPathQueue(m_jobMutex);
        m_workers.clear();
        m_runningJobs = 0;
        m_completedPlanJobs.clear();
        m_stopWorkers = false;
    }
}

void ActorPathRuntime::workerLoop()
{
#if defined(OPENYAMM_VITA_LOADER)
    SDL_SetHint("openyamm_vita_thread_role", "path");
#endif
    struct ActiveSearch
    {
        PendingPlanJob job;
        PathSearch search;
        std::chrono::steady_clock::time_point startedAt;
        bool refined = false;
        bool active = false;
    };
    // Bounded workspaces: avoid a long FIFO head starving all later actors, without
    // creating a thread or an A* workspace for every monster in the map.
    std::array<ActiveSearch, 4> searches;
#if defined(OPENYAMM_VITA_LOADER)
    static thread_local uint64_t lastYield = 0;
    lastYield = 0;
#endif
    while (true)
    {
        Engine::VitaStall::workerStage(30);
        for (ActiveSearch &slot : searches)
        {
            if (!slot.active)
            {
                {
                    auto lock = lockPathQueue(m_jobMutex);
                    if (m_stopWorkers) { return; }
                    if (m_pendingPlanJobs.empty()) { continue; }
                    // Reserve one workspace for hard routes; three keep serving FIFO requests.
                    if (&slot == &searches.back() && m_pendingPlanJobs.front().request.progressiveDelivery) { continue; }
                    slot.job = std::move(m_pendingPlanJobs.front());
                    m_pendingPlanJobs.pop_front();
                    ++m_runningJobs;
                    slot.startedAt = std::chrono::steady_clock::now();
                    slot.refined = false;
                    m_maxQueueMs = std::max(m_maxQueueMs, uint64_t(std::max<int64_t>(0,
                        std::chrono::duration_cast<std::chrono::milliseconds>(slot.startedAt - slot.job.queuedAt).count())));
                }
                PendingPlanJob &job = slot.job;
        PathPlanRequest planRequest = {};
        planRequest.pCancelled = job.cancelled.get();
#if defined(OPENYAMM_VITA_LOADER)
        planRequest.cooperate = []()
        {
            if (oy_path_cooperate)
            {
                oy_path_cooperate(&lastYield);
            }
        };
#endif
        planRequest.actorIndex = job.request.actorIndex;
        planRequest.source = job.request.source;
        planRequest.target = job.request.target;
        planRequest.object = job.request.object;
        planRequest.preferredSourceFacetSourceId = job.request.preferredSourceFacetSourceId;
        planRequest.nodeLimit = job.request.nodeLimit;
        planRequest.mapRevision = static_cast<uint32_t>(job.request.mapRevision);
        planRequest.sourceSnapDistance = job.request.sourceSnapDistance;
        planRequest.allowDirect = job.request.allowDirect;
        planRequest.allowPartialPath = job.request.allowPartialPath;

                slot.search.reset(*job.pathMap, planRequest);
                slot.active = true;
            }
            PendingPlanJob &job = slot.job;
            Engine::VitaStall::workerStage(31, job.request.actorIndex);
            // A node can contain expensive exact geometry tests; this is a work
            // quantum, not a wall-time guarantee. Cooperative CPU yields remain active.
            bool complete = slot.search.advance(8);
            if (!complete && job.request.progressiveDelivery && slot.search.analyzedNodes() >= 64)
            {
                const float usefulDistance = std::max(job.request.object.stepLength * 4.0f,
                    job.request.waypointReachDistance * 2.0f + job.request.object.stepLength);
                complete = slot.search.finishProgress(usefulDistance);
                if (!complete && slot.search.analyzedNodes() >= 256 && &slot != &searches.back())
                {
                    ActiveSearch &refinement = searches.back();
                    if (!refinement.active)
                    {
                        std::swap(slot, refinement); // Retain the exact frontier and query cache.
                        refinement.refined = true;
                        continue;
                    }
                    // An unsuccessful quick pass is explicit, and uses the existing retry cooldown.
                    // Do not let four no-progress searches monopolize the entire actor queue.
                    slot.search.finishBudget();
                    complete = true;
                }
            }
            if (!complete) { continue; }
        CompletedPlanJob completedJob = {};
        completedJob.jobId = job.jobId;
        completedJob.generation = job.generation;
        completedJob.request = job.request;
        Engine::VitaStall::workerStage(31, job.request.actorIndex);
        completedJob.result = slot.search.takeResult();
        Engine::VitaStall::workerStage(32, job.request.actorIndex);

        {
            auto lock = lockPathQueue(m_jobMutex);

            --m_runningJobs;
            if (job.request.actorIndex < m_actorJobCancellation.size()
                && m_actorJobCancellation[job.request.actorIndex] == job.cancelled)
            {
                m_actorJobCancellation[job.request.actorIndex].reset();
            }
            if (!m_stopWorkers && completedJob.generation == m_generation
                && !job.cancelled->load(std::memory_order_relaxed))
            {
                ++m_completedJobs;
                m_partialJobs += completedJob.result.status == PathPlanStatus::Partial;
                m_successJobs += completedJob.result.status == PathPlanStatus::Success;
                m_budgetJobs += completedJob.result.status == PathPlanStatus::NodeLimitExceeded;
                m_refinementJobs += slot.refined;
                m_maxSearchMs = std::max(m_maxSearchMs, uint64_t(std::max<int64_t>(0,
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - slot.startedAt).count())));
                m_analyzedNodes += completedJob.result.analyzedNodeCount;
                const auto deliveryMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - job.queuedAt).count();
                m_maxDeliveryMs = std::max(m_maxDeliveryMs, uint64_t(std::max<int64_t>(0, deliveryMs)));
                m_completedPlanJobs.push_back(std::move(completedJob));
            }
        }
            slot.active = false;
            slot.job = {}; // Release snapshots/results outside the queue mutex.
        }
        const bool hasActive = std::any_of(searches.begin(), searches.end(),
            [](const ActiveSearch &slot) { return slot.active; });
        if (!hasActive)
        {
            std::unique_lock<std::mutex> lock(m_jobMutex);
            m_jobCondition.wait(lock, [this]() { return m_stopWorkers || !m_pendingPlanJobs.empty(); });
            if (m_stopWorkers) { return; }
#if defined(OPENYAMM_VITA_LOADER)
            lastYield = 0;
#endif
        }
    }
}
}
