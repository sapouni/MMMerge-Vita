#pragma once

#include <array>
#include <cstdint>

namespace OpenYAMM::Engine::VitaMovementProfile
{
// Bound only around an opt-in NPC application batch. No allocation or log writes.
// Query intervals are inclusive across different kinds; recursive same-kind calls are counted once.
enum Kind { Floor, Sector, Collision, PathQueueWait, Count };
struct State
{
    uint64_t (*clock)() = nullptr;
    std::array<uint64_t, Count> ns{};
    std::array<unsigned, Count> calls{}, depth{};
    uint64_t sectorCatalogue = 0, sectorCandidates = 0;
    uint64_t floorFaceCatalogue = 0, floorFaceCandidates = 0;
    uint64_t ceilingFaceCatalogue = 0, ceilingFaceCandidates = 0, sweepFaceCatalogue = 0, sweepFaceCandidates = 0;
    unsigned floorProjectionChecks = 0, floorProjectionHits = 0;
    unsigned supportMemoChecks = 0, supportMemoHits = 0;
};
inline thread_local State *current = nullptr;

class Binding
{
    State *m_previous;
public:
    explicit Binding(State *state) : m_previous(current) { current = state; }
    ~Binding() { current = m_previous; }
    Binding(const Binding &) = delete;
    Binding &operator=(const Binding &) = delete;
};

class Scope
{
    State *m_state;
    Kind m_kind;
    uint64_t m_begin = 0;
    bool m_outer = false;
public:
    explicit Scope(Kind kind) : m_state(current), m_kind(kind)
    {
        if (m_state && m_state->clock)
        {
            ++m_state->calls[kind];
            m_outer = m_state->depth[kind]++ == 0;
            if (m_outer) { m_begin = m_state->clock(); }
        }
    }
    ~Scope()
    {
        if (m_state && m_state->clock)
        {
            if (m_outer) { m_state->ns[m_kind] += m_state->clock() - m_begin; }
            --m_state->depth[m_kind];
        }
    }
    Scope(const Scope &) = delete;
    Scope &operator=(const Scope &) = delete;
};
}
