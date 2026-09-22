#pragma once
#include "VitaStallProtocol.h"
#if defined(OPENYAMM_VITA_LOADER)
extern "C" void oy_worker_stage(unsigned, unsigned) __attribute__((weak));
extern "C" void oy_capture_activity(unsigned) __attribute__((weak));
extern "C" void oy_path_cooperate(uint64_t *) __attribute__((weak));
extern "C" OyStallProgress *oy_stall_frame(unsigned enabled) __attribute__((weak));
#endif
namespace OpenYAMM::Engine::VitaStall
{
enum Activity { PartyDamage, DamagePortrait, NewUiAsset, NewSoundDecode,
    CacheWriteQueued, CacheWriteSkipped, CacheWriteDone, CacheWriteFailed, ActorRayHit, ActorRayMiss };
inline void activity(Activity kind)
{
#if defined(OPENYAMM_VITA_LOADER)
    if (oy_capture_activity) oy_capture_activity(unsigned(kind));
#endif
}
inline thread_local OyStallProgress *progress = nullptr;
inline void workerStage(unsigned stage, unsigned detail = 0)
{
#if defined(OPENYAMM_VITA_LOADER)
    if (oy_worker_stage) oy_worker_stage(stage, detail);
#endif
}
template<class Pool> inline void publishPool(unsigned kind, const Pool &pool)
{
    if (!progress || kind >= 3) return;
    static thread_local uint32_t last[3] = {};
    const uint32_t frame = __atomic_load_n(&progress->frame, __ATOMIC_RELAXED);
    if (last[kind] && frame-last[kind]<32) return;
    last[kind]=frame;
    const auto s=pool.snapshot(); // Outside audio/stream state locks; fixed scalar copy.
    auto &p=progress->pools[kind];
    __atomic_fetch_add(&p.sequence,1,__ATOMIC_ACQ_REL);
    const uint32_t values[]={uint32_t(reinterpret_cast<uintptr_t>(&pool)),frame,
        uint32_t(s.limit),uint32_t(s.retained),uint32_t(s.peak),uint32_t(s.hits),
        uint32_t(s.misses),uint32_t(s.returned),uint32_t(s.rejected),uint32_t(s.maxRequest)};
    uint32_t *fields[]={&p.instance,&p.frame,&p.limit,&p.retained,&p.peak,&p.hits,&p.misses,&p.returned,&p.rejected,&p.maxRequest};
    for(unsigned i=0;i<10;++i) __atomic_store_n(fields[i],values[i],__ATOMIC_RELAXED);
    __atomic_fetch_add(&p.sequence,1,__ATOMIC_RELEASE);
}
inline void begin(bool enabled)
{
#if defined(OPENYAMM_VITA_LOADER)
    // Once per physical frame, including frames omitted by the incident ring.
    if (oy_stall_frame && (enabled || progress)) { progress = oy_stall_frame(enabled); }
#endif
}
inline constexpr bool coarsePhase(OyStallPhase phase)
{
    switch (phase)
    {
    case OyStallFacts: case OyStallDecisions: case OyStallApply: case OyStallActorState:
    case OyStallMovement: case OyStallPhysics: case OyStallPathBuild: case OyStallPathResolve:
    case OyStallMoveSweep: case OyStallMovePost: case OyStallMoveFacts:
    case OyStallMelee: case OyStallCorpse: return false;
    default: return true;
    }
}
inline void set(OyStallPhase phase, unsigned actor = 0xffffffu)
{
    if (!coarsePhase(phase) || !progress) { return; }
    __atomic_store_n(&progress->context, (unsigned(phase) << 24) | (actor & 0xffffffu), __ATOMIC_RELAXED);
    __atomic_store_n(&progress->detail, 0, __ATOMIC_RELAXED);
    __atomic_fetch_add(&progress->progress, 1u, __ATOMIC_RELAXED);
}
inline void simulation(unsigned hz, unsigned step)
{
    if (!progress) { return; }
    __atomic_store_n(&progress->hz, hz, __ATOMIC_RELAXED);
    __atomic_store_n(&progress->step, step, __ATOMIC_RELAXED);
}
inline void sector(int value)
{
    if (progress) { __atomic_store_n(&progress->sector, uint32_t(value), __ATOMIC_RELAXED); }
}
inline void detail(unsigned value)
{
    if (progress) { __atomic_store_n(&progress->detail, value, __ATOMIC_RELAXED); }
}
class Scope
{
    uint32_t m_previous = 0;
    bool m_enabled = false;
public:
    explicit Scope(OyStallPhase phase, unsigned actor = 0xffffffu)
    {
        if (coarsePhase(phase) && progress)
        {
            m_enabled = true;
            m_previous = __atomic_load_n(&progress->context, __ATOMIC_RELAXED);
            if (actor == 0xffffffu) { actor = m_previous & 0xffffffu; }
            set(phase, actor);
        }
    }
    ~Scope()
    {
        if (m_enabled) { set(OyStallPhase(m_previous >> 24), m_previous & 0xffffffu); }
    }
};
}
