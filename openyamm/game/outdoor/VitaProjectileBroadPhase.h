#pragma once
#include <algorithm>

namespace OpenYAMM::Game
{
// Reject only actors outside the segment's expanded XY bounds. Keep original order and exact narrow-phase tests.
inline bool vitaProjectileBoundsMayHit(float startX, float startY, float endX, float endY,
    float actorX, float actorY, float radius)
{
    return !(actorX < std::min(startX, endX) - radius || actorX > std::max(startX, endX) + radius
        || actorY < std::min(startY, endY) - radius || actorY > std::max(startY, endY) + radius);
}
}
