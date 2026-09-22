#include "engine/ApplicationConfig.h"

#include "engine/AssetPaths.h"
#if defined(OPENYAMM_VITA_LOADER)
#include "packaging/vita/asset-location.h"
#include <filesystem>
#endif

namespace OpenYAMM::Engine
{
ApplicationConfig ApplicationConfig::createDefault()
{
    ApplicationConfig config;
    config.appName = "OpenYAMM";
#if defined(OPENYAMM_VITA_LOADER)
    config.assetRoot = oy_asset_root(0, [](const char *path) -> int {
        std::error_code error;
        const auto size = std::filesystem::file_size(path, error);
        return !error && size > 0;
    });
#elif defined(__ANDROID__)
    config.assetRoot = "assets";
#else
    config.assetRoot = AssetPaths::getDevelopmentAssetRoot();
#endif
    config.activeWorldId = "mm8";
    config.startupMapFileOverride.clear();
    config.loadUniqueActorIndex.reset();
    config.assetScaleTier = AssetScaleTier::X1;
    config.assetScaleProfile = createUniformAssetScaleProfile(config.assetScaleTier);
    config.windowWidth = OPENYAMM_WINDOW_WIDTH;
    config.windowHeight = OPENYAMM_WINDOW_HEIGHT;
    config.windowMode = WindowMode::Windowed;
    config.verticalSync = false;
    config.fpsTrace = false;
    config.performanceTrace = false;
    config.hitchTrace = false;
    config.hitchThresholdMilliseconds = 8.0f;
    return config;
}
}
