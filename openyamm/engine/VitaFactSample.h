#pragma once
#include <cstddef>
#include <cstdint>
#include "engine/VitaVisibilityProfile.h"
namespace OpenYAMM::Engine
{
// Sparse scoped measurements, not PC sampling: at most one actor in 64 per step.
// No clocks for unsampled actors. Worker mode is intentionally excluded to avoid shared-counter contention.
class VitaFactSample
{
public:
    VitaFactSample(size_t actor, bool serial)
    {
#if defined(OPENYAMM_VITA_PROFILE) && defined(OPENYAMM_VITA_LOADER)
        m_enabled = serial && VitaProfile::frame.active && ((actor + VitaVisibility::sequence) & 63) == 0;
        if (m_enabled) { m_begin = VitaProfile::now(); }
#endif
    }
    void next(unsigned part)
    {
#if defined(OPENYAMM_VITA_PROFILE) && defined(OPENYAMM_VITA_LOADER)
        if (!m_enabled) { return; }
        const uint64_t end = VitaProfile::now();
        VitaVisibility::counts.factSampleUs[m_part] += end - m_begin;
        ++VitaVisibility::counts.factSamples[m_part];
        m_part = part; m_begin = end;
#endif
    }
    ~VitaFactSample() { next(m_part); }
private:
    bool m_enabled = false;
    unsigned m_part = 0;
    uint64_t m_begin = 0;
};
}
