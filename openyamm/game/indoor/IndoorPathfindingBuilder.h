#pragma once

#include "game/indoor/IndoorGeometryUtils.h"
#include "game/pathfinding/PathMap.h"

#include <cstddef>
#include <vector>

namespace OpenYAMM::Game
{
struct IndoorPathMapBuildResult
{
    PathMap pathMap;
    size_t sourceFaceCount = 0;
    size_t pathFacetCount = 0;
    size_t skippedFaceCount = 0;
};

class IndoorPathfindingBuilder
{
public:
    static void updateFaces(PathMap &map, const IndoorMapData &data,
        const std::vector<IndoorVertex> &vertices, IndoorFaceGeometryCache &cache,
        const std::vector<uint16_t> &faces);
    static IndoorPathMapBuildResult buildPathMap(
        const IndoorMapData &indoorMapData,
        const std::vector<IndoorVertex> &vertices,
        const MapDeltaData *pMapDeltaData = nullptr,
        IndoorFaceGeometryCache *pGeometryCache = nullptr,
        float spatialGridCellSize = 512.0f
    );
};
}
