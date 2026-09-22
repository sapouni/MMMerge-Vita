#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace OpenYAMM::Game
{
// A flat balanced bounds tree. Queries retain source order for deterministic collision tie breaking.
class VitaSpatialIndex
{
public:
    struct Bounds { float minX, minY, maxX, maxY; };
    void clear() { m_entries.clear(); m_nodes.clear(); m_entryBySource.clear(); m_leafBySource.clear(); m_refits = 0; ++m_revision; }
    uint64_t revision() const { return m_revision; }
    size_t size() const { return m_entries.size(); }
    template<class BoundsFor> void rebuild(size_t count, BoundsFor boundsFor)
    {
        clear();
        m_entries.reserve(count); m_nodes.reserve(count * 2);
        for (size_t i = 0; i < count; ++i) { m_entries.push_back({boundsFor(i), i}); }
        m_entryBySource.resize(count); m_leafBySource.resize(count);
        if (count) { build(0, count); }
        for (size_t i = 0; i < m_entries.size(); ++i) { m_entryBySource[m_entries[i].index] = i; }
    }
    // Update one moving collider without scanning/repartitioning the whole actor list.
    void update(size_t sourceIndex, const Bounds &bounds)
    {
        if (sourceIndex >= m_entryBySource.size()) { return; }
        auto &old = m_entries[m_entryBySource[sourceIndex]].bounds;
        if (old.minX == bounds.minX && old.minY == bounds.minY
            && old.maxX == bounds.maxX && old.maxY == bounds.maxY) return;
        ++m_revision;
        old = bounds;
        size_t index = m_leafBySource[sourceIndex];
        Node &leaf = m_nodes[index];
        leaf.bounds = m_entries[leaf.begin].bounds;
        for (size_t i = leaf.begin + 1; i < leaf.end; ++i)
        { leaf.bounds = combined(leaf.bounds, m_entries[i].bounds); }
        while (index)
        {
            index = m_nodes[index].parent;
            Node &node = m_nodes[index];
            node.bounds = combined(m_nodes[node.left].bounds, m_nodes[node.right].bounds);
        }
    }
    void query(const Bounds &bounds, std::vector<size_t> &result) const
    {
        result.clear();
        if (!m_nodes.empty()) visit(0,bounds,result);
        std::sort(result.begin(),result.end());
    }
    // Wide-target fast path is opt-in; small collision/floor queries retain the old path.
    void queryTargets(const Bounds &bounds, std::vector<size_t> &result) const
    {
        result.clear();
        if (m_nodes.empty()) return;
        if (contains(bounds, m_nodes[0].bounds))
        {
            // Wide combat queries often contain an entire actor cluster. Preserve
            // source order directly, without walking leaves and then sorting it back.
            result.resize(m_entries.size());
            for (size_t i = 0; i < result.size(); ++i) result[i] = i;
            return;
        }
        visit(0, bounds, result);
        std::sort(result.begin(), result.end());
    }
    template<class Colliders> void rebuildColliders(const Colliders &colliders)
    {
        rebuild(colliders.size(), [&colliders](size_t i)
        {
            const auto &collider = colliders[i];
            const float x = float(collider.worldX), y = float(collider.worldY), radius = float(collider.radius);
            return Bounds{x - radius, y - radius, x + radius, y + radius};
        });
    }
    template<class BoundsFor> void refit(size_t count, BoundsFor boundsFor)
    {
        // Refresh current positions without repartitioning the same actor list for every moving actor.
        // Periodic rebuilding keeps the tree useful after large movements; queries retain source order.
        if (count != m_entries.size() || ++m_refits >= 128)
        {
            rebuild(count, boundsFor);
            return;
        }
        bool changed = false;
        for (Entry &entry : m_entries)
        {
            const Bounds bounds = boundsFor(entry.index);
            changed |= entry.bounds.minX != bounds.minX || entry.bounds.minY != bounds.minY
                || entry.bounds.maxX != bounds.maxX || entry.bounds.maxY != bounds.maxY;
            entry.bounds = bounds;
        }
        if (!changed) { return; }
        ++m_revision;
        for (size_t i = m_nodes.size(); i-- > 0;)
        {
            Node &node = m_nodes[i];
            if (node.left)
            {
                node.bounds = combined(m_nodes[node.left].bounds, m_nodes[node.right].bounds);
            }
            else
            {
                node.bounds = m_entries[node.begin].bounds;
                for (size_t j = node.begin + 1; j < node.end; ++j)
                {
                    node.bounds = combined(node.bounds, m_entries[j].bounds);
                }
            }
        }
    }
    template<class Colliders> void refitColliders(const Colliders &colliders)
    {
        refit(colliders.size(), [&colliders](size_t i)
        {
            const auto &c = colliders[i];
            const float x = float(c.worldX), y = float(c.worldY), radius = float(c.radius);
            return Bounds{x - radius, y - radius, x + radius, y + radius};
        });
    }

private:
    struct Entry { Bounds bounds; size_t index; };
    struct Node { Bounds bounds; size_t begin, end, left = 0, right = 0, parent = 0; };
    std::vector<Entry> m_entries;
    std::vector<Node> m_nodes;
    std::vector<size_t> m_entryBySource, m_leafBySource;
    unsigned m_refits = 0;
    uint64_t m_revision = 0;
    static Bounds combined(const Bounds &a, const Bounds &b)
    {
        return {std::min(a.minX, b.minX), std::min(a.minY, b.minY),
            std::max(a.maxX, b.maxX), std::max(a.maxY, b.maxY)};
    }
    static bool contains(const Bounds &outer, const Bounds &inner)
    {
        return outer.minX <= inner.minX && outer.minY <= inner.minY
            && outer.maxX >= inner.maxX && outer.maxY >= inner.maxY;
    }
    static bool overlaps(const Bounds &a, const Bounds &b)
    {
        return !(a.maxX < b.minX || a.minX > b.maxX || a.maxY < b.minY || a.minY > b.maxY);
    }
    size_t build(size_t begin, size_t end)
    {
        Bounds bounds = m_entries[begin].bounds;
        for (size_t i = begin + 1; i < end; ++i)
        {
            bounds.minX = std::min(bounds.minX, m_entries[i].bounds.minX);
            bounds.minY = std::min(bounds.minY, m_entries[i].bounds.minY);
            bounds.maxX = std::max(bounds.maxX, m_entries[i].bounds.maxX);
            bounds.maxY = std::max(bounds.maxY, m_entries[i].bounds.maxY);
        }
        const size_t node = m_nodes.size();
        m_nodes.push_back({bounds, begin, end});
        if (end - begin > 8)
        {
            const bool splitX = bounds.maxX - bounds.minX >= bounds.maxY - bounds.minY;
            const size_t middle = begin + (end - begin) / 2;
            std::nth_element(m_entries.begin() + begin, m_entries.begin() + middle, m_entries.begin() + end,
                [splitX](const Entry &a, const Entry &b)
                {
                    return splitX ? a.bounds.minX + a.bounds.maxX < b.bounds.minX + b.bounds.maxX :
                        a.bounds.minY + a.bounds.maxY < b.bounds.minY + b.bounds.maxY;
                });
            const size_t left = build(begin, middle), right = build(middle, end);
            m_nodes[node].left = left; m_nodes[node].right = right;
            m_nodes[left].parent = node; m_nodes[right].parent = node;
        }
        else
        {
            for (size_t i = begin; i < end; ++i) { m_leafBySource[m_entries[i].index] = node; }
        }
        return node;
    }
    void visit(size_t index, const Bounds &bounds, std::vector<size_t> &result) const
    {
        const Node &node = m_nodes[index];
        if (!overlaps(node.bounds, bounds)) { return; }
        if (node.left)
        {
            visit(node.left, bounds, result); visit(node.right, bounds, result);
            return;
        }
        for (size_t i = node.begin; i < node.end; ++i)
        {
            if (overlaps(m_entries[i].bounds, bounds)) { result.push_back(m_entries[i].index); }
        }
    }
};
}
