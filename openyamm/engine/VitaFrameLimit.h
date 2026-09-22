#pragma once
#include <cstdint>

namespace OpenYAMM::Engine::VitaFrameLimit
{
// Owned by the game thread. Deliberate pacing happens after diagnostic frame completion.
inline unsigned fps = 0;
inline bool gameplay = false;
constexpr bool valid(unsigned value)
{
    return value == 0 || value == 20 || value == 30 || value == 60;
}
constexpr uint64_t waitNanoseconds(uint64_t begin, uint64_t now, unsigned limit, bool inGameplay)
{
    if (!inGameplay || !valid(limit) || limit == 0 || now < begin)
        return 0;
    const uint64_t period = 1000000000ULL / limit;
    const uint64_t elapsed = now - begin;
    return elapsed < period ? period - elapsed : 0;
}
}
