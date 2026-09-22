#pragma once
#include <cmath>
#include <cstdio>
#include "engine/VitaFrameLimit.h"

namespace OpenYAMM::Game
{
struct VitaPerformanceSettings
{
#if defined(OPENYAMM_VITA_LOADER)
    unsigned vanillaDistance = 1;
#else
    unsigned vanillaDistance = 0;
#endif
    unsigned prewarmSprites = 0;
    unsigned frameLimit = 0;
    unsigned geometryHighlight = 1;
    unsigned opaqueSurfaces = 1;
    unsigned worldResolution = 0, fastOpaqueShader = 1;
#if defined(OPENYAMM_VITA_LOADER)
    unsigned adaptiveWorldResolution = 1;
#else
    unsigned adaptiveWorldResolution = 0;
#endif
    unsigned spriteHalfResolution = 0, indoorVertexLighting = 0;
    unsigned houseDiagnostics = 0, housePausePresentation = 0;
    unsigned indoorDiagnostics = 0, movementDiagnostics = 0;
    unsigned houseWorkerAffinity = 1;
    unsigned pathWorkerAffinity = 1;
    unsigned pathWorkers = 1;
    unsigned pathProgressive = 0, resolutionIndicator = 1, movementInterpolation = 1;
    unsigned initialViewWarmup = 1;
    unsigned indoorFrameMovement = 0;
    unsigned yamlEventCache = 1, hudReadyCache = 1, actorFactReuse = 1, spriteFastShader = 0;
    unsigned actorRenderLod = 0;
    unsigned lowTextureFiltering = 0;
    float actorRenderLodDistance = 8192, actorRenderLodPixels = 12, actorRenderMinPixels = 1;
    float farClip = 0, actorCutoff = 0, fogNear = -1, fogFar = -1;
#if defined(OPENYAMM_VITA_LOADER)
    unsigned actorSubstepCap = 0, simulationHz = 64, aiLodPeriod = 4, simulationMaxSteps = 6;
#else
    unsigned actorSubstepCap = 0, simulationHz = 0, aiLodPeriod = 0, simulationMaxSteps = 0;
#endif
    unsigned terrainResidentFrames = 1, terrainFastShader = 1;
    unsigned terrainFrustum = 1, lightingCoalesce = 0, extraFrustum = 1;
    unsigned actorWorkers = 0;
    unsigned actorFactWorkers = 0;
    unsigned dormantActors = 0;
    float dormantRadius = 24576;
};
inline VitaPerformanceSettings &vitaPerformanceSettings()
{
    static VitaPerformanceSettings settings = []
    {
        VitaPerformanceSettings result;
#if defined(OPENYAMM_VITA_LOADER)
        FILE *file = std::fopen("/data/vita-performance.ini", "r");
        if (file)
        {
            char line[160];
            while (std::fgets(line, sizeof(line), file))
            {
                unsigned value = 0;
                if (std::sscanf(line, " path_progressive = %u", &value) == 1 && value <= 1) result.pathProgressive = value;
                if (std::sscanf(line, " resolution_indicator = %u", &value) == 1 && value <= 1) result.resolutionIndicator = value;
                if (std::sscanf(line, " frame_limit = %u", &value) == 1 && Engine::VitaFrameLimit::valid(value)) result.frameLimit = value;
                if (std::sscanf(line, " movement_interpolation = %u", &value) == 1 && value <= 1) result.movementInterpolation = value;
                if (std::sscanf(line, " sprite_half_resolution = %u", &value) == 1 && value <= 1) result.spriteHalfResolution = value;
                if (std::sscanf(line, " indoor_vertex_lighting = %u", &value) == 1 && value <= 1) result.indoorVertexLighting = value;
                if (std::sscanf(line, " adaptive_world_resolution = %u", &value) == 1 && value <= 1) result.adaptiveWorldResolution = value;
                if (std::sscanf(line, " world_resolution = %u", &value) == 1 && value <= 1) result.worldResolution = value;
                if (std::sscanf(line, " fast_opaque_shader = %u", &value) == 1 && value <= 1) result.fastOpaqueShader = value;
                if (std::sscanf(line, " opaque_surfaces = %u", &value) == 1 && value <= 1) result.opaqueSurfaces = value;
                if (std::sscanf(line, " geometry_highlight = %u", &value) == 1 && value <= 1) result.geometryHighlight = value;
                if (std::sscanf(line, " house_worker_affinity = %u", &value) == 1 && value <= 1)
                { result.houseWorkerAffinity = value; }
                if (std::sscanf(line, " path_worker_affinity = %u", &value) == 1 && value <= 1)
                { result.pathWorkerAffinity = value; }
                if (std::sscanf(line, " initial_view_warmup = %u", &value) == 1 && value <= 1) result.initialViewWarmup = value;
                if (std::sscanf(line, " indoor_frame_movement = %u", &value) == 1 && value <= 1) result.indoorFrameMovement = value;
                if (std::sscanf(line, " path_workers = %u", &value) == 1 && (value == 1 || value == 2)) result.pathWorkers = value;
                if (std::sscanf(line, " movement_diagnostics = %u", &value) == 1 && value <= 1) result.movementDiagnostics = value;
                if (std::sscanf(line, " indoor_diagnostics = %u", &value) == 1 && value <= 1) result.indoorDiagnostics = value;
                if (std::sscanf(line, " house_diagnostics = %u", &value) == 1 && value <= 1) result.houseDiagnostics = value;
                if (std::sscanf(line, " house_pause_presentation = %u", &value) == 1 && value <= 1) result.housePausePresentation = value;
                if (std::sscanf(line, " yaml_event_cache = %u", &value) == 1 && value <= 1) result.yamlEventCache = value;
                if (std::sscanf(line, " hud_ready_cache = %u", &value) == 1 && value <= 1) result.hudReadyCache = value;
                if (std::sscanf(line, " actor_fact_reuse = %u", &value) == 1 && value <= 1) result.actorFactReuse = value;
                if (std::sscanf(line, " sprite_fast_shader = %u", &value) == 1 && value <= 1) result.spriteFastShader = value;
                if (std::sscanf(line, " low_texture_filtering = %u", &value) == 1 && value <= 1) result.lowTextureFiltering = value;
                float radius = 0;
                if (std::sscanf(line, " prewarm_sprites = %u", &value) == 1 && value <= 1)
                { result.prewarmSprites = value; }
                if (std::sscanf(line, " simulation_max_steps = %u", &value) == 1 && value <= 64)
                { result.simulationMaxSteps = value; }
                // dormant_actors is intentionally ignored: dormancy has been retired.
                if (std::sscanf(line, " actor_fact_workers = %u", &value) == 1 && (value == 0 || value == 2 || value == 3))
                { result.actorFactWorkers = value; }
                if (std::sscanf(line, " actor_workers = %u", &value) == 1 && (value == 0 || value == 2 || value == 3))
                { result.actorWorkers = value; }
                if (std::sscanf(line, " dormant_radius = %f", &radius) == 1 && std::isfinite(radius)
                    && radius >= 0 && radius <= 32768) { result.dormantRadius = radius; }
                if (std::sscanf(line, " terrain_resident_frames = %u", &value) == 1 && value <= 1)
                { result.terrainResidentFrames = value; }
                if (std::sscanf(line, " terrain_fast_shader = %u", &value) == 1 && value <= 1)
                { result.terrainFastShader = value; }
                if (std::sscanf(line, " terrain_frustum = %u", &value) == 1 && value <= 1)
                { result.terrainFrustum = value; }
                if (std::sscanf(line, " lighting_coalesce = %u", &value) == 1 && value <= 1)
                { result.lightingCoalesce = value; }
                if (std::sscanf(line, " extra_frustum = %u", &value) == 1 && value <= 1)
                { result.extraFrustum = value; }
                if (std::sscanf(line, " actor_render_lod = %u", &value) == 1 && value <= 1)
                { result.actorRenderLod = value; }
                float distance = 0;
                if (std::sscanf(line, " actor_render_lod_distance = %f", &distance) == 1
                    && std::isfinite(distance) && distance >= 0 && distance <= 32768)
                { result.actorRenderLodDistance = distance; }
                if (std::sscanf(line, " actor_render_lod_pixels = %f", &distance) == 1
                    && std::isfinite(distance) && distance >= 0 && distance <= 64)
                { result.actorRenderLodPixels = distance; }
                if (std::sscanf(line, " actor_render_min_pixels = %f", &distance) == 1
                    && std::isfinite(distance) && distance >= 0 && distance <= 4)
                { result.actorRenderMinPixels = distance; }
                if (std::sscanf(line, " vanilla_distance = %u", &value) == 1 && value <= 1)
                { result.vanillaDistance = value; }
                if (std::sscanf(line, " far_clip = %f", &distance) == 1 && std::isfinite(distance)
                    && distance >= 0 && distance <= 32768) { result.farClip = distance; }
                if (std::sscanf(line, " actor_cutoff = %f", &distance) == 1 && std::isfinite(distance)
                    && distance >= 0 && distance <= 32768) { result.actorCutoff = distance; }
                if (std::sscanf(line, " fog_near = %f", &distance) == 1 && std::isfinite(distance)
                    && distance >= -1 && distance <= 32768) { result.fogNear = distance; }
                if (std::sscanf(line, " fog_far = %f", &distance) == 1 && std::isfinite(distance)
                    && distance >= -1 && distance <= 32768) { result.fogFar = distance; }
                if (std::sscanf(line, " actor_substep_cap = %u", &value) == 1 && value <= 8)
                { result.actorSubstepCap = value; }
                if (std::sscanf(line, " simulation_hz = %u", &value) == 1 && (value == 0 || value == 32 || value == 64))
                { result.simulationHz = value; }
                if (std::sscanf(line, " ai_lod_period = %u", &value) == 1 && (value == 0 || value == 4))
                { result.aiLodPeriod = value; }
            }
            std::fclose(file);
        }
        else
        {
            file = std::fopen("/data/vita-performance.ini", "w");
            if (file)
            {
                std::fputs("# Restart after editing. Simulation rate/cap apply indoors and outdoors.\n"
                    "vanilla_distance=1\nprewarm_sprites=0\n"
                    "# Diagnostic cap drops actor time; not a shipping option.\n"
                    "actor_substep_cap=0\n# 0: original; 32 or 64: adjusted decision dt.\n"
                    "simulation_hz=64\n# 4: defer distant off-screen decisions; movement remains continuous.\n"
                    "ai_lod_period=4\nsimulation_max_steps=6\nactor_workers=0\nactor_fact_workers=0\ndormant_actors=0\ndormant_radius=24576\nexplicit_buffer_sync=0\nterrain_frustum=1\nlighting_coalesce=0\ntransient_reuse=1\nextra_frustum=1\n", file);
                std::fclose(file);
            }
        }
        std::fprintf(stderr, "VITA PERFORMANCE actor_substep_cap=%u simulation_hz=%u ai_lod_period=%u\n",
            result.actorSubstepCap, result.simulationHz, result.aiLodPeriod);
#endif
        return result;
    }();
    return settings;
}
inline unsigned vitaActorSubstepCap()
{
    const auto &settings = vitaPerformanceSettings();
    const unsigned diagnostic = settings.actorSubstepCap, overload = settings.simulationMaxSteps;
    return diagnostic && overload ? (diagnostic < overload ? diagnostic : overload) : diagnostic + overload;
}
inline thread_local bool vitaActorPhysicsSlice = false;
inline float vitaActorStepSeconds()
{
    const unsigned hz = vitaPerformanceSettings().simulationHz;
    return vitaActorPhysicsSlice || !hz ? 1.0f / 128.0f : 1.0f / float(hz);
}
// Re-enter integration at the original collision interval; decisions/timers age once at the larger AI dt.
class VitaActorPhysicsSlices
{
public:
    VitaActorPhysicsSlices() : m_previous(vitaActorPhysicsSlice),
        count(unsigned(std::lround(vitaActorStepSeconds() * 128.0f))) { vitaActorPhysicsSlice = true; }
    ~VitaActorPhysicsSlices() { vitaActorPhysicsSlice = m_previous; }
    bool m_previous;
    unsigned count;
};
inline unsigned limitVitaActorSteps(float &accumulator, float step, unsigned cap)
{
    if (!cap || accumulator < step) { return 0; }
    const unsigned available = unsigned(accumulator / step);
    const unsigned dropped = available > cap ? available - cap : 0;
    accumulator -= float(dropped) * step;
    return dropped;
}
}
