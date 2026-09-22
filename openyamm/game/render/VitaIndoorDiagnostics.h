#pragma once
#include "engine/VitaIncident.h"
#include "engine/VitaMovementProfile.h"
#include "game/render/VitaPerformanceConfig.h"
#include "game/party/Party.h"
namespace OpenYAMM::Game
{
struct VitaIndoorWork
{
    using Timing = Engine::VitaIncident::Child;
    using Counters = Engine::VitaIncident::Work;
    static constexpr Timing Ai = Timing::Ai, Facts = Timing::Facts, Decisions = Timing::Decisions;
    static constexpr Timing Apply = Timing::Apply, Projectiles = Timing::Projectiles, Recovery = Timing::Recovery;
    static constexpr Timing Colliders = Timing::Colliders, Movement = Timing::Movement, Physics = Timing::Physics;
    static constexpr Timing Path = Timing::Path;
    static constexpr Timing PathBuild = Timing::PathBuild, PathResolve = Timing::PathResolve;
    static constexpr Timing PlayerMove = Timing::PlayerMove, Reveal = Timing::Reveal, Minimap = Timing::Minimap;
    static constexpr Timing MoveSweep = Timing::MoveSweep, MovePost = Timing::MovePost, MoveFacts = Timing::MoveFacts;
    static constexpr Timing Melee = Timing::Melee, Corpse = Timing::Corpse;
    static constexpr Timing HudSubmit = Timing::HudSubmit, Visibility = Timing::Visibility, Lighting = Timing::Lighting;
    inline static thread_local bool active = false;
    inline static thread_local Counters counters{};
    static bool movementDetail() { return active && vitaPerformanceSettings().movementDiagnostics != 0; }
    static uint64_t tick() { return active ? SDL_GetTicksNS() : 0; }
    static void add(Timing timing, uint64_t begin)
    { if (active) { counters.ns[timing] += SDL_GetTicksNS() - begin; } }
};
class VitaIndoorScope
{
    VitaIndoorWork::Timing m_timing;
    uint64_t m_begin;
    bool m_enabled;
public:
    explicit VitaIndoorScope(VitaIndoorWork::Timing timing)
        : m_timing(timing), m_begin(0), m_enabled(false)
    {
        const bool movement = timing == VitaIndoorWork::Movement || timing == VitaIndoorWork::Physics
            || (timing >= VitaIndoorWork::MoveSweep && timing <= VitaIndoorWork::Corpse)
            || timing >= Engine::VitaIncident::ApplySetup;
        if (movement && !VitaIndoorWork::movementDetail()) { return; }
        m_enabled = VitaIndoorWork::active;
        if (m_enabled) { m_begin = SDL_GetTicksNS(); }
    }
    void next(VitaIndoorWork::Timing timing)
    {
        if (m_enabled) { const uint64_t now=SDL_GetTicksNS(); VitaIndoorWork::counters.ns[m_timing]+=now-m_begin; m_begin=now; }
        m_timing=timing;
    }
    ~VitaIndoorScope()
    { if (m_enabled) { VitaIndoorWork::counters.ns[m_timing] += SDL_GetTicksNS() - m_begin; } }
};
// Opt-in query aggregates. Short per-batch kernel CPU deltas failed hardware
// calibration and are deliberately not collected or interpreted as lock waits.
class VitaIndoorApplicationProfile
{
    bool m_enabled = VitaIndoorWork::movementDetail();
    Engine::VitaMovementProfile::State m_queries;
    Engine::VitaMovementProfile::Binding m_binding{m_enabled ? &m_queries : nullptr};
public:
    VitaIndoorApplicationProfile()
    {
        if (m_enabled) { m_queries.clock = SDL_GetTicksNS; }
        // Kernel runClocks did not resolve short batches in the hardware calibration.
        // Keep wire fields reserved/zero; do not infer mutex time from unusable deltas.
    }
    ~VitaIndoorApplicationProfile()
    {
        if (!m_enabled) { return; }
        auto &work = VitaIndoorWork::counters;
        using namespace Engine::VitaMovementProfile;
        work.ns[Engine::VitaIncident::MoveFloorQuery] += m_queries.ns[Floor];
        work.ns[Engine::VitaIncident::MoveSectorQuery] += m_queries.ns[Sector];
        work.ns[Engine::VitaIncident::MoveCollisionQuery] += m_queries.ns[Collision];
        work.ns[Engine::VitaIncident::PathQueueWait] += m_queries.ns[PathQueueWait];
        work.floorQueries += m_queries.calls[Floor];
        work.sectorQueries += m_queries.calls[Sector];
        work.collisionQueries += m_queries.calls[Collision];
        work.pathQueueAcquires += m_queries.calls[PathQueueWait];
        work.sectorCatalogue += m_queries.sectorCatalogue;
        work.sectorCandidates += m_queries.sectorCandidates;
        work.floorFaceCatalogue += m_queries.floorFaceCatalogue;
        work.floorFaceCandidates += m_queries.floorFaceCandidates;
        work.ceilingFaceCatalogue += m_queries.ceilingFaceCatalogue;
        work.ceilingFaceCandidates += m_queries.ceilingFaceCandidates;
        work.sweepFaceCatalogue += m_queries.sweepFaceCatalogue;
        work.sweepFaceCandidates += m_queries.sweepFaceCandidates;

        work.floorProjectionChecks += m_queries.floorProjectionChecks;
        work.floorProjectionHits += m_queries.floorProjectionHits;
        work.supportMemoChecks += m_queries.supportMemoChecks;
        work.supportMemoHits += m_queries.supportMemoHits;
    }
};

class VitaIndoorCpuScope
{
    Engine::VitaIncident::Child m_kind;
    uint64_t m_begin = UINT64_MAX;
public:
    explicit VitaIndoorCpuScope(Engine::VitaIncident::Child kind) : m_kind(kind)
    { if (VitaIndoorWork::active && oy_thread_run_ticks) m_begin = oy_thread_run_ticks(); }
    ~VitaIndoorCpuScope()
    {
        if (m_begin == UINT64_MAX) return;
        const uint64_t end = oy_thread_run_ticks();
        if (end != UINT64_MAX && end >= m_begin) VitaIndoorWork::counters.ns[m_kind] += end-m_begin;
    }
};
class VitaIndoorStage
{
    Engine::VitaStall::Scope m_phase;
    VitaIndoorScope m_timer;
public:
    VitaIndoorStage(Engine::VitaIncident::Child timing, OyStallPhase phase)
        : m_phase(phase), m_timer(timing) {}
};
class VitaIndoorFrame
{
    bool m_enabled = false;
    uint64_t m_last = 0;
    std::array<uint64_t, 7> m_times{};
    unsigned m_dead = 0;
public:
    VitaIndoorFrame(bool indoors, const char *map, unsigned house)
    {
        const auto &config = vitaPerformanceSettings();
        if (!config.indoorDiagnostics && !config.houseDiagnostics)
        {
            if (Engine::VitaIncident::state().enabled) { Engine::VitaIncident::configure(false, 0, "", 0); }
            VitaIndoorWork::active = false;
            return;
        }
        if (!house && Engine::VitaIncident::houseVideoSessions) { house = UINT32_MAX; }
        // The gameplay recorder also covers outdoor approaches, which older captures omitted.
        Engine::VitaIncident::configure((!house && config.indoorDiagnostics) || (house && config.houseDiagnostics),
            indoors ? 1u : 0u, map, house);
        Engine::VitaStall::set(OyStallSimulation);
        m_enabled = Engine::VitaIncident::active();
        VitaIndoorWork::active = m_enabled;
        if (m_enabled)
        {
            VitaIndoorWork::counters = {};
            VitaIndoorWork::counters.fineSample = config.movementDiagnostics ? 2u : 0u;
            m_last = SDL_GetTicksNS();
        }
    }
    void mark(unsigned phase, const Party *party)
    {
        const OyStallPhase next[] = {OyStallPlayer, OyStallCallback, OyStallWorldRender, OyStallHud, OyStallCallback, OyStallAudio, OyStallCallback};
        if (phase < 7) { Engine::VitaStall::set(next[phase]); }
        if (!m_enabled) { return; }
        const uint64_t now = SDL_GetTicksNS();
        if (phase < m_times.size()) { m_times[phase] += now - m_last; }
        if (party && phase == 4)
        {
            for (size_t i = 0; i < party->members().size() && i < 32; ++i)
            {
                const auto &conditions = party->members()[i].conditions;
                if (conditions.test(size_t(CharacterCondition::Dead))
                    || conditions.test(size_t(CharacterCondition::Eradicated))) { m_dead |= 1u << i; }
            }
        }
        m_last = SDL_GetTicksNS();
    }
    ~VitaIndoorFrame()
    {
        if (m_enabled) { Engine::VitaIncident::game(m_times, VitaIndoorWork::counters, m_dead); }
        VitaIndoorWork::active = false;
    }
};
}
