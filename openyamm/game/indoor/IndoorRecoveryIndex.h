#pragma once

#include "game/outdoor/VitaSpatialIndex.h"
#include <cmath>
#include <limits>
#include <vector>

namespace OpenYAMM::Game
{
// Entries retain the old sector/floor traversal order, including duplicate face references.
// Exact floor tests still decide support; this only excludes faces outside the XY probe.
class IndoorRecoveryIndex
{
public:
    void invalidate() { m_valid = false; }
    template<class Map, class Vertices>
    void query(const Map &map, const Vertices &vertices, float x, float y, std::vector<size_t> &faces)
    {
        if (!m_valid)
        {
            m_faces.clear();
            for (const auto &sector : map.sectors)
            {
                m_faces.insert(m_faces.end(), sector.floorFaceIds.begin(), sector.floorFaceIds.end());
                if ((sector.flags & 0x8) != 0)
                { m_faces.insert(m_faces.end(), sector.portalFaceIds.begin(), sector.portalFaceIds.end()); }
            }
            m_index.rebuild(m_faces.size(), [&](size_t index)
            {
                const float infinity = std::numeric_limits<float>::infinity();
                const float extent = std::numeric_limits<float>::max() / 4;
                VitaSpatialIndex::Bounds bounds{infinity, infinity, -infinity, -infinity};
                const size_t face = m_faces[index];
                if (face < map.faces.size())
                {
                    for (size_t vertex : map.faces[face].vertexIndices)
                    {
                        if (vertex >= vertices.size()) { continue; }
                        const float vx = float(vertices[vertex].x), vy = float(vertices[vertex].y);
                        if (!std::isfinite(vx) || !std::isfinite(vy))
                        { return VitaSpatialIndex::Bounds{-extent, -extent, extent, extent}; }
                        bounds.minX = std::min(bounds.minX, vx); bounds.maxX = std::max(bounds.maxX, vx);
                        bounds.minY = std::min(bounds.minY, vy); bounds.maxY = std::max(bounds.maxY, vy);
                    }
                }
                // Malformed/empty faces remain candidates for the authoritative validator.
                if (bounds.minX > bounds.maxX)
                { return VitaSpatialIndex::Bounds{-extent, -extent, extent, extent}; }
                // Matches evaluateIndoorFloorFace's FloorSlack (8 units).
                bounds.minX -= 8.0f; bounds.maxX += 8.0f;
                bounds.minY -= 8.0f; bounds.maxY += 8.0f;
                return bounds;
            });
            m_valid = true;
        }
        m_index.query({x, y, x, y}, faces);
        for (size_t &face : faces) { face = m_faces[face]; }
    }
    size_t totalFaces() const { return m_faces.size(); }
private:
    bool m_valid = false;
    std::vector<size_t> m_faces;
    VitaSpatialIndex m_index;
};
}
