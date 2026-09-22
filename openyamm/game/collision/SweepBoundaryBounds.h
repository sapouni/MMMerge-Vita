#pragma once
#include <algorithm>
#include <bx/math.h>

namespace OpenYAMM::Game
{
// A conservative broad phase only. Keep the original edge/point solver and order
// for anything that can intersect. Padding also covers its endpoint tolerances.
struct SweepBoundaryBounds
{
    bx::Vec3 min{0,0,0}, max{0,0,0};
    SweepBoundaryBounds(const bx::Vec3 &start, const bx::Vec3 &direction, float distance, float radius)
    {
        const bx::Vec3 end{start.x + direction.x * distance, start.y + direction.y * distance,
            start.z + direction.z * distance};
        const float extent = std::max(0.0f, radius) + 0.125f;
        min = {std::min(start.x, end.x) - extent, std::min(start.y, end.y) - extent,
            std::min(start.z, end.z) - extent};
        max = {std::max(start.x, end.x) + extent, std::max(start.y, end.y) + extent,
            std::max(start.z, end.z) + extent};
    }
    bool touches(const bx::Vec3 &point) const
    {
        return !(point.x < min.x || point.x > max.x || point.y < min.y || point.y > max.y
            || point.z < min.z || point.z > max.z);
    }
    bool touches(const bx::Vec3 &a, const bx::Vec3 &b) const
    {
        return !(std::max(a.x,b.x) < min.x || std::min(a.x,b.x) > max.x
            || std::max(a.y,b.y) < min.y || std::min(a.y,b.y) > max.y
            || std::max(a.z,b.z) < min.z || std::min(a.z,b.z) > max.z);
    }
};
}
