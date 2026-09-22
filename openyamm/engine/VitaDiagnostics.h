#pragma once

#if defined(OPENYAMM_VITA_LOADER)
namespace OpenYAMM::Engine
{
// Supplied by the native host; never interpret native allocator structs in Android.
inline void (*vitaMemorySnapshot)(const char *stage) = nullptr;
inline void reportVitaMemory(const char *stage)
{
#if !defined(OPENYAMM_VITA_CLEAN)
    if (vitaMemorySnapshot != nullptr)
    {
        vitaMemorySnapshot(stage);
    }
#endif
}
}
#endif
