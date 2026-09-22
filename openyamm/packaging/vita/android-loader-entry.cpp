#include "engine/VitaFrameProfile.h"
#include "game/app/OpenYammMain.h"
#include "engine/VitaDiagnostics.h"

#include <exception>
#include <filesystem>
#include <iostream>

// The Vita host supplies SDL and platform services and calls this after relocation.
// Data and settings are preinstalled under dataRoot; no Android Activity is needed.
extern "C" __attribute__((visibility("default"))) int openyamm_vita_main(
    const char *dataRoot, void (*memorySnapshot)(const char *),
    uint32_t profileMode, void (*profileEvent)(uint32_t, uint64_t))
{
#if defined(OPENYAMM_VITA_PROFILE)
    OpenYAMM::Engine::VitaProfile::mode = profileMode;
    OpenYAMM::Engine::VitaProfile::nativeEvent = profileEvent;
    if (profileEvent != nullptr)
    {
        profileEvent(3, 0);
#if defined(OPENYAMM_VITA_LTO)
        profileEvent(11, 1);
#else
        profileEvent(11, 0);
#endif
    }
#endif
    // Native stdio flushes complete lines. Avoid a disk flush for each << fragment.
    std::cerr.unsetf(std::ios_base::unitbuf);
    std::cerr.tie(nullptr);
    OpenYAMM::Engine::vitaMemorySnapshot = memorySnapshot;
    if (dataRoot == nullptr || dataRoot[0] == '\0')
    {
        return 1;
    }
    try
    {
        std::cerr << "STARTUP: selecting writable directory\n";
        std::filesystem::current_path(dataRoot);
        std::cerr << "STARTUP: writable directory selected; starting application\n";
        char applicationName[] = "openyamm";
        char *arguments[] = {applicationName, nullptr};
        return OpenYAMM::Game::runApplication(1, arguments);
    }
    catch (const std::exception &exception)
    {
        std::cerr << "OpenYAMM Vita application failed: " << exception.what() << '\n';
        return 1;
    }
}

#if defined(OPENYAMM_VITA_PROFILE)
extern "C" void openyamm_vita_render_marker(uint32_t id)
{
    if (OpenYAMM::Engine::VitaProfile::nativeEvent != nullptr)
    {
        OpenYAMM::Engine::VitaProfile::nativeEvent(2, id);
    }
}
#endif

#if defined(OPENYAMM_VITA_PROFILE)
extern "C" void openyamm_vita_resource_marker(uint32_t kind)
{
    if (OpenYAMM::Engine::VitaProfile::nativeEvent != nullptr)
    {
        OpenYAMM::Engine::VitaProfile::nativeEvent(6, kind);
    }
}
#endif
