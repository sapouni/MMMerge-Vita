#pragma once
#include "engine/VitaFrameProfile.h"

namespace OpenYAMM::Game
{
struct VitaViewCosts
{
    uint64_t actorUs = 0, billboardUs = 0, worldUs = 0, uiUs = 0;
    unsigned fullTicks = 0, reducedTicks = 0, steps = 0, rendered = 0;
    unsigned reducedSprites = 0, subpixelSkipped = 0, lightReused = 0, lightRefreshed = 0;
    unsigned actorsIn = 0, actorsBeyond = 0, drawsIn = 0, drawsBeyond = 0;
    float yaw = 0, pitch = 0;
};
inline thread_local VitaViewCosts vitaViewCosts, vitaCompletedViewCosts;
class VitaViewCostScope
{
public:
    explicit VitaViewCostScope(uint64_t &destination) : m_destination(destination)
    {
#if defined(OPENYAMM_VITA_LOADER) && defined(OPENYAMM_VITA_PROFILE)
        m_begin = Engine::VitaProfile::now();
#endif
    }
    ~VitaViewCostScope()
    {
#if defined(OPENYAMM_VITA_LOADER) && defined(OPENYAMM_VITA_PROFILE)
        m_destination = Engine::VitaProfile::now() - m_begin;
#endif
    }
private:
    uint64_t &m_destination;
    uint64_t m_begin = 0;
};
}
