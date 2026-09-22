#pragma once
#include <algorithm>
#include <cmath>
#include <limits>
namespace OpenYAMM::Game
{
// Conservative 2D row clipping. Keep source cell order and leave the exact 3D triangles unchanged.
struct VitaTerrainRayCells
{
    float x, y, dx, dy; int width, height;
    template<class Visit> void visit(Visit cell) const
    {
        const auto clip = [](float origin, float direction, float low, float high, float &begin, float &end)
        {
            if (direction == 0) { return origin >= low && origin <= high; }
            float a = (low - origin) / direction, b = (high - origin) / direction;
            if (a > b) { std::swap(a, b); }
            begin = std::max(begin, a); end = std::min(end, b); return begin <= end;
        };
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(dx) || !std::isfinite(dy)) { return; }
        constexpr float margin = 0.002f;
        for (int row = 0; row < height; ++row)
        {
            float begin = 0, end = std::numeric_limits<float>::infinity();
            if (!clip(y, dy, float(row) - margin, float(row + 1) + margin, begin, end)
                || !clip(x, dx, -margin, float(width) + margin, begin, end)) { continue; }
            const float a = dx == 0 ? x : x + dx * begin;
            const float b = dx == 0 ? x : x + dx * end;
            // Catastrophic cancellation can only widen the fallback, never reject a valid hit.
            int first = 0, last = width - 1;
            if (std::isfinite(a) && std::isfinite(b))
            {
                first = std::max(0, int(std::floor(std::clamp(std::min(a,b), -1.0f, float(width)))) - 1);
                last = std::min(width - 1, int(std::floor(std::clamp(std::max(a,b), -1.0f, float(width)))) + 1);
            }
            for (int column = first; column <= last; ++column) { cell(column, row); }
        }
    }
};
}
