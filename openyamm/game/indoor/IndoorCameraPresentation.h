#pragma once
#include "game/indoor/IndoorMovementController.h"
#include <algorithm>
#include <cmath>
namespace OpenYAMM::Game
{
inline bx::Vec3 interpolateIndoorEye(const IndoorMoveState &a, const IndoorMoveState &b,
    float fraction, bool continuous)
{
    const bx::Vec3 current{b.x, b.y, b.eyeZ()};
    // Never interpolate a teleport, support/portal crossing, landing or large correction.
    if (!continuous || !std::isfinite(fraction) || a.sectorId != b.sectorId || a.eyeSectorId != b.eyeSectorId
        || a.supportFaceIndex != b.supportFaceIndex || a.grounded != b.grounded
        || b.landedThisFrame || std::abs(a.x-b.x) > 16 || std::abs(a.y-b.y) > 16
        || std::abs(a.eyeZ()-b.eyeZ()) > 16) { return current; }
    const float t = std::clamp(fraction, 0.0f, 1.0f);
    return {a.x + (b.x-a.x)*t, a.y + (b.y-a.y)*t, a.eyeZ() + (b.eyeZ()-a.eyeZ())*t};
}
}
