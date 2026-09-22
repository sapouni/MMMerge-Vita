#pragma once
#include "engine/VitaFrameProfile.h"

namespace OpenYAMM::Game
{
class VitaPickTiming
{
public:
    explicit VitaPickTiming(const char *name) : m_name(name)
    {
#if defined(OPENYAMM_VITA_PROFILE)
        m_enabled = Engine::VitaProfile::frame.active;
        if (m_enabled) { m_begin = Engine::VitaProfile::now(); }
#endif
    }
    ~VitaPickTiming()
    {
#if defined(OPENYAMM_VITA_PROFILE)
        if (m_enabled)
        {
            Engine::VitaProfile::print("PROFILE PICK n=%u name=%s ms=%.3f\n",
                Engine::VitaProfile::frame.index + 1, m_name, (Engine::VitaProfile::now() - m_begin) / 1000.0);
        }
#endif
    }
private:
    const char *m_name;
    uint64_t m_begin = 0;
    bool m_enabled = false;
};
}
