#pragma once
#include "engine/VitaFrameProfile.h"

namespace OpenYAMM::Engine::VitaDetailProfile
{
enum Bucket
{
    ActorFrame, ActorSelection, ActorSpatialIndex, ActorFacts, ActorTargets, ActorTargetResolve,
    ActorLos, ActorMarshal, ActorDecision, ActorBackground, ActorStatus, ActorChooseTarget,
    ActorMerge, ActorApply, ActorColliderBuild, ActorColliderIndex, ActorMove,
    CollisionFloor, CollisionModels, CollisionDecorations, CollisionActors, CollisionOverlaps,
    CollisionSprites, ActorRequests,
    ActorInactive, ActorSideEffects, ProjectileStep, ProjectileCollision, ProjectileArea, ProjectileApply,
    ProjectileParty, ProjectileActors, ProjectileFaces, ProjectileTerrain, TargetQuery, TargetFilter, TargetPolicy, MarshalIdentity, MarshalStats, MarshalRuntime, MarshalStatus, MarshalTargets, MarshalMovement, ActorGlobal, ActorStep, ActorCollisionQuery, ActorCollisionFilter, ActorCollisionNarrow, MoveSetup, MoveSweep, MoveResponse,
    RenderWorld, RenderTerrain, TerrainAnimation, TerrainPadding, TerrainMips, TerrainUpload, TerrainCacheBuild,
    TerrainCull, TerrainSubmit, RenderBuildings, RenderDecorations, RenderActors, RenderSky,
    RenderItems, RenderProjectiles,
    BillboardActorSetup, BillboardTexture, BillboardLighting, BillboardCull, BillboardSort, BillboardGeometry, BillboardSubmit, BillboardDecorSetup, BuildingVisibility, BuildingAnimation, BuildingSubmit, BuildingRuntimeScan, BuildingRefresh, ViewRoot, ViewOverlay, ViewWarmup, ViewMatrices, ViewFx, ViewAudio, UiRoot, UiHud, UiLayout, UiText, UiQuads, UiPortraitFx, UiStatus, UiCounters, UiBuffs, UiArt, UiParty, UiActorScan, Count
};

#if defined(OPENYAMM_VITA_PROFILE)
// Separate storage preserves the existing profiler ABI used by prebuilt bgfx objects.
// These nested counters supplement PART records; do not add DETAIL totals to PART totals.
struct Counter
{
    uint64_t elapsed = 0, self = 0, maximum = 0, selfAllocations = 0;
    uint32_t calls = 0;
};
inline bool timedBucket(Bucket bucket)
{
#if defined(OPENYAMM_VITA_AGGREGATE)
    switch (bucket)
    {
    case ActorFrame: case ActorStep: case ActorGlobal:
    case RenderWorld: case RenderTerrain:
    case RenderBuildings: case RenderDecorations: case RenderActors: case RenderSky:
    case RenderItems: case RenderProjectiles: case BuildingRuntimeScan:
    case ViewRoot: case ViewOverlay: case ViewWarmup: case ViewMatrices: case ViewFx: case ViewAudio:
    case UiRoot: case UiHud: case UiArt: case UiParty: case UiActorScan: case UiStatus: case UiCounters:
        return true;
    default: return false;
    }
#else
    return true;
#endif
}
class Scope;
struct CaptureFrame
{
    Counter counters[Count]{};
    Scope *top = nullptr;
    unsigned index = ~0u, capture = 0;
    unsigned actorDepth = 0;
};
inline thread_local CaptureFrame detail;

inline void addCalls(Bucket bucket, unsigned calls)
{
#if defined(OPENYAMM_VITA_AGGREGATE)
    // Called once after a batch, never once per candidate. An active outer scope owns the frame reset.
    if (VitaProfile::frame.active && detail.actorDepth) { detail.counters[bucket].calls += calls; }
#endif
}

class Scope
{
public:
    explicit Scope(Bucket bucket)
    {
        // Constant bucket arguments eliminate hot scopes before any TLS access or clock read.
        if (!timedBucket(bucket)) { return; }
        if (!VitaProfile::frame.active) { return; }
        if (detail.index != VitaProfile::frame.index || detail.capture != VitaProfile::frame.capture)
        {
            detail = {};
            detail.index = VitaProfile::frame.index;
            detail.capture = VitaProfile::frame.capture;
        }
        // Shared floor/LOS/collider helpers also serve party movement and projectiles.
        // Attribute them to actors only while the outer actor-update scope is live.
        if (bucket < RenderWorld && bucket != ActorFrame && !detail.actorDepth) { return; }
        m_actorFrame = bucket == ActorFrame;
        if (m_actorFrame) { ++detail.actorDepth; }
        m_counter = &detail.counters[bucket];
        m_parent = detail.top;
        detail.top = this;
        m_begin = VitaProfile::now();
        m_allocations = VitaProfile::allocations();
    }
    ~Scope() { finish(); }
    void finish()
    {
        if (!m_counter) { return; }
        const uint64_t elapsed = VitaProfile::now() - m_begin;
        const uint32_t allocations = VitaProfile::allocations() - m_allocations;
        detail.top = m_parent;
        if (m_parent)
        {
            m_parent->m_children += elapsed;
            m_parent->m_childAllocations += allocations;
        }
        m_counter->elapsed += elapsed;
        m_counter->self += elapsed >= m_children ? elapsed - m_children : 0;
        m_counter->maximum = elapsed > m_counter->maximum ? elapsed : m_counter->maximum;
        m_counter->selfAllocations += allocations - m_childAllocations;
        ++m_counter->calls;
        if (m_actorFrame) { --detail.actorDepth; }
        m_counter = nullptr;
    }
    Scope(const Scope &) = delete;
    Scope &operator=(const Scope &) = delete;
private:
    Counter *m_counter = nullptr;
    Scope *m_parent = nullptr;
    bool m_actorFrame = false;
    uint64_t m_begin = 0, m_children = 0;
    uint32_t m_allocations = 0, m_childAllocations = 0;
};

inline void end()
{
    if (!VitaProfile::frame.active || detail.index != VitaProfile::frame.index
        || detail.capture != VitaProfile::frame.capture) { return; }
    constexpr const char *names[] = {
        "actor_frame", "active_selection", "target_spatial_index", "fact_collection", "target_candidates",
        "target_resolution", "los_geometry", "fact_marshalling", "active_decision", "background_decision",
        "status_effects", "decision_target", "decision_result_merge", "apply_results", "collider_list",
        "collider_index", "movement_resolve", "floor_queries", "model_collision", "decoration_collision",
        "actor_collision", "actor_overlap", "sprite_collision", "projectile_audio_fx_requests",
        "inactive_actor_updates", "actor_side_effects", "projectile_steps", "projectile_collision",
        "projectile_area_facts", "projectile_apply",
        "projectile_party_probe", "projectile_actor_probes", "projectile_face_probes", "projectile_terrain_probe", "target_spatial_query", "target_filter", "target_policy_copy", "marshal_identity", "marshal_stats", "marshal_runtime", "marshal_status", "marshal_target_copy", "marshal_movement", "actor_global_effects", "actor_step_lifetime", "actor_collision_query", "actor_collision_filter", "actor_collision_narrow", "movement_setup", "movement_sweep", "movement_response",
        "world_passes", "terrain", "terrain_animation", "terrain_edge_padding", "terrain_mip_generation",
        "terrain_upload_enqueue", "terrain_cache_build", "terrain_visibility", "terrain_draw_submit", "buildings",
        "decoration_billboards", "actor_billboards", "sky", "world_items", "world_projectiles",
        "billboard_actor_setup", "billboard_texture_lookup", "billboard_lighting", "billboard_cull", "billboard_sort", "billboard_geometry", "billboard_submit", "billboard_decoration_setup", "building_visibility", "building_animation", "building_submit", "building_runtime_scan", "building_group_refresh", "view_frame", "view_overlay_inspection", "view_sprite_warmup", "view_matrices", "view_fx_sync", "view_audio_feedback", "ui_frame", "ui_hud", "ui_layout_resolve", "ui_text", "ui_quad_submit", "ui_portrait_fx", "ui_status_bar", "ui_counters", "ui_buff_labels", "ui_art", "ui_party_portraits", "ui_nearest_hostile_scan"
    };
    static_assert(sizeof(names) / sizeof(names[0]) == Count);
    for (unsigned i = 0; i < Count; ++i)
    {
        const Counter &counter = detail.counters[i];
        if (!counter.calls) { continue; }
#if defined(OPENYAMM_VITA_AGGREGATE)
        if (!timedBucket(Bucket(i)))
        {
            VitaProfile::print("PROFILE DETAIL_COUNT n=%u part=%s calls=%u\n",
                detail.index + 1, names[i], counter.calls);
            continue;
        }
#endif
        VitaProfile::print("PROFILE DETAIL n=%u group=%s part=%s ms=%.3f self_ms=%.3f "
            "calls=%u max_ms=%.3f self_allocs=%llu\n", detail.index + 1,
            i < RenderWorld ? "actor" : i < UiRoot ? "render" : "ui", names[i], counter.elapsed / 1000.0,
            counter.self / 1000.0, counter.calls, counter.maximum / 1000.0,
            (unsigned long long)counter.selfAllocations);
    }
}
#else
class Scope
{
public:
    explicit Scope(Bucket) {}
    void finish() {}
};
inline void end() {}
inline void addCalls(Bucket, unsigned) {}
#endif
}
