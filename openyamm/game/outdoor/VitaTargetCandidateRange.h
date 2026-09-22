#pragma once
#include <algorithm>
#include <cmath>
namespace OpenYAMM::Game
{
// The resolver selects a sensed party even when its LOS is blocked. An actor
// farther than that provisional target cannot replace it; retain ties conservatively.
inline float targetCandidateRange(float dx, float dy, float dz, float partySenseRange,
    bool partyInvisible, float maximumRange)
{
    const float d2 = dx * dx + dy * dy + dz * dz;
    if (partyInvisible || partySenseRange <= 0.0f || std::abs(dx) > partySenseRange
        || std::abs(dy) > partySenseRange || std::abs(dz) > partySenseRange
        || !(d2 <= partySenseRange * partySenseRange)) { return maximumRange; }
    // One world unit of slack keeps the broad-phase bound conservative under float rounding.
    return std::min(maximumRange, std::sqrt(d2) + 1.0f);
}
}
