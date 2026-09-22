#pragma once

#include <cstdint>

namespace OpenYAMM::Engine::VitaProfile
{
enum Bucket
{
    GameUpdate, WorldUpdate, WorldRender, UiRender, Audio, Asset, AudioMix,
    BgfxApi, BgfxResource, BgfxUniform, BgfxSubmit, BgfxState,
    SharedState, Input, Physics, ActorAi, ActorFacts, ActorDecisions, Pathfinding, Collision,
    SoundTriggers, ScriptTick, Containers, Interaction, Combat, Projectiles, QuestMarkers, SpriteHeader, ImageDecode, SpriteMask, EdgeBleed, TargetBuild, CollisionCandidates, CollisionTotal, WorldPasses, TerrainDraw, BuildingDraw, BillboardGather, BillboardBatch,
    WorldItems, WorldProjectiles, WorldShadows, WorldParticles, LightSelect, LightSample, WorldFx, StreamPoll, Count
};
inline uint32_t mode = 0;
inline bool noAudio() { return (mode & 1) != 0; }
inline bool uiOnly() { return (mode & 2) != 0; }
}

#if defined(OPENYAMM_VITA_PROFILE)
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdarg>
#include <time.h>

namespace OpenYAMM::Engine::VitaProfile
{
inline volatile uint32_t *allocationTotal = nullptr;
inline uint32_t allocations()
{
#if defined(OPENYAMM_VITA_AGGREGATE)
    return 0; // Allocation wrappers are absent; zero is unavailable, not measured allocation count.
#else
    return allocationTotal ? *allocationTotal : 0;
#endif
}
inline bool timedBucket(Bucket bucket)
{
#if defined(OPENYAMM_VITA_AGGREGATE)
    switch (bucket)
    {
    case GameUpdate: case WorldUpdate: case WorldRender: case UiRender: case Audio: case AudioMix:
    case SharedState: case Input: case Physics: case ActorAi: case ActorFacts: case ActorDecisions:
    case SoundTriggers: case ScriptTick: case Containers: case Interaction: case Combat: case Projectiles:
    case QuestMarkers: case WorldPasses: case TerrainDraw: case BuildingDraw: case BillboardGather:
    case WorldItems: case WorldProjectiles: case WorldShadows: case WorldParticles: case WorldFx: case StreamPoll:
        return true;
    default: return false;
    }
#else
    return true;
#endif
}
inline void (*nativeEvent)(uint32_t, uint64_t) = nullptr;
inline void emit(const char *text, size_t bytes)
{
    struct Packet { const char *data; uint32_t size; } packet{text, uint32_t(bytes)};
    if (nativeEvent != nullptr) { nativeEvent(5, reinterpret_cast<uintptr_t>(&packet)); }
}
inline void print(const char *format, ...)
{
    char text[1024];
    va_list arguments;
    va_start(arguments, format);
    int size = std::vsnprintf(text, sizeof(text), format, arguments);
    va_end(arguments);
    if (size > 0) { emit(text, std::min(size_t(size), sizeof(text) - 1)); }
}
inline uint64_t now()
{
    timespec value{};
    clock_gettime(CLOCK_MONOTONIC, &value);
    return uint64_t(value.tv_sec) * 1000000 + value.tv_nsec / 1000;
}
inline std::atomic<bool> captureRequested{false};
inline std::atomic<unsigned> captureEpoch{1};
class Scope;
struct Counter { uint64_t us = 0, selfUs = 0, maximum = 0, units = 0; uint32_t calls = 0, depth = 0; uint64_t selfAllocations = 0; };
struct Frame
{
    Counter counters[Count]{};
    uint64_t begin = 0;
    uint32_t index = 0;
    bool started = false, active = false;
    unsigned stableFrames = 0;
    uint64_t firstWorldUs = 0;
    unsigned capture = 1;
    bool rotationPending = false;
    bool nearActorPending = false, farActorPending = false;
    bool proximityArmed = false;
    Scope *top = nullptr;
};
inline thread_local Frame frame;
inline void units(Bucket bucket, uint64_t count)
{
    if (frame.active) { frame.counters[bucket].units += count; }
}
inline void begin()
{
    if (nativeEvent != nullptr)
    {
        if (!allocationTotal) { nativeEvent(10, reinterpret_cast<uintptr_t>(&allocationTotal)); }
        nativeEvent(8, 0);
    }
    // The native render thread acknowledges rearming at a swap boundary, after the first file is flushed.
    if (frame.capture == 3 && !frame.proximityArmed && nativeEvent != nullptr)
    {
        uint32_t armed = 0;
        nativeEvent(12, reinterpret_cast<uintptr_t>(&armed));
        frame.proximityArmed = armed != 0;
    }
    if (frame.index == 20 && frame.firstWorldUs && nativeEvent != nullptr &&
        ((frame.capture == 1 && now() - frame.firstWorldUs >= 90000000) ||
         (frame.capture == 2 && frame.rotationPending && now() - frame.firstWorldUs >= 110000000) ||
         (frame.capture == 3 && frame.proximityArmed && frame.nearActorPending) ||
         (frame.capture == 4 && frame.farActorPending)))
    {
        uint32_t ready = 0;
        nativeEvent(9, reinterpret_cast<uintptr_t>(&ready));
        if (ready)
        {
            ++frame.capture;
            frame.rotationPending = false;
            frame.index = 0;
            captureEpoch.store(frame.capture, std::memory_order_release);
            captureRequested.store(true, std::memory_order_release);
            nativeEvent(1, 20);
        }
    }
    for (Counter &counter : frame.counters) { counter = {}; }
    frame.begin = now();
    frame.active = frame.started && frame.index < 20;
}
inline void worldReady()
{
    if (!frame.firstWorldUs) { frame.firstWorldUs = now(); }
    if (!frame.started && ++frame.stableFrames >= 2)
    {
        frame.started = frame.active = true;
        captureRequested.store(true, std::memory_order_release);
        if (nativeEvent != nullptr) { nativeEvent(1, 20); }
    }
}
class Scope
{
public:
    explicit Scope(Bucket bucket) : m_counter(timedBucket(bucket) && frame.active ? &frame.counters[bucket] : nullptr)
    {
        if (m_counter != nullptr)
        {
            ++m_counter->depth;
            m_begin = now();
            m_allocBegin = allocations();
            m_parent = frame.top;
            frame.top = this;
        }
    }
    ~Scope()
    {
        if (m_counter != nullptr)
        {
            const uint64_t elapsed = now() - m_begin;
            const uint32_t allocated = allocations() - m_allocBegin;
            frame.top = m_parent;
            if (m_parent) { m_parent->m_childAllocations += allocated; }
            m_counter->selfAllocations += allocated - m_childAllocations;
            if (m_parent != nullptr) { m_parent->m_children += elapsed; }
            m_counter->selfUs += elapsed >= m_children ? elapsed - m_children : 0;
            if (--m_counter->depth == 0)
            {
                m_counter->us += elapsed;
                m_counter->maximum = std::max(m_counter->maximum, elapsed);
                ++m_counter->calls;
            }
        }
    }
    Scope(const Scope &) = delete;
    Scope &operator=(const Scope &) = delete;
private:
    Counter *m_counter;
    uint64_t m_begin = 0;
    uint64_t m_children = 0;
    uint32_t m_allocBegin = 0, m_childAllocations = 0;
    Scope *m_parent = nullptr;
};
class ApiScope
{
public:
    explicit ApiScope(Bucket kind) : m_all(BgfxApi), m_kind(kind) {}
private:
    Scope m_all, m_kind;
};
struct RenderPhase
{
    explicit RenderPhase(uint64_t &total) : value(total), begin(now()) {}
    ~RenderPhase() { value += now() - begin; }
    uint64_t &value, begin;
};
struct RenderResourceCounters
{
    uint64_t us[3]{}, bytes[3]{};
    unsigned calls[3]{}, slowId[3]{};
    uint64_t slowUs[3]{};
};
inline thread_local RenderResourceCounters renderResourceCounters;
struct RenderCycle
{
    RenderCycle() { renderResourceCounters = {}; }
    inline static thread_local unsigned samples = 0;
    inline static thread_local unsigned epoch = 0;
    static bool shouldCapture()
    {
        const unsigned requestedEpoch = captureEpoch.load(std::memory_order_acquire);
        if (epoch != requestedEpoch) { epoch = requestedEpoch; samples = 0; }
        return captureRequested.load(std::memory_order_acquire) && samples < 20;
    }
    bool enabled = shouldCapture();
    bool completed = false;
    uint64_t begin = now(), waitUs = 0, preUs = 0, drawUs = 0, postUs = 0, screenshotUs = 0, flipUs = 0;
    uint32_t workFrame = 0, flipFrame = 0;
    ~RenderCycle()
    {
        if (!enabled || !completed) { return; }
        const uint64_t finish = now();
        const uint64_t total = finish - begin;
        print(
            "PROFILE RENDER n=%u warmup=%u work_frame=%u flip_frame=%u t_us=%llu cycle_ms=%.3f "
            "api_idle_ms=%.3f active_ms=%.3f pre_ms=%.3f draw_ms=%.3f post_ms=%.3f "
            "screenshot_ms=%.3f flip_ms=%.3f\n",
            samples + 1, 0u, workFrame, flipFrame, (unsigned long long)finish, total / 1000.0,
            waitUs / 1000.0, (total >= waitUs ? total - waitUs : 0) / 1000.0,
            preUs / 1000.0, drawUs / 1000.0, postUs / 1000.0, screenshotUs / 1000.0, flipUs / 1000.0);
        const char *resourceNames[] = {"dynamic_index", "dynamic_vertex", "texture_update"};
        for (unsigned i = 0; i < 3; ++i)
        {
            const auto &c = renderResourceCounters;
            print("PROFILE RENDER_RESOURCE n=%u part=%s calls=%u ms=%.3f bytes=%llu slow_id=%u slow_ms=%.3f\n",
                samples + 1, resourceNames[i], c.calls[i], c.us[i] / 1000.0,
                (unsigned long long)c.bytes[i], c.slowId[i], c.slowUs[i] / 1000.0);
        }
        ++samples;
        if (samples == 20 && nativeEvent != nullptr)
        {
            captureRequested.store(false, std::memory_order_release);
            nativeEvent(7, 0);
        }
    }
};
class RenderResourceScope
{
public:
    RenderResourceScope(unsigned part, unsigned identity, uint64_t bytes)
        : m_part(part), m_identity(identity), m_bytes(bytes), m_enabled(RenderCycle::shouldCapture())
    { if (m_enabled) { m_begin = now(); } }
    ~RenderResourceScope()
    {
        if (!m_enabled) { return; }
        const uint64_t elapsed = now() - m_begin; auto &c = renderResourceCounters;
        c.us[m_part] += elapsed; c.bytes[m_part] += m_bytes; ++c.calls[m_part];
        if (elapsed > c.slowUs[m_part]) { c.slowUs[m_part] = elapsed; c.slowId[m_part] = m_identity; }
    }
private:
    unsigned m_part, m_identity; uint64_t m_bytes, m_begin = 0; bool m_enabled;
};
inline void end(uint32_t bgfxFrame, uint64_t callbackUs, uint64_t submitUs)
{
    if (!frame.active) { return; }
    const uint64_t finish = now();
    const char *names[] = {"game_update", "world_update", "world_render", "ui", "audio", "asset", "audio_mix",
        "bgfx_api", "bgfx_resource", "bgfx_uniform", "bgfx_submit", "bgfx_state",
        "shared_state", "input", "physics", "actor_ai", "actor_facts", "actor_decisions", "pathfinding", "collision",
        "sound_triggers", "script_tick", "containers", "interaction", "combat", "projectiles", "quest_markers", "sprite_header", "image_decode", "sprite_mask", "edge_bleed", "target_build", "collision_candidates", "collision_total", "world_passes", "terrain_draw", "building_draw", "billboard_gather", "billboard_batch",
        "world_items", "world_projectiles", "world_shadows", "world_particles", "light_select", "light_sample", "world_fx", "stream_poll"};
    char text[16384];
    ++frame.index;
    int size = std::snprintf(text, sizeof(text),
        "PROFILE GAME n=%u warmup=%u mode=%u bgfx_return=%u t_us=%llu total_ms=%.3f callback_ms=%.3f bgfx_ms=%.3f\n",
        frame.index, 0u, mode, bgfxFrame, (unsigned long long)finish,
        (finish - frame.begin) / 1000.0, callbackUs / 1000.0, submitUs / 1000.0);
    for (int i = 0; i < Count; ++i)
    {
        const Counter &c = frame.counters[i];
#if defined(OPENYAMM_VITA_AGGREGATE)
        if (!timedBucket(Bucket(i)))
        {
            if (c.calls || c.units)
                size += std::snprintf(text + size, sizeof(text) - size,
                    "PROFILE COUNT n=%u part=%s calls=%u units=%llu\n",
                    frame.index, names[i], c.calls, (unsigned long long)c.units);
            continue;
        }
#endif
        size += std::snprintf(text + size, sizeof(text) - size,
            "PROFILE PART n=%u part=%s calls=%u ms=%.3f self_ms=%.3f max_ms=%.3f units=%llu self_allocs=%llu\n",
            frame.index, names[i], c.calls, c.us / 1000.0, c.selfUs / 1000.0, c.maximum / 1000.0, (unsigned long long)c.units, (unsigned long long)c.selfAllocations);
    }
    frame.active = false;
    emit(text, size);
    if (nativeEvent != nullptr) { nativeEvent(4, frame.index); }
    print( "PROFILE LOG n=%u ms=%.3f\n", frame.index, (now() - finish) / 1000.0);
}
}
#else
namespace OpenYAMM::Engine::VitaProfile
{
class Scope { public: explicit Scope(Bucket) {} };
inline void begin() {}
inline void worldReady() {}
inline void units(Bucket, uint64_t) {}
inline void end(uint32_t, uint64_t, uint64_t) {}
}
#endif
