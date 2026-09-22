#pragma once
#include <cstdint>
namespace OpenYAMM::Engine::VitaInitialView
{
// Only the main/API thread accesses this state; decode workers never touch it.
inline bool active = false;
inline uint32_t animationTicks = 0;
inline unsigned pending = 0, failed = 0, suppressedDraws = 0;
inline void missing(bool permanent = false)
{
    if (!active) return;
    if (permanent) ++failed; else ++pending;
}
struct Scope
{
    bool previous = active;
    Scope() { active = true; pending = failed = suppressedDraws = 0; }
    ~Scope() { active = previous; }
};
}
