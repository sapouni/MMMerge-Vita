#pragma once

#include "game/pathfinding/PathMap.h"
#include <memory>
#include "game/pathfinding/PathfindingTypes.h"

namespace OpenYAMM::Game
{
// Resumable search; advancing preserves A* ordering and exact collision tests.
// The caller must retain the immutable map and cancellation token until completion.
class PathSearch
{
public:
    PathSearch();
    ~PathSearch();
    PathSearch(PathSearch &&) noexcept;
    PathSearch &operator=(PathSearch &&) noexcept;
    void reset(const PathMap &map, const PathPlanRequest &request);
    bool advance(size_t nodeBudget);
    size_t analyzedNodes() const;
    // Only returns a prefix made of already validated edges, with genuine progress.
    bool finishProgress(float minimumDistance = 0.0f);
    void finishBudget();
    PathPlanResult takeResult();
private:
    struct State;
    std::unique_ptr<State> m_state;
};

class PathPlanner
{
public:
    PathPlanResult plan(const PathMap &map, const PathPlanRequest &request);
};
}
