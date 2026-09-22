#include "engine/VitaVisibilityProfile.h"
#include "engine/VitaDetailProfile.h"
#include "engine/VitaFrameProfile.h"
#include "engine/BgfxContext.h"
#include "engine/EngineApplication.h"
#include "engine/VitaIncident.h"
#include "engine/VitaAdaptiveResolution.h"
#include "engine/VitaFrameLimit.h"
#include <time.h>
extern "C" void oy_bgfx_adaptive_enable(int) __attribute__((weak));
extern "C" void oy_bgfx_capture_enable(int) __attribute__((weak));
#include "engine/VitaDiagnostics.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace OpenYAMM::Engine
{
namespace
{
constexpr int MinimumWindowAspectWidth = 4;
constexpr int MinimumWindowAspectHeight = 3;

#if defined(__ANDROID__)
constexpr Sint64 AndroidInputTypeClassText = 0x00000001;
constexpr Sint64 AndroidInputTypeTextVariationVisiblePassword = 0x00000090;
constexpr Sint64 AndroidInputTypeTextFlagNoSuggestions = 0x00080000;

bool startManagedAndroidTextInput(SDL_Window *pWindow)
{
    SDL_PropertiesID properties = SDL_CreateProperties();

    if (properties == 0)
    {
        return SDL_StartTextInput(pWindow);
    }

    const Sint64 inputType =
        AndroidInputTypeClassText
        | AndroidInputTypeTextVariationVisiblePassword
        | AndroidInputTypeTextFlagNoSuggestions;

    SDL_SetNumberProperty(properties, SDL_PROP_TEXTINPUT_ANDROID_INPUTTYPE_NUMBER, inputType);
    SDL_SetBooleanProperty(properties, SDL_PROP_TEXTINPUT_AUTOCORRECT_BOOLEAN, false);
    SDL_SetBooleanProperty(properties, SDL_PROP_TEXTINPUT_MULTILINE_BOOLEAN, false);

    const bool started = SDL_StartTextInputWithProperties(pWindow, properties);
    SDL_DestroyProperties(properties);
    return started;
}
#endif

uint64_t averageNanoseconds(uint64_t totalNanoseconds, uint64_t count)
{
    return count != 0 ? totalNanoseconds / count : 0;
}

uint64_t nanosecondsToMicroseconds(uint64_t nanoseconds)
{
    return nanoseconds / 1000ULL;
}

class SdlSubsystemGuard
{
public:
    explicit SdlSubsystemGuard(bool isInitialized)
        : m_isInitialized(isInitialized)
    {
    }

    SdlSubsystemGuard(const SdlSubsystemGuard &) = delete;
    SdlSubsystemGuard &operator=(const SdlSubsystemGuard &) = delete;

    ~SdlSubsystemGuard()
    {
        if (m_isInitialized)
        {
            SDL_Quit();
        }
    }

private:
    bool m_isInitialized;
};

struct SdlWindowDeleter
{
    void operator()(SDL_Window *pWindow) const
    {
        if (pWindow != nullptr)
        {
            SDL_DestroyWindow(pWindow);
        }
    }
};

void enforceMinimumWindowAspect(SDL_Window *pWindow)
{
    if (pWindow == nullptr)
    {
        return;
    }

    int windowWidth = 0;
    int windowHeight = 0;
    SDL_GetWindowSize(pWindow, &windowWidth, &windowHeight);

    if (windowWidth <= 0 || windowHeight <= 0)
    {
        return;
    }

    if (windowWidth * MinimumWindowAspectHeight >= windowHeight * MinimumWindowAspectWidth)
    {
        return;
    }

    const int correctedWidth =
        (windowHeight * MinimumWindowAspectWidth + MinimumWindowAspectHeight - 1) / MinimumWindowAspectHeight;

    if (correctedWidth != windowWidth)
    {
        SDL_SetWindowSize(pWindow, correctedWidth, windowHeight);
    }
}

uint64_t initialWindowFlagsForMode(WindowMode mode)
{
    uint64_t flags = 0;

    if (mode == WindowMode::Windowed)
    {
        flags |= SDL_WINDOW_RESIZABLE;
    }

    return flags;
}

SDL_Window *createApplicationWindow(const ApplicationConfig &config)
{
    const uint64_t flags = initialWindowFlagsForMode(config.windowMode);

#if defined(__ANDROID__)
    const SDL_PropertiesID properties = SDL_CreateProperties();

    if (properties == 0)
    {
        return nullptr;
    }

    SDL_SetStringProperty(properties, SDL_PROP_WINDOW_CREATE_TITLE_STRING, config.appName.c_str());
    SDL_SetNumberProperty(properties, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, config.windowWidth);
    SDL_SetNumberProperty(properties, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, config.windowHeight);
    SDL_SetNumberProperty(properties, SDL_PROP_WINDOW_CREATE_FLAGS_NUMBER, static_cast<Sint64>(flags));
    SDL_SetBooleanProperty(properties, SDL_PROP_WINDOW_CREATE_EXTERNAL_GRAPHICS_CONTEXT_BOOLEAN, true);

    SDL_Window *pWindow = SDL_CreateWindowWithProperties(properties);
    SDL_DestroyProperties(properties);
    return pWindow;
#else
    return SDL_CreateWindow(
        config.appName.c_str(),
        config.windowWidth,
        config.windowHeight,
        flags);
#endif
}

bool applyWindowMode(SDL_Window *pWindow, const ApplicationConfig &config)
{
    if (pWindow == nullptr)
    {
        return false;
    }

    switch (config.windowMode)
    {
    case WindowMode::Windowed:
        return true;

    case WindowMode::WindowedFullscreen:
        if (!SDL_SetWindowBordered(pWindow, false))
        {
            std::cerr << "SDL_SetWindowBordered failed: " << SDL_GetError() << '\n';
            return false;
        }

        if (!SDL_SetWindowFullscreen(pWindow, true))
        {
            std::cerr << "SDL_SetWindowFullscreen failed: " << SDL_GetError() << '\n';
            return false;
        }

        return true;

    case WindowMode::Fullscreen:
    {
        const SDL_DisplayID displayId = SDL_GetPrimaryDisplay();
        const SDL_DisplayMode requestedMode = {
            .displayID = displayId,
            .format = SDL_PIXELFORMAT_UNKNOWN,
            .w = config.windowWidth,
            .h = config.windowHeight,
            .pixel_density = 0.0f,
            .refresh_rate = 0.0f,
            .refresh_rate_numerator = 0,
            .refresh_rate_denominator = 0,
            .internal = nullptr
        };

        if (!SDL_SetWindowFullscreenMode(pWindow, &requestedMode))
        {
            std::cerr << "SDL_SetWindowFullscreenMode failed: " << SDL_GetError() << '\n';
            return false;
        }

        if (!SDL_SetWindowFullscreen(pWindow, true))
        {
            std::cerr << "SDL_SetWindowFullscreen failed: " << SDL_GetError() << '\n';
            return false;
        }

        return true;
    }
    }

    return true;
}

const char *windowModeName(WindowMode mode)
{
    switch (mode)
    {
    case WindowMode::Windowed:
        return "windowed";

    case WindowMode::WindowedFullscreen:
        return "windowed_fullscreen";

    case WindowMode::Fullscreen:
        return "fullscreen";
    }

    return "windowed";
}

class ApplicationShutdownGuard
{
public:
    explicit ApplicationShutdownGuard(const EngineApplication::ShutdownCallback &callback) : m_callback(callback) {}
    ~ApplicationShutdownGuard()
    {
        try { invoke(); }
        catch (...) { std::cerr << "Application shutdown failed during cleanup\n"; }
    }
    void invoke()
    {
        if (m_invoked) { return; }
        m_invoked = true;
        if (m_callback) { m_callback(); }
    }
private:
    const EngineApplication::ShutdownCallback &m_callback;
    bool m_invoked = false;
};
}

EngineApplication::EngineApplication(
    const ApplicationConfig &config,
    StartupCallback startupCallback,
    RenderSetupCallback renderSetupCallback,
    EventCallback eventCallback,
    RenderFrameCallback renderFrameCallback,
    ShutdownCallback shutdownCallback,
    TextInputActiveCallback textInputActiveCallback
)
    : m_config(config)
    , m_startupCallback(std::move(startupCallback))
    , m_renderSetupCallback(std::move(renderSetupCallback))
    , m_eventCallback(std::move(eventCallback))
    , m_renderFrameCallback(std::move(renderFrameCallback))
    , m_shutdownCallback(std::move(shutdownCallback))
    , m_textInputActiveCallback(std::move(textInputActiveCallback))
{
}

int EngineApplication::run() const
{
#if defined(OPENYAMM_VITA_LOADER)
    std::cerr << "STARTUP: engine run entered\n";
#endif
    if (!validateConfiguration())
    {
        return 1;
    }

#if defined(__ANDROID__)
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
#endif

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS))
    {
        std::cerr << "SDL_Init failed: " << SDL_GetError() << '\n';
        return 1;
    }

    const SdlSubsystemGuard sdlGuard(true);
    AssetFileSystem assetFileSystem;
    // Destruction order: game/stream workers, renderer, window, filesystem, SDL.
    std::unique_ptr<SDL_Window, SdlWindowDeleter> pWindow;
    BgfxContext bgfxContext;
    ApplicationShutdownGuard shutdownGuard(m_shutdownCallback);

    if (!initializeAssetFileSystem(assetFileSystem))
    {
        return 1;
    }

#if defined(OPENYAMM_VITA_LOADER)
    std::cerr << "STARTUP: asset filesystem ready; loading game data\n";
#endif
    if (m_startupCallback && !m_startupCallback(assetFileSystem))
    {
        return 1;
    }

#if defined(OPENYAMM_VITA_LOADER)
    std::cerr << "STARTUP: game data loaded; creating window\n";
#endif
    SDL_Window *pRawWindow = createApplicationWindow(m_config);

    if (pRawWindow == nullptr)
    {
        std::cerr << "SDL_CreateWindow failed: " << SDL_GetError() << '\n';
        shutdownGuard.invoke();
        return 1;
    }

    pWindow.reset(pRawWindow);
    if (!applyWindowMode(pWindow.get(), m_config))
    {
        shutdownGuard.invoke();
        return 1;
    }

    if (m_config.windowMode == WindowMode::Windowed)
    {
        enforceMinimumWindowAspect(pWindow.get());
    }

    int drawableWidth = 0;
    int drawableHeight = 0;
    SDL_GetWindowSizeInPixels(pWindow.get(), &drawableWidth, &drawableHeight);

#if defined(OPENYAMM_VITA_LOADER)
    std::cerr << "STARTUP: starting bgfx renderer\n";
#endif
    if (!bgfxContext.initialize(pWindow.get(), drawableWidth, drawableHeight, m_config.verticalSync))
    {
        shutdownGuard.invoke();
        return 1;
    }

#if defined(OPENYAMM_VITA_LOADER)
    std::cerr << "STARTUP: bgfx ready; preparing game rendering\n";
#endif
    if (m_renderSetupCallback && !m_renderSetupCallback())
    {
        shutdownGuard.invoke();
        return 1;
    }

    std::cout << m_config.appName << '\n';
    std::cout << "Development assets: " << m_config.assetRoot << '\n';
    const AssetScaleProfile &assetScaleProfile = m_config.assetScaleProfile;
    std::cout << "Asset scale: default=" << assetScaleTierToString(m_config.assetScaleTier)
              << " texture=" << assetScaleTierToString(assetScaleProfile.textures)
              << " terrain=" << assetScaleTierToString(assetScaleProfile.terrain)
              << " sky=" << assetScaleTierToString(assetScaleProfile.sky)
              << " sprites=" << assetScaleTierToString(assetScaleProfile.sprites)
              << " decorations=" << assetScaleTierToString(assetScaleProfile.decorations)
              << " icons=" << assetScaleTierToString(assetScaleProfile.icons)
              << " ui=" << assetScaleTierToString(assetScaleProfile.ui) << '\n';
    std::cout << "Window mode: " << windowModeName(m_config.windowMode) << '\n';
    std::cout << "Window requested: " << m_config.windowWidth << "x" << m_config.windowHeight << '\n';
    std::cout << "Window drawable: " << drawableWidth << "x" << drawableHeight << '\n';
    std::cout << "VSync: " << (m_config.verticalSync ? "on" : "off") << '\n';
    std::cout << "Renderer: " << bgfx::getRendererName(bgfxContext.getRendererType()) << '\n';
    std::cout << "Mounted search paths:\n";

    const std::vector<std::string> searchPaths = assetFileSystem.getSearchPaths();

    for (const std::string &searchPath : searchPaths)
    {
        std::cout << "  " << searchPath << '\n';
    }

    const bool hasDataDirectory = assetFileSystem.exists("Data");
    std::cout << "Data directory available: " << (hasDataDirectory ? "yes" : "no") << '\n';
    std::cout << "Close the window to exit.\n";

    bool isRunning = true;
    uint64_t lastFrameTickCount = SDL_GetTicksNS();
#if defined(OPENYAMM_VITA_CLEAN)
    constexpr bool logFps = false, collectPerformanceDiagnostics = false, logFrameHitches = false;
    constexpr bool collectFrameTimings = false;
#else
    const bool logFps = m_config.fpsTrace;
    const bool collectPerformanceDiagnostics = m_config.performanceTrace;
    const bool logFrameHitches = m_config.hitchTrace;
#if defined(OPENYAMM_VITA_LOADER)
    const bool collectFrameTimings = true;
#else
    const bool collectFrameTimings = collectPerformanceDiagnostics || logFrameHitches;
#endif
#endif
    const bool sampleFrames = logFps || collectPerformanceDiagnostics;
    const uint64_t hitchThresholdNanoseconds =
        static_cast<uint64_t>(std::max(0.1f, m_config.hitchThresholdMilliseconds) * 1000000.0f);
    float fpsSampleSeconds = 0.0f;
    uint32_t fpsSampleFrameCount = 0;
    uint64_t fpsSampleEventCount = 0;
    uint64_t fpsLoopNanoseconds = 0;
    uint64_t fpsEventNanoseconds = 0;
    uint64_t fpsWindowSizeNanoseconds = 0;
    uint64_t fpsRenderCallbackNanoseconds = 0;
    uint64_t fpsBgfxFrameNanoseconds = 0;
#if defined(__ANDROID__)
    bool managedTextInputActive = false;
    const auto syncManagedTextInput =
        [&managedTextInputActive, this, &pWindow]()
        {
            const bool textInputRequested = m_textInputActiveCallback && m_textInputActiveCallback();

#if defined(OPENYAMM_VITA_LOADER)
            const char *imeState = SDL_GetHint("openyamm_ime_state");
            if (managedTextInputActive && imeState && std::string(imeState) == "handled")
            {
                SDL_StopTextInput(pWindow.get());
                managedTextInputActive = false;
                return;
            }
#endif
            if (textInputRequested && !managedTextInputActive)
            {
                managedTextInputActive = startManagedAndroidTextInput(pWindow.get());
            }
            else if (!textInputRequested && managedTextInputActive)
            {
                SDL_StopTextInput(pWindow.get());
                managedTextInputActive = false;
            }
        };
#endif

#if defined(OPENYAMM_VITA_PROFILE)
    uint64_t vitaNextMemoryLog = 0;
    bool vitaDrawLimitReported = false;
#endif
    while (isRunning)
    {
        VitaVisibility::flush(); // Prior window: format before starting the next measured frame.
        VitaProfile::begin();
        VitaVisibility::begin();
        VitaIncident::begin();
        const uint64_t currentFrameTickCount = SDL_GetTicksNS();
        const uint64_t loopBeginTickCount = collectFrameTimings ? currentFrameTickCount : 0;
        uint64_t frameDeltaNanoseconds = 16666667ULL;
        float deltaSeconds = 1.0f / 60.0f;

        if (currentFrameTickCount > lastFrameTickCount)
        {
            frameDeltaNanoseconds = currentFrameTickCount - lastFrameTickCount;
            deltaSeconds = static_cast<float>(frameDeltaNanoseconds) / 1000000000.0f;
        }

        lastFrameTickCount = currentFrameTickCount;
        float mouseWheelDelta = 0.0f;
        SDL_Event event;
        bool refreshRenderWindow = false;
        const uint64_t eventBeginTickCount = collectFrameTimings ? SDL_GetTicksNS() : 0;
        uint64_t frameEventCount = 0;

        while (SDL_PollEvent(&event))
        {
            ++frameEventCount;

            if (event.type == SDL_EVENT_QUIT)
            {
                isRunning = false;
            }

            if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
            {
                isRunning = false;
            }

            if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED)
            {
                enforceMinimumWindowAspect(pWindow.get());
                refreshRenderWindow = true;
            }

            if (event.type == SDL_EVENT_DID_ENTER_FOREGROUND)
            {
                frameDeltaNanoseconds = 16666667ULL;
                deltaSeconds = 1.0f / 60.0f;
                lastFrameTickCount = SDL_GetTicksNS();
                refreshRenderWindow = true;
            }

            if (event.type == SDL_EVENT_MOUSE_WHEEL)
            {
                mouseWheelDelta += event.wheel.y;
            }

            if (m_eventCallback)
            {
                m_eventCallback(event);
            }
        }

        if (refreshRenderWindow)
        {
            int resizedDrawableWidth = 0;
            int resizedDrawableHeight = 0;
            SDL_GetWindowSizeInPixels(pWindow.get(), &resizedDrawableWidth, &resizedDrawableHeight);
            bgfxContext.resize(pWindow.get(), resizedDrawableWidth, resizedDrawableHeight);
        }

        uint64_t frameEventNanoseconds = 0;

        if (collectFrameTimings)
        {
            frameEventNanoseconds = SDL_GetTicksNS() - eventBeginTickCount;

            if (collectPerformanceDiagnostics)
            {
                fpsEventNanoseconds += frameEventNanoseconds;
                fpsSampleEventCount += frameEventCount;
            }
        }

#if defined(__ANDROID__)
        syncManagedTextInput();
#endif

        const uint64_t windowSizeBeginTickCount = collectFrameTimings ? SDL_GetTicksNS() : 0;
        int drawableWidth = 0;
        int drawableHeight = 0;
        SDL_GetWindowSizeInPixels(pWindow.get(), &drawableWidth, &drawableHeight);

        uint64_t frameWindowSizeNanoseconds = 0;

        if (collectFrameTimings)
        {
            frameWindowSizeNanoseconds = SDL_GetTicksNS() - windowSizeBeginTickCount;

            if (collectPerformanceDiagnostics)
            {
                fpsWindowSizeNanoseconds += frameWindowSizeNanoseconds;
            }
        }

        const uint64_t renderCallbackBeginTickCount = collectFrameTimings ? SDL_GetTicksNS() : 0;

#if defined(OPENYAMM_VITA_PROFILE)
        const uint64_t vitaCallbackBegin = VitaProfile::now();
#endif
        const uint64_t adaptiveBegin = VitaAdaptiveResolution::enabled ? SDL_GetTicksNS() : 0;
        if (oy_bgfx_adaptive_enable) oy_bgfx_adaptive_enable(VitaAdaptiveResolution::enabled);
        VitaIncident::mark(0);
        if (oy_bgfx_capture_enable) oy_bgfx_capture_enable(VitaIncident::active());
        if (m_renderFrameCallback)
        {
            m_renderFrameCallback(drawableWidth, drawableHeight, mouseWheelDelta, deltaSeconds);
        }
        else
        {
            bgfx::setViewRect(0, 0, 0, static_cast<uint16_t>(drawableWidth), static_cast<uint16_t>(drawableHeight));
            bgfx::touch(0);
        }
        const uint64_t adaptiveCallback = adaptiveBegin ? SDL_GetTicksNS() - adaptiveBegin : 0;
        VitaIncident::mark(1);
#if defined(OPENYAMM_VITA_PROFILE)
        const uint64_t vitaCallbackUs = VitaProfile::now() - vitaCallbackBegin;
        VitaVisibility::record();
#endif

#if defined(__ANDROID__)
        syncManagedTextInput();
#endif

        uint64_t frameRenderCallbackNanoseconds = 0;

        if (collectFrameTimings)
        {
            frameRenderCallbackNanoseconds = SDL_GetTicksNS() - renderCallbackBeginTickCount;

            if (collectPerformanceDiagnostics)
            {
                fpsRenderCallbackNanoseconds += frameRenderCallbackNanoseconds;
            }
        }

        const uint64_t bgfxFrameBeginTickCount = collectFrameTimings ? SDL_GetTicksNS() : 0;
#if defined(OPENYAMM_VITA_PROFILE)
        const uint64_t vitaSubmitBegin = VitaProfile::now();
#endif
        VitaIncident::mark(2);
        const uint32_t vitaProfileBgfxId = bgfx::frame();
        VitaIncident::mark(3);
        if (adaptiveBegin)
        {
            const bgfx::Stats *stats = bgfx::getStats();
            if (stats && stats->cpuTimerFreq > 0)
                VitaAdaptiveResolution::controller.sample(deltaSeconds, double(adaptiveCallback) / 1e6,
                    double(stats->cpuTimeEnd - stats->cpuTimeBegin) * 1000.0 / double(stats->cpuTimerFreq),
                    VitaAdaptiveResolution::gameplayThisFrame);
        }
        if (VitaIncident::active())
        {
            const bgfx::Stats *stats = bgfx::getStats();
            if (stats && stats->cpuTimerFreq > 0)
            {
                const auto ns = [stats](int64_t ticks)
                { return ticks > 0 ? uint64_t(double(ticks) * 1e9 / double(stats->cpuTimerFreq)) : 0ull; };
                VitaIncident::renderer(vitaProfileBgfxId, stats->numDraw,
                    ns(stats->cpuTimeEnd - stats->cpuTimeBegin), ns(stats->waitSubmit), ns(stats->waitRender));
            }
        }
#if defined(OPENYAMM_VITA_PROFILE)
        const uint64_t vitaSubmitNanoseconds = (VitaProfile::now() - vitaSubmitBegin) * 1000;
        VitaDetailProfile::end();
        VitaProfile::end(vitaProfileBgfxId, vitaCallbackUs,
            vitaSubmitNanoseconds / 1000);
        const bgfx::Stats *pVitaStats = bgfx::getStats();
        if (pVitaStats != nullptr && pVitaStats->numDraw >= 16383 && !vitaDrawLimitReported)
        {
            std::cerr << "VITA RENDER BUDGET EXCEEDED: draw queue reached 16383; visuals may be incomplete\n";
            vitaDrawLimitReported = true;
        }
        const uint64_t vitaNow = SDL_GetTicksNS();
        if (vitaNow >= vitaNextMemoryLog)
        {
            reportVitaMemory("frame loop");
            if (pVitaStats != nullptr)
            {
                const double renderMicroseconds = pVitaStats->cpuTimerFreq > 0
                    ? (pVitaStats->cpuTimeEnd - pVitaStats->cpuTimeBegin) * 1000000.0 / pVitaStats->cpuTimerFreq
                    : 0.0;
                std::fprintf(stderr,
                    "VITA FRAME: draw_calls=%u callback_us=%llu submit_us=%llu render_cpu_us=%.0f "
                    "textures=%u texture_bytes=%lld render_target_bytes=%lld\n",
                    pVitaStats->numDraw, (unsigned long long)vitaCallbackUs,
                    (unsigned long long)(vitaSubmitNanoseconds / 1000), renderMicroseconds,
                    pVitaStats->numTextures, (long long)pVitaStats->textureMemoryUsed,
                    (long long)pVitaStats->rtMemoryUsed);
            }
            vitaNextMemoryLog = vitaNow + 10000000000ull;
        }
#endif

        if (collectFrameTimings)
        {
            const uint64_t frameBgfxFrameNanoseconds = SDL_GetTicksNS() - bgfxFrameBeginTickCount;
            const uint64_t frameLoopNanoseconds = SDL_GetTicksNS() - loopBeginTickCount;

            if (collectPerformanceDiagnostics)
            {
                fpsBgfxFrameNanoseconds += frameBgfxFrameNanoseconds;
                fpsLoopNanoseconds += frameLoopNanoseconds;
            }

            if (logFrameHitches && frameLoopNanoseconds >= hitchThresholdNanoseconds)
            {
                const uint64_t measuredNanoseconds =
                    frameEventNanoseconds
                    + frameWindowSizeNanoseconds
                    + frameRenderCallbackNanoseconds
                    + frameBgfxFrameNanoseconds;
                const uint64_t untrackedNanoseconds =
                    frameLoopNanoseconds > measuredNanoseconds
                        ? frameLoopNanoseconds - measuredNanoseconds
                        : 0;
                std::cout << "[FrameHitch]"
                          << " frame_us=" << nanosecondsToMicroseconds(frameLoopNanoseconds)
                          << " delta_us=" << nanosecondsToMicroseconds(frameDeltaNanoseconds)
                          << " threshold_us=" << nanosecondsToMicroseconds(hitchThresholdNanoseconds)
                          << " events=" << frameEventCount
                          << " events_us=" << nanosecondsToMicroseconds(frameEventNanoseconds)
                          << " window_size_us=" << nanosecondsToMicroseconds(frameWindowSizeNanoseconds)
                          << " render_callback_us=" << nanosecondsToMicroseconds(frameRenderCallbackNanoseconds)
                          << " bgfx_frame_us=" << nanosecondsToMicroseconds(frameBgfxFrameNanoseconds)
                          << " untracked_us=" << nanosecondsToMicroseconds(untrackedNanoseconds)
                          << '\n';
            }
        }

        if (sampleFrames)
        {
            fpsSampleSeconds += deltaSeconds;
            ++fpsSampleFrameCount;

            if (fpsSampleSeconds >= 1.0f)
            {
                const float averageFps = fpsSampleSeconds > 0.0f
                    ? static_cast<float>(fpsSampleFrameCount) / fpsSampleSeconds
                    : 0.0f;

                if (logFps)
                {
                    std::cout << "Average FPS (last second): " << averageFps << '\n';
                }

                if (collectPerformanceDiagnostics)
                {
                    const uint64_t measuredNanoseconds =
                        fpsEventNanoseconds
                        + fpsWindowSizeNanoseconds
                        + fpsRenderCallbackNanoseconds
                        + fpsBgfxFrameNanoseconds;
                    const uint64_t untrackedNanoseconds =
                        fpsLoopNanoseconds > measuredNanoseconds
                            ? fpsLoopNanoseconds - measuredNanoseconds
                            : 0;
                    std::cout << "[FramePerf]"
                              << " frames=" << fpsSampleFrameCount
                              << " events=" << fpsSampleEventCount
                              << " avg_loop_us=" << nanosecondsToMicroseconds(averageNanoseconds(
                                  fpsLoopNanoseconds,
                                  fpsSampleFrameCount))
                              << " avg_untracked_us=" << nanosecondsToMicroseconds(averageNanoseconds(
                                  untrackedNanoseconds,
                                  fpsSampleFrameCount))
                              << " avg_events_us=" << nanosecondsToMicroseconds(averageNanoseconds(
                                  fpsEventNanoseconds,
                                  fpsSampleFrameCount))
                              << " avg_window_size_us=" << nanosecondsToMicroseconds(averageNanoseconds(
                                  fpsWindowSizeNanoseconds,
                                  fpsSampleFrameCount))
                              << " avg_render_callback_us=" << nanosecondsToMicroseconds(averageNanoseconds(
                                  fpsRenderCallbackNanoseconds,
                                  fpsSampleFrameCount))
                              << " avg_bgfx_frame_us=" << nanosecondsToMicroseconds(averageNanoseconds(
                                  fpsBgfxFrameNanoseconds,
                                  fpsSampleFrameCount))
                              << '\n';
                }

                fpsSampleSeconds = 0.0f;
                fpsSampleFrameCount = 0;
                fpsSampleEventCount = 0;
                fpsLoopNanoseconds = 0;
                fpsEventNanoseconds = 0;
                fpsWindowSizeNanoseconds = 0;
                fpsRenderCallbackNanoseconds = 0;
                fpsBgfxFrameNanoseconds = 0;
            }
        }
        VitaIncident::end();
#if defined(OPENYAMM_VITA_LOADER)
        if (VitaFrameLimit::fps != 0 && VitaFrameLimit::gameplay && isRunning)
        {
            const uint64_t wait = VitaFrameLimit::waitNanoseconds(
                currentFrameTickCount, SDL_GetTicksNS(), VitaFrameLimit::fps, true);
            if (wait != 0)
            {
                const timespec delay = {0, static_cast<long>(wait)};
                nanosleep(&delay, nullptr);
            }
        }
#endif
    }

#if defined(__ANDROID__)
    if (managedTextInputActive)
    {
        SDL_StopTextInput(pWindow.get());
    }
#endif

    shutdownGuard.invoke();

    return 0;
}

void EngineApplication::setConfiguration(const ApplicationConfig &config)
{
    m_config = config;
}

bool EngineApplication::validateConfiguration() const
{
#if defined(__ANDROID__)
    const bool androidApkAssetRoot = m_config.assetRoot == "assets"
        || std::filesystem::path(m_config.assetRoot).filename() == "assets";

    if (!androidApkAssetRoot && !std::filesystem::exists(m_config.assetRoot))
#else
    if (!std::filesystem::exists(m_config.assetRoot))
#endif
    {
        std::cerr << "Asset root does not exist: " << m_config.assetRoot << '\n';
        return false;
    }

    if (m_config.windowWidth <= 0 || m_config.windowHeight <= 0)
    {
        std::cerr << "Invalid window size: " << m_config.windowWidth << "x" << m_config.windowHeight << '\n';
        return false;
    }

    return true;
}

bool EngineApplication::initializeAssetFileSystem(AssetFileSystem &assetFileSystem) const
{
    const char *pBasePathChars = SDL_GetBasePath();
    std::filesystem::path basePath = std::filesystem::current_path();

    if (pBasePathChars != nullptr)
    {
        basePath = pBasePathChars;
    }

    return assetFileSystem.initialize(
        basePath,
        m_config.assetRoot,
        m_config.assetScaleTier,
        m_config.assetScaleProfile,
        m_config.activeWorldId);
}
}
