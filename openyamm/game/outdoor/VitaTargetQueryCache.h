#pragma once
#include "game/outdoor/VitaSpatialIndex.h"
#include <array>
#include <cstdint>
#include <vector>
namespace OpenYAMM::Game
{
// Target-query reuse only: no targeting decision, policy, LOS or actor state is cached.
// Revision changes on every bounds/membership change; equal inputs have equal output order.
class VitaTargetQueryCache
{
    struct Entry
    {
        VitaSpatialIndex::Bounds bounds{};
        const VitaSpatialIndex *owner = nullptr;
        uint64_t revision = 0;
        size_t actor = 0;
        bool valid = false;
        std::vector<size_t> result;
    };
    std::array<Entry,64> m_entries{};
public:
    bool query(const VitaSpatialIndex &index, size_t actor,
        const VitaSpatialIndex::Bounds &bounds, std::vector<size_t> &result)
    {
        if (index.size() > 512) { index.query(bounds,result); return false; }
        auto &entry = m_entries[actor % m_entries.size()];
        const auto &b = entry.bounds;
        const bool hit = entry.valid && entry.owner == &index && entry.actor == actor && entry.revision == index.revision()
            && b.minX == bounds.minX && b.minY == bounds.minY
            && b.maxX == bounds.maxX && b.maxY == bounds.maxY;
        if (!hit)
        {
            entry.valid = false;
            index.queryTargets(bounds,entry.result);
            entry.owner = &index; entry.bounds = bounds; entry.actor = actor;
            entry.revision = index.revision(); entry.valid = true;
        }
        result = entry.result;
        return hit;
    }
};
}
