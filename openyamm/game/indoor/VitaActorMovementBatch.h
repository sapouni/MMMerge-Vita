#pragma once
#include <cmath>
namespace OpenYAMM::Game
{
struct VitaActorMovementBatch
{
    bool enabled = false, integrate = true;
    float seconds = 0;
};
inline thread_local VitaActorMovementBatch vitaActorMovementBatch;
inline unsigned admittedActorSteps(float debt, float step, unsigned limit)
{
    if (!std::isfinite(debt) || !std::isfinite(step) || step <= 0) return 0;
    unsigned count = 0;
    while (debt >= step && (!limit || count < limit) && count < 128) { debt -= step; ++count; }
    return count;
}
struct VitaActorMovementBatchScope
{
    VitaActorMovementBatch previous = vitaActorMovementBatch;
    VitaActorMovementBatchScope(bool enabled, float seconds)
    { vitaActorMovementBatch = {enabled, !enabled, seconds}; }
    ~VitaActorMovementBatchScope() { vitaActorMovementBatch = previous; }
};
}
