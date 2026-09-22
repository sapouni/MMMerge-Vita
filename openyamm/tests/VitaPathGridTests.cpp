#include "doctest/doctest.h"
#include "game/pathfinding/PathMap.h"
#include "game/indoor/IndoorRecoveryIndex.h"
#include <random>
#include <cmath>
using namespace OpenYAMM::Game;
namespace
{
PathFacet floorFacet(float x, float y, float width, float z)
{
    PathFacet f;
    f.kind = PathFacetKind::Floor; f.walkableFloor = true; f.blocking = false;
    f.vertices = {{x,y,z},{x+width,y,z},{x+width,y+width,z},{x,y+width,z}};
    return f;
}
}
TEST_CASE("Bounded path grid retains oversized floors and collision walls")
{
    std::vector<PathFacet> facets{floorFacet(-1000000,-1000000,2000000,0),floorFacet(-16,-16,32,20)};
    PathFacet wall; wall.kind = PathFacetKind::Wall;
    wall.vertices = {{50,-1000000,-1000000},{50,1000000,-1000000},
        {50,1000000,1000000},{50,-1000000,1000000}};
    facets.push_back(wall);
    PathMap grid, reference; grid.setFacets(facets); reference.setFacets(facets); grid.buildSpatialGrid(1);
    CHECK(grid.gridMemberships() <= 262144);
    CHECK(grid.overflowFacetCount() >= 2);
    std::mt19937 random(56);
    for (unsigned i = 0; i < 10000; ++i)
    {
        const float x = int(random()%2000)-1000, y = int(random()%2000)-1000;
        const PathPoint p{x,y,50};
        const auto a=grid.floorAt(p), b=reference.floorAt(p);
        REQUIRE(a.hasFloor == b.hasFloor);
        REQUIRE(a.inVoid == b.inVoid); REQUIRE(a.z == b.z); REQUIRE(a.facetIndex == b.facetIndex);
        const auto ta=grid.traceLine({x,y,40},{x+200,y,40}), tb=reference.traceLine({x,y,40},{x+200,y,40});
        REQUIRE(ta.blocked == tb.blocked);
        if (ta.blocked) { REQUIRE(std::fabs(ta.point.x-tb.point.x) < 0.001f); }
    }
    // A very broad query must take the exact fallback, not enumerate a cell-volume cube.
    CHECK(grid.traceLine({-100000,0,40},{100000,0,40}).blocked);
    grid.clear(); CHECK(grid.gridMemberships()==0); CHECK(grid.overflowFacetCount()==0);
}
TEST_CASE("Path grid total membership budget retains late facets")
{
    std::vector<PathFacet> facets;
    for (int i=0;i<200;++i) { facets.push_back(floorFacet(-31,-31,62,float(i*5))); }
    PathMap grid, reference; grid.setFacets(facets); reference.setFacets(facets); grid.buildSpatialGrid(1);
    CHECK(grid.gridMemberships() <= 262144); CHECK(grid.overflowFacetCount() > 0);
    for (int i=0;i<1200;++i)
    {
        const auto a=grid.floorAt({0,0,float(i)}), b=reference.floorAt({0,0,float(i)});
        REQUIRE(a.hasFloor == b.hasFloor); REQUIRE(a.inVoid == b.inVoid);
        REQUIRE(a.z == b.z); REQUIRE(a.facetIndex == b.facetIndex);
    }
    // Mutation discards old overflow and grid memberships.
    grid.setFacets({floorFacet(100,100,8,10)});
    CHECK_FALSE(grid.floorAt({0,0,20}).hasFloor);
}
TEST_CASE("Recovery broad phase preserves floor traversal order and moved geometry")
{
    struct Vertex {float x,y;};
    struct Face {std::vector<size_t> vertexIndices;};
    struct Sector {unsigned flags=0;std::vector<size_t> floorFaceIds,portalFaceIds;};
    struct Map {std::vector<Face> faces;std::vector<Sector> sectors;};
    Map map;map.sectors.resize(9);std::vector<Vertex> vertices;
    std::mt19937 random(56);
    for(size_t i=0;i<700;++i)
    {
        float x=int(random()%10000)-5000,y=int(random()%10000)-5000;
        vertices.push_back({x,y});vertices.push_back({x+32,y+64});
        map.faces.push_back({{i*2,i*2+1}});map.sectors[i%9].floorFaceIds.push_back(i);
        if(i%7==0){map.sectors[i%9].flags=8;map.sectors[i%9].portalFaceIds.push_back(i);}
    }
    IndoorRecoveryIndex index;std::vector<size_t> actual,expected;
    for(unsigned iteration=0;iteration<20000;++iteration)
    {
        if(iteration%37==0)
        {
            const size_t i=random()%700;vertices[i*2].x+=150;vertices[i*2+1].x+=150;index.invalidate();
        }
        const float x=int(random()%12000)-6000,y=int(random()%12000)-6000;
        expected.clear();
        auto append=[&](size_t i)
        {
            const auto a=vertices[i*2],b=vertices[i*2+1];
            if(x>=a.x-8&&x<=b.x+8&&y>=a.y-8&&y<=b.y+8)expected.push_back(i);
        };
        for(const auto &sector:map.sectors)
        {for(size_t i:sector.floorFaceIds)append(i);if(sector.flags&8)for(size_t i:sector.portalFaceIds)append(i);}
        index.query(map,vertices,x,y,actual);REQUIRE(actual==expected);
    }
    map={};vertices.clear();index.invalidate();index.query(map,vertices,0,0,actual);CHECK(actual.empty());
}
