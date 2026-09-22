#pragma once
#include <algorithm>
#include <cmath>
namespace OpenYAMM::Game
{
inline float gameplayMovementSeconds(float elapsed, bool turnBasedStep)
{
    if (!std::isfinite(elapsed) || elapsed <= 0.0f) { return 0.0f; }
    // Loading, suspend and multi-second hitches must not become a held-input teleport.
    // Ordinary gameplay down to 4 FPS retains all elapsed time.
    return turnBasedStep ? elapsed : (elapsed > 0.25f ? 0.0f : elapsed);
}
inline float accumulatePartyMovement(float remainder, float elapsed, bool turnBasedStep)
{
    const float admitted = gameplayMovementSeconds(elapsed, turnBasedStep);
    return admitted == 0.0f ? 0.0f : remainder + admitted;
}
}
