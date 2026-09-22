#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace OpenYAMM::Game
{
// Presentation only. Never use these tests to stop simulation or change AI visibility.
inline bool vitaActorRenderReduced(float distanceSquared, float estimatedPixels,
    float distanceLimit, float pixelLimit, bool highlighted)
{
    return !highlighted && ((distanceLimit > 0 && distanceSquared > distanceLimit * distanceLimit)
        || (pixelLimit > 0 && estimatedPixels > 0 && estimatedPixels < pixelLimit));
}

// A sphere enclosing the complete sprite rectangle gives an upper bound on its projected size.
// Do not reject near-plane intersections or non-finite values.
inline bool vitaActorSubpixel(float radius, float depth, float viewportHeight, float pixelLimit)
{
    if (!(radius > 0 && depth > radius && viewportHeight > 0 && pixelLimit > 0)) { return false; }
    const float pixels = radius * viewportHeight / ((depth - radius) * 0.57735026919f);
    return std::isfinite(pixels) && pixels < pixelLimit;
}

inline uint32_t vitaActorPresentationTicks(uint32_t ticks, bool reduced)
{
    // Four visual samples per simulated second. Actor state and movement are not modified.
    return reduced ? ticks / 32u * 32u : ticks;
}
}
