#include "game/render/VitaIndoorDiagnostics.h"
#include "engine/VitaInitialView.h"
#include "engine/VitaVisibilityProfile.h"
#include "game/render/VitaActorRenderLod.h"
#include "game/render/VitaViewCost.h"
#include "engine/VitaDetailProfile.h"
#include "game/render/VitaAnimationBudget.h"
#include "game/render/VitaPerformanceConfig.h"
#include "game/render/VitaFrustum.h"
#include <map>
#include "engine/VitaFrameProfile.h"
#include "engine/VitaCleanDiagnostics.h"
#include "game/render/VitaSpriteHeader.h"
#include "game/gameplay/VitaPickTiming.h"
#include "game/outdoor/OutdoorBillboardRenderer.h"
#include "game/render/SelectionTint.h"

#include "game/app/GameSession.h"
#include "game/data/GameDataRepository.h"
#include "game/events/EventProjectileSpells.h"
#include "game/fx/ParticleRecipes.h"
#include "game/fx/ParticleSystem.h"
#include "game/gameplay/GameplayRuntimeInterfaces.h"
#include "game/gameplay/GameplayScreenRuntime.h"
#include "game/outdoor/OutdoorGameView.h"
#include "game/outdoor/OutdoorFogProfile.h"
#include "game/outdoor/OutdoorInteractionController.h"
#include "game/render/BillboardGeometry.h"
#include "game/render/CombatActorHealthBarPolicy.h"
#include "game/render/QuestMarkerGeometry.h"
#include "game/render/TextureFiltering.h"
#include "game/StringUtils.h"
#include "game/scene/OutdoorSceneRuntime.h"
#include "engine/ImageAssetLoader.h"

#include <SDL3/SDL.h>
#include <bx/math.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <future>
#include <iostream>
#include <iterator>
#include <limits>
#include <optional>
#include <thread>
#include <unordered_map>
#include <vector>

namespace OpenYAMM::Game
{
namespace
{
constexpr float Pi = 3.14159265358979323846f;
constexpr uint64_t RenderHitchLogThresholdNanoseconds = 16 * 1000 * 1000;
constexpr float BillboardNearDepth = 0.1f;
constexpr bool DebugProjectileDrawLogging = false;
constexpr uint64_t BillboardAlphaRenderState =
    BGFX_STATE_WRITE_RGB
    | BGFX_STATE_WRITE_A
    | BGFX_STATE_WRITE_Z
    | BGFX_STATE_DEPTH_TEST_LEQUAL
    | BGFX_STATE_BLEND_ALPHA;
constexpr size_t MaxPreloadDecodeWorkerCount = 8;
constexpr bool DebugSpritePreloadLogging = false;
constexpr uint64_t ColoredAlphaRenderState =
    BGFX_STATE_WRITE_RGB
    | BGFX_STATE_WRITE_A
    | BGFX_STATE_DEPTH_TEST_LEQUAL
    | BGFX_STATE_BLEND_ALPHA;
constexpr uint64_t ColoredAdditiveRenderState =
    BGFX_STATE_WRITE_RGB
    | BGFX_STATE_WRITE_A
    | BGFX_STATE_DEPTH_TEST_LEQUAL
    | BGFX_STATE_BLEND_ADD;
constexpr float BillboardAmbientLight = 0.85f;
constexpr float BillboardLightContributionScale = 0.7f;
constexpr const char *ContactShadowTextureName = "__contact_shadow_blob__";
constexpr float HoveredActorOutlineThicknessPixels = 2.0f;
constexpr float OutdoorFogNearOpacity = 0.04f;
constexpr float OutdoorFogStrongOpacity = 176.0f / 255.0f;
constexpr float OutdoorUnderwaterTintOpacity = 0.28f;
constexpr uint8_t UnderwaterFogRed = 33;
constexpr uint8_t UnderwaterFogGreen = 142;
constexpr uint8_t UnderwaterFogBlue = 90;

uint32_t makeAbgr(uint8_t red, uint8_t green, uint8_t blue)
{
    return 0xff000000u
        | (static_cast<uint32_t>(blue) << 16)
        | (static_cast<uint32_t>(green) << 8)
        | static_cast<uint32_t>(red);
}

uint32_t makeAbgrAlpha(uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha)
{
    return (static_cast<uint32_t>(alpha) << 24)
        | (static_cast<uint32_t>(blue) << 16)
        | (static_cast<uint32_t>(green) << 8)
        | static_cast<uint32_t>(red);
}

uint8_t lightContributionByte(float value)
{
    return static_cast<uint8_t>(
        std::clamp(std::lround(value * BillboardLightContributionScale * 255.0f), 0l, 255l));
}

bool timingEnvironmentFlagEnabled(const char *pName)
{
    const char *pValue = std::getenv(pName);
    return pValue != nullptr
        && pValue[0] != '\0'
        && std::strcmp(pValue, "0") != 0
        && std::strcmp(pValue, "false") != 0;
}

bool actorRenderHitchLoggingEnabled()
{
#if defined(OPENYAMM_VITA_CLEAN)
    return false;
#endif
    static const bool enabled =
        timingEnvironmentFlagEnabled("OPENYAMM_MAP_LOAD_TIMING")
        || timingEnvironmentFlagEnabled("OPENYAMM_ACTOR_RENDER_TIMING");
    return enabled;
}

float outdoorBillboardDepthSliceSize(const OutdoorGameView &view)
{
    return std::clamp(view.settingsSnapshot().outdoorBillboardDepthSlice, 0.0f, 8192.0f);
}

size_t spritePreloadDecodeWorkerCount(size_t requestCount)
{
    if (requestCount <= 1)
    {
        return requestCount;
    }

    const unsigned int hardwareThreadCount = std::thread::hardware_concurrency();
    const size_t availableThreadCount = hardwareThreadCount > 1 ? static_cast<size_t>(hardwareThreadCount - 1) : 1;
    const size_t cappedThreadCount = std::min(availableThreadCount, MaxPreloadDecodeWorkerCount);
    return std::min(cappedThreadCount, requestCount);
}

uint32_t makeTintedFogColor(
    uint8_t brightness,
    bool hasFogTint,
    uint8_t tintRed,
    uint8_t tintGreen,
    uint8_t tintBlue)
{
    if (!hasFogTint)
    {
        return makeAbgr(brightness, brightness, brightness);
    }

    const uint8_t red =
        static_cast<uint8_t>(std::clamp(std::lround(static_cast<float>(brightness) * tintRed / 255.0f), 0l, 255l));
    const uint8_t green =
        static_cast<uint8_t>(std::clamp(std::lround(static_cast<float>(brightness) * tintGreen / 255.0f), 0l, 255l));
    const uint8_t blue =
        static_cast<uint8_t>(std::clamp(std::lround(static_cast<float>(brightness) * tintBlue / 255.0f), 0l, 255l));
    return makeAbgr(red, green, blue);
}

struct SpriteTexturePreloadRequest
{
    std::string textureName;
    std::string virtualPath;
    int16_t paletteId = 0;
    std::vector<uint8_t> bitmapBytes;
    std::optional<std::array<uint8_t, 256 * 3>> overridePalette;
};

struct DecodedSpriteTexture
{
    std::string textureName;
    int16_t paletteId = 0;
    int width = 0;
    int height = 0;
    std::vector<uint8_t> pixels;
};

uint32_t resolveHoveredActorOutlineColor(const OutdoorWorldRuntime::MapActorState &actor)
{
    if (actor.isDead)
    {
        return makeAbgr(255, 224, 64);
    }

    if (actor.hostileToParty)
    {
        return makeAbgr(255, 64, 64);
    }

    return makeAbgr(64, 255, 64);
}

uint32_t hoveredWorldItemOutlineColor()
{
    return makeAbgr(64, 128, 255);
}

uint32_t contextActionHighlightOutlineColor()
{
    return makeAbgr(56, 216, 255);
}

const GameplayWorldHit *selectedContextActionWorldHit(const GameplayContextActionState &state)
{
    if (!state.visible || state.primaryIndex >= state.actions.size())
    {
        return nullptr;
    }

    const GameplayWorldHit &hit = state.actions[state.primaryIndex].worldHit;
    return hit.hasHit ? &hit : nullptr;
}

bool contextActionHighlightsActor(const GameplayWorldHit *pHit, size_t actorIndex)
{
    return pHit != nullptr
        && pHit->kind == GameplayWorldHitKind::Actor
        && pHit->actor.has_value()
        && pHit->actor->actorIndex == actorIndex;
}

bool contextActionHighlightsWorldItem(const GameplayWorldHit *pHit, size_t worldItemIndex)
{
    return pHit != nullptr
        && pHit->kind == GameplayWorldHitKind::WorldItem
        && pHit->worldItem.has_value()
        && pHit->worldItem->worldItemIndex == worldItemIndex;
}

bool contextActionHighlightsOutdoorDecoration(const GameplayWorldHit *pHit, size_t decorationIndex)
{
    return pHit != nullptr
        && pHit->kind == GameplayWorldHitKind::EventTarget
        && pHit->eventTarget.has_value()
        && pHit->eventTarget->targetKind == GameplayWorldEventTargetKind::Decoration
        && pHit->eventTarget->targetIndex == decorationIndex;
}

uint32_t computeClearDistanceFogColorAbgr(const OutdoorWorldRuntime &worldRuntime)
{
    const uint8_t brightness = outdoorClearDistanceFogBrightness(worldRuntime.gameMinutes());
    return makeAbgr(brightness, brightness, brightness);
}

uint32_t computeBillboardFogColorAbgr(const OutdoorWorldRuntime::AtmosphereState &atmosphereState)
{
    if (atmosphereState.hasAuthoredFogColor)
    {
        return makeAbgr(
            atmosphereState.authoredFogRed,
            atmosphereState.authoredFogGreen,
            atmosphereState.authoredFogBlue);
    }

    if (atmosphereState.underwater)
    {
        return makeAbgr(UnderwaterFogRed, UnderwaterFogGreen, UnderwaterFogBlue);
    }

    if (atmosphereState.isNight)
    {
        if (atmosphereState.hasFogTint)
        {
            return makeTintedFogColor(
                48,
                true,
                atmosphereState.fogTintRed,
                atmosphereState.fogTintGreen,
                atmosphereState.fogTintBlue);
        }

        return atmosphereState.redFog ? makeAbgr(48, 18, 18) : makeAbgr(31, 31, 31);
    }

    const int fogLevel = std::clamp(
        static_cast<int>(std::lround(
            (1.0f - atmosphereState.fogDensity) * 200.0f + atmosphereState.fogDensity * 31.0f)),
        0,
        255);
    const uint8_t red = static_cast<uint8_t>(fogLevel);

    if (atmosphereState.hasFogTint)
    {
        return makeTintedFogColor(
            red,
            true,
            atmosphereState.fogTintRed,
            atmosphereState.fogTintGreen,
            atmosphereState.fogTintBlue);
    }

    if (atmosphereState.redFog)
    {
        const uint8_t green =
            static_cast<uint8_t>(std::lround(static_cast<float>(fogLevel) * 0.35f));
        const uint8_t blue =
            static_cast<uint8_t>(std::lround(static_cast<float>(fogLevel) * 0.35f));
        return makeAbgr(red, green, blue);
    }

    return makeAbgr(red, red, red);
}

uint32_t currentAnimationTicks()
{
    if (Engine::VitaInitialView::active) return Engine::VitaInitialView::animationTicks;
    return static_cast<uint32_t>((static_cast<uint64_t>(SDL_GetTicks()) * 128ULL) / 1000ULL);
}

std::optional<std::vector<uint8_t>> loadSpriteBitmapPixelsBgra(
    const std::vector<uint8_t> &bitmapBytes,
    const std::string &virtualPath,
    const std::optional<std::array<uint8_t, 256 * 3>> &overridePalette,
    int &width,
    int &height)
{
    Engine::ImageDecodeOptions decodeOptions = {};
    decodeOptions.overridePalette = overridePalette;
    decodeOptions.applyPaletteZeroTransparencyKey = true;

    const std::optional<Engine::ImagePixelsBgra> image =
        Engine::decodeImagePixelsBgra(bitmapBytes, virtualPath, decodeOptions);

    if (!image)
    {
        return std::nullopt;
    }

    width = image->width;
    height = image->height;
    return image->pixels;
}

struct ProjectedBillboardRect
{
    float left = 0.0f;
    float right = 0.0f;
    float top = 0.0f;
    float bottom = 0.0f;
};

bool projectWorldPointToScreenForKeyboardCache(
    const bx::Vec3 &worldPoint,
    int viewWidth,
    int viewHeight,
    const float *pViewProjectionMatrix,
    float &screenX,
    float &screenY)
{
    const bx::Vec3 projectedPoint = bx::mulH(worldPoint, pViewProjectionMatrix);

    if (!std::isfinite(projectedPoint.x)
        || !std::isfinite(projectedPoint.y)
        || !std::isfinite(projectedPoint.z))
    {
        return false;
    }

    screenX = (projectedPoint.x * 0.5f + 0.5f) * static_cast<float>(viewWidth);
    screenY = (1.0f - (projectedPoint.y * 0.5f + 0.5f)) * static_cast<float>(viewHeight);
    return std::isfinite(screenX) && std::isfinite(screenY);
}

bool projectVisibleBillboardRect(
    const bx::Vec3 &center,
    const bx::Vec3 &right,
    const bx::Vec3 &up,
    int viewWidth,
    int viewHeight,
    const float *pViewProjectionMatrix,
    ProjectedBillboardRect &rect)
{
    const std::array<bx::Vec3, 4> corners = {{
        {center.x - right.x + up.x, center.y - right.y + up.y, center.z - right.z + up.z},
        {center.x + right.x + up.x, center.y + right.y + up.y, center.z + right.z + up.z},
        {center.x - right.x - up.x, center.y - right.y - up.y, center.z - right.z - up.z},
        {center.x + right.x - up.x, center.y + right.y - up.y, center.z + right.z - up.z}
    }};
    bool hasProjection = false;
    float minX = 0.0f;
    float maxX = 0.0f;
    float minY = 0.0f;
    float maxY = 0.0f;

    for (const bx::Vec3 &corner : corners)
    {
        float projectedX = 0.0f;
        float projectedY = 0.0f;

        if (!projectWorldPointToScreenForKeyboardCache(
                corner,
                viewWidth,
                viewHeight,
                pViewProjectionMatrix,
                projectedX,
                projectedY))
        {
            return false;
        }

        if (!hasProjection)
        {
            minX = projectedX;
            maxX = projectedX;
            minY = projectedY;
            maxY = projectedY;
            hasProjection = true;
            continue;
        }

        minX = std::min(minX, projectedX);
        maxX = std::max(maxX, projectedX);
        minY = std::min(minY, projectedY);
        maxY = std::max(maxY, projectedY);
    }

    if (!hasProjection)
    {
        return false;
    }

    rect.left = std::clamp(minX, 0.0f, static_cast<float>(viewWidth - 1));
    rect.right = std::clamp(maxX, 0.0f, static_cast<float>(viewWidth - 1));
    rect.top = std::clamp(minY, 0.0f, static_cast<float>(viewHeight - 1));
    rect.bottom = std::clamp(maxY, 0.0f, static_cast<float>(viewHeight - 1));
    return rect.right > rect.left && rect.bottom > rect.top;
}

void fillKeyboardInteractionSamplePoints(
    const ProjectedBillboardRect &rect,
    OutdoorGameView::KeyboardInteractionBillboardCandidate &candidate)
{
    candidate.samplePointCount = 0;
    const auto appendPoint =
        [&](float x, float y)
        {
            if (candidate.samplePointCount >= candidate.samplePoints.size())
            {
                return;
            }

            candidate.samplePoints[candidate.samplePointCount++] = {x, y};
        };

    const float centerX = (rect.left + rect.right) * 0.5f;
    const float centerY = (rect.top + rect.bottom) * 0.5f;
    appendPoint(centerX, centerY);
    appendPoint(rect.left, rect.top);
    appendPoint(rect.right, rect.top);
    appendPoint(rect.left, rect.bottom);
    appendPoint(rect.right, rect.bottom);
    appendPoint(centerX, rect.bottom);
}

uint32_t scaleAlpha(uint32_t colorAbgr, float alphaScale)
{
    const uint32_t clampedAlpha =
        static_cast<uint32_t>(std::lround(
            std::clamp(alphaScale, 0.0f, 1.0f) * static_cast<float>((colorAbgr >> 24) & 0xffu)));
    return (colorAbgr & 0x00ffffffu) | (clampedAlpha << 24);
}

float redChannel(uint32_t colorAbgr)
{
    return static_cast<float>(colorAbgr & 0xffu) / 255.0f;
}

float greenChannel(uint32_t colorAbgr)
{
    return static_cast<float>((colorAbgr >> 8) & 0xffu) / 255.0f;
}

float blueChannel(uint32_t colorAbgr)
{
    return static_cast<float>((colorAbgr >> 16) & 0xffu) / 255.0f;
}

float alphaChannel(uint32_t colorAbgr)
{
    return static_cast<float>((colorAbgr >> 24) & 0xffu) / 255.0f;
}

std::vector<uint8_t> buildContactShadowBlobPixels(int width, int height)
{
    std::vector<uint8_t> pixels(static_cast<size_t>(width) * static_cast<size_t>(height) * 4, 0);

    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            const float normalizedX = ((static_cast<float>(x) + 0.5f) / static_cast<float>(width)) * 2.0f - 1.0f;
            const float normalizedY = ((static_cast<float>(y) + 0.5f) / static_cast<float>(height)) * 2.0f - 1.0f;
            const float radialDistance = std::sqrt(normalizedX * normalizedX + normalizedY * normalizedY);
            const float alpha = 1.0f - std::clamp((radialDistance - 0.2f) / 0.8f, 0.0f, 1.0f);
            const size_t offset =
                (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4;
            pixels[offset + 0] = 0;
            pixels[offset + 1] = 0;
            pixels[offset + 2] = 0;
            pixels[offset + 3] = static_cast<uint8_t>(std::lround(alpha * alpha * 255.0f));
        }
    }

    return pixels;
}


} // namespace

bool OutdoorBillboardRenderer::uploadBillboardTexture(OutdoorGameView &view, const OutdoorBitmapTexture &texture)
{
    if (texture.textureName.empty()
        || texture.pixels.empty()
        || texture.physicalWidth <= 0
        || texture.physicalHeight <= 0
        || view.findBillboardTexture(texture.textureName, texture.paletteId) != nullptr)
    {
        return false;
    }

    OutdoorGameView::BillboardTextureHandle billboardTexture = {};
    billboardTexture.textureName = toLowerCopy(texture.textureName);
    billboardTexture.paletteId = texture.paletteId;
    billboardTexture.width = texture.width;
    billboardTexture.height = texture.height;
    billboardTexture.physicalWidth = texture.physicalWidth;
    billboardTexture.physicalHeight = texture.physicalHeight;
    billboardTexture.opacityMask.assignFromBgra(
        texture.pixels,
        texture.physicalWidth,
        texture.physicalHeight);
    billboardTexture.textureHandle = createBgraTexture2D(
        uint16_t(texture.physicalWidth),
        uint16_t(texture.physicalHeight),
        texture.pixels.data(),
        uint32_t(texture.pixels.size()),
        TextureFilterProfile::Billboard,
        BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP,
        texture.pixelsPreparedForUpload
            ? BgraTexturePixelPreparation::AlreadyPrepared
            : BgraTexturePixelPreparation::Required, texture.textureName.c_str());

    if (!bgfx::isValid(billboardTexture.textureHandle))
    {
        return false;
    }

    view.m_billboardTextureHandles.push_back(std::move(billboardTexture));
    view.m_billboardTextureIndexByPalette[texture.paletteId][view.m_billboardTextureHandles.back().textureName] =
        view.m_billboardTextureHandles.size() - 1;
    return true;
}

void OutdoorBillboardRenderer::applyBillboardFogUniforms(OutdoorGameView &view, float renderDistance)
{
    if (!bgfx::isValid(view.m_outdoorFogColorUniformHandle)
        || !bgfx::isValid(view.m_outdoorFogDensitiesUniformHandle)
        || !bgfx::isValid(view.m_outdoorFogDistancesUniformHandle))
    {
        return;
    }

    const float clampedRenderDistance = std::max(renderDistance, 1.0f);
    float fogColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    float fogDensities[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float fogDistances[4] = {
        clampedRenderDistance,
        clampedRenderDistance,
        clampedRenderDistance,
        0.0f
    };

    if (view.m_pOutdoorWorldRuntime != nullptr)
    {
        const OutdoorWorldRuntime::AtmosphereState &atmosphereState =
            view.m_pOutdoorWorldRuntime->atmosphereState();
        uint32_t fogColorAbgr = computeClearDistanceFogColorAbgr(*view.m_pOutdoorWorldRuntime);
        const OutdoorFogProfile clearFogProfile = buildOutdoorClearDistanceFogProfile(clampedRenderDistance);
        fogColor[0] = static_cast<float>(fogColorAbgr & 0xffu) / 255.0f;
        fogColor[1] = static_cast<float>((fogColorAbgr >> 8) & 0xffu) / 255.0f;
        fogColor[2] = static_cast<float>((fogColorAbgr >> 16) & 0xffu) / 255.0f;
        fogDensities[0] = clearFogProfile.nearOpacity;
        fogDensities[1] = clearFogProfile.strongOpacity;
        fogDistances[0] = clearFogProfile.weakDistance;
        fogDistances[1] = clearFogProfile.strongDistance;
        fogDistances[2] = clearFogProfile.farDistance;

        if ((atmosphereState.weatherFlags & 1) != 0
            && atmosphereState.fogWeakDistance >= 0
            && atmosphereState.fogStrongDistance > atmosphereState.fogWeakDistance)
        {
            const OutdoorFogProfile fogProfile = atmosphereState.directFog
                ? buildOutdoorDirectFogProfile(
                    atmosphereState.fogWeakDistance,
                    atmosphereState.fogStrongDistance)
                : buildOutdoorFogProfile(
                    atmosphereState.fogWeakDistance,
                    atmosphereState.fogStrongDistance,
                    clampedRenderDistance,
                    OutdoorFogNearOpacity,
                    OutdoorFogStrongOpacity);
            fogColorAbgr = computeBillboardFogColorAbgr(atmosphereState);
            fogColor[0] = static_cast<float>(fogColorAbgr & 0xffu) / 255.0f;
            fogColor[1] = static_cast<float>((fogColorAbgr >> 8) & 0xffu) / 255.0f;
            fogColor[2] = static_cast<float>((fogColorAbgr >> 16) & 0xffu) / 255.0f;
            fogDensities[0] = fogProfile.nearOpacity;
            fogDensities[1] = fogProfile.strongOpacity;
            fogDensities[3] = atmosphereState.directFog ? 1.0f : 0.0f;
            if (atmosphereState.underwater)
            {
                fogDensities[2] = OutdoorUnderwaterTintOpacity;
            }
            fogDistances[0] = fogProfile.weakDistance;
            fogDistances[1] = fogProfile.strongDistance;
            fogDistances[2] = fogProfile.farDistance;
        }
    }

    bgfx::setUniform(view.m_outdoorFogColorUniformHandle, fogColor);
    bgfx::setUniform(view.m_outdoorFogDensitiesUniformHandle, fogDensities);
    bgfx::setUniform(view.m_outdoorFogDistancesUniformHandle, fogDistances);
}

void OutdoorBillboardRenderer::appendWorldQuadVertices(
    std::vector<OutdoorGameView::TerrainVertex> &vertices,
    const bx::Vec3 &center,
    const bx::Vec3 &right,
    const bx::Vec3 &up,
    uint32_t colorAbgr)
{
    vertices.push_back({center.x - right.x - up.x, center.y - right.y - up.y, center.z - right.z - up.z, colorAbgr});
    vertices.push_back({center.x - right.x + up.x, center.y - right.y + up.y, center.z - right.z + up.z, colorAbgr});
    vertices.push_back({center.x + right.x + up.x, center.y + right.y + up.y, center.z + right.z + up.z, colorAbgr});
    vertices.push_back({center.x - right.x - up.x, center.y - right.y - up.y, center.z - right.z - up.z, colorAbgr});
    vertices.push_back({center.x + right.x + up.x, center.y + right.y + up.y, center.z + right.z + up.z, colorAbgr});
    vertices.push_back({center.x + right.x - up.x, center.y + right.y - up.y, center.z + right.z - up.z, colorAbgr});
}

void OutdoorBillboardRenderer::submitColoredVertices(
    OutdoorGameView &view,
    uint16_t viewId,
    const std::vector<OutdoorGameView::TerrainVertex> &vertices,
    uint64_t renderState)
{
    if (vertices.empty() || !bgfx::isValid(view.m_programHandle))
    {
        return;
    }

    if (bgfx::getAvailTransientVertexBuffer(
            static_cast<uint32_t>(vertices.size()),
            OutdoorGameView::TerrainVertex::ms_layout) < vertices.size())
    {
        return;
    }

    bgfx::TransientVertexBuffer transientVertexBuffer = {};
    bgfx::allocTransientVertexBuffer(
        &transientVertexBuffer,
        static_cast<uint32_t>(vertices.size()),
        OutdoorGameView::TerrainVertex::ms_layout);
    std::memcpy(
        transientVertexBuffer.data,
        vertices.data(),
        static_cast<size_t>(vertices.size() * sizeof(OutdoorGameView::TerrainVertex)));

    float modelMatrix[16] = {};
    bx::mtxIdentity(modelMatrix);
    bgfx::setTransform(modelMatrix);
    bgfx::setVertexBuffer(0, &transientVertexBuffer, 0, static_cast<uint32_t>(vertices.size()));
    bgfx::setState(renderState);
    bgfx::submit(viewId, view.m_programHandle);
}

void OutdoorBillboardRenderer::appendLitBillboardVertices(
    std::vector<OutdoorGameView::LitBillboardVertex> &vertices,
    const bx::Vec3 &center,
    const bx::Vec3 &right,
    const bx::Vec3 &up,
    float u0,
    float u1,
    uint32_t lightContributionAbgr)
{
    vertices.push_back(
        {center.x - right.x - up.x, center.y - right.y - up.y, center.z - right.z - up.z, u0, 1.0f,
         lightContributionAbgr});
    vertices.push_back(
        {center.x - right.x + up.x, center.y - right.y + up.y, center.z - right.z + up.z, u0, 0.0f,
         lightContributionAbgr});
    vertices.push_back(
        {center.x + right.x + up.x, center.y + right.y + up.y, center.z + right.z + up.z, u1, 0.0f,
         lightContributionAbgr});
    vertices.push_back(
        {center.x - right.x - up.x, center.y - right.y - up.y, center.z - right.z - up.z, u0, 1.0f,
         lightContributionAbgr});
    vertices.push_back(
        {center.x + right.x + up.x, center.y + right.y + up.y, center.z + right.z + up.z, u1, 0.0f,
         lightContributionAbgr});
    vertices.push_back(
        {center.x + right.x - up.x, center.y + right.y - up.y, center.z + right.z - up.z, u1, 1.0f,
         lightContributionAbgr});
}

void OutdoorBillboardRenderer::writeLitBillboardVertices(
    OutdoorGameView::LitBillboardVertex *pVertices,
    const bx::Vec3 &center,
    const bx::Vec3 &right,
    const bx::Vec3 &up,
    float u0,
    float u1,
    uint32_t lightContributionAbgr)
{
    pVertices[0] = {
        center.x - right.x - up.x,
        center.y - right.y - up.y,
        center.z - right.z - up.z,
        u0,
        1.0f,
        lightContributionAbgr};
    pVertices[1] = {
        center.x - right.x + up.x,
        center.y - right.y + up.y,
        center.z - right.z + up.z,
        u0,
        0.0f,
        lightContributionAbgr};
    pVertices[2] = {
        center.x + right.x + up.x,
        center.y + right.y + up.y,
        center.z + right.z + up.z,
        u1,
        0.0f,
        lightContributionAbgr};
    pVertices[3] = {
        center.x - right.x - up.x,
        center.y - right.y - up.y,
        center.z - right.z - up.z,
        u0,
        1.0f,
        lightContributionAbgr};
    pVertices[4] = {
        center.x + right.x + up.x,
        center.y + right.y + up.y,
        center.z + right.z + up.z,
        u1,
        0.0f,
        lightContributionAbgr};
    pVertices[5] = {
        center.x + right.x - up.x,
        center.y + right.y - up.y,
        center.z + right.z - up.z,
        u1,
        1.0f,
        lightContributionAbgr};
}

uint32_t OutdoorBillboardRenderer::computeBillboardLightContributionAbgr(
    OutdoorGameView &view,
    float x,
    float y,
    float z)
{
    OY_VIS(++Engine::VitaVisibility::counts.lighting[Engine::VitaVisibility::origin]);
    LightingStats *pLightingStats = OY_VITA_TRACE(view.m_gameSettings.performanceTrace) ? &view.m_outdoorLightingStats : nullptr;
    const uint64_t sampleBeginTickCount = pLightingStats != nullptr ? SDL_GetTicksNS() : 0;

    if (pLightingStats != nullptr)
    {
        ++pLightingStats->billboardSamples;
    }

    const std::array<float, 3> rgb = view.m_outdoorLightingRuntime.sampleLightingRgb({x, y, z});

    if (pLightingStats != nullptr)
    {
        pLightingStats->billboardSampleNanoseconds += SDL_GetTicksNS() - sampleBeginTickCount;
    }

    return makeAbgr(
        lightContributionByte(rgb[0]),
        lightContributionByte(rgb[1]),
        lightContributionByte(rgb[2]));
}

std::optional<OutdoorGameView::InspectHit> OutdoorBillboardRenderer::resolveHoveredOutlineInspectHit(
    OutdoorGameView &view,
    const float *pViewMatrix)
{
    (void)pViewMatrix;

    if (view.m_cachedHoverInspectHitValid)
    {
        return view.m_cachedHoverInspectHit;
    }

    if (!view.m_gameSession.gameplayScreenState().pendingSpellTarget().active)
    {
        return std::nullopt;
    }

    return std::nullopt;
}

void OutdoorBillboardRenderer::applyBillboardAmbientUniform(OutdoorGameView &view)
{
    const OutdoorLightingData *pLightingData =
        view.m_pOutdoorMapData != nullptr && view.m_pOutdoorMapData->lightingData
            ? &*view.m_pOutdoorMapData->lightingData
            : nullptr;
    const uint32_t mapAmbientColor = pLightingData != nullptr ? pLightingData->ambientColorAbgr : 0;
    float ambient[4] = {
        mapAmbientColor != 0 ? redChannel(mapAmbientColor) : BillboardAmbientLight,
        mapAmbientColor != 0 ? greenChannel(mapAmbientColor) : BillboardAmbientLight,
        mapAmbientColor != 0 ? blueChannel(mapAmbientColor) : BillboardAmbientLight,
        0.0f,
    };

    if (pLightingData != nullptr)
    {
        for (const OutdoorAuthoredLight &light : pLightingData->authoredLights)
        {
            if (!light.lightsObjects() || !light.globalObjectLight())
            {
                continue;
            }

            ambient[0] = std::clamp(
                ambient[0] + redChannel(light.effectiveColorAbgr) * BillboardLightContributionScale,
                0.0f,
                1.0f);
            ambient[1] = std::clamp(
                ambient[1] + greenChannel(light.effectiveColorAbgr) * BillboardLightContributionScale,
                0.0f,
                1.0f);
            ambient[2] = std::clamp(
                ambient[2] + blueChannel(light.effectiveColorAbgr) * BillboardLightContributionScale,
                0.0f,
                1.0f);
        }
    }

    bgfx::setUniform(view.m_outdoorBillboardAmbientUniformHandle, ambient);
}

void OutdoorBillboardRenderer::applyBillboardOverrideColorUniform(
    OutdoorGameView &view,
    uint32_t colorAbgr,
    float alpha)
{
    if (!bgfx::isValid(view.m_outdoorBillboardOverrideColorUniformHandle))
    {
        return;
    }

    const float overrideColor[4] = {
        redChannel(colorAbgr),
        greenChannel(colorAbgr),
        blueChannel(colorAbgr),
        alpha
    };
    bgfx::setUniform(view.m_outdoorBillboardOverrideColorUniformHandle, overrideColor);
}

void OutdoorBillboardRenderer::applyBillboardOutlineParamsUniform(
    OutdoorGameView &view,
    float texelWidth,
    float texelHeight,
    float thicknessPixels)
{
    if (!bgfx::isValid(view.m_outdoorBillboardOutlineParamsUniformHandle))
    {
        return;
    }

    const float outlineParams[4] = {texelWidth, texelHeight, thicknessPixels, 1.0f};
    bgfx::setUniform(view.m_outdoorBillboardOutlineParamsUniformHandle, outlineParams);
}

void OutdoorBillboardRenderer::initializeBillboardResources(OutdoorGameView &view)
{
    if (view.findBillboardTexture(ContactShadowTextureName, 0) == nullptr)
    {
        OutdoorGameView::BillboardTextureHandle shadowTexture = {};
        shadowTexture.textureName = ContactShadowTextureName;
        shadowTexture.paletteId = 0;
        shadowTexture.width = 64;
        shadowTexture.height = 64;
        shadowTexture.physicalWidth = 64;
        shadowTexture.physicalHeight = 64;
        const std::vector<uint8_t> pixels = buildContactShadowBlobPixels(64, 64);
        shadowTexture.textureHandle = createBgraTexture2D(
            64,
            64,
            pixels.data(),
            uint32_t(pixels.size()),
            TextureFilterProfile::Billboard,
            BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);

        if (bgfx::isValid(shadowTexture.textureHandle))
        {
            view.m_billboardTextureHandles.push_back(std::move(shadowTexture));
            view.m_billboardTextureIndexByPalette[0][ContactShadowTextureName] =
                view.m_billboardTextureHandles.size() - 1;
        }
    }

    if (view.m_outdoorDecorationBillboardSet)
    {
        for (OutdoorBitmapTexture &texture : view.m_outdoorDecorationBillboardSet->textures)
        {
            uploadBillboardTexture(view, texture);
#if defined(OPENYAMM_VITA_LOADER)
            std::vector<uint8_t>().swap(texture.pixels);
#endif
        }

        std::vector<OutdoorBitmapTexture>().swap(view.m_outdoorDecorationBillboardSet->textures);
    }

    if (view.m_pOutdoorSceneRuntime != nullptr && view.m_pOutdoorSceneRuntime->localEventProgram())
    {
        queueEventBillboardTextureWarmup(view, *view.m_pOutdoorSceneRuntime->localEventProgram());
    }

    if (view.m_pOutdoorSceneRuntime != nullptr && view.m_pOutdoorSceneRuntime->globalEventProgram())
    {
        queueEventBillboardTextureWarmup(view, *view.m_pOutdoorSceneRuntime->globalEventProgram());
    }

    if (!view.m_outdoorSpriteObjectBillboardSet)
    {
        return;
    }

    for (const OutdoorBitmapTexture &texture : view.m_outdoorSpriteObjectBillboardSet->textures)
    {
        uploadBillboardTexture(view, texture);
    }

    std::vector<OutdoorBitmapTexture>().swap(view.m_outdoorSpriteObjectBillboardSet->textures);

}

void OutdoorBillboardRenderer::preloadPendingLevelSpriteTextures(OutdoorGameView &view)
{
#if defined(OPENYAMM_VITA_LOADER)
    if (!vitaPerformanceSettings().prewarmSprites)
    {
        std::fprintf(stderr, "VITA PREWARM skipped by policy; static map sprites retained, actor frames stream on demand\n");
        return;
    }
    if (!view.m_pAssetFileSystem || !view.m_outdoorActorPreviewBillboardSet) { return; }
    const SpriteFrameTable &table = view.m_outdoorActorPreviewBillboardSet->spriteFrameTable;
    std::map<std::pair<std::string, int16_t>, uint64_t> requests;
    auto add = [&](const std::string &name, int16_t palette)
    {
        if (!name.empty()) { requests.try_emplace({toLowerCopy(name), palette}, 0); }
    };
    auto addAnimation = [&](uint16_t start)
    {
        if (!start) { return; }
        for (uint32_t i = start; i <= UINT16_MAX; ++i)
        {
            const auto *frame = table.getFrame(uint16_t(i), 0);
            if (!frame) { break; }
            for (int octant = 0; octant < 8; ++octant)
            {
                add(SpriteFrameTable::resolveTexture(*frame, octant).textureName, frame->paletteId);
            }
            if (!SpriteFrameTable::hasFlag(frame->flags, SpriteFrameFlag::HasMore)) { break; }
        }
    };
    if (view.m_pOutdoorWorldRuntime)
    {
        for (size_t i = 0; i < view.m_pOutdoorWorldRuntime->mapActorCount(); ++i)
        {
            const auto *actor = view.m_pOutdoorWorldRuntime->mapActorState(i);
            if (!actor) { continue; }
            const size_t action = size_t(actor->animation);
            const uint16_t current = action < actor->actionSpriteFrameIndices.size()
                && actor->actionSpriteFrameIndices[action] ? actor->actionSpriteFrameIndices[action] : actor->spriteFrameIndex;
            addAnimation(current);
        }
    }
    if (view.m_outdoorDecorationBillboardSet)
    {
        for (const auto &decor : view.m_outdoorDecorationBillboardSet->billboards) { addAnimation(decor.spriteId); }
    }
    uint64_t working = 0, needed = 0;
    size_t unknown = 0;
    for (auto &[key, bytes] : requests)
    {
        int width = 0, height = 0;
        if (!readVitaSpriteDimensions(view.m_pAssetFileSystem, view.m_spriteLoadCache, key.first, width, height))
        {
            ++unknown; continue;
        }
        bytes = vitaSpriteBackingBytes(width, height);
        working += bytes;
        const auto *existing = view.findBillboardTexture(key.first, key.second);
        if (!existing || !bgfx::isValid(existing->textureHandle)) { needed += bytes; }
    }
    constexpr uint64_t budget = VitaAnimationBudget::Limit;
    // Existing static decorations are outside the managed animation budget; do not count them twice.
    uint64_t managed = needed;
    for (const auto &texture : view.m_billboardTextureHandles)
    {
        if (texture.vitaManaged && bgfx::isValid(texture.textureHandle)) { managed += texture.vitaBackingBytes; }
    }
    const uint64_t excess = managed > budget ? managed - budget : 0;
    std::fprintf(stderr, "VITA PREWARM map=%s sprites=%u total_bytes=%llu managed_bytes=%llu "
        "budget=%llu excess_bytes=%llu unknown=%u scope=current_actor_actions_and_present_decorations\n",
        view.m_map ? view.m_map->fileName.c_str() : "?", unsigned(requests.size()),
        (unsigned long long)working, (unsigned long long)managed, (unsigned long long)budget,
        (unsigned long long)excess, unsigned(unknown));
    if (excess || unknown)
    {
        std::fprintf(stderr, "VITA PREWARM skipped: entire declared working set must fit; no partial prewarm\n");
        return;
    }
    size_t uploaded = 0;
    for (const auto &[key, bytes] : requests)
    {
        const auto *existing = view.findBillboardTexture(key.first, key.second);
        if (existing && bgfx::isValid(existing->textureHandle)) { continue; }
        // One ready payload at a time. Worker does disk/decode/bleed; main owns GPU operations.
        const uint64_t deadline = SDL_GetTicksNS() + 60000000000ull;
        for (;;)
        {
            ++view.m_vitaTextureFrame;
            view.m_vitaTextureUploadBytes = 0;
            const auto *texture = ensureSpriteBillboardTexture(view, key.first, key.second, "map_prewarm");
            if (texture && bgfx::isValid(texture->textureHandle)) { ++uploaded; break; }
            const auto *pending = view.findBillboardTexture(key.first, key.second);
            if (SDL_GetTicksNS() >= deadline || !pending || !view.m_vitaSpriteStream
                || view.m_vitaSpriteStream->failed(pending->vitaStreamTicket))
            {
                std::fprintf(stderr, "VITA PREWARM failed texture=%s; runtime streaming retained\n", key.first.c_str());
                return;
            }
            SDL_Delay(1);
        }
        // Drain resource commands during loading instead of retaining copies for the whole area.
        bgfx::frame();
    }
    std::fprintf(stderr, "VITA PREWARM complete uploaded=%u sprites=%u\n", unsigned(uploaded), unsigned(requests.size()));
    return;
#endif
    if (view.m_outdoorActorPreviewBillboardSet)
    {
        ActorPreviewBillboardSet &billboardSet = *view.m_outdoorActorPreviewBillboardSet;
        std::vector<OutdoorBitmapTexture>().swap(billboardSet.textures);
    }

    preloadPendingSpriteFrameWarmupsParallel(view);

    // Saved state and on-load events can change actor visuals after map assets are prepared. Warm only the exact
    // texture each final runtime actor currently needs; other frames and viewing angles load when first rendered.
    if (view.m_pOutdoorWorldRuntime == nullptr || !view.m_outdoorActorPreviewBillboardSet)
    {
        view.m_spriteLoadCache.binaryFilesByPath.clear();
        view.m_spriteLoadCache.binaryFilesByPath.rehash(0);
        return;
    }

    const SpriteFrameTable &spriteFrameTable = view.m_outdoorActorPreviewBillboardSet->spriteFrameTable;

    for (size_t actorIndex = 0; actorIndex < view.m_pOutdoorWorldRuntime->mapActorCount(); ++actorIndex)
    {
        const OutdoorWorldRuntime::MapActorState *pActorState =
            view.m_pOutdoorWorldRuntime->mapActorState(actorIndex);

        if (pActorState == nullptr)
        {
            continue;
        }

        uint16_t spriteFrameIndex = pActorState->spriteFrameIndex;
        const size_t animationIndex = static_cast<size_t>(pActorState->animation);

        if (animationIndex < pActorState->actionSpriteFrameIndices.size()
            && pActorState->actionSpriteFrameIndices[animationIndex] != 0)
        {
            spriteFrameIndex = pActorState->actionSpriteFrameIndices[animationIndex];
        }

        const uint32_t frameTimeTicks = static_cast<uint32_t>(std::max(0.0f, pActorState->animationTimeTicks));
        const SpriteFrameEntry *pFrame = spriteFrameTable.getFrame(spriteFrameIndex, frameTimeTicks);

        if (pFrame == nullptr)
        {
            continue;
        }

        const float angleToCamera = std::atan2(
            static_cast<float>(pActorState->y) - view.m_cameraTargetY,
            static_cast<float>(pActorState->x) - view.m_cameraTargetX);
        const float octantAngle = pActorState->yawRadians - angleToCamera + Pi + (Pi / 8.0f);
        const int octant = static_cast<int>(std::floor(octantAngle / (Pi / 4.0f))) & 7;
        const ResolvedSpriteTexture resolvedTexture = SpriteFrameTable::resolveTexture(*pFrame, octant);
        ensureSpriteBillboardTexture(view, resolvedTexture.textureName, pFrame->paletteId, "level_warmup");
    }

    view.m_spriteLoadCache.binaryFilesByPath.clear();
    view.m_spriteLoadCache.binaryFilesByPath.rehash(0);
}

void OutdoorBillboardRenderer::prepareKeyboardInteractionBillboardCache(
    OutdoorGameView &view,
    int viewWidth,
    int viewHeight,
    const float *pViewMatrix,
    const float *pProjectionMatrix,
    const bx::Vec3 &cameraPosition)
{
    VitaPickTiming pickTiming("candidate_metadata");
    view.m_keyboardInteractionBillboardCandidates.clear();

    if (viewWidth <= 0
        || viewHeight <= 0
        || pViewMatrix == nullptr
        || pProjectionMatrix == nullptr)
    {
        return;
    }

    float viewProjectionMatrix[16] = {};
    bx::mtxMul(viewProjectionMatrix, pViewMatrix, pProjectionMatrix);

    const float cosPitch = std::cos(view.m_cameraPitchRadians);
    const bx::Vec3 cameraForward = {
        std::cos(view.m_cameraYawRadians) * cosPitch,
        std::sin(view.m_cameraYawRadians) * cosPitch,
        std::sin(view.m_cameraPitchRadians)
    };
    const bx::Vec3 cameraRight = {pViewMatrix[0], pViewMatrix[4], pViewMatrix[8]};
    const bx::Vec3 cameraUp = {pViewMatrix[1], pViewMatrix[5], pViewMatrix[9]};
    const uint32_t animationTimeTicks = currentAnimationTicks();

    view.m_keyboardInteractionBillboardCandidates.reserve(
        (view.m_outdoorDecorationBillboardSet ? view.m_outdoorDecorationBillboardSet->billboards.size() : 0)
        + (view.m_pOutdoorWorldRuntime != nullptr ? view.m_pOutdoorWorldRuntime->mapActorCount() : 0)
        + (view.m_pOutdoorWorldRuntime != nullptr ? view.m_pOutdoorWorldRuntime->worldItemCount() : 0)
        + (view.m_outdoorSpriteObjectBillboardSet ? view.m_outdoorSpriteObjectBillboardSet->billboards.size() : 0));

    const auto appendCandidate =
        [&view](OutdoorGameView::KeyboardInteractionBillboardCandidate &&candidate)
        {
            if (candidate.samplePointCount == 0)
            {
                return;
            }

            view.m_keyboardInteractionBillboardCandidates.push_back(std::move(candidate));
        };

    if (view.m_outdoorDecorationBillboardSet)
    {
        for (size_t decorationIndex = 0; decorationIndex < view.m_outdoorDecorationBillboardSet->billboards.size(); ++decorationIndex)
        {
            const DecorationBillboard &billboard = view.m_outdoorDecorationBillboardSet->billboards[decorationIndex];

            if (OutdoorInteractionController::isInteractiveDecorationHidden(view, billboard.entityIndex))
            {
                continue;
            }

            bool hidden = false;
            const uint16_t spriteId =
                OutdoorInteractionController::resolveDecorationBillboardSpriteId(view, billboard, hidden);

            if (hidden || spriteId == 0)
            {
                continue;
            }

            std::optional<uint16_t> directEventId;

            if (view.m_pOutdoorMapData && billboard.entityIndex < view.m_pOutdoorMapData->entities.size())
            {
                const OutdoorEntity &entity = view.m_pOutdoorMapData->entities[billboard.entityIndex];
                if (entity.eventIdSecondary != 0)
                {
                    directEventId = entity.eventIdSecondary;
                }
            }

            if (!directEventId.has_value()
                && !OutdoorInteractionController::resolveInteractiveDecorationEventId(view, billboard.entityIndex).has_value())
            {
                continue;
            }

            const float deltaX = static_cast<float>(billboard.x) - cameraPosition.x;
            const float deltaY = static_cast<float>(billboard.y) - cameraPosition.y;
            const float deltaZ = static_cast<float>(billboard.z) - cameraPosition.z;
            const float distanceSquared = deltaX * deltaX + deltaY * deltaY + deltaZ * deltaZ;
            const float cameraDepth = deltaX * cameraForward.x + deltaY * cameraForward.y + deltaZ * cameraForward.z;

            if (distanceSquared > view.m_viewDistanceCache.decorationBillboardDistanceSquared
                || cameraDepth <= BillboardNearDepth)
            {
                continue;
            }

            const uint32_t animationOffsetTicks =
                animationTimeTicks + static_cast<uint32_t>(std::abs(billboard.x + billboard.y));
            const SpriteFrameEntry *pFrame =
                view.m_outdoorDecorationBillboardSet->spriteFrameTable.getFrame(spriteId, animationOffsetTicks);

            if (pFrame == nullptr)
            {
                continue;
            }

            const float facingRadians = static_cast<float>(billboard.facing) * Pi / 180.0f;
            const float angleToCamera = std::atan2(
                static_cast<float>(billboard.y) - cameraPosition.y,
                static_cast<float>(billboard.x) - cameraPosition.x);
            const float octantAngle = facingRadians - angleToCamera + Pi + (Pi / 8.0f);
            const int octant = static_cast<int>(std::floor(octantAngle / (Pi / 4.0f))) & 7;
            const ResolvedSpriteTexture resolvedTexture = SpriteFrameTable::resolveTexture(*pFrame, octant);
            const OutdoorGameView::BillboardTextureHandle *pTexture =
                (
#if defined(OPENYAMM_VITA_LOADER)
                    view.findBillboardTexture(resolvedTexture.textureName, pFrame->paletteId)
#else
                    ensureSpriteBillboardTexture(view, resolvedTexture.textureName, pFrame->paletteId)
#endif
                );

            if (pTexture == nullptr
#if !defined(OPENYAMM_VITA_LOADER)
                || !bgfx::isValid(pTexture->textureHandle)
#endif
                || pTexture->width <= 0 || pTexture->height <= 0)
            {
                continue;
            }

            const float spriteScale = std::max(pFrame->scale, 0.01f);
            const float worldWidth = static_cast<float>(pTexture->width) * spriteScale;
            const float worldHeight = static_cast<float>(pTexture->height) * spriteScale;
            const float halfWidth = worldWidth * 0.5f;
            const bx::Vec3 center = bottomAnchoredBillboardCenter(
                static_cast<float>(billboard.x),
                static_cast<float>(billboard.y),
                static_cast<float>(billboard.z),
                cameraUp,
                worldHeight);
            const bx::Vec3 right = {
                cameraRight.x * halfWidth,
                cameraRight.y * halfWidth,
                cameraRight.z * halfWidth
            };
            const bx::Vec3 up = {
                cameraUp.x * worldHeight * 0.5f,
                cameraUp.y * worldHeight * 0.5f,
                cameraUp.z * worldHeight * 0.5f
            };
            ProjectedBillboardRect rect = {};

            if (!projectVisibleBillboardRect(
                    center,
                    right,
                    up,
                    viewWidth,
                    viewHeight,
                    viewProjectionMatrix,
                    rect))
            {
                continue;
            }

            OutdoorGameView::KeyboardInteractionBillboardCandidate candidate = {};
            candidate.inspectHit.hasHit = true;
            candidate.inspectHit.kind = "decoration";
            candidate.inspectHit.bModelIndex = decorationIndex;
            candidate.inspectHit.name = billboard.name;
            candidate.inspectHit.decorationId = billboard.decorationId;
            candidate.inspectHit.distance = cameraDepth;

            if (view.m_pOutdoorMapData && billboard.entityIndex < view.m_pOutdoorMapData->entities.size())
            {
                const OutdoorEntity &entity = view.m_pOutdoorMapData->entities[billboard.entityIndex];
                candidate.inspectHit.eventIdPrimary = entity.eventIdPrimary;
                candidate.inspectHit.eventIdSecondary = entity.eventIdSecondary;
            }

            candidate.cameraDepth = cameraDepth;
            fillKeyboardInteractionSamplePoints(rect, candidate);
            appendCandidate(std::move(candidate));
        }
    }

    if (view.m_pOutdoorWorldRuntime != nullptr && view.m_outdoorActorPreviewBillboardSet)
    {
        struct ActorPreviewLookup
        {
            size_t billboardIndex = static_cast<size_t>(-1);
            const ActorPreviewBillboard *pBillboard = nullptr;
        };

        std::unordered_map<size_t, ActorPreviewLookup> previewByRuntimeActorIndex;
        previewByRuntimeActorIndex.reserve(view.m_outdoorActorPreviewBillboardSet->billboards.size());

        for (size_t billboardIndex = 0; billboardIndex < view.m_outdoorActorPreviewBillboardSet->billboards.size(); ++billboardIndex)
        {
            const ActorPreviewBillboard &billboard = view.m_outdoorActorPreviewBillboardSet->billboards[billboardIndex];

            if (billboard.runtimeActorIndex == static_cast<size_t>(-1))
            {
                continue;
            }

            previewByRuntimeActorIndex.try_emplace(
                billboard.runtimeActorIndex,
                ActorPreviewLookup {billboardIndex, &billboard});
        }

        for (size_t actorIndex = 0; actorIndex < view.m_pOutdoorWorldRuntime->mapActorCount(); ++actorIndex)
        {
            const OutdoorWorldRuntime::MapActorState *pActorState = view.m_pOutdoorWorldRuntime->mapActorState(actorIndex);

            if (pActorState == nullptr || pActorState->isInvisible)
            {
                continue;
            }

            uint16_t spriteFrameIndex = pActorState->spriteFrameIndex;
            const size_t animationIndex = static_cast<size_t>(pActorState->animation);

            if (animationIndex < pActorState->actionSpriteFrameIndices.size()
                && pActorState->actionSpriteFrameIndices[animationIndex] != 0)
            {
                spriteFrameIndex = pActorState->actionSpriteFrameIndices[animationIndex];
            }

            const uint32_t frameTimeTicks = pActorState->useStaticSpriteFrame
                ? 0u
                : static_cast<uint32_t>(std::max(0.0f, pActorState->animationTimeTicks));
            const SpriteFrameEntry *pFrame =
                view.m_outdoorActorPreviewBillboardSet->spriteFrameTable.getFrame(spriteFrameIndex, frameTimeTicks);

            if (pFrame == nullptr)
            {
                continue;
            }

            const float deltaX = static_cast<float>(pActorState->x) - cameraPosition.x;
            const float deltaY = static_cast<float>(pActorState->y) - cameraPosition.y;
            const float deltaZ = static_cast<float>(pActorState->z) - cameraPosition.z;
            const float distanceSquared = deltaX * deltaX + deltaY * deltaY + deltaZ * deltaZ;
            const float cameraDepth = deltaX * cameraForward.x + deltaY * cameraForward.y + deltaZ * cameraForward.z;

            if (distanceSquared > view.m_viewDistanceCache.actorBillboardDistanceSquared
                || cameraDepth <= BillboardNearDepth)
            {
                continue;
            }

            const float angleToCamera = std::atan2(
                static_cast<float>(pActorState->y) - cameraPosition.y,
                static_cast<float>(pActorState->x) - cameraPosition.x);
            const float octantAngle = pActorState->yawRadians - angleToCamera + Pi + (Pi / 8.0f);
            const int octant = static_cast<int>(std::floor(octantAngle / (Pi / 4.0f))) & 7;
            const ResolvedSpriteTexture resolvedTexture = SpriteFrameTable::resolveTexture(*pFrame, octant);
            const OutdoorGameView::BillboardTextureHandle *pTexture =
                (
#if defined(OPENYAMM_VITA_LOADER)
                    view.findBillboardTexture(resolvedTexture.textureName, pFrame->paletteId)
#else
                    ensureSpriteBillboardTexture(view, resolvedTexture.textureName, pFrame->paletteId)
#endif
                );

            if (pTexture == nullptr
#if !defined(OPENYAMM_VITA_LOADER)
                || !bgfx::isValid(pTexture->textureHandle)
#endif
                || pTexture->width <= 0 || pTexture->height <= 0)
            {
                continue;
            }

            const float spriteScale = std::max(pFrame->scale, 0.01f);
            const float worldWidth = static_cast<float>(pTexture->width) * spriteScale;
            const float worldHeight = static_cast<float>(pTexture->height) * spriteScale;
            const float halfWidth = worldWidth * 0.5f;
            const bx::Vec3 center = bottomAnchoredBillboardCenter(
                static_cast<float>(pActorState->x),
                static_cast<float>(pActorState->y),
                static_cast<float>(pActorState->z),
                cameraUp,
                worldHeight);
            const bx::Vec3 right = {
                cameraRight.x * halfWidth,
                cameraRight.y * halfWidth,
                cameraRight.z * halfWidth
            };
            const bx::Vec3 up = {
                cameraUp.x * worldHeight * 0.5f,
                cameraUp.y * worldHeight * 0.5f,
                cameraUp.z * worldHeight * 0.5f
            };
            ProjectedBillboardRect rect = {};

            if (!projectVisibleBillboardRect(
                    center,
                    right,
                    up,
                    viewWidth,
                    viewHeight,
                    viewProjectionMatrix,
                    rect))
            {
                continue;
            }

            OutdoorGameView::KeyboardInteractionBillboardCandidate candidate = {};
            candidate.inspectHit.hasHit = true;
            candidate.inspectHit.kind = "actor";
            candidate.inspectHit.isFriendly = !pActorState->hostileToParty;
            candidate.inspectHit.runtimeActorIndex = actorIndex;
            const auto previewIterator = previewByRuntimeActorIndex.find(actorIndex);
            if (previewIterator != previewByRuntimeActorIndex.end())
            {
                candidate.inspectHit.bModelIndex = previewIterator->second.billboardIndex;
                candidate.inspectHit.name = !previewIterator->second.pBillboard->actorName.empty()
                    ? previewIterator->second.pBillboard->actorName
                    : pActorState->displayName;
                candidate.inspectHit.npcId = previewIterator->second.pBillboard->npcId;
                candidate.inspectHit.actorGroup = previewIterator->second.pBillboard->group;
            }
            else
            {
                candidate.inspectHit.bModelIndex = actorIndex;
                candidate.inspectHit.name = pActorState->displayName;
                candidate.inspectHit.npcId = 0;
                candidate.inspectHit.actorGroup = pActorState->group;
            }
            candidate.inspectHit.distance = cameraDepth;
            candidate.cameraDepth = cameraDepth;
            fillKeyboardInteractionSamplePoints(rect, candidate);
            appendCandidate(std::move(candidate));
        }
    }

    if (view.m_pOutdoorWorldRuntime != nullptr)
    {
        const SpriteFrameTable *pSpriteFrameTable = nullptr;

        if (view.m_outdoorSpriteObjectBillboardSet)
        {
            pSpriteFrameTable = &view.m_outdoorSpriteObjectBillboardSet->spriteFrameTable;
        }
        else if (view.m_outdoorActorPreviewBillboardSet)
        {
            pSpriteFrameTable = &view.m_outdoorActorPreviewBillboardSet->spriteFrameTable;
        }

        if (pSpriteFrameTable != nullptr)
        {
            for (size_t worldItemIndex = 0; worldItemIndex < view.m_pOutdoorWorldRuntime->worldItemCount(); ++worldItemIndex)
            {
                const OutdoorWorldRuntime::WorldItemState *pWorldItem =
                    view.m_pOutdoorWorldRuntime->worldItemState(worldItemIndex);

                if (pWorldItem == nullptr)
                {
                    continue;
                }

                uint16_t spriteFrameIndex = pWorldItem->objectSpriteFrameIndex;

                if (spriteFrameIndex == 0 && !pWorldItem->objectSpriteName.empty())
                {
                    const std::optional<uint16_t> spriteFrameIndexByName =
                        pSpriteFrameTable->findFrameIndexBySpriteName(pWorldItem->objectSpriteName);

                    if (spriteFrameIndexByName)
                    {
                        spriteFrameIndex = *spriteFrameIndexByName;
                    }
                }

                if (spriteFrameIndex == 0)
                {
                    spriteFrameIndex = pWorldItem->objectSpriteId;
                }

                if (spriteFrameIndex == 0)
                {
                    continue;
                }

                const SpriteFrameEntry *pFrame =
                    pSpriteFrameTable->getFrame(spriteFrameIndex, pWorldItem->timeSinceCreatedTicks);

                if (pFrame == nullptr)
                {
                    continue;
                }

                const ResolvedSpriteTexture resolvedTexture = SpriteFrameTable::resolveTexture(*pFrame, 0);
                const OutdoorGameView::BillboardTextureHandle *pTexture =
                    (
#if defined(OPENYAMM_VITA_LOADER)
                    view.findBillboardTexture(resolvedTexture.textureName, pFrame->paletteId)
#else
                    ensureSpriteBillboardTexture(view, resolvedTexture.textureName, pFrame->paletteId)
#endif
                );

                if (pTexture == nullptr
#if !defined(OPENYAMM_VITA_LOADER)
                || !bgfx::isValid(pTexture->textureHandle)
#endif
                || pTexture->width <= 0 || pTexture->height <= 0)
                {
                    continue;
                }

                const float deltaX = pWorldItem->x - cameraPosition.x;
                const float deltaY = pWorldItem->y - cameraPosition.y;
                const float deltaZ = pWorldItem->z - cameraPosition.z;
                const float cameraDepth =
                    deltaX * cameraForward.x + deltaY * cameraForward.y + deltaZ * cameraForward.z;

                if (cameraDepth <= BillboardNearDepth)
                {
                    continue;
                }

                const float spriteScale = std::max(pFrame->scale, 0.01f);
                const float worldWidth = static_cast<float>(pTexture->width) * spriteScale;
                const float worldHeight = static_cast<float>(pTexture->height) * spriteScale;
                const float halfWidth = worldWidth * 0.5f;
                const bx::Vec3 center = bottomAnchoredBillboardCenter(
                    pWorldItem->x,
                    pWorldItem->y,
                    pWorldItem->z,
                    cameraUp,
                    worldHeight);
                const bx::Vec3 right = {
                    cameraRight.x * halfWidth,
                    cameraRight.y * halfWidth,
                    cameraRight.z * halfWidth
                };
                const bx::Vec3 up = {
                    cameraUp.x * worldHeight * 0.5f,
                    cameraUp.y * worldHeight * 0.5f,
                    cameraUp.z * worldHeight * 0.5f
                };
                ProjectedBillboardRect rect = {};

                if (!projectVisibleBillboardRect(
                        center,
                        right,
                        up,
                        viewWidth,
                        viewHeight,
                        viewProjectionMatrix,
                        rect))
                {
                    continue;
                }

                OutdoorGameView::KeyboardInteractionBillboardCandidate candidate = {};
                candidate.inspectHit.hasHit = true;
                candidate.inspectHit.kind = "world_item";
                candidate.inspectHit.bModelIndex = worldItemIndex;
                candidate.inspectHit.name = pWorldItem->objectName;
                candidate.inspectHit.objectDescriptionId = pWorldItem->objectDescriptionId;
                candidate.inspectHit.objectSpriteId = pWorldItem->objectSpriteId;
                candidate.inspectHit.attributes = pWorldItem->attributes;
                candidate.inspectHit.distance = cameraDepth;
                candidate.cameraDepth = cameraDepth;
                fillKeyboardInteractionSamplePoints(rect, candidate);
                appendCandidate(std::move(candidate));
            }
        }
    }

    std::sort(
        view.m_keyboardInteractionBillboardCandidates.begin(),
        view.m_keyboardInteractionBillboardCandidates.end(),
        [](const OutdoorGameView::KeyboardInteractionBillboardCandidate &left,
           const OutdoorGameView::KeyboardInteractionBillboardCandidate &right)
        {
            return left.cameraDepth < right.cameraDepth;
        });
}

void OutdoorBillboardRenderer::queueEventBillboardTextureWarmup(
    OutdoorGameView &view,
    const ScriptedEventProgram &eventProgram)
{
    const SpellTable &spellTable = view.data().spellTable();
    const ObjectTable &objectTable = view.data().objectTable();

    auto queueSpellObjectSpriteFrame =
        [&view, &objectTable](int objectId)
        {
            if (objectId <= 0)
            {
                return;
            }

            const std::optional<uint16_t> descriptionId =
                objectTable.findDescriptionIdByObjectId(static_cast<int16_t>(objectId));

            if (!descriptionId)
            {
                return;
            }

            const ObjectEntry *pObjectEntry = objectTable.get(*descriptionId);

            if (pObjectEntry == nullptr)
            {
                return;
            }

            if (pObjectEntry->spriteId != 0)
            {
                view.queueSpriteFrameWarmup(pObjectEntry->spriteId);
                return;
            }

            if (pObjectEntry->spriteName.empty())
            {
                return;
            }

            const SpriteFrameTable *pSpriteFrameTable = nullptr;

            if (view.m_outdoorSpriteObjectBillboardSet)
            {
                pSpriteFrameTable = &view.m_outdoorSpriteObjectBillboardSet->spriteFrameTable;
            }
            else if (view.m_outdoorActorPreviewBillboardSet)
            {
                pSpriteFrameTable = &view.m_outdoorActorPreviewBillboardSet->spriteFrameTable;
            }

            if (pSpriteFrameTable == nullptr)
            {
                return;
            }

            const std::optional<uint16_t> spriteFrameIndex =
                pSpriteFrameTable->findFrameIndexBySpriteName(pObjectEntry->spriteName);

            if (spriteFrameIndex)
            {
                view.queueSpriteFrameWarmup(*spriteFrameIndex);
            }
        };

    for (uint32_t spellId : eventProgram.castSpellIds())
    {
        const EventProjectileSpellDefinition *pEventProjectile = eventProjectileSpellDefinition(spellId);

        if (pEventProjectile != nullptr)
        {
            queueSpellObjectSpriteFrame(pEventProjectile->objectId);
            queueSpellObjectSpriteFrame(pEventProjectile->impactObjectId);
            continue;
        }

        const SpellEntry *pSpellEntry = spellTable.findById(static_cast<int>(spellId));

        if (pSpellEntry == nullptr)
        {
            continue;
        }

        queueSpellObjectSpriteFrame(pSpellEntry->displayObjectId);
        queueSpellObjectSpriteFrame(pSpellEntry->impactDisplayObjectId);
    }

    const SpriteFrameTable *pEventSpriteFrameTable = nullptr;

    if (view.m_outdoorDecorationBillboardSet)
    {
        pEventSpriteFrameTable = &view.m_outdoorDecorationBillboardSet->spriteFrameTable;
    }
    else if (view.m_outdoorSpriteObjectBillboardSet)
    {
        pEventSpriteFrameTable = &view.m_outdoorSpriteObjectBillboardSet->spriteFrameTable;
    }
    else if (view.m_outdoorActorPreviewBillboardSet)
    {
        pEventSpriteFrameTable = &view.m_outdoorActorPreviewBillboardSet->spriteFrameTable;
    }

    if (pEventSpriteFrameTable == nullptr)
    {
        return;
    }

    for (const std::string &spriteName : eventProgram.spriteNames())
    {
        if (spriteName.empty() || spriteName == "0")
        {
            continue;
        }

        const std::optional<uint16_t> spriteFrameIndex =
            pEventSpriteFrameTable->findFrameIndexBySpriteName(spriteName);

        if (spriteFrameIndex)
        {
            view.queueSpriteFrameWarmup(*spriteFrameIndex);
        }
    }
}

void OutdoorBillboardRenderer::preloadPendingSpriteFrameWarmupsParallel(OutdoorGameView &view)
{
    if (view.m_pendingSpriteFrameWarmups.empty() || view.m_pAssetFileSystem == nullptr)
    {
        return;
    }

    const Engine::AssetScaleTier assetScaleTier = view.m_pAssetFileSystem->getAssetScaleTier();
    const SpriteFrameTable *pSpriteFrameTable = nullptr;

    if (view.m_outdoorActorPreviewBillboardSet)
    {
        pSpriteFrameTable = &view.m_outdoorActorPreviewBillboardSet->spriteFrameTable;
    }
    else if (view.m_outdoorSpriteObjectBillboardSet)
    {
        pSpriteFrameTable = &view.m_outdoorSpriteObjectBillboardSet->spriteFrameTable;
    }

    if (pSpriteFrameTable == nullptr)
    {
        return;
    }

    std::vector<SpriteTexturePreloadRequest> preloadRequests;
    std::unordered_map<int16_t, std::unordered_map<std::string, size_t>> requestIndexByPaletteAndName;
    std::unordered_map<int16_t, std::optional<std::array<uint8_t, 256 * 3>>> paletteCache;
    const uint64_t preloadStartTickCount = SDL_GetTicksNS();

    auto queueTextureRequest =
        [&view, &preloadRequests, &requestIndexByPaletteAndName, &paletteCache](
            const std::string &textureName,
            int16_t paletteId)
        {
            if (textureName.empty() || view.findBillboardTexture(textureName, paletteId) != nullptr)
            {
                return;
            }

            const std::string normalizedTextureName = toLowerCopy(textureName);

            if (requestIndexByPaletteAndName[paletteId].contains(normalizedTextureName))
            {
                return;
            }

            std::optional<std::string> spritePath =
                view.findCachedAssetPath("Data/sprites", normalizedTextureName + ".png");

            if (!spritePath)
            {
                spritePath = view.findCachedAssetPath("Data/sprites", normalizedTextureName + ".bmp");
            }

            if (!spritePath)
            {
                if (DebugSpritePreloadLogging)
                {
                    std::cout << "Preload sprite skipped texture=\"" << normalizedTextureName
                              << "\" palette=" << paletteId
                              << " reason=missing_bitmap\n";
                }
                return;
            }

            std::optional<std::vector<uint8_t>> bitmapBytes =
                view.m_pAssetFileSystem->readBinaryFile(*spritePath);

            if (!bitmapBytes || bitmapBytes->empty())
            {
                if (DebugSpritePreloadLogging)
                {
                    std::cout << "Preload sprite skipped texture=\"" << normalizedTextureName
                              << "\" palette=" << paletteId
                              << " reason=empty_bitmap\n";
                }
                return;
            }

            SpriteTexturePreloadRequest request = {};
            request.textureName = normalizedTextureName;
            request.virtualPath = *spritePath;
            request.paletteId = paletteId;
            request.bitmapBytes = std::move(*bitmapBytes);

            if (!paletteCache.contains(paletteId))
            {
                paletteCache[paletteId] = view.loadCachedActPalette(paletteId);
            }

            request.overridePalette = paletteCache[paletteId];
            requestIndexByPaletteAndName[paletteId][normalizedTextureName] = preloadRequests.size();
            preloadRequests.push_back(std::move(request));
        };

    for (uint16_t spriteFrameIndex : view.m_pendingSpriteFrameWarmups)
    {
        if (spriteFrameIndex == 0)
        {
            continue;
        }

        size_t frameIndex = spriteFrameIndex;

        while (frameIndex <= std::numeric_limits<uint16_t>::max())
        {
            const SpriteFrameEntry *pFrame = pSpriteFrameTable->getFrame(static_cast<uint16_t>(frameIndex), 0);

            if (pFrame == nullptr)
            {
                break;
            }

            for (int octant = 0; octant < 8; ++octant)
            {
                const ResolvedSpriteTexture resolvedTexture = SpriteFrameTable::resolveTexture(*pFrame, octant);
                queueTextureRequest(resolvedTexture.textureName, pFrame->paletteId);
            }

            if (!SpriteFrameTable::hasFlag(pFrame->flags, SpriteFrameFlag::HasMore))
            {
                break;
            }

            ++frameIndex;
        }
    }

    if (preloadRequests.empty())
    {
        view.m_pendingSpriteFrameWarmups.clear();
        view.m_nextPendingSpriteFrameWarmupIndex = 0;
        return;
    }

    if (DebugSpritePreloadLogging)
    {
        const size_t workerCount = spritePreloadDecodeWorkerCount(preloadRequests.size());
        std::cout << "Preloading sprite textures: requests=" << preloadRequests.size()
                  << " workers=" << workerCount
                  << '\n';
    }

    const size_t workerCount = spritePreloadDecodeWorkerCount(preloadRequests.size());
    std::vector<std::future<std::vector<DecodedSpriteTexture>>> futures;
    futures.reserve(workerCount);

    for (size_t workerIndex = 0; workerIndex < workerCount; ++workerIndex)
    {
        futures.push_back(std::async(
            std::launch::async,
            [workerIndex, workerCount, &preloadRequests]()
            {
                std::vector<DecodedSpriteTexture> decodedTextures;

                for (size_t requestIndex = workerIndex; requestIndex < preloadRequests.size(); requestIndex += workerCount)
                {
                    const SpriteTexturePreloadRequest &request = preloadRequests[requestIndex];
                    int textureWidth = 0;
                    int textureHeight = 0;
                    std::optional<std::vector<uint8_t>> pixels = loadSpriteBitmapPixelsBgra(
                        request.bitmapBytes,
                        request.virtualPath,
                        request.overridePalette,
                        textureWidth,
                        textureHeight);

                    if (!pixels || textureWidth <= 0 || textureHeight <= 0)
                    {
                        continue;
                    }

                    DecodedSpriteTexture decodedTexture = {};
                    decodedTexture.textureName = request.textureName;
                    decodedTexture.paletteId = request.paletteId;
                    decodedTexture.width = textureWidth;
                    decodedTexture.height = textureHeight;
                    decodedTexture.pixels = std::move(*pixels);
                    prepareBgraTexturePixelsForUploadInPlace(
                        static_cast<uint16_t>(textureWidth),
                        static_cast<uint16_t>(textureHeight),
                        decodedTexture.pixels,
                        TextureFilterProfile::Billboard);
                    decodedTextures.push_back(std::move(decodedTexture));
                }

                return decodedTextures;
            }));
    }

    std::vector<DecodedSpriteTexture> decodedTextures;

    for (std::future<std::vector<DecodedSpriteTexture>> &future : futures)
    {
        std::vector<DecodedSpriteTexture> workerTextures = future.get();
        decodedTextures.insert(
            decodedTextures.end(),
            std::make_move_iterator(workerTextures.begin()),
            std::make_move_iterator(workerTextures.end()));
    }

    size_t uploadedCount = 0;

    for (DecodedSpriteTexture &decodedTexture : decodedTextures)
    {
        if (view.findBillboardTexture(decodedTexture.textureName, decodedTexture.paletteId) != nullptr)
        {
            continue;
        }

        OutdoorGameView::BillboardTextureHandle billboardTexture = {};
        billboardTexture.textureName = decodedTexture.textureName;
        billboardTexture.paletteId = decodedTexture.paletteId;
        billboardTexture.width = Engine::scalePhysicalPixelsToLogical(decodedTexture.width, assetScaleTier);
        billboardTexture.height = Engine::scalePhysicalPixelsToLogical(decodedTexture.height, assetScaleTier);
        billboardTexture.physicalWidth = decodedTexture.width;
        billboardTexture.physicalHeight = decodedTexture.height;
        billboardTexture.opacityMask.assignFromBgra(
            decodedTexture.pixels,
            decodedTexture.width,
            decodedTexture.height);
        billboardTexture.textureHandle = createBgraTexture2D(
            uint16_t(decodedTexture.width),
            uint16_t(decodedTexture.height),
            decodedTexture.pixels.data(),
            uint32_t(decodedTexture.pixels.size()),
            TextureFilterProfile::Billboard,
            BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP,
            BgraTexturePixelPreparation::AlreadyPrepared);

        if (!bgfx::isValid(billboardTexture.textureHandle))
        {
            if (DebugSpritePreloadLogging)
            {
                std::cout << "Preload sprite failed texture=\"" << decodedTexture.textureName
                          << "\" palette=" << decodedTexture.paletteId
                          << " reason=create_texture\n";
            }
            continue;
        }

        if (DebugSpritePreloadLogging)
        {
            std::cout << "Preloaded sprite texture=\"" << decodedTexture.textureName
                      << "\" palette=" << decodedTexture.paletteId
                      << " size=" << decodedTexture.width << "x" << decodedTexture.height
                      << '\n';
        }

        view.m_billboardTextureHandles.push_back(std::move(billboardTexture));
        view.m_billboardTextureIndexByPalette[decodedTexture.paletteId][view.m_billboardTextureHandles.back().textureName] =
            view.m_billboardTextureHandles.size() - 1;
        ++uploadedCount;
    }

    const uint64_t preloadElapsedNanoseconds = SDL_GetTicksNS() - preloadStartTickCount;

    if (DebugSpritePreloadLogging)
    {
        std::cout << "Sprite preload complete uploaded=" << uploadedCount
                  << " elapsed_ms=" << static_cast<double>(preloadElapsedNanoseconds) / 1000000.0
                  << '\n';
    }

    view.m_pendingSpriteFrameWarmups.clear();
    view.m_nextPendingSpriteFrameWarmupIndex = 0;
}

void OutdoorBillboardRenderer::processPendingSpriteFrameWarmups(OutdoorGameView &view, size_t maxSpriteFrames)
{
    if (maxSpriteFrames == 0)
    {
        return;
    }

    const SpriteFrameTable *pSpriteFrameTable = nullptr;

    if (view.m_outdoorActorPreviewBillboardSet)
    {
        pSpriteFrameTable = &view.m_outdoorActorPreviewBillboardSet->spriteFrameTable;
    }
    else if (view.m_outdoorSpriteObjectBillboardSet)
    {
        pSpriteFrameTable = &view.m_outdoorSpriteObjectBillboardSet->spriteFrameTable;
    }

    if (pSpriteFrameTable == nullptr)
    {
        return;
    }

    size_t processedCount = 0;

    while (processedCount < maxSpriteFrames
        && view.m_nextPendingSpriteFrameWarmupIndex < view.m_pendingSpriteFrameWarmups.size())
    {
        view.preloadSpriteFrameTextures(
            *pSpriteFrameTable,
            view.m_pendingSpriteFrameWarmups[view.m_nextPendingSpriteFrameWarmupIndex]);
        ++view.m_nextPendingSpriteFrameWarmupIndex;
        ++processedCount;
    }

    if (view.m_nextPendingSpriteFrameWarmupIndex >= view.m_pendingSpriteFrameWarmups.size())
    {
        view.m_pendingSpriteFrameWarmups.clear();
        view.m_nextPendingSpriteFrameWarmupIndex = 0;
    }
}

const OutdoorGameView::BillboardTextureHandle *OutdoorBillboardRenderer::ensureSpriteBillboardTexture(
    OutdoorGameView &view,
    const std::string &textureName,
    int16_t paletteId,
    const char *pLoadPhase)
{
    const OutdoorGameView::BillboardTextureHandle *pExistingTexture = view.findBillboardTexture(textureName, paletteId);

    if (pExistingTexture != nullptr && bgfx::isValid(pExistingTexture->textureHandle))
    {
#if defined(OPENYAMM_VITA_LOADER)
        if (view.m_vitaSpriteStream) { view.m_vitaSpriteStream->touch(
            pExistingTexture->vitaStreamTicket, view.m_vitaTextureFrame); }
#endif

        return pExistingTexture;
    }

#if defined(OPENYAMM_VITA_LOADER)
    // Retain dimensions even when admission is deferred; never decode merely to discover allocation size.
    if (pExistingTexture == nullptr)
    {
        OutdoorGameView::BillboardTextureHandle pending = {};
        pending.textureName = toLowerCopy(textureName);
        pending.paletteId = paletteId;
        pending.vitaManaged = true;
        view.m_billboardTextureHandles.push_back(std::move(pending));
        auto &metadata = view.m_billboardTextureHandles.back();
        view.m_billboardTextureIndexByPalette[paletteId][metadata.textureName] = view.m_billboardTextureHandles.size() - 1;
        pExistingTexture = &metadata;
    }
    auto &metadata = const_cast<OutdoorGameView::BillboardTextureHandle &>(*pExistingTexture);
    OY_VIS(++Engine::VitaVisibility::counts.spriteMisses);
    if (metadata.vitaAttemptFrame == view.m_vitaTextureFrame)
    {
        ++view.m_vitaTextureDeduplicated;
        return nullptr;
    }
    metadata.vitaAttemptFrame = view.m_vitaTextureFrame;
    if (!view.m_pAssetFileSystem) { return nullptr; }
    if (!view.m_vitaSpriteStream)
    {
        view.m_vitaSpriteStream = std::make_unique<VitaSpriteStream>(*view.m_pAssetFileSystem, view.m_map ? view.m_map->worldId : std::string());
    }
    Engine::VitaProfile::Scope streamScope(Engine::VitaProfile::StreamPoll);
    const VitaSpriteImage *prepared = view.m_vitaSpriteStream->request(
        metadata.vitaStreamTicket, textureName, paletteId, view.m_vitaTextureFrame);
    if (!prepared) { OY_VIS(++Engine::VitaVisibility::counts.spritePending); return nullptr; }
    metadata.physicalWidth = prepared->width; metadata.physicalHeight = prepared->height;
    metadata.width = Engine::scalePhysicalPixelsToLogical(prepared->sourceWidth, view.m_pAssetFileSystem->getAssetScaleTier());
    metadata.height = Engine::scalePhysicalPixelsToLogical(prepared->sourceHeight, view.m_pAssetFileSystem->getAssetScaleTier());
    metadata.vitaBackingBytes = std::max<uint64_t>(prepared->bc3 ? 16 : 4, vitaSpriteBackingBytes(prepared->width, prepared->height) / (prepared->bc3 ? 4 : 1));
    if (!view.admitVitaAnimationTexture(metadata.vitaBackingBytes)) { return nullptr; }
#endif
    if (view.m_pAssetFileSystem == nullptr)
    {
        return nullptr;
    }

    const std::string normalizedTextureName = toLowerCopy(textureName);
    const std::string warningKey = std::to_string(paletteId) + ":" + normalizedTextureName;
#if defined(OPENYAMM_VITA_CLEAN) || defined(OPENYAMM_VITA_LOADER)
    constexpr bool logCacheMiss = false;
#else
    const bool logCacheMiss =
        pLoadPhase != nullptr && view.m_runtimeBillboardLoadWarningKeys.insert(warningKey).second;
#endif
    const uint64_t loadBeginTickNanoseconds = logCacheMiss ? SDL_GetTicksNS() : 0;
    const auto logLoadResult = [&](const char *pResult, int width, int height)
    {
        if (!logCacheMiss)
        {
            return;
        }

        std::cerr << "[AssetLoadWarning] kind=sprite phase=" << pLoadPhase << " scene=outdoor"
                  << " map=\"" << (view.m_map ? view.m_map->fileName : std::string()) << "\""
                  << " texture=\"" << normalizedTextureName << "\""
                  << " palette=" << paletteId
                  << " result=" << pResult
                  << " load_us=" << (SDL_GetTicksNS() - loadBeginTickNanoseconds) / 1000
                  << " size=" << width << 'x' << height
                  << '\n';
    };

#if defined(OPENYAMM_VITA_LOADER)
    const int textureWidth = prepared->width, textureHeight = prepared->height;
    const std::vector<uint8_t> *pixels = &prepared->pixels;
#else
    int textureWidth = 0;
    int textureHeight = 0;
    const std::optional<std::vector<uint8_t>> pixels =
        view.loadSpriteBitmapPixelsBgraCached(textureName, paletteId, textureWidth, textureHeight);

#endif

    if (!pixels || textureWidth <= 0 || textureHeight <= 0)
    {
        logLoadResult("failed", textureWidth, textureHeight);
        return nullptr;
    }

    OutdoorGameView::BillboardTextureHandle billboardTexture = {};
    billboardTexture.textureName = toLowerCopy(textureName);
    billboardTexture.paletteId = paletteId;
    billboardTexture.width = Engine::scalePhysicalPixelsToLogical(
#if defined(OPENYAMM_VITA_LOADER)
        prepared->sourceWidth,
#else
        textureWidth,
#endif
        view.m_pAssetFileSystem->getAssetScaleTier());
    billboardTexture.height = Engine::scalePhysicalPixelsToLogical(
#if defined(OPENYAMM_VITA_LOADER)
        prepared->sourceHeight,
#else
        textureHeight,
#endif
        view.m_pAssetFileSystem->getAssetScaleTier());
    billboardTexture.physicalWidth = textureWidth;
    billboardTexture.physicalHeight = textureHeight;
    {
        Engine::VitaProfile::Scope timing(Engine::VitaProfile::SpriteMask);
#if defined(OPENYAMM_VITA_LOADER)
        if (prepared->bc3) { billboardTexture.opacityMask.assignFromBits(prepared->maskBits, prepared->sourceWidth, prepared->sourceHeight); }
        else
#endif
        { billboardTexture.opacityMask.assignFromBgra(*pixels, textureWidth, textureHeight); }
    }
#if defined(OPENYAMM_VITA_LOADER)
    billboardTexture.vitaBackingBytes = metadata.vitaBackingBytes;
    billboardTexture.vitaManaged = true;
    billboardTexture.vitaLastUse = view.m_vitaTextureFrame;
    billboardTexture.vitaAttemptFrame = view.m_vitaTextureFrame;
    // Header and decoder must agree; never allocate outside the admitted size.
    if (billboardTexture.vitaBackingBytes != metadata.vitaBackingBytes) { return nullptr; }
#endif
#if defined(OPENYAMM_VITA_LOADER)
    if (prepared->bc3)
    {
        billboardTexture.textureHandle = bgfx::createTexture2D(uint16_t(textureWidth), uint16_t(textureHeight),
            false, 1, bgfx::TextureFormat::BC3,
            textureFilterSamplerFlags(TextureFilterProfile::Billboard) | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP,
            bgfx::copy(pixels->data(), uint32_t(pixels->size())));
    }
    else
#endif
    billboardTexture.textureHandle = createBgraTexture2D(
        uint16_t(textureWidth),
        uint16_t(textureHeight),
        pixels->data(),
        uint32_t(pixels->size()),
        TextureFilterProfile::Billboard,
        BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP,
        #if defined(OPENYAMM_VITA_LOADER)
        BgraTexturePixelPreparation::AlreadyPrepared, textureName.c_str()
#else
        BgraTexturePixelPreparation::Required, textureName.c_str()
#endif
        );

    if (!bgfx::isValid(billboardTexture.textureHandle))
    {
        logLoadResult("failed", textureWidth, textureHeight);
        return nullptr;
    }

#if defined(OPENYAMM_VITA_LOADER)
    billboardTexture.vitaStreamTicket = metadata.vitaStreamTicket;
    view.m_vitaSpriteStream->consumed(metadata.vitaStreamTicket);
    view.m_vitaTextureUploadBytes += billboardTexture.vitaBackingBytes;
    OY_VIS(Engine::VitaVisibility::upload(billboardTexture.vitaBackingBytes));
#endif
    if (pExistingTexture != nullptr)
    {
        // Deque entries stay stable so picking metadata survives GPU eviction.
        auto &existing = const_cast<OutdoorGameView::BillboardTextureHandle &>(*pExistingTexture);
        existing = std::move(billboardTexture);
        return &existing;
    }
    view.m_billboardTextureHandles.push_back(std::move(billboardTexture));
    view.m_billboardTextureIndexByPalette[paletteId][view.m_billboardTextureHandles.back().textureName] =
        view.m_billboardTextureHandles.size() - 1;
    // Successful runtime loads are expected now that outdoor actor sprites stream on demand.
    // logLoadResult("loaded", textureWidth, textureHeight);
    return &view.m_billboardTextureHandles.back();
}

void OutdoorBillboardRenderer::invalidateRenderAssets(OutdoorGameView &view)
{
    view.m_vitaSpriteStream.reset();
    for (OutdoorGameView::BillboardTextureHandle &textureHandle : view.m_billboardTextureHandles)
    {
        textureHandle.textureHandle = BGFX_INVALID_HANDLE;
    }

    view.m_vitaActorFallbackTextures.clear();
    view.m_vitaActorLightSamples.clear();
    view.m_vitaTexturePeak = view.m_vitaTextureUploadBytes = 0;
    view.m_vitaTextureThrottled = view.m_vitaTextureDeduplicated = 0;
    view.m_billboardTextureHandles.clear();
    view.m_vitaRetiredTextures.clear();
    view.m_vitaTextureFrame = view.m_vitaTextureEvictions = view.m_vitaTextureDeferred = 0;
    view.m_billboardTextureIndexByPalette.clear();
    view.m_runtimeBillboardLoadWarningKeys.clear();
    view.m_spriteLoadCache.directoryAssetPathsByPath.clear();
    view.m_spriteLoadCache.assetPathByKey.clear();
    view.m_spriteLoadCache.binaryFilesByPath.clear();
    view.m_spriteLoadCache.actPalettesByKey.clear();
    view.m_pendingSpriteFrameWarmups.clear();
    view.m_queuedSpriteFrameWarmups.clear();
    view.m_nextPendingSpriteFrameWarmupIndex = 0;
}

void OutdoorBillboardRenderer::destroyRenderAssets(OutdoorGameView &view)
{
    for (OutdoorGameView::BillboardTextureHandle &textureHandle : view.m_billboardTextureHandles)
    {
        if (bgfx::isValid(textureHandle.textureHandle))
        {
            bgfx::destroy(textureHandle.textureHandle);
            textureHandle.textureHandle = BGFX_INVALID_HANDLE;
        }
    }

    invalidateRenderAssets(view);
}

void OutdoorBillboardRenderer::renderDecorationBillboards(
    OutdoorGameView &view,
    uint16_t viewId,
    uint16_t viewWidth,
    uint16_t viewHeight,
    const float *pViewMatrix,
    const bx::Vec3 &cameraPosition)
{
    Engine::VitaDetailProfile::Scope detailScope(Engine::VitaDetailProfile::RenderDecorations);
    Engine::VitaProfile::Scope vitaScope(Engine::VitaProfile::BillboardGather);
    (void)viewWidth;
    (void)viewHeight;

    if (!view.m_outdoorDecorationBillboardSet
        || !bgfx::isValid(view.m_outdoorLitBillboardProgramHandle)
        || !bgfx::isValid(view.m_terrainTextureSamplerHandle)
        || !bgfx::isValid(view.m_outdoorBillboardOverrideColorUniformHandle)
        || !bgfx::isValid(view.m_outdoorBillboardOutlineParamsUniformHandle))
    {
        return;
    }

    const bx::Vec3 cameraRight = {pViewMatrix[0], pViewMatrix[4], pViewMatrix[8]};
    const bx::Vec3 cameraUp = {pViewMatrix[1], pViewMatrix[5], pViewMatrix[9]};
    const float cosPitch = std::cos(view.m_cameraPitchRadians);
    const bx::Vec3 cameraForward = {
        std::cos(view.m_cameraYawRadians) * cosPitch,
        std::sin(view.m_cameraYawRadians) * cosPitch,
        std::sin(view.m_cameraPitchRadians)
    };
    const float decorationRenderDistance = view.m_viewDistanceCache.decorationBillboardDistance;
    applyBillboardFogUniforms(view, decorationRenderDistance);
    const uint32_t animationTimeTicks = currentAnimationTicks();

    struct BillboardDrawItem
    {
        const DecorationBillboard *pBillboard = nullptr;
        const SpriteFrameEntry *pFrame = nullptr;
        const OutdoorGameView::BillboardTextureHandle *pTexture = nullptr;
        bool mirrored = false;
        float distanceSquared = 0.0f;
        uint32_t lightContributionAbgr = 0xff000000u;
    };
    const auto resolveBillboardVisual = [&view](const DecorationBillboard &billboard, bool &hidden)
    {
        hidden = false;
        uint16_t spriteId = billboard.spriteId;

        if (!view.m_outdoorDecorationBillboardSet || view.m_pOutdoorWorldRuntime == nullptr)
        {
            return spriteId;
        }

        const EventRuntimeState *pEventRuntimeState = view.m_pOutdoorWorldRuntime->eventRuntimeState();

        if (pEventRuntimeState == nullptr)
        {
            return spriteId;
        }

        if (pEventRuntimeState->spriteOverrides.empty())
        {
            return spriteId;
        }

        const uint32_t overrideKey = billboard.spriteOverrideKey();

        const auto overrideIterator = pEventRuntimeState->spriteOverrides.find(overrideKey);

        if (overrideIterator == pEventRuntimeState->spriteOverrides.end())
        {
            return spriteId;
        }

        hidden = overrideIterator->second.hidden;

        if (!overrideIterator->second.textureName.has_value() || overrideIterator->second.textureName->empty())
        {
            return spriteId;
        }

        if (const DecorationEntry *pDecoration =
                view.m_outdoorDecorationBillboardSet->decorationTable.findByInternalName(
                    *overrideIterator->second.textureName))
        {
            return pDecoration->spriteId;
        }

        if (const std::optional<uint16_t> overrideSpriteId =
                view.m_outdoorDecorationBillboardSet->spriteFrameTable.findFrameIndexBySpriteName(
                    *overrideIterator->second.textureName))
        {
            return *overrideSpriteId;
        }

        return spriteId;
    };

    std::vector<size_t> candidateBillboardIndices;
    OutdoorInteractionController::collectDecorationBillboardCandidates(view, 
        cameraPosition.x - decorationRenderDistance,
        cameraPosition.y - decorationRenderDistance,
        cameraPosition.x + decorationRenderDistance,
        cameraPosition.y + decorationRenderDistance,
        candidateBillboardIndices);

    if (candidateBillboardIndices.empty())
    {
        return;
    }

    std::vector<BillboardDrawItem> drawItems;
    drawItems.reserve(candidateBillboardIndices.size());

    for (size_t billboardIndex : candidateBillboardIndices)
    {
        const DecorationBillboard &billboard = view.m_outdoorDecorationBillboardSet->billboards[billboardIndex];
        bool hidden = false;
        const uint16_t spriteId = resolveBillboardVisual(billboard, hidden);

        if (OutdoorInteractionController::isInteractiveDecorationHidden(view, billboard.entityIndex)
            || hidden
            || spriteId == 0)
        {
            continue;
        }

        const float baseX = static_cast<float>(billboard.x);
        const float baseY = static_cast<float>(billboard.y);
        const float baseZ = static_cast<float>(billboard.z);
        const float deltaX = baseX - cameraPosition.x;
        const float deltaY = baseY - cameraPosition.y;
        const float deltaZ = baseZ - cameraPosition.z;
        const float distanceSquared = deltaX * deltaX + deltaY * deltaY + deltaZ * deltaZ;

        if (distanceSquared > decorationRenderDistance * decorationRenderDistance)
        {
            continue;
        }

        if (deltaX * cameraForward.x + deltaY * cameraForward.y + deltaZ * cameraForward.z <= BillboardNearDepth)
        {
            continue;
        }

        const uint32_t animationOffsetTicks =
            animationTimeTicks + static_cast<uint32_t>(std::abs(billboard.x + billboard.y));
        const SpriteFrameEntry *pFrame =
            view.m_outdoorDecorationBillboardSet->spriteFrameTable.getFrame(spriteId, animationOffsetTicks);

        if (pFrame == nullptr)
        {
            continue;
        }

        const float facingRadians = static_cast<float>(billboard.facing) * Pi / 180.0f;
        const float angleToCamera = std::atan2(
            static_cast<float>(billboard.y) - cameraPosition.y,
            static_cast<float>(billboard.x) - cameraPosition.x);
        const float octantAngle = facingRadians - angleToCamera + Pi + (Pi / 8.0f);
        const int octant = static_cast<int>(std::floor(octantAngle / (Pi / 4.0f))) & 7;
        const ResolvedSpriteTexture resolvedTexture = SpriteFrameTable::resolveTexture(*pFrame, octant);
        const OutdoorGameView::BillboardTextureHandle *pTexture =
            ensureSpriteBillboardTexture(view, resolvedTexture.textureName, pFrame->paletteId);

        if (pTexture == nullptr || !bgfx::isValid(pTexture->textureHandle) || pTexture->width <= 0 || pTexture->height <= 0)
        {
            continue;
        }

        BillboardDrawItem drawItem = {};
        drawItem.pBillboard = &billboard;
        drawItem.pFrame = pFrame;
        drawItem.pTexture = pTexture;
        drawItem.mirrored = resolvedTexture.mirrored;
        drawItem.distanceSquared = distanceSquared;
        drawItems.push_back(drawItem);
    }

    std::sort(
        drawItems.begin(),
        drawItems.end(),
        [](const BillboardDrawItem &left, const BillboardDrawItem &right)
        {
            const uint16_t leftTextureId = left.pTexture != nullptr ? left.pTexture->textureHandle.idx : 0;
            const uint16_t rightTextureId = right.pTexture != nullptr ? right.pTexture->textureHandle.idx : 0;

            if (leftTextureId != rightTextureId)
            {
                return leftTextureId < rightTextureId;
            }

            return left.distanceSquared > right.distanceSquared;
        });

    if (OY_VITA_TRACE(view.m_gameSettings.performanceTrace))
    {
        view.m_outdoorSpriteRenderDiagnostics.decorationItems += drawItems.size();
    }

    size_t groupBegin = 0;

    while (groupBegin < drawItems.size())
    {
        const OutdoorGameView::BillboardTextureHandle *pTexture = drawItems[groupBegin].pTexture;

        if (pTexture == nullptr || !bgfx::isValid(pTexture->textureHandle))
        {
            ++groupBegin;
            continue;
        }

        size_t groupEnd = groupBegin + 1;

        while (groupEnd < drawItems.size() && drawItems[groupEnd].pTexture == pTexture)
        {
            ++groupEnd;
        }

        const uint32_t vertexCount = static_cast<uint32_t>((groupEnd - groupBegin) * 6);

        if (bgfx::getAvailTransientVertexBuffer(vertexCount, OutdoorGameView::LitBillboardVertex::ms_layout) < vertexCount)
        {
            groupBegin = groupEnd;
            continue;
        }

        std::vector<OutdoorGameView::LitBillboardVertex> vertices;
        vertices.reserve(static_cast<size_t>(vertexCount));

        for (size_t index = groupBegin; index < groupEnd; ++index)
        {
            const BillboardDrawItem &drawItem = drawItems[index];
            const DecorationBillboard &billboard = *drawItem.pBillboard;
            const SpriteFrameEntry &frame = *drawItem.pFrame;
            const float spriteScale = std::max(frame.scale, 0.01f);
            const float worldWidth = static_cast<float>(pTexture->width) * spriteScale;
            const float worldHeight = static_cast<float>(pTexture->height) * spriteScale;
            const float halfWidth = worldWidth * 0.5f;
            const bx::Vec3 center = bottomAnchoredBillboardCenter(
                static_cast<float>(billboard.x),
                static_cast<float>(billboard.y),
                static_cast<float>(billboard.z),
                cameraUp,
                worldHeight);
            const bx::Vec3 right = {cameraRight.x * halfWidth, cameraRight.y * halfWidth, cameraRight.z * halfWidth};
            const bx::Vec3 up = {
                cameraUp.x * worldHeight * 0.5f,
                cameraUp.y * worldHeight * 0.5f,
                cameraUp.z * worldHeight * 0.5f
            };
            const float u0 = drawItem.mirrored ? 1.0f : 0.0f;
            const float u1 = drawItem.mirrored ? 0.0f : 1.0f;
            const uint32_t lightContributionAbgr =
                computeBillboardLightContributionAbgr(
                    view,
                    static_cast<float>(billboard.x),
                    static_cast<float>(billboard.y),
                    static_cast<float>(billboard.z) + worldHeight * 0.5f);
            appendLitBillboardVertices(
                vertices,
                center,
                right,
                up,
                u0,
                u1,
                lightContributionAbgr);
        }

        bgfx::TransientVertexBuffer transientVertexBuffer = {};
        bgfx::allocTransientVertexBuffer(
            &transientVertexBuffer,
            vertexCount,
            OutdoorGameView::LitBillboardVertex::ms_layout);
        std::memcpy(
            transientVertexBuffer.data,
            vertices.data(),
            static_cast<size_t>(vertices.size() * sizeof(OutdoorGameView::LitBillboardVertex)));

        float modelMatrix[16] = {};
        bx::mtxIdentity(modelMatrix);
        bgfx::setTransform(modelMatrix);
        bgfx::setVertexBuffer(0, &transientVertexBuffer, 0, vertexCount);
        bindTexture(
            0,
            view.m_terrainTextureSamplerHandle,
            pTexture->textureHandle,
            TextureFilterProfile::Billboard,
            BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
        applyBillboardAmbientUniform(view);
        applyBillboardOverrideColorUniform(view, 0, 0.0f);
        applyBillboardOutlineParamsUniform(view, 0.0f, 0.0f, 0.0f);
        bgfx::setState(BillboardAlphaRenderState);
        bgfx::submit(viewId,
            !view.settingsSnapshot().spriteOutline && bgfx::isValid(view.m_outdoorFastBillboardProgramHandle)
                ? view.m_outdoorFastBillboardProgramHandle : view.m_outdoorLitBillboardProgramHandle);

        if (OY_VITA_TRACE(view.m_gameSettings.performanceTrace))
        {
            ++view.m_outdoorSpriteRenderDiagnostics.decorationBatchSubmits;
            view.m_outdoorSpriteRenderDiagnostics.decorationBatchedItems += groupEnd - groupBegin;
            ++view.m_outdoorSpriteRenderDiagnostics.decorationTextureGroups;
        }

        groupBegin = groupEnd;
    }
}

void OutdoorBillboardRenderer::renderActorPreviewBillboards(
    OutdoorGameView &view,
    uint16_t viewId,
    const float *pViewMatrix,
    const bx::Vec3 &cameraPosition)
{
    VitaViewCostScope viewCost45(vitaViewCosts.billboardUs);
#if defined(OPENYAMM_VITA_PROFILE)
    vitaViewCosts.reducedSprites = vitaViewCosts.subpixelSkipped = 0;
    vitaViewCosts.lightReused = vitaViewCosts.lightRefreshed = 0;
    vitaViewCosts.yaw = view.m_cameraYawRadians; vitaViewCosts.pitch = view.m_cameraPitchRadians;
#endif
    unsigned earlyRejected45 = 0;
    Engine::VitaDetailProfile::Scope detailScope(Engine::VitaDetailProfile::RenderActors);
    Engine::VitaProfile::Scope vitaScope(Engine::VitaProfile::BillboardGather);
    if (!view.m_outdoorActorPreviewBillboardSet && !view.m_outdoorDecorationBillboardSet)
    {
        return;
    }

    const bx::Vec3 cameraRight = {pViewMatrix[0], pViewMatrix[4], pViewMatrix[8]};
    const bx::Vec3 cameraUp = {pViewMatrix[1], pViewMatrix[5], pViewMatrix[9]};
    const float cosPitch = std::cos(view.m_cameraPitchRadians);
    const bx::Vec3 cameraForward = {
        std::cos(view.m_cameraYawRadians) * cosPitch,
        std::sin(view.m_cameraYawRadians) * cosPitch,
        std::sin(view.m_cameraPitchRadians)
    };
    const uint32_t animationTimeTicks = currentAnimationTicks();
    const bool collectRenderDiagnostics =
        OY_VITA_TRACE(view.m_gameSettings.performanceTrace) || actorRenderHitchLoggingEnabled();
    const uint64_t actorRenderStartTickCount = collectRenderDiagnostics ? SDL_GetTicksNS() : 0;
    const uint64_t renderHitchLogThresholdNanoseconds =
        OY_VITA_TRACE(view.m_gameSettings.performanceTrace)
            ? static_cast<uint64_t>(
                std::max(0.1f, view.m_gameSettings.hitchThresholdMilliseconds) * 1000000.0f)
            : RenderHitchLogThresholdNanoseconds;
    uint64_t gatherStageNanoseconds = 0;
    uint64_t placeholderStageNanoseconds = 0;
    uint64_t sortStageNanoseconds = 0;
    uint64_t submitStageNanoseconds = 0;
    size_t textureGroupCount = 0;
    size_t ensuredTextureLoadCount = 0;
    unsigned vitaActors[2]{}, vitaDecor[2]{}, vitaDrawRanges[4]{};

    struct BillboardDrawItem
    {
        size_t actorIndex = static_cast<size_t>(-1);
        const SpriteFrameEntry *pFrame = nullptr;
        const OutdoorGameView::BillboardTextureHandle *pTexture = nullptr;
        bool mirrored = false;
        bool hovered = false;
        bool actor = false;
        bool decoration = false;
        uint32_t hoveredOutlineColorAbgr = 0;
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        float heightScale = 1.0f;
        float distanceSquared = 0.0f;
        float cameraDepth = 0.0f;
        uint32_t lightContributionAbgr = 0xff000000u;
        bool hasHealthBar = false;
        float healthRatio = 1.0f;
        float healthBarZ = 0.0f;
        float healthBarScale = 1.0f;
        float questMarkerZ = 0.0f;
    };

    thread_local std::vector<BillboardDrawItem> drawItems;
    drawItems.clear();
    size_t drawItemReserveCount = 0;

    if (view.m_outdoorDecorationBillboardSet)
    {
        drawItemReserveCount += view.m_outdoorDecorationBillboardSet->billboards.size();
    }

    if (view.m_outdoorActorPreviewBillboardSet)
    {
        drawItemReserveCount += view.m_outdoorActorPreviewBillboardSet->billboards.size();
    }

    if (view.m_pOutdoorWorldRuntime != nullptr)
    {
        drawItemReserveCount += view.m_pOutdoorWorldRuntime->mapActorCount();
    }

    drawItems.reserve(drawItemReserveCount);
    const auto resolveDecorationBillboardSpriteId = [&view](const DecorationBillboard &billboard, bool &hidden)
    {
        hidden = false;
        uint16_t spriteId = billboard.spriteId;

        if (!view.m_outdoorDecorationBillboardSet || view.m_pOutdoorWorldRuntime == nullptr)
        {
            return spriteId;
        }

        const EventRuntimeState *pEventRuntimeState = view.m_pOutdoorWorldRuntime->eventRuntimeState();

        if (pEventRuntimeState == nullptr)
        {
            return spriteId;
        }

        if (pEventRuntimeState->spriteOverrides.empty())
        {
            return spriteId;
        }

        const uint32_t overrideKey = billboard.spriteOverrideKey();

        const auto overrideIterator = pEventRuntimeState->spriteOverrides.find(overrideKey);

        if (overrideIterator == pEventRuntimeState->spriteOverrides.end())
        {
            return spriteId;
        }

        hidden = overrideIterator->second.hidden;

        if (!overrideIterator->second.textureName.has_value() || overrideIterator->second.textureName->empty())
        {
            return spriteId;
        }

        if (const DecorationEntry *pDecoration =
                view.m_outdoorDecorationBillboardSet->decorationTable.findByInternalName(
                    *overrideIterator->second.textureName))
        {
            return pDecoration->spriteId;
        }

        if (const std::optional<uint16_t> overrideSpriteId =
                view.m_outdoorDecorationBillboardSet->spriteFrameTable.findFrameIndexBySpriteName(
                    *overrideIterator->second.textureName))
        {
            return *overrideSpriteId;
        }

        return spriteId;
    };

    thread_local std::vector<OutdoorGameView::TerrainVertex> placeholderVertices;
    placeholderVertices.clear();

    if (view.m_outdoorActorPreviewBillboardSet)
    {
        placeholderVertices.reserve(view.m_outdoorActorPreviewBillboardSet->billboards.size() * 6);
    }

    const bool spriteOutlineEnabled = view.settingsSnapshot().spriteOutline;
    const std::optional<OutdoorGameView::InspectHit> hoveredInspectHit =
        (spriteOutlineEnabled || view.settingsSnapshot().selectionTint)
            ? resolveHoveredOutlineInspectHit(view, pViewMatrix) : std::nullopt;
    std::optional<size_t> hoveredRuntimeActorIndex;

    if (hoveredInspectHit && hoveredInspectHit->kind == "actor")
    {
        hoveredRuntimeActorIndex =
            OutdoorInteractionController::resolveRuntimeActorIndexForInspectHit(view, *hoveredInspectHit);
    }

    const GameplayWorldHit *pContextActionHit = nullptr;
    if (view.settingsSnapshot().contextActionPopup)
    {
        pContextActionHit =
            selectedContextActionWorldHit(view.m_gameSession.gameplayScreenRuntime().contextActionStateReadOnly());
    }

    thread_local std::vector<bool> coveredRuntimeActors;
    coveredRuntimeActors.clear();

    if (view.m_pOutdoorWorldRuntime != nullptr)
    {
        coveredRuntimeActors.assign(view.m_pOutdoorWorldRuntime->mapActorCount(), false);
#if defined(OPENYAMM_VITA_LOADER)
        if (vitaPerformanceSettings().actorRenderLod)
        {
            view.m_vitaActorLightSamples.resize(coveredRuntimeActors.size());
        }
#endif
    }

    const bool renderCombatActorHealthBars = view.settingsSnapshot().combatActorHealthBars;
    CombatActorHealthBarSelection healthBarSelection = {};
    const float actorViewAspectRatio =
        static_cast<float>(std::max(view.m_lastRenderWidth, 1))
        / static_cast<float>(std::max(view.m_lastRenderHeight, 1));
    constexpr float ActorViewVerticalTangent = 0.57735026919f;
    const float actorViewHorizontalTangent = ActorViewVerticalTangent * actorViewAspectRatio;

    auto appendActorDrawItem =
        [&](const OutdoorWorldRuntime::MapActorState *pRuntimeActor,
            std::optional<size_t> runtimeActorIndex,
            int actorX,
            int actorY,
            int actorZ,
            uint16_t actorRadius,
            uint16_t actorHeight,
            uint16_t sourceBillboardHeight,
            uint16_t spriteFrameIndex,
            const std::array<uint16_t, 8> &actionSpriteFrameIndices,
            bool useStaticFrame)
        {
            OY_VIS(Engine::VitaVisibility::Origin origin(Engine::VitaVisibility::Actor,
                runtimeActorIndex.value_or(size_t(-1))));
            Engine::VitaDetailProfile::Scope setup45(Engine::VitaDetailProfile::BillboardActorSetup);
            if (pRuntimeActor != nullptr && pRuntimeActor->isInvisible)
            {
                return;
            }

            const float deltaX = static_cast<float>(actorX) - cameraPosition.x;
            const float deltaY = static_cast<float>(actorY) - cameraPosition.y;
            const float deltaZ = static_cast<float>(actorZ) - cameraPosition.z;
            const float distanceSquared = deltaX * deltaX + deltaY * deltaY + deltaZ * deltaZ;

            ++vitaActors[distanceSquared > 8192.0f * 8192.0f];
            if (distanceSquared > view.m_viewDistanceCache.actorBillboardDistanceSquared)
            {
                return;
            }

            if (deltaX * cameraForward.x + deltaY * cameraForward.y + deltaZ * cameraForward.z <= BillboardNearDepth)
            {
                return;
            }

            uint32_t frameTimeTicks = useStaticFrame ? 0U : animationTimeTicks;

            if (pRuntimeActor != nullptr)
            {
                const size_t animationIndex = static_cast<size_t>(pRuntimeActor->animation);

                if (animationIndex < actionSpriteFrameIndices.size() && actionSpriteFrameIndices[animationIndex] != 0)
                {
                    spriteFrameIndex = actionSpriteFrameIndices[animationIndex];
                }

                frameTimeTicks = static_cast<uint32_t>(std::max(0.0f, pRuntimeActor->animationTimeTicks));
            }

            bool reduced45 = false;
#if defined(OPENYAMM_VITA_LOADER)
            const bool highlighted45 = runtimeActorIndex &&
                ((hoveredRuntimeActorIndex && *runtimeActorIndex == *hoveredRuntimeActorIndex)
                    || contextActionHighlightsActor(pContextActionHit, *runtimeActorIndex));
            const auto &lod45 = vitaPerformanceSettings();
            const float depth45 = deltaX * cameraForward.x + deltaY * cameraForward.y + deltaZ * cameraForward.z;
            if (lod45.actorRenderLod && !highlighted45)
            {
                // Distance is already available for visibility; avoid projection arithmetic for distant actors.
                reduced45 = lod45.actorRenderLodDistance > 0
                    && distanceSquared > lod45.actorRenderLodDistance * lod45.actorRenderLodDistance;
                if (!reduced45 && lod45.actorRenderLodPixels > 0)
                {
                    const float estimatedPixels = float(std::max(actorHeight, sourceBillboardHeight))
                        * float(std::max(view.m_lastRenderHeight, 1))
                        / (2 * 0.57735026919f * std::max(depth45, 1.0f));
                    reduced45 = estimatedPixels > 0 && estimatedPixels < lod45.actorRenderLodPixels;
                }
                frameTimeTicks = vitaActorPresentationTicks(frameTimeTicks, reduced45);
            }
#endif
            const SpriteFrameEntry *pFrame =
                view.m_outdoorActorPreviewBillboardSet->spriteFrameTable.getFrame(spriteFrameIndex, frameTimeTicks);

            if (pFrame == nullptr)
            {
#if !defined(OPENYAMM_VITA_LOADER) || defined(OPENYAMM_VITA_MISSING_TEXTURE_MARKERS)
                const float markerHalfSize =
                    std::max(48.0f, static_cast<float>(std::max(actorRadius, static_cast<uint16_t>(48))));
                const float baseX = static_cast<float>(actorX);
                const float baseY = static_cast<float>(actorY);
                const float baseZ = static_cast<float>(actorZ) + 96.0f;
                const uint32_t markerColor = 0xff3030ff;

                placeholderVertices.push_back({baseX - markerHalfSize, baseY, baseZ - markerHalfSize, markerColor});
                placeholderVertices.push_back({baseX + markerHalfSize, baseY, baseZ + markerHalfSize, markerColor});
                placeholderVertices.push_back({baseX + markerHalfSize, baseY, baseZ - markerHalfSize, markerColor});
                placeholderVertices.push_back({baseX - markerHalfSize, baseY, baseZ + markerHalfSize, markerColor});
                placeholderVertices.push_back({baseX, baseY - markerHalfSize, baseZ, markerColor});
                placeholderVertices.push_back({baseX, baseY + markerHalfSize, baseZ, markerColor});
#endif
                return;
            }

            Engine::VitaDetailProfile::Scope texture45(Engine::VitaDetailProfile::BillboardTexture);
            const float angleToCamera = std::atan2(
                static_cast<float>(actorY) - cameraPosition.y,
                static_cast<float>(actorX) - cameraPosition.x);
            const float actorYaw = pRuntimeActor != nullptr ? pRuntimeActor->yawRadians : 0.0f;
            const float octantAngle = actorYaw - angleToCamera + Pi + (Pi / 8.0f);
            const int octant = static_cast<int>(std::floor(octantAngle / (Pi / 4.0f))) & 7;
            const ResolvedSpriteTexture resolvedTexture = SpriteFrameTable::resolveTexture(*pFrame, octant);
            const OutdoorGameView::BillboardTextureHandle *pExistingTexture =
                view.findBillboardTexture(resolvedTexture.textureName, pFrame->paletteId);
            const OutdoorGameView::BillboardTextureHandle *pTexture =
                pExistingTexture != nullptr && bgfx::isValid(pExistingTexture->textureHandle)
                    ? pExistingTexture
                    : ensureSpriteBillboardTexture(view, resolvedTexture.textureName, pFrame->paletteId);
#if defined(OPENYAMM_VITA_LOADER)
            if (runtimeActorIndex)
            {
                auto &pFallback = view.m_vitaActorFallbackTextures[*runtimeActorIndex];
                if (pTexture != nullptr && bgfx::isValid(pTexture->textureHandle)) { pFallback = pTexture; }
                else if (pFallback != nullptr && bgfx::isValid(pFallback->textureHandle))
                { pTexture = pFallback; OY_VIS(++Engine::VitaVisibility::counts.actorFallbacks); }
                if (pTexture != nullptr) { pTexture->vitaLastUse = view.m_vitaTextureFrame; }
            }
#endif

            if (pExistingTexture == nullptr && pTexture != nullptr)
            {
                ++ensuredTextureLoadCount;
            }

            if (pTexture == nullptr || !bgfx::isValid(pTexture->textureHandle))
            {
#if !defined(OPENYAMM_VITA_LOADER) || defined(OPENYAMM_VITA_MISSING_TEXTURE_MARKERS)
                const float markerHalfSize =
                    std::max(48.0f, static_cast<float>(std::max(actorRadius, static_cast<uint16_t>(48))));
                const float baseX = static_cast<float>(actorX);
                const float baseY = static_cast<float>(actorY);
                const float baseZ = static_cast<float>(actorZ) + 96.0f;
                const uint32_t markerColor = 0xff3030ff;

                placeholderVertices.push_back({baseX - markerHalfSize, baseY, baseZ - markerHalfSize, markerColor});
                placeholderVertices.push_back({baseX + markerHalfSize, baseY, baseZ + markerHalfSize, markerColor});
                placeholderVertices.push_back({baseX + markerHalfSize, baseY, baseZ - markerHalfSize, markerColor});
                placeholderVertices.push_back({baseX - markerHalfSize, baseY, baseZ + markerHalfSize, markerColor});
                placeholderVertices.push_back({baseX, baseY - markerHalfSize, baseZ, markerColor});
                placeholderVertices.push_back({baseX, baseY + markerHalfSize, baseZ, markerColor});
#endif
                return;
            }

            texture45.finish();
            BillboardDrawItem drawItem = {};
            drawItem.pFrame = pFrame;
            drawItem.pTexture = pTexture;
            drawItem.mirrored = resolvedTexture.mirrored;
            drawItem.actor = true;
            drawItem.actorIndex = runtimeActorIndex.value_or(static_cast<size_t>(-1));
            const bool contextHighlighted =
                runtimeActorIndex.has_value()
                && contextActionHighlightsActor(pContextActionHit, *runtimeActorIndex);
            drawItem.hovered =
                view.settingsSnapshot().spriteOutline && (contextHighlighted
                || (runtimeActorIndex.has_value()
                    && hoveredRuntimeActorIndex.has_value()
                    && *runtimeActorIndex == *hoveredRuntimeActorIndex));
            drawItem.hoveredOutlineColorAbgr =
                drawItem.hovered && pRuntimeActor != nullptr
                    ? (contextHighlighted ? contextActionHighlightOutlineColor() : resolveHoveredActorOutlineColor(*pRuntimeActor))
                    : 0;
            drawItem.x = static_cast<float>(actorX);
            drawItem.y = static_cast<float>(actorY);
            drawItem.z = static_cast<float>(actorZ);
            drawItem.heightScale =
                pRuntimeActor != nullptr && sourceBillboardHeight > 0
                    ? static_cast<float>(actorHeight) / static_cast<float>(sourceBillboardHeight)
                    : 1.0f;
            if (pRuntimeActor != nullptr && pRuntimeActor->shrinkRemainingSeconds > 0.0f)
            {
                drawItem.heightScale *= std::clamp(pRuntimeActor->shrinkDamageMultiplier, 0.25f, 1.0f);
            }
            drawItem.distanceSquared = distanceSquared;
            drawItem.cameraDepth = deltaX * cameraForward.x + deltaY * cameraForward.y + deltaZ * cameraForward.z;
            const float previewScale = std::max(pFrame->scale * drawItem.heightScale, 0.01f);
            const float worldHeight = static_cast<float>(pTexture->height) * previewScale;
            drawItem.questMarkerZ = drawItem.z
                + worldHeight * (1.0f - pTexture->opacityMask.opaqueTopNormalized());
#if defined(OPENYAMM_VITA_LOADER)
            // Same conservative full-sprite sphere as the final cull, now before lighting/health setup.
            const float sphere45 = previewScale * std::hypot(float(pTexture->width), float(pTexture->height));
            const bool outside45 = !vitaSphereVisible(deltaX * cameraRight.x + deltaY * cameraRight.y + deltaZ * cameraRight.z,
                deltaX * cameraUp.x + deltaY * cameraUp.y + deltaZ * cameraUp.z, depth45,
                sphere45, actorViewAspectRatio, view.m_viewDistanceCache.actorBillboardDistance);
            if (vitaPerformanceSettings().extraFrustum && outside45) { ++earlyRejected45; return; }
#endif
#if defined(OPENYAMM_VITA_LOADER)
            if (runtimeActorIndex && view.m_pOutdoorWorldRuntime)
            {
                const bx::Vec3 centerDelta = {deltaX, deltaY, deltaZ + worldHeight * 0.5f};
                const float depth = bx::dot(centerDelta, cameraForward);
                const float halfWidth = pTexture->width * previewScale * 0.5f;
                if (depth > 0 && std::abs(bx::dot(centerDelta, cameraRight))
                    <= depth * actorViewHorizontalTangent + halfWidth
                    && std::abs(bx::dot(centerDelta, cameraUp))
                    <= depth * ActorViewVerticalTangent + worldHeight * 0.5f)
                {
                    view.m_pOutdoorWorldRuntime->markVitaActorVisible(*runtimeActorIndex);
                }
            }
#endif
#if defined(OPENYAMM_VITA_LOADER)
            // Visibility above remains unchanged for AI, including actors too small to draw.
            if (lod45.actorRenderLod && !highlighted45 && vitaActorSubpixel(sphere45, depth45,
                float(view.m_lastRenderHeight), lod45.actorRenderMinPixels))
            {
#if defined(OPENYAMM_VITA_PROFILE)
                ++vitaViewCosts.subpixelSkipped;
#endif
                return;
            }
#if defined(OPENYAMM_VITA_PROFILE)
            vitaViewCosts.reducedSprites += reduced45;
#endif
#endif
            {
                Engine::VitaDetailProfile::Scope lighting45(Engine::VitaDetailProfile::BillboardLighting);
                bool reused45 = false;
#if defined(OPENYAMM_VITA_LOADER)
                if (lod45.actorRenderLod && runtimeActorIndex && pRuntimeActor)
                {
                    auto &sample = view.m_vitaActorLightSamples[*runtimeActorIndex];
                    if (reduced45 && sample.valid && sample.actorId == pRuntimeActor->actorId
                        && uint32_t(animationTimeTicks - sample.ticks) < 32u)
                    { drawItem.lightContributionAbgr = sample.color; reused45 = true; }
                    else
                    {
                        sample.color = computeBillboardLightContributionAbgr(view, drawItem.x, drawItem.y,
                            drawItem.z + worldHeight * 0.5f);
                        sample.actorId = pRuntimeActor->actorId; sample.ticks = animationTimeTicks; sample.valid = true;
                        drawItem.lightContributionAbgr = sample.color;
                    }
                }
                else
#endif
                { drawItem.lightContributionAbgr = computeBillboardLightContributionAbgr(view,
                    drawItem.x, drawItem.y, drawItem.z + worldHeight * 0.5f); }
#if defined(OPENYAMM_VITA_PROFILE)
                vitaViewCosts.lightReused += reused45; vitaViewCosts.lightRefreshed += !reused45;
#endif
            }

            if (!spriteOutlineEnabled && view.settingsSnapshot().selectionTint
                && (contextHighlighted || (runtimeActorIndex && hoveredRuntimeActorIndex
                    && *runtimeActorIndex == *hoveredRuntimeActorIndex)))
            {
                drawItem.lightContributionAbgr = selectionTintLighting(drawItem.lightContributionAbgr);
            }
            if (renderCombatActorHealthBars
                && pRuntimeActor != nullptr
                && pRuntimeActor->hostileToParty
                && pRuntimeActor->currentHp > 0
                && pRuntimeActor->maxHp > 0)
            {
                const float worldWidth = static_cast<float>(pTexture->width) * previewScale;
                const bx::Vec3 billboardCenterDelta = {deltaX, deltaY, deltaZ + worldHeight * 0.5f};
                const float billboardCenterDepth = bx::dot(billboardCenterDelta, cameraForward);
                const float billboardHorizontalOffset = bx::dot(billboardCenterDelta, cameraRight);
                const float billboardVerticalOffset = bx::dot(billboardCenterDelta, cameraUp);
                const bool billboardIntersectsView =
                    billboardCenterDepth > BillboardNearDepth
                    && std::abs(billboardHorizontalOffset)
                        <= billboardCenterDepth * actorViewHorizontalTangent + worldWidth * 0.5f
                    && std::abs(billboardVerticalOffset)
                        <= billboardCenterDepth * ActorViewVerticalTangent + worldHeight * 0.5f;

                if (billboardIntersectsView)
                {
                    drawItem.healthRatio =
                        std::clamp(
                            static_cast<float>(pRuntimeActor->currentHp)
                                / static_cast<float>(pRuntimeActor->maxHp),
                            0.0f,
                            1.0f);
                    considerCombatActorHealthBarCandidate(
                        healthBarSelection,
                        drawItems.size(),
                        drawItem.distanceSquared);
                }
            }


            drawItem.healthBarZ = drawItem.z + worldHeight + 26.0f * drawItem.heightScale;
            drawItems.push_back(drawItem);
        };

    if (view.m_showDecorationBillboards && view.m_outdoorDecorationBillboardSet)
    {
        for (size_t decorationIndex = 0;
             decorationIndex < view.m_outdoorDecorationBillboardSet->billboards.size();
             ++decorationIndex)
        {
            OY_VIS(Engine::VitaVisibility::Origin origin(Engine::VitaVisibility::Decoration, decorationIndex));
            Engine::VitaDetailProfile::Scope decoration45(Engine::VitaDetailProfile::BillboardDecorSetup);
            const DecorationBillboard &billboard = view.m_outdoorDecorationBillboardSet->billboards[decorationIndex];
            bool hidden = false;
            const uint16_t spriteId =
                OutdoorInteractionController::resolveDecorationBillboardSpriteId(view, billboard, hidden);

            if (OutdoorInteractionController::isInteractiveDecorationHidden(view, billboard.entityIndex)
                || hidden
                || spriteId == 0)
            {
                continue;
            }

            const float deltaX = static_cast<float>(billboard.x) - cameraPosition.x;
            const float deltaY = static_cast<float>(billboard.y) - cameraPosition.y;
            const float deltaZ = static_cast<float>(billboard.z) - cameraPosition.z;
            const float distanceSquared = deltaX * deltaX + deltaY * deltaY + deltaZ * deltaZ;
            const float cameraDepth = deltaX * cameraForward.x + deltaY * cameraForward.y + deltaZ * cameraForward.z;

            ++vitaDecor[distanceSquared > 8192.0f * 8192.0f];
            if (distanceSquared > view.m_viewDistanceCache.decorationBillboardDistanceSquared
                || cameraDepth <= BillboardNearDepth)
            {
                continue;
            }

            const uint32_t animationOffsetTicks =
                animationTimeTicks + static_cast<uint32_t>(std::abs(billboard.x + billboard.y));
            const SpriteFrameEntry *pFrame =
                view.m_outdoorDecorationBillboardSet->spriteFrameTable.getFrame(spriteId, animationOffsetTicks);

            if (pFrame == nullptr)
            {
                continue;
            }

            const float facingRadians = static_cast<float>(billboard.facing) * Pi / 180.0f;
            const float angleToCamera = std::atan2(
                static_cast<float>(billboard.y) - cameraPosition.y,
                static_cast<float>(billboard.x) - cameraPosition.x);
            const float octantAngle = facingRadians - angleToCamera + Pi + (Pi / 8.0f);
            const int octant = static_cast<int>(std::floor(octantAngle / (Pi / 4.0f))) & 7;
            const ResolvedSpriteTexture resolvedTexture = SpriteFrameTable::resolveTexture(*pFrame, octant);
            const OutdoorGameView::BillboardTextureHandle *pTexture =
                ensureSpriteBillboardTexture(view, resolvedTexture.textureName, pFrame->paletteId);

            if (pTexture == nullptr || !bgfx::isValid(pTexture->textureHandle))
            {
                continue;
            }

            BillboardDrawItem drawItem = {};
            drawItem.pFrame = pFrame;
            drawItem.pTexture = pTexture;
            drawItem.mirrored = resolvedTexture.mirrored;
            drawItem.decoration = true;
#if defined(OPENYAMM_VITA_PROFILE)
            drawItem.actorIndex = decorationIndex;
#endif
            drawItem.x = static_cast<float>(billboard.x);
            drawItem.y = static_cast<float>(billboard.y);
            drawItem.z = static_cast<float>(billboard.z);
            drawItem.heightScale = 1.0f;
            drawItem.hovered = view.settingsSnapshot().spriteOutline && (contextActionHighlightsOutdoorDecoration(pContextActionHit, decorationIndex));
            drawItem.hoveredOutlineColorAbgr =
                drawItem.hovered ? contextActionHighlightOutlineColor() : 0;
            drawItem.distanceSquared = distanceSquared;
            drawItem.cameraDepth = cameraDepth;
            drawItem.lightContributionAbgr = computeBillboardLightContributionAbgr(
                view,
                drawItem.x,
                drawItem.y,
                drawItem.z + static_cast<float>(pTexture->height) * std::max(pFrame->scale, 0.01f) * 0.5f);
            drawItems.push_back(drawItem);
        }
    }

    if (view.m_showActors && view.m_outdoorActorPreviewBillboardSet)
    {
        for (size_t billboardIndex = 0; billboardIndex < view.m_outdoorActorPreviewBillboardSet->billboards.size(); ++billboardIndex)
        {
            const ActorPreviewBillboard &billboard = view.m_outdoorActorPreviewBillboardSet->billboards[billboardIndex];

            if (billboard.source != ActorPreviewSource::Companion)
            {
                continue;
            }

            const OutdoorWorldRuntime::MapActorState *pRuntimeActor = OutdoorInteractionController::runtimeActorStateForBillboard(view, billboard);
            const int actorX = pRuntimeActor != nullptr ? pRuntimeActor->x : billboard.x;
            const int actorY = pRuntimeActor != nullptr ? pRuntimeActor->y : billboard.y;
            const int actorZ = pRuntimeActor != nullptr ? pRuntimeActor->z : billboard.z;
            const uint16_t actorRadius = pRuntimeActor != nullptr ? pRuntimeActor->radius : billboard.radius;
            const uint16_t actorHeight = pRuntimeActor != nullptr ? pRuntimeActor->height : billboard.height;

            if (pRuntimeActor != nullptr && billboard.runtimeActorIndex < coveredRuntimeActors.size())
            {
                coveredRuntimeActors[billboard.runtimeActorIndex] = true;
            }

            appendActorDrawItem(
                pRuntimeActor,
                pRuntimeActor != nullptr ? std::optional<size_t>(billboard.runtimeActorIndex) : std::nullopt,
                actorX,
                actorY,
                actorZ,
                actorRadius,
                actorHeight,
                billboard.height,
                billboard.spriteFrameIndex,
                billboard.actionSpriteFrameIndices,
                billboard.useStaticFrame);
        }
    }

    if (view.m_showActors && view.m_pOutdoorWorldRuntime != nullptr)
    {
        for (size_t actorIndex = 0; actorIndex < view.m_pOutdoorWorldRuntime->mapActorCount(); ++actorIndex)
        {
            if (actorIndex < coveredRuntimeActors.size() && coveredRuntimeActors[actorIndex])
            {
                continue;
            }

            const OutdoorWorldRuntime::MapActorState *pRuntimeActor = view.m_pOutdoorWorldRuntime->mapActorState(actorIndex);

            if (pRuntimeActor == nullptr)
            {
                continue;
            }

            appendActorDrawItem(
                pRuntimeActor,
                actorIndex,
                pRuntimeActor->x,
                pRuntimeActor->y,
                pRuntimeActor->z,
                pRuntimeActor->radius,
                pRuntimeActor->height,
                pRuntimeActor->height,
                pRuntimeActor->spriteFrameIndex,
                pRuntimeActor->actionSpriteFrameIndices,
                pRuntimeActor->useStaticSpriteFrame);
        }
    }

    for (size_t selectionIndex = 0; selectionIndex < healthBarSelection.count; ++selectionIndex)
    {
        const size_t drawItemIndex = healthBarSelection.candidates[selectionIndex].drawItemIndex;

        if (drawItemIndex < drawItems.size())
        {
            drawItems[drawItemIndex].hasHealthBar = true;
            drawItems[drawItemIndex].healthBarScale = combatActorHealthBarScale(
                healthBarSelection.candidates[selectionIndex].distanceSquared);
        }
    }

    if (collectRenderDiagnostics)
    {
        gatherStageNanoseconds += SDL_GetTicksNS() - actorRenderStartTickCount;
    }

    if (!placeholderVertices.empty())
    {
        const uint64_t placeholderStageStartTickCount = collectRenderDiagnostics ? SDL_GetTicksNS() : 0;

        if (bgfx::getAvailTransientVertexBuffer(
                static_cast<uint32_t>(placeholderVertices.size()),
                OutdoorGameView::TerrainVertex::ms_layout) >= placeholderVertices.size())
        {
            bgfx::TransientVertexBuffer transientVertexBuffer = {};
            bgfx::allocTransientVertexBuffer(
                &transientVertexBuffer,
                static_cast<uint32_t>(placeholderVertices.size()),
                OutdoorGameView::TerrainVertex::ms_layout);
            std::memcpy(
                transientVertexBuffer.data,
                placeholderVertices.data(),
                static_cast<size_t>(placeholderVertices.size() * sizeof(OutdoorGameView::TerrainVertex)));

            float modelMatrix[16] = {};
            bx::mtxIdentity(modelMatrix);
            bgfx::setTransform(modelMatrix);
            bgfx::setVertexBuffer(0, &transientVertexBuffer, 0, static_cast<uint32_t>(placeholderVertices.size()));
            bgfx::setState(
                BGFX_STATE_WRITE_RGB
                | BGFX_STATE_WRITE_Z
                | BGFX_STATE_PT_LINES
                | BGFX_STATE_LINEAA
                | BGFX_STATE_DEPTH_TEST_LEQUAL);
            bgfx::submit(viewId, view.m_programHandle);
        }

        if (collectRenderDiagnostics)
        {
            placeholderStageNanoseconds += SDL_GetTicksNS() - placeholderStageStartTickCount;
        }
    }

    if (!bgfx::isValid(view.m_outdoorLitBillboardProgramHandle)
        || !bgfx::isValid(view.m_terrainTextureSamplerHandle)
        || !bgfx::isValid(view.m_outdoorBillboardOverrideColorUniformHandle)
        || !bgfx::isValid(view.m_outdoorBillboardOutlineParamsUniformHandle)
        || !bgfx::isValid(view.m_outdoorBillboardAmbientUniformHandle))
    {
        const uint64_t totalActorRenderNanoseconds =
            collectRenderDiagnostics ? SDL_GetTicksNS() - actorRenderStartTickCount : 0;

        if (collectRenderDiagnostics && totalActorRenderNanoseconds >= renderHitchLogThresholdNanoseconds)
        {
            std::cout << "[OutdoorActorRenderHitchDetail]"
                      << " total_us=" << totalActorRenderNanoseconds / 1000ULL
                      << " gather_us=" << gatherStageNanoseconds / 1000ULL
                      << " placeholder_us=" << placeholderStageNanoseconds / 1000ULL
                      << " sort_us=" << sortStageNanoseconds / 1000ULL
                      << " submit_us=" << submitStageNanoseconds / 1000ULL
                      << " draw_items=" << drawItems.size()
                      << " placeholder_vertices=" << placeholderVertices.size()
                      << " texture_groups=" << textureGroupCount
                      << " ensured_loads=" << ensuredTextureLoadCount
                      << '\n';
        }

        return;
    }

#if defined(OPENYAMM_VITA_LOADER)
    Engine::VitaDetailProfile::Scope cull45(Engine::VitaDetailProfile::BillboardCull);
    unsigned candidateCounts[2]{}, rejectedCounts[2]{};
    std::erase_if(drawItems, [&](const BillboardDrawItem &item)
    {
        const unsigned category = item.actor ? 0 : 1;
        ++candidateCounts[category];
        const float scale = std::max(item.pFrame->scale * item.heightScale, 0.01f);
        // Enclose either sprite anchor and its full pixel rectangle, never just collision bounds.
        const float radius = scale * std::hypot(float(item.pTexture->width), float(item.pTexture->height));
        const bx::Vec3 delta = bx::sub(bx::Vec3{item.x, item.y, item.z}, cameraPosition);
        const bool outside = !vitaSphereVisible(bx::dot(delta, cameraRight), bx::dot(delta, cameraUp),
            bx::dot(delta, cameraForward), radius, actorViewHorizontalTangent / ActorViewVerticalTangent,
            view.m_viewDistanceCache.actorBillboardDistance);
        rejectedCounts[category] += outside;
        return vitaPerformanceSettings().extraFrustum && outside;
    });
#if defined(OPENYAMM_VITA_PROFILE)
    if (Engine::VitaProfile::frame.active)
    for (unsigned category = 0; category < 2; ++category)
    {
        Engine::VitaProfile::print("PROFILE CULL category=%s candidates=%u outside=%u culled=%u drawn=%u\n",
            category ? "decoration" : "actor", candidateCounts[category], rejectedCounts[category],
            vitaPerformanceSettings().extraFrustum ? rejectedCounts[category] : 0,
            candidateCounts[category] - (vitaPerformanceSettings().extraFrustum ? rejectedCounts[category] : 0));
    }
#endif
#endif
#if defined(OPENYAMM_VITA_LOADER)
    cull45.finish();
#if defined(OPENYAMM_VITA_PROFILE)
    if (Engine::VitaProfile::frame.active)
    {
    Engine::VitaProfile::print("PROFILE ACTOR_RENDER_LOD n=%u early_culled=%u reduced=%u subpixel=%u "
        "light_reused=%u light_refreshed=%u distance=%.0f pixels=%.1f min_pixels=%.1f enabled=%u\n",
        Engine::VitaProfile::frame.index + 1, earlyRejected45, vitaViewCosts.reducedSprites,
        vitaViewCosts.subpixelSkipped, vitaViewCosts.lightReused, vitaViewCosts.lightRefreshed,
        vitaPerformanceSettings().actorRenderLodDistance, vitaPerformanceSettings().actorRenderLodPixels,
        vitaPerformanceSettings().actorRenderMinPixels, vitaPerformanceSettings().actorRenderLod);
    }
#endif
#endif
    Engine::VitaDetailProfile::Scope sort45(Engine::VitaDetailProfile::BillboardSort);
    OY_VIS(for (const BillboardDrawItem &item : drawItems)
        Engine::VitaVisibility::visible(item.actor ? Engine::VitaVisibility::Actor : Engine::VitaVisibility::Decoration,
            item.actorIndex, item.pTexture->vitaStreamTicket));
    const uint64_t sortStageStartTickCount = collectRenderDiagnostics ? SDL_GetTicksNS() : 0;
    std::sort(
        drawItems.begin(),
        drawItems.end(),
        [](const BillboardDrawItem &left, const BillboardDrawItem &right)
        {
            if (left.cameraDepth != right.cameraDepth)
            {
                return left.cameraDepth > right.cameraDepth;
            }

            return left.distanceSquared > right.distanceSquared;
        });
    if (collectRenderDiagnostics)
    {
        sortStageNanoseconds += SDL_GetTicksNS() - sortStageStartTickCount;
    }

    if (OY_VITA_TRACE(view.m_gameSettings.performanceTrace))
    {
        const OutdoorGameView::BillboardTextureHandle *pLastActorTexture = nullptr;
        const OutdoorGameView::BillboardTextureHandle *pLastDecorationTexture = nullptr;

        for (const BillboardDrawItem &drawItem : drawItems)
        {
            if (drawItem.actor)
            {
                ++view.m_outdoorSpriteRenderDiagnostics.actorItems;

                if (drawItem.pTexture != pLastActorTexture)
                {
                    ++view.m_outdoorSpriteRenderDiagnostics.actorTextureSwitches;
                    pLastActorTexture = drawItem.pTexture;
                }
            }

            if (drawItem.decoration)
            {
                ++view.m_outdoorSpriteRenderDiagnostics.decorationItems;

                if (drawItem.pTexture != pLastDecorationTexture)
                {
                    ++view.m_outdoorSpriteRenderDiagnostics.decorationTextureSwitches;
                    pLastDecorationTexture = drawItem.pTexture;
                }
            }
        }
    }

    sort45.finish();
    const uint32_t vertexCount = 6;
    applyBillboardFogUniforms(view, view.m_viewDistanceCache.actorBillboardDistance);
    applyBillboardAmbientUniform(view);

    struct CombinedBillboardBatch
    {
        const OutdoorGameView::BillboardTextureHandle *pTexture = nullptr;
        std::vector<OutdoorGameView::LitBillboardVertex> vertices;
        unsigned distanceMask = 0;
        size_t actorItems = 0;
        size_t decorationItems = 0;
    };

    const auto resetBillboardBatch =
        [](CombinedBillboardBatch &batch)
        {
            batch.vertices.clear();
            batch.pTexture = nullptr;
            batch.distanceMask = 0;
            batch.actorItems = 0;
            batch.decorationItems = 0;
        };

    thread_local std::vector<CombinedBillboardBatch> billboardBatchPool;

    if (billboardBatchPool.capacity() < 32)
    {
        billboardBatchPool.reserve(32);
    }

    bool standardBillboardEffectUniformsActive = false;
    const auto ensureStandardBillboardEffectUniforms =
        [&]()
        {
            if (standardBillboardEffectUniformsActive)
            {
                return;
            }

            applyBillboardOverrideColorUniform(view, 0, 0.0f);
            applyBillboardOutlineParamsUniform(view, 0.0f, 0.0f, 0.0f);
            standardBillboardEffectUniformsActive = true;
        };

    uint32_t vitaBatchInputs = 0, vitaBatchDraws = 0;
    const auto flushBillboardBatch =
        [&](CombinedBillboardBatch &batch)
        {
            if (batch.pTexture == nullptr || batch.vertices.empty())
            {
                resetBillboardBatch(batch);
                return;
            }

            Engine::VitaDetailProfile::Scope submit45(Engine::VitaDetailProfile::BillboardSubmit);
            Engine::VitaProfile::Scope batchScope(Engine::VitaProfile::BillboardBatch);
            const uint64_t submitStageStartTickCount = collectRenderDiagnostics ? SDL_GetTicksNS() : 0;
            const uint32_t batchVertexCount = static_cast<uint32_t>(batch.vertices.size());

            if (bgfx::getAvailTransientVertexBuffer(
                    batchVertexCount,
                    OutdoorGameView::LitBillboardVertex::ms_layout) >= batchVertexCount)
            {
                bgfx::TransientVertexBuffer transientVertexBuffer = {};
                bgfx::allocTransientVertexBuffer(
                    &transientVertexBuffer,
                    batchVertexCount,
                    OutdoorGameView::LitBillboardVertex::ms_layout);
                std::memcpy(
                    transientVertexBuffer.data,
                    batch.vertices.data(),
                    static_cast<size_t>(batch.vertices.size() * sizeof(OutdoorGameView::LitBillboardVertex)));

                float modelMatrix[16] = {};
                bx::mtxIdentity(modelMatrix);
                bgfx::setTransform(modelMatrix);
                bgfx::setVertexBuffer(0, &transientVertexBuffer, 0, batchVertexCount);
                bindTexture(
                    0,
                    view.m_terrainTextureSamplerHandle,
                    batch.pTexture->textureHandle,
                    TextureFilterProfile::Billboard,
                    BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
                ensureStandardBillboardEffectUniforms();
                bgfx::setState(BillboardAlphaRenderState);
                bgfx::submit(viewId,
            !view.settingsSnapshot().spriteOutline && bgfx::isValid(view.m_outdoorFastBillboardProgramHandle)
                ? view.m_outdoorFastBillboardProgramHandle : view.m_outdoorLitBillboardProgramHandle);
                if (VitaIndoorWork::active)
                {
                    VitaIndoorWork::counters.outActorDraws += batch.actorItems>0;
                    VitaIndoorWork::counters.outDecorationDraws += batch.decorationItems>0;
                }
                ++textureGroupCount;
                ++vitaBatchDraws; ++vitaDrawRanges[batch.distanceMask];

                if (OY_VITA_TRACE(view.m_gameSettings.performanceTrace))
                {
                    if (batch.actorItems > 0)
                    {
                        ++view.m_outdoorSpriteRenderDiagnostics.actorBatchSubmits;
                        ++view.m_outdoorSpriteRenderDiagnostics.actorSubmits;
                        view.m_outdoorSpriteRenderDiagnostics.actorBatchedItems += batch.actorItems;
                    }

                    if (batch.decorationItems > 0)
                    {
                        ++view.m_outdoorSpriteRenderDiagnostics.decorationBatchSubmits;
                        ++view.m_outdoorSpriteRenderDiagnostics.decorationSubmits;
                        view.m_outdoorSpriteRenderDiagnostics.decorationBatchedItems += batch.decorationItems;
                    }
                }
            }

            if (collectRenderDiagnostics)
            {
                submitStageNanoseconds += SDL_GetTicksNS() - submitStageStartTickCount;
            }
            resetBillboardBatch(batch);
        };

    thread_local CombinedBillboardBatch pendingBatch;
    resetBillboardBatch(pendingBatch);
    const auto submitBillboardBatch = [&](CombinedBillboardBatch &batch)
    {
        if (!batch.pTexture || batch.vertices.empty()) { resetBillboardBatch(batch); return; }
        ++vitaBatchInputs;
        // 16K vertices bounds transient staging and avoids creating a giant combined draw.
        if (pendingBatch.pTexture != batch.pTexture || pendingBatch.vertices.size() + batch.vertices.size() > 16384)
        {
            flushBillboardBatch(pendingBatch);
        }
        if (pendingBatch.vertices.empty())
        {
            pendingBatch.vertices.swap(batch.vertices);
            pendingBatch.pTexture = batch.pTexture;
        }
        else { pendingBatch.vertices.insert(pendingBatch.vertices.end(), batch.vertices.begin(), batch.vertices.end()); }
        pendingBatch.distanceMask |= batch.distanceMask;
        pendingBatch.actorItems += batch.actorItems;
        pendingBatch.decorationItems += batch.decorationItems;
        resetBillboardBatch(batch);
    };

    const auto appendBillboardVertices =
        [&](const BillboardDrawItem &drawItem, std::vector<OutdoorGameView::LitBillboardVertex> &vertices) -> bool
        {
            Engine::VitaDetailProfile::Scope geometry45(Engine::VitaDetailProfile::BillboardGeometry);
            const OutdoorGameView::BillboardTextureHandle *pTexture = drawItem.pTexture;

            if (pTexture == nullptr || !bgfx::isValid(pTexture->textureHandle))
            {
                return false;
            }

            const SpriteFrameEntry &frame = *drawItem.pFrame;
            const float spriteScale = std::max(frame.scale * drawItem.heightScale, 0.01f);
            const float worldWidth = static_cast<float>(pTexture->width) * spriteScale;
            const float worldHeight = static_cast<float>(pTexture->height) * spriteScale;
            const float halfWidth = worldWidth * 0.5f;
            const bx::Vec3 center = bottomAnchoredBillboardCenter(
                drawItem.x,
                drawItem.y,
                drawItem.z,
                cameraUp,
                worldHeight);
            const bx::Vec3 right = {
                cameraRight.x * halfWidth,
                cameraRight.y * halfWidth,
                cameraRight.z * halfWidth
            };
            const bx::Vec3 up = {
                cameraUp.x * worldHeight * 0.5f,
                cameraUp.y * worldHeight * 0.5f,
                cameraUp.z * worldHeight * 0.5f
            };
            const float u0 = drawItem.mirrored ? 1.0f : 0.0f;
            const float u1 = drawItem.mirrored ? 0.0f : 1.0f;

            appendLitBillboardVertices(vertices, center, right, up, u0, u1, drawItem.lightContributionAbgr);
            return true;
        };

    const auto appendDrawItemToBatch =
        [&](const BillboardDrawItem &drawItem, CombinedBillboardBatch &batch) -> bool
        {
            if (drawItem.pTexture == nullptr || !bgfx::isValid(drawItem.pTexture->textureHandle))
            {
                return false;
            }

            if (batch.pTexture == nullptr)
            {
                batch.pTexture = drawItem.pTexture;
            }

            if (!appendBillboardVertices(drawItem, batch.vertices))
            {
                return false;
            }

            batch.distanceMask |= drawItem.distanceSquared > 8192.0f * 8192.0f ? 2 : 1;
            batch.actorItems += drawItem.actor ? 1 : 0;
            batch.decorationItems += drawItem.decoration ? 1 : 0;
            return true;
        };

    const auto submitHoveredBillboard =
        [&](const BillboardDrawItem &drawItem)
        {
            flushBillboardBatch(pendingBatch);
            Engine::VitaDetailProfile::Scope geometry45(Engine::VitaDetailProfile::BillboardGeometry);
            const OutdoorGameView::BillboardTextureHandle *pTexture = drawItem.pTexture;

            if (pTexture == nullptr || !bgfx::isValid(pTexture->textureHandle))
            {
                return;
            }

            const SpriteFrameEntry &frame = *drawItem.pFrame;
            const float spriteScale = std::max(frame.scale * drawItem.heightScale, 0.01f);
            const float worldWidth = static_cast<float>(pTexture->width) * spriteScale;
            const float worldHeight = static_cast<float>(pTexture->height) * spriteScale;
            const float halfWidth = worldWidth * 0.5f;
            const bx::Vec3 center = bottomAnchoredBillboardCenter(
                drawItem.x,
                drawItem.y,
                drawItem.z,
                cameraUp,
                worldHeight);
            const bx::Vec3 right = {
                cameraRight.x * halfWidth,
                cameraRight.y * halfWidth,
                cameraRight.z * halfWidth
            };
            const bx::Vec3 up = {
                cameraUp.x * worldHeight * 0.5f,
                cameraUp.y * worldHeight * 0.5f,
                cameraUp.z * worldHeight * 0.5f
            };
            const float u0 = drawItem.mirrored ? 1.0f : 0.0f;
            const float u1 = drawItem.mirrored ? 0.0f : 1.0f;

            const float paddingU = HoveredActorOutlineThicknessPixels / static_cast<float>(pTexture->width);
            const float paddingV = HoveredActorOutlineThicknessPixels / static_cast<float>(pTexture->height);
            const float outlinedHalfWidth =
                (static_cast<float>(pTexture->width) * spriteScale
                    + HoveredActorOutlineThicknessPixels * 2.0f * spriteScale) * 0.5f;
            const float outlinedHalfHeight =
                (static_cast<float>(pTexture->height) * spriteScale
                    + HoveredActorOutlineThicknessPixels * 2.0f * spriteScale) * 0.5f;
            const bx::Vec3 outlineRight = {
                cameraRight.x * outlinedHalfWidth,
                cameraRight.y * outlinedHalfWidth,
                cameraRight.z * outlinedHalfWidth
            };
            const bx::Vec3 outlineUp = {
                cameraUp.x * outlinedHalfHeight,
                cameraUp.y * outlinedHalfHeight,
                cameraUp.z * outlinedHalfHeight
            };
            const float outlineU0 = drawItem.mirrored ? 1.0f + paddingU : -paddingU;
            const float outlineU1 = drawItem.mirrored ? -paddingU : 1.0f + paddingU;
            const float outlineVTop = -paddingV;
            const float outlineVBottom = 1.0f + paddingV;
            std::array<OutdoorGameView::LitBillboardVertex, 6> outlineVertices = {};
            outlineVertices[0] = {
                center.x - outlineRight.x - outlineUp.x,
                center.y - outlineRight.y - outlineUp.y,
                center.z - outlineRight.z - outlineUp.z,
                outlineU0,
                outlineVBottom,
                drawItem.lightContributionAbgr};
            outlineVertices[1] = {
                center.x - outlineRight.x + outlineUp.x,
                center.y - outlineRight.y + outlineUp.y,
                center.z - outlineRight.z + outlineUp.z,
                outlineU0,
                outlineVTop,
                drawItem.lightContributionAbgr};
            outlineVertices[2] = {
                center.x + outlineRight.x + outlineUp.x,
                center.y + outlineRight.y + outlineUp.y,
                center.z + outlineRight.z + outlineUp.z,
                outlineU1,
                outlineVTop,
                drawItem.lightContributionAbgr};
            outlineVertices[3] = outlineVertices[0];
            outlineVertices[4] = outlineVertices[2];
            outlineVertices[5] = {
                center.x + outlineRight.x - outlineUp.x,
                center.y + outlineRight.y - outlineUp.y,
                center.z + outlineRight.z - outlineUp.z,
                outlineU1,
                outlineVBottom,
                drawItem.lightContributionAbgr};

            if (bgfx::getAvailTransientVertexBuffer(vertexCount, OutdoorGameView::LitBillboardVertex::ms_layout)
                >= vertexCount)
            {
                const uint64_t outlineSubmitStageStartTickCount =
                    collectRenderDiagnostics ? SDL_GetTicksNS() : 0;
                bgfx::TransientVertexBuffer outlineTransientVertexBuffer = {};
                bgfx::allocTransientVertexBuffer(
                    &outlineTransientVertexBuffer,
                    vertexCount,
                    OutdoorGameView::LitBillboardVertex::ms_layout);
                std::memcpy(
                    outlineTransientVertexBuffer.data,
                    outlineVertices.data(),
                    static_cast<size_t>(outlineVertices.size() * sizeof(OutdoorGameView::LitBillboardVertex)));

                float outlineModelMatrix[16] = {};
                bx::mtxIdentity(outlineModelMatrix);
                bgfx::setTransform(outlineModelMatrix);
                bgfx::setVertexBuffer(0, &outlineTransientVertexBuffer, 0, vertexCount);
                bindTexture(
                    0,
                    view.m_terrainTextureSamplerHandle,
                    pTexture->textureHandle,
                    TextureFilterProfile::Billboard,
                    BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
                applyBillboardOverrideColorUniform(view, drawItem.hoveredOutlineColorAbgr, 1.0f);
                applyBillboardOutlineParamsUniform(
                    view,
                    1.0f / static_cast<float>(pTexture->width),
                    1.0f / static_cast<float>(pTexture->height),
                    HoveredActorOutlineThicknessPixels);
                standardBillboardEffectUniformsActive = false;
                bgfx::setState(BillboardAlphaRenderState);
                bgfx::submit(viewId,
            !view.settingsSnapshot().spriteOutline && bgfx::isValid(view.m_outdoorFastBillboardProgramHandle)
                ? view.m_outdoorFastBillboardProgramHandle : view.m_outdoorLitBillboardProgramHandle);
                if (collectRenderDiagnostics)
                {
                    submitStageNanoseconds += SDL_GetTicksNS() - outlineSubmitStageStartTickCount;
                }

                if (OY_VITA_TRACE(view.m_gameSettings.performanceTrace))
                {
                    if (drawItem.actor)
                    {
                        ++view.m_outdoorSpriteRenderDiagnostics.actorOutlineSubmits;
                    }
                    else if (drawItem.decoration)
                    {
                        ++view.m_outdoorSpriteRenderDiagnostics.decorationOutlineSubmits;
                    }
                }
            }

            std::array<OutdoorGameView::LitBillboardVertex, 6> vertices = {};
            writeLitBillboardVertices(
                vertices.data(),
                center,
                right,
                up,
                u0,
                u1,
                drawItem.lightContributionAbgr);

            if (bgfx::getAvailTransientVertexBuffer(vertexCount, OutdoorGameView::LitBillboardVertex::ms_layout)
                < vertexCount)
            {
                return;
            }

            const uint64_t submitStageStartTickCount = collectRenderDiagnostics ? SDL_GetTicksNS() : 0;
            bgfx::TransientVertexBuffer transientVertexBuffer = {};
            bgfx::allocTransientVertexBuffer(
                &transientVertexBuffer,
                vertexCount,
                OutdoorGameView::LitBillboardVertex::ms_layout);
            std::memcpy(
                transientVertexBuffer.data,
                vertices.data(),
                static_cast<size_t>(vertices.size() * sizeof(OutdoorGameView::LitBillboardVertex)));

            float modelMatrix[16] = {};
            bx::mtxIdentity(modelMatrix);
            bgfx::setTransform(modelMatrix);
            bgfx::setVertexBuffer(0, &transientVertexBuffer, 0, vertexCount);
            bindTexture(
                0,
                view.m_terrainTextureSamplerHandle,
                pTexture->textureHandle,
                TextureFilterProfile::Billboard,
                BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
            ensureStandardBillboardEffectUniforms();
            bgfx::setState(BillboardAlphaRenderState);
            bgfx::submit(viewId,
            !view.settingsSnapshot().spriteOutline && bgfx::isValid(view.m_outdoorFastBillboardProgramHandle)
                ? view.m_outdoorFastBillboardProgramHandle : view.m_outdoorLitBillboardProgramHandle);

            if (VitaIndoorWork::active)
            {
                VitaIndoorWork::counters.outActorDraws += drawItem.actor;
                VitaIndoorWork::counters.outDecorationDraws += drawItem.decoration;
            }
            ++textureGroupCount;

            if (OY_VITA_TRACE(view.m_gameSettings.performanceTrace))
            {
                if (drawItem.actor)
                {
                    ++view.m_outdoorSpriteRenderDiagnostics.actorSubmits;
                }
                else if (drawItem.decoration)
                {
                    ++view.m_outdoorSpriteRenderDiagnostics.decorationSubmits;
                }
            }
            if (collectRenderDiagnostics)
            {
                submitStageNanoseconds += SDL_GetTicksNS() - submitStageStartTickCount;
            }
        };

    const float depthSliceSize = outdoorBillboardDepthSliceSize(view);
    size_t drawItemIndex = 0;

    if (depthSliceSize <= 0.0f)
    {
        thread_local CombinedBillboardBatch exactBatch;
        resetBillboardBatch(exactBatch);
        exactBatch.vertices.reserve(drawItems.size() * 6);

        for (const BillboardDrawItem &drawItem : drawItems)
        {
            if (drawItem.hovered)
            {
                submitBillboardBatch(exactBatch);
                submitHoveredBillboard(drawItem);
                continue;
            }

            if (drawItem.pTexture == nullptr || !bgfx::isValid(drawItem.pTexture->textureHandle))
            {
                continue;
            }

            if (exactBatch.pTexture != drawItem.pTexture)
            {
                submitBillboardBatch(exactBatch);
                exactBatch.pTexture = drawItem.pTexture;
            }

            appendDrawItemToBatch(drawItem, exactBatch);
        }

        submitBillboardBatch(exactBatch);
    }
    else
    {
        while (drawItemIndex < drawItems.size())
        {
            const BillboardDrawItem &firstDrawItem = drawItems[drawItemIndex];

            if (firstDrawItem.hovered)
            {
                submitHoveredBillboard(firstDrawItem);
                ++drawItemIndex;
                continue;
            }

            const float sliceNearDepth = firstDrawItem.cameraDepth - depthSliceSize;
            size_t sliceItemCount = 0;
            size_t activeBillboardBatchCount = 0;

            while (drawItemIndex < drawItems.size())
            {
                const BillboardDrawItem &drawItem = drawItems[drawItemIndex];

                if (drawItem.hovered || drawItem.cameraDepth < sliceNearDepth)
                {
                    break;
                }

                Engine::VitaDetailProfile::Scope geometry45(Engine::VitaDetailProfile::BillboardGeometry);
            const OutdoorGameView::BillboardTextureHandle *pTexture = drawItem.pTexture;

                if (pTexture != nullptr && bgfx::isValid(pTexture->textureHandle))
                {
                    CombinedBillboardBatch *pBatch = nullptr;

                    for (size_t batchIndex = 0; batchIndex < activeBillboardBatchCount; ++batchIndex)
                    {
                        CombinedBillboardBatch &batch = billboardBatchPool[batchIndex];

                        if (batch.pTexture == pTexture)
                        {
                            pBatch = &batch;
                            break;
                        }
                    }

                    if (pBatch == nullptr)
                    {
                        if (activeBillboardBatchCount == billboardBatchPool.size())
                        {
                            billboardBatchPool.emplace_back();
                        }

                        pBatch = &billboardBatchPool[activeBillboardBatchCount];
                        ++activeBillboardBatchCount;
                        resetBillboardBatch(*pBatch);
                        pBatch->pTexture = pTexture;
                    }

                    if (appendDrawItemToBatch(drawItem, *pBatch))
                    {
                        ++sliceItemCount;
                    }
                }

                ++drawItemIndex;
            }

            if (OY_VITA_TRACE(view.m_gameSettings.performanceTrace) && sliceItemCount > 0)
            {
                ++view.m_outdoorSpriteRenderDiagnostics.combinedDepthSlices;
                view.m_outdoorSpriteRenderDiagnostics.combinedDepthSliceTextureGroups += activeBillboardBatchCount;
                view.m_outdoorSpriteRenderDiagnostics.combinedDepthSliceItems += sliceItemCount;
            }

            for (size_t batchIndex = 0; batchIndex < activeBillboardBatchCount; ++batchIndex)
            {
                submitBillboardBatch(billboardBatchPool[batchIndex]);
            }
        }
    }

    flushBillboardBatch(pendingBatch);
#if defined(OPENYAMM_VITA_PROFILE)
    Engine::VitaVisibility::counts.billboardDraws += vitaBatchDraws;
    vitaViewCosts.rendered = unsigned(drawItems.size());
    vitaViewCosts.actorsIn = vitaActors[0]; vitaViewCosts.actorsBeyond = vitaActors[1];
    vitaViewCosts.drawsIn = vitaDrawRanges[1]; vitaViewCosts.drawsBeyond = vitaDrawRanges[2];
    if (Engine::VitaProfile::frame.active)
    {
        Engine::VitaProfile::print("PROFILE DISTANCE n=%u actors_in=%u actors_beyond=%u decor_in=%u decor_beyond=%u "
            "billboard_draws_in=%u billboard_draws_beyond=%u billboard_draws_mixed=%u\n",
            Engine::VitaProfile::frame.index + 1, vitaActors[0], vitaActors[1], vitaDecor[0], vitaDecor[1],
            vitaDrawRanges[1], vitaDrawRanges[2], vitaDrawRanges[3]);
        Engine::VitaProfile::print("PROFILE BILLBOARD_BATCH n=%u visible_items=%u input_groups=%u draws=%u\n",
            Engine::VitaProfile::frame.index + 1, unsigned(drawItems.size()), vitaBatchInputs, vitaBatchDraws);
    }
#endif
    thread_local std::vector<OutdoorGameView::TerrainVertex> healthBarVertices;
    healthBarVertices.clear();

    for (const BillboardDrawItem &drawItem : drawItems)
    {
        if (!drawItem.hasHealthBar)
        {
            continue;
        }

        const float barScale = std::max(0.65f, drawItem.healthBarScale);
        const float barWidth = 92.0f * barScale;
        const float barHeight = 11.0f * barScale;
        const bx::Vec3 center = {drawItem.x, drawItem.y, drawItem.healthBarZ};
        const bx::Vec3 shadowCenter = {
            center.x - cameraUp.x * 3.0f * barScale,
            center.y - cameraUp.y * 3.0f * barScale,
            center.z - cameraUp.z * 3.0f * barScale
        };
        const bx::Vec3 frameRight = {
            cameraRight.x * barWidth * 0.5f,
            cameraRight.y * barWidth * 0.5f,
            cameraRight.z * barWidth * 0.5f
        };
        const bx::Vec3 frameUp = {
            cameraUp.x * barHeight * 0.5f,
            cameraUp.y * barHeight * 0.5f,
            cameraUp.z * barHeight * 0.5f
        };
        appendWorldQuadVertices(healthBarVertices, shadowCenter, frameRight, frameUp, makeAbgrAlpha(0, 0, 0, 150));
        appendWorldQuadVertices(healthBarVertices, center, frameRight, frameUp, makeAbgrAlpha(18, 12, 10, 230));

        const float innerWidth = std::max(2.0f, barWidth - 6.0f * barScale);
        const float innerHeight = std::max(2.0f, barHeight - 4.0f * barScale);
        const float fillWidth = std::max(1.0f, innerWidth * drawItem.healthRatio);
        const float fillOffset = (innerWidth - fillWidth) * 0.5f;
        const bx::Vec3 fillCenter = {
            center.x - cameraRight.x * fillOffset,
            center.y - cameraRight.y * fillOffset,
            center.z - cameraRight.z * fillOffset
        };
        const bx::Vec3 fillRight = {
            cameraRight.x * fillWidth * 0.5f,
            cameraRight.y * fillWidth * 0.5f,
            cameraRight.z * fillWidth * 0.5f
        };
        const bx::Vec3 fillUp = {
            cameraUp.x * innerHeight * 0.5f,
            cameraUp.y * innerHeight * 0.5f,
            cameraUp.z * innerHeight * 0.5f
        };
        appendWorldQuadVertices(healthBarVertices, fillCenter, fillRight, fillUp, makeAbgrAlpha(177, 25, 28, 245));

        const bx::Vec3 glossCenter = {
            fillCenter.x + cameraUp.x * innerHeight * 0.22f,
            fillCenter.y + cameraUp.y * innerHeight * 0.22f,
            fillCenter.z + cameraUp.z * innerHeight * 0.22f
        };
        const bx::Vec3 glossUp = {
            cameraUp.x * innerHeight * 0.16f,
            cameraUp.y * innerHeight * 0.16f,
            cameraUp.z * innerHeight * 0.16f
        };
        appendWorldQuadVertices(healthBarVertices, glossCenter, fillRight, glossUp, makeAbgrAlpha(255, 108, 82, 155));
    }

    submitColoredVertices(view, viewId, healthBarVertices, ColoredAlphaRenderState);

    struct QuestMarkerBatch
    {
        Mm9QuestMarkerState state = Mm9QuestMarkerState::None;
        const OutdoorGameView::BillboardTextureHandle *pTexture = nullptr;
        std::vector<OutdoorGameView::LitBillboardVertex> vertices;
    };

    thread_local std::array<QuestMarkerBatch, 3> questMarkerBatches = {{
        {Mm9QuestMarkerState::Available},
        {Mm9QuestMarkerState::InProgress},
        {Mm9QuestMarkerState::Ready},
    }};

    for (auto &batch : questMarkerBatches) { batch.vertices.clear(); batch.pTexture = nullptr; }
    const bool hasVisibleQuestMarker =
        view.settingsSnapshot().questMarkers
        && std::any_of(
            drawItems.begin(),
            drawItems.end(),
            [&view](const BillboardDrawItem &drawItem)
            {
                return drawItem.actor
                    && drawItem.actorIndex != static_cast<size_t>(-1)
                    && questMarkerAlpha(std::sqrt(drawItem.distanceSquared)) > 0
                    && !questMarkerTextureName(view.m_gameSession.questMarkerForActor(drawItem.actorIndex)).empty();
            });

    if (hasVisibleQuestMarker)
    {
        ensureSpriteBillboardTexture(view, "quest_marker_exclamation", 0, "quest_marker");
        ensureSpriteBillboardTexture(view, "quest_marker_question", 0, "quest_marker");

        for (QuestMarkerBatch &batch : questMarkerBatches)
        {
            batch.pTexture = view.findBillboardTexture(std::string(questMarkerTextureName(batch.state)), 0);
        }

        for (const BillboardDrawItem &drawItem : drawItems)
        {
            if (!drawItem.actor || drawItem.actorIndex == static_cast<size_t>(-1))
            {
                continue;
            }
            const Mm9QuestMarkerState markerState = view.m_gameSession.questMarkerForActor(drawItem.actorIndex);
            const float distance = std::sqrt(drawItem.distanceSquared);
            const uint8_t alpha = questMarkerAlpha(distance);
            const auto batchIterator = std::find_if(
                questMarkerBatches.begin(),
                questMarkerBatches.end(),
                [markerState](const QuestMarkerBatch &batch)
                {
                    return batch.state == markerState;
                });

            if (alpha == 0
                || batchIterator == questMarkerBatches.end()
                || batchIterator->pTexture == nullptr
                || !bgfx::isValid(batchIterator->pTexture->textureHandle))
            {
                continue;
            }

            const float scale = questMarkerWorldScale(distance);
            const float halfExtent = questMarkerHalfExtent(scale);
            const bx::Vec3 center = {
                drawItem.x,
                drawItem.y,
                drawItem.questMarkerZ + questMarkerOriginOffset(scale),
            };
            const bx::Vec3 right = {
                cameraRight.x * halfExtent,
                cameraRight.y * halfExtent,
                cameraRight.z * halfExtent,
            };
            const bx::Vec3 up = {
                cameraUp.x * halfExtent,
                cameraUp.y * halfExtent,
                cameraUp.z * halfExtent,
            };
            appendLitBillboardVertices(
                batchIterator->vertices,
                center,
                right,
                up,
                0.0f,
                1.0f,
                makeAbgrAlpha(0, 0, 0, alpha));
        }
    }

    for (const QuestMarkerBatch &batch : questMarkerBatches)
    {
        if (batch.pTexture == nullptr
            || batch.vertices.empty()
            || bgfx::getAvailTransientVertexBuffer(
                static_cast<uint32_t>(batch.vertices.size()),
                OutdoorGameView::LitBillboardVertex::ms_layout) < batch.vertices.size())
        {
            continue;
        }

        bgfx::TransientVertexBuffer transientVertexBuffer = {};
        bgfx::allocTransientVertexBuffer(
            &transientVertexBuffer,
            static_cast<uint32_t>(batch.vertices.size()),
            OutdoorGameView::LitBillboardVertex::ms_layout);
        std::memcpy(
            transientVertexBuffer.data,
            batch.vertices.data(),
            batch.vertices.size() * sizeof(OutdoorGameView::LitBillboardVertex));

        const uint32_t colorAbgr = questMarkerColorAbgr(batch.state, 255);
        const float ambient[4] = {
            redChannel(colorAbgr),
            greenChannel(colorAbgr),
            blueChannel(colorAbgr),
            1.0f,
        };
        const float clearUniform[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        const float fogDistances[4] = {4096.0f, 4096.0f, 4096.0f, 0.0f};
        float modelMatrix[16] = {};
        bx::mtxIdentity(modelMatrix);
        bgfx::setTransform(modelMatrix);
        bgfx::setVertexBuffer(0, &transientVertexBuffer, 0, static_cast<uint32_t>(batch.vertices.size()));
        bindTexture(
            0,
            view.m_terrainTextureSamplerHandle,
            batch.pTexture->textureHandle,
            TextureFilterProfile::Billboard,
            BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
        bgfx::setUniform(view.m_outdoorBillboardAmbientUniformHandle, ambient);
        bgfx::setUniform(view.m_outdoorBillboardOverrideColorUniformHandle, clearUniform);
        bgfx::setUniform(view.m_outdoorBillboardOutlineParamsUniformHandle, clearUniform);
        bgfx::setUniform(view.m_outdoorFogColorUniformHandle, clearUniform);
        bgfx::setUniform(view.m_outdoorFogDensitiesUniformHandle, clearUniform);
        bgfx::setUniform(view.m_outdoorFogDistancesUniformHandle, fogDistances);
        bgfx::setState(ColoredAlphaRenderState);
        bgfx::submit(viewId,
            !view.settingsSnapshot().spriteOutline && bgfx::isValid(view.m_outdoorFastBillboardProgramHandle)
                ? view.m_outdoorFastBillboardProgramHandle : view.m_outdoorLitBillboardProgramHandle);
    }

    const uint64_t totalActorRenderNanoseconds =
        collectRenderDiagnostics ? SDL_GetTicksNS() - actorRenderStartTickCount : 0;

    if (collectRenderDiagnostics && totalActorRenderNanoseconds >= renderHitchLogThresholdNanoseconds)
    {
        std::cout << "[OutdoorActorRenderHitchDetail]"
                  << " total_us=" << totalActorRenderNanoseconds / 1000ULL
                  << " gather_us=" << gatherStageNanoseconds / 1000ULL
                  << " placeholder_us=" << placeholderStageNanoseconds / 1000ULL
                  << " sort_us=" << sortStageNanoseconds / 1000ULL
                  << " submit_us=" << submitStageNanoseconds / 1000ULL
                  << " draw_items=" << drawItems.size()
                  << " placeholder_vertices=" << placeholderVertices.size()
                  << " texture_groups=" << textureGroupCount
                  << " ensured_loads=" << ensuredTextureLoadCount
                  << '\n';
    }
}

void OutdoorBillboardRenderer::renderRuntimeWorldItems(
    OutdoorGameView &view,
    uint16_t viewId,
    const float *pViewMatrix,
    const bx::Vec3 &cameraPosition)
{
    Engine::VitaDetailProfile::Scope detailScope(Engine::VitaDetailProfile::RenderItems);
    Engine::VitaProfile::Scope vitaScope(Engine::VitaProfile::WorldItems);
    if (view.m_pOutdoorWorldRuntime == nullptr)
    {
        return;
    }

    if (!bgfx::isValid(view.m_outdoorLitBillboardProgramHandle)
        || !bgfx::isValid(view.m_terrainTextureSamplerHandle)
        || !bgfx::isValid(view.m_outdoorBillboardOverrideColorUniformHandle)
        || !bgfx::isValid(view.m_outdoorBillboardOutlineParamsUniformHandle)
        || !bgfx::isValid(view.m_outdoorBillboardAmbientUniformHandle))
    {
        return;
    }

    const SpriteFrameTable *pSpriteFrameTable = nullptr;

    if (view.m_outdoorSpriteObjectBillboardSet)
    {
        pSpriteFrameTable = &view.m_outdoorSpriteObjectBillboardSet->spriteFrameTable;
    }
    else if (view.m_outdoorActorPreviewBillboardSet)
    {
        pSpriteFrameTable = &view.m_outdoorActorPreviewBillboardSet->spriteFrameTable;
    }

    if (pSpriteFrameTable == nullptr)
    {
        return;
    }

    const bx::Vec3 cameraRight = {pViewMatrix[0], pViewMatrix[4], pViewMatrix[8]};
    const bx::Vec3 cameraUp = {pViewMatrix[1], pViewMatrix[5], pViewMatrix[9]};
    applyBillboardFogUniforms(view, view.m_viewDistanceCache.actorBillboardDistance);

    struct BillboardDrawItem
    {
        const OutdoorWorldRuntime::WorldItemState *pWorldItem = nullptr;
        const SpriteFrameEntry *pFrame = nullptr;
        const OutdoorGameView::BillboardTextureHandle *pTexture = nullptr;
        bool mirrored = false;
        bool hovered = false;
        uint32_t hoveredOutlineColorAbgr = 0;
        float distanceSquared = 0.0f;
        float distance = 0.0f;
        uint32_t lightContributionAbgr = 0xff000000u;
    };

    std::vector<BillboardDrawItem> drawItems;
    drawItems.reserve(view.m_pOutdoorWorldRuntime->worldItemCount());
    const bool spriteOutlineEnabled = view.settingsSnapshot().spriteOutline;
    const std::optional<OutdoorGameView::InspectHit> hoveredInspectHit =
        (spriteOutlineEnabled || view.settingsSnapshot().selectionTint)
            ? resolveHoveredOutlineInspectHit(view, pViewMatrix) : std::nullopt;
    std::optional<size_t> hoveredWorldItemIndex;

    if (hoveredInspectHit && hoveredInspectHit->kind == "world_item")
    {
        hoveredWorldItemIndex = hoveredInspectHit->bModelIndex;
    }

    const GameplayWorldHit *pContextActionHit = nullptr;
    if (view.settingsSnapshot().contextActionPopup)
    {
        pContextActionHit =
            selectedContextActionWorldHit(view.m_gameSession.gameplayScreenRuntime().contextActionStateReadOnly());
    }

    for (size_t worldItemIndex = 0; worldItemIndex < view.m_pOutdoorWorldRuntime->worldItemCount(); ++worldItemIndex)
    {
        const OutdoorWorldRuntime::WorldItemState *pWorldItem = view.m_pOutdoorWorldRuntime->worldItemState(worldItemIndex);

        if (pWorldItem == nullptr)
        {
            continue;
        }

        uint16_t spriteFrameIndex = pWorldItem->objectSpriteFrameIndex;

        if (spriteFrameIndex == 0 && !pWorldItem->objectSpriteName.empty())
        {
            const std::optional<uint16_t> spriteFrameIndexByName =
                pSpriteFrameTable->findFrameIndexBySpriteName(pWorldItem->objectSpriteName);

            if (spriteFrameIndexByName)
            {
                spriteFrameIndex = *spriteFrameIndexByName;
            }
        }

        if (spriteFrameIndex == 0)
        {
            spriteFrameIndex = pWorldItem->objectSpriteId;
        }

        if (spriteFrameIndex == 0)
        {
            continue;
        }

        const SpriteFrameEntry *pFrame = pSpriteFrameTable->getFrame(spriteFrameIndex, pWorldItem->timeSinceCreatedTicks);

        if (pFrame == nullptr)
        {
            continue;
        }

        const ResolvedSpriteTexture resolvedTexture = SpriteFrameTable::resolveTexture(*pFrame, 0);
        const OutdoorGameView::BillboardTextureHandle *pTexture =
            ensureSpriteBillboardTexture(view, resolvedTexture.textureName, pFrame->paletteId);

        if (pTexture == nullptr || !bgfx::isValid(pTexture->textureHandle))
        {
            continue;
        }

        const float deltaX = pWorldItem->x - cameraPosition.x;
        const float deltaY = pWorldItem->y - cameraPosition.y;
        const float deltaZ = pWorldItem->z - cameraPosition.z;

        BillboardDrawItem drawItem = {};
        drawItem.pWorldItem = pWorldItem;
        drawItem.pFrame = pFrame;
        drawItem.pTexture = pTexture;
        drawItem.mirrored = resolvedTexture.mirrored;
        const bool contextHighlighted = contextActionHighlightsWorldItem(pContextActionHit, worldItemIndex);
        drawItem.hovered =
            view.settingsSnapshot().spriteOutline && (contextHighlighted
            || (hoveredWorldItemIndex.has_value() && *hoveredWorldItemIndex == worldItemIndex));
        drawItem.hoveredOutlineColorAbgr =
            drawItem.hovered
                ? (contextHighlighted ? contextActionHighlightOutlineColor() : hoveredWorldItemOutlineColor())
                : 0;
        drawItem.distanceSquared = deltaX * deltaX + deltaY * deltaY + deltaZ * deltaZ;
        drawItem.distance = std::sqrt(drawItem.distanceSquared);
        drawItem.lightContributionAbgr = computeBillboardLightContributionAbgr(
            view,
            pWorldItem->x,
            pWorldItem->y,
            pWorldItem->z + static_cast<float>(pTexture->height) * std::max(pFrame->scale, 0.01f) * 0.5f);
        drawItems.push_back(drawItem);
    }

    std::sort(
        drawItems.begin(),
        drawItems.end(),
        [](const BillboardDrawItem &left, const BillboardDrawItem &right)
        {
            return left.distance > right.distance;
        });

    if (OY_VITA_TRACE(view.m_gameSettings.performanceTrace))
    {
        view.m_outdoorSpriteRenderDiagnostics.worldItemItems += drawItems.size();

        const OutdoorGameView::BillboardTextureHandle *pLastTexture = nullptr;
        for (const BillboardDrawItem &drawItem : drawItems)
        {
            if (drawItem.pTexture != pLastTexture)
            {
                ++view.m_outdoorSpriteRenderDiagnostics.worldItemTextureSwitches;
                pLastTexture = drawItem.pTexture;
            }
        }
    }

    const float depthSliceSize = outdoorBillboardDepthSliceSize(view);
    size_t worldItemSliceBegin = 0;

    while (depthSliceSize > 0.0f && worldItemSliceBegin < drawItems.size())
    {
        if (drawItems[worldItemSliceBegin].hovered)
        {
            ++worldItemSliceBegin;
            continue;
        }

        const float sliceNearDistance = drawItems[worldItemSliceBegin].distance - depthSliceSize;
        size_t worldItemSliceEnd = worldItemSliceBegin + 1;

        while (worldItemSliceEnd < drawItems.size()
            && !drawItems[worldItemSliceEnd].hovered
            && drawItems[worldItemSliceEnd].distance >= sliceNearDistance)
        {
            ++worldItemSliceEnd;
        }

        std::stable_sort(
            drawItems.begin() + static_cast<std::ptrdiff_t>(worldItemSliceBegin),
            drawItems.begin() + static_cast<std::ptrdiff_t>(worldItemSliceEnd),
            [](const BillboardDrawItem &left, const BillboardDrawItem &right)
            {
                const uint16_t leftTextureId = left.pTexture != nullptr ? left.pTexture->textureHandle.idx : 0;
                const uint16_t rightTextureId = right.pTexture != nullptr ? right.pTexture->textureHandle.idx : 0;

                if (leftTextureId != rightTextureId)
                {
                    return leftTextureId < rightTextureId;
                }

                return left.distance > right.distance;
            });

        if (OY_VITA_TRACE(view.m_gameSettings.performanceTrace))
        {
            ++view.m_outdoorSpriteRenderDiagnostics.worldItemDepthSlices;
            view.m_outdoorSpriteRenderDiagnostics.worldItemDepthSliceItems +=
                worldItemSliceEnd - worldItemSliceBegin;

            const OutdoorGameView::BillboardTextureHandle *pLastTexture = nullptr;
            for (size_t index = worldItemSliceBegin; index < worldItemSliceEnd; ++index)
            {
                if (drawItems[index].pTexture != pLastTexture)
                {
                    ++view.m_outdoorSpriteRenderDiagnostics.worldItemDepthSliceTextureGroups;
                    pLastTexture = drawItems[index].pTexture;
                }
            }
        }

        worldItemSliceBegin = worldItemSliceEnd;
    }

    struct WorldItemBillboardBatch
    {
        const OutdoorGameView::BillboardTextureHandle *pTexture = nullptr;
        std::vector<OutdoorGameView::LitBillboardVertex> vertices;
        size_t itemCount = 0;
    };

    WorldItemBillboardBatch billboardBatch = {};
    billboardBatch.vertices.reserve(drawItems.size() * 6);

    const auto flushBillboardBatch =
        [&]()
        {
            if (billboardBatch.pTexture == nullptr || billboardBatch.vertices.empty())
            {
                billboardBatch.vertices.clear();
                billboardBatch.pTexture = nullptr;
                billboardBatch.itemCount = 0;
                return;
            }

            const uint32_t batchVertexCount = static_cast<uint32_t>(billboardBatch.vertices.size());

            if (bgfx::getAvailTransientVertexBuffer(
                    batchVertexCount,
                    OutdoorGameView::LitBillboardVertex::ms_layout) >= batchVertexCount)
            {
                bgfx::TransientVertexBuffer transientVertexBuffer = {};
                bgfx::allocTransientVertexBuffer(
                    &transientVertexBuffer,
                    batchVertexCount,
                    OutdoorGameView::LitBillboardVertex::ms_layout);
                std::memcpy(
                    transientVertexBuffer.data,
                    billboardBatch.vertices.data(),
                    static_cast<size_t>(
                        billboardBatch.vertices.size() * sizeof(OutdoorGameView::LitBillboardVertex)));

                float modelMatrix[16] = {};
                bx::mtxIdentity(modelMatrix);
                bgfx::setTransform(modelMatrix);
                bgfx::setVertexBuffer(0, &transientVertexBuffer, 0, batchVertexCount);
                bindTexture(
                    0,
                    view.m_terrainTextureSamplerHandle,
                    billboardBatch.pTexture->textureHandle,
                    TextureFilterProfile::Billboard,
                    BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
                applyBillboardAmbientUniform(view);
                applyBillboardOverrideColorUniform(view, 0, 0.0f);
                applyBillboardOutlineParamsUniform(view, 0.0f, 0.0f, 0.0f);
                bgfx::setState(BillboardAlphaRenderState);
                bgfx::submit(viewId,
            !view.settingsSnapshot().spriteOutline && bgfx::isValid(view.m_outdoorFastBillboardProgramHandle)
                ? view.m_outdoorFastBillboardProgramHandle : view.m_outdoorLitBillboardProgramHandle);

                if (OY_VITA_TRACE(view.m_gameSettings.performanceTrace))
                {
                    ++view.m_outdoorSpriteRenderDiagnostics.worldItemBatchSubmits;
                    ++view.m_outdoorSpriteRenderDiagnostics.worldItemSubmits;
                    view.m_outdoorSpriteRenderDiagnostics.worldItemBatchedItems += billboardBatch.itemCount;
                }
            }

            billboardBatch.vertices.clear();
            billboardBatch.pTexture = nullptr;
            billboardBatch.itemCount = 0;
        };

    for (const BillboardDrawItem &drawItem : drawItems)
    {
        const OutdoorWorldRuntime::WorldItemState &worldItem = *drawItem.pWorldItem;
        const SpriteFrameEntry &frame = *drawItem.pFrame;
        const OutdoorGameView::BillboardTextureHandle &texture = *drawItem.pTexture;
        const float spriteScale = std::max(frame.scale, 0.01f);
        const float worldWidth = static_cast<float>(texture.width) * spriteScale;
        const float worldHeight = static_cast<float>(texture.height) * spriteScale;
        const float halfWidth = worldWidth * 0.5f;
        const bx::Vec3 center = bottomAnchoredBillboardCenter(
            worldItem.x,
            worldItem.y,
            worldItem.z,
            cameraUp,
            worldHeight);
        const bx::Vec3 right = {cameraRight.x * halfWidth, cameraRight.y * halfWidth, cameraRight.z * halfWidth};
        const bx::Vec3 up = {
            cameraUp.x * worldHeight * 0.5f,
            cameraUp.y * worldHeight * 0.5f,
            cameraUp.z * worldHeight * 0.5f
        };
        const float u0 = drawItem.mirrored ? 1.0f : 0.0f;
        const float u1 = drawItem.mirrored ? 0.0f : 1.0f;

        if (drawItem.hovered)
        {
            flushBillboardBatch();

            const float paddingU = HoveredActorOutlineThicknessPixels / static_cast<float>(texture.width);
            const float paddingV = HoveredActorOutlineThicknessPixels / static_cast<float>(texture.height);
            const float outlinedHalfWidth =
                (static_cast<float>(texture.width) * spriteScale
                    + HoveredActorOutlineThicknessPixels * 2.0f * spriteScale) * 0.5f;
            const float outlinedHalfHeight =
                (static_cast<float>(texture.height) * spriteScale
                    + HoveredActorOutlineThicknessPixels * 2.0f * spriteScale) * 0.5f;
            const bx::Vec3 outlineRight = {
                cameraRight.x * outlinedHalfWidth,
                cameraRight.y * outlinedHalfWidth,
                cameraRight.z * outlinedHalfWidth
            };
            const bx::Vec3 outlineUp = {
                cameraUp.x * outlinedHalfHeight,
                cameraUp.y * outlinedHalfHeight,
                cameraUp.z * outlinedHalfHeight
            };
            const float outlineU0 = drawItem.mirrored ? 1.0f + paddingU : -paddingU;
            const float outlineU1 = drawItem.mirrored ? -paddingU : 1.0f + paddingU;
            const float outlineVTop = -paddingV;
            const float outlineVBottom = 1.0f + paddingV;
            std::array<OutdoorGameView::LitBillboardVertex, 6> outlineVertices = {};
            outlineVertices[0] = {
                center.x - outlineRight.x - outlineUp.x,
                center.y - outlineRight.y - outlineUp.y,
                center.z - outlineRight.z - outlineUp.z,
                outlineU0,
                outlineVBottom,
                drawItem.lightContributionAbgr};
            outlineVertices[1] = {
                center.x - outlineRight.x + outlineUp.x,
                center.y - outlineRight.y + outlineUp.y,
                center.z - outlineRight.z + outlineUp.z,
                outlineU0,
                outlineVTop,
                drawItem.lightContributionAbgr};
            outlineVertices[2] = {
                center.x + outlineRight.x + outlineUp.x,
                center.y + outlineRight.y + outlineUp.y,
                center.z + outlineRight.z + outlineUp.z,
                outlineU1,
                outlineVTop,
                drawItem.lightContributionAbgr};
            outlineVertices[3] = outlineVertices[0];
            outlineVertices[4] = outlineVertices[2];
            outlineVertices[5] = {
                center.x + outlineRight.x - outlineUp.x,
                center.y + outlineRight.y - outlineUp.y,
                center.z + outlineRight.z - outlineUp.z,
                outlineU1,
                outlineVBottom,
                drawItem.lightContributionAbgr};

            if (bgfx::getAvailTransientVertexBuffer(
                    static_cast<uint32_t>(outlineVertices.size()),
                    OutdoorGameView::LitBillboardVertex::ms_layout) >= outlineVertices.size())
            {
                bgfx::TransientVertexBuffer outlineTransientVertexBuffer = {};
                bgfx::allocTransientVertexBuffer(
                    &outlineTransientVertexBuffer,
                    static_cast<uint32_t>(outlineVertices.size()),
                    OutdoorGameView::LitBillboardVertex::ms_layout);
                std::memcpy(
                    outlineTransientVertexBuffer.data,
                    outlineVertices.data(),
                    static_cast<size_t>(outlineVertices.size() * sizeof(OutdoorGameView::LitBillboardVertex)));

                float outlineModelMatrix[16] = {};
                bx::mtxIdentity(outlineModelMatrix);
                bgfx::setTransform(outlineModelMatrix);
                bgfx::setVertexBuffer(
                    0,
                    &outlineTransientVertexBuffer,
                    0,
                    static_cast<uint32_t>(outlineVertices.size()));
                bindTexture(
                    0,
                    view.m_terrainTextureSamplerHandle,
                    texture.textureHandle,
                    TextureFilterProfile::Billboard,
                    BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
                applyBillboardAmbientUniform(view);
                applyBillboardOverrideColorUniform(view, drawItem.hoveredOutlineColorAbgr, 1.0f);
                applyBillboardOutlineParamsUniform(
                    view,
                    1.0f / static_cast<float>(texture.width),
                    1.0f / static_cast<float>(texture.height),
                    HoveredActorOutlineThicknessPixels);
                bgfx::setState(BillboardAlphaRenderState);
                bgfx::submit(viewId,
            !view.settingsSnapshot().spriteOutline && bgfx::isValid(view.m_outdoorFastBillboardProgramHandle)
                ? view.m_outdoorFastBillboardProgramHandle : view.m_outdoorLitBillboardProgramHandle);

                if (OY_VITA_TRACE(view.m_gameSettings.performanceTrace))
                {
                    ++view.m_outdoorSpriteRenderDiagnostics.worldItemOutlineSubmits;
                }
            }
        }

        std::array<OutdoorGameView::LitBillboardVertex, 6> vertices = {};
        writeLitBillboardVertices(
            vertices.data(),
            center,
            right,
            up,
            u0,
            u1,
            drawItem.lightContributionAbgr);

        if (!drawItem.hovered)
        {
            if (billboardBatch.pTexture != &texture)
            {
                flushBillboardBatch();
                billboardBatch.pTexture = &texture;
            }

            billboardBatch.vertices.insert(billboardBatch.vertices.end(), vertices.begin(), vertices.end());
            ++billboardBatch.itemCount;
            continue;
        }

        if (bgfx::getAvailTransientVertexBuffer(
                static_cast<uint32_t>(vertices.size()),
                OutdoorGameView::LitBillboardVertex::ms_layout) < vertices.size())
        {
            continue;
        }

        bgfx::TransientVertexBuffer transientVertexBuffer = {};
        bgfx::allocTransientVertexBuffer(
            &transientVertexBuffer,
            static_cast<uint32_t>(vertices.size()),
            OutdoorGameView::LitBillboardVertex::ms_layout);
        std::memcpy(
            transientVertexBuffer.data,
            vertices.data(),
            static_cast<size_t>(vertices.size() * sizeof(OutdoorGameView::LitBillboardVertex)));

        float modelMatrix[16] = {};
        bx::mtxIdentity(modelMatrix);
        bgfx::setTransform(modelMatrix);
        bgfx::setVertexBuffer(0, &transientVertexBuffer, 0, static_cast<uint32_t>(vertices.size()));
        bindTexture(
            0,
            view.m_terrainTextureSamplerHandle,
            texture.textureHandle,
            TextureFilterProfile::Billboard,
            BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
        applyBillboardAmbientUniform(view);
        applyBillboardOverrideColorUniform(view, 0, 0.0f);
        applyBillboardOutlineParamsUniform(view, 0.0f, 0.0f, 0.0f);
        bgfx::setState(BillboardAlphaRenderState);
        bgfx::submit(viewId,
            !view.settingsSnapshot().spriteOutline && bgfx::isValid(view.m_outdoorFastBillboardProgramHandle)
                ? view.m_outdoorFastBillboardProgramHandle : view.m_outdoorLitBillboardProgramHandle);

        if (OY_VITA_TRACE(view.m_gameSettings.performanceTrace))
        {
            ++view.m_outdoorSpriteRenderDiagnostics.worldItemSubmits;
        }
    }

    flushBillboardBatch();
}

void OutdoorBillboardRenderer::renderRuntimeProjectiles(
    OutdoorGameView &view,
    uint16_t viewId,
    const float *pViewMatrix,
    const bx::Vec3 &cameraPosition)
{
    Engine::VitaDetailProfile::Scope detailScope(Engine::VitaDetailProfile::RenderProjectiles);
    Engine::VitaProfile::Scope vitaScope(Engine::VitaProfile::WorldProjectiles);
    if (view.m_pOutdoorWorldRuntime == nullptr)
    {
        return;
    }

    if (!bgfx::isValid(view.m_outdoorLitBillboardProgramHandle)
        || !bgfx::isValid(view.m_terrainTextureSamplerHandle)
        || !bgfx::isValid(view.m_outdoorBillboardOverrideColorUniformHandle)
        || !bgfx::isValid(view.m_outdoorBillboardOutlineParamsUniformHandle)
        || !bgfx::isValid(view.m_outdoorBillboardAmbientUniformHandle))
    {
        return;
    }

    const SpriteFrameTable *pSpriteFrameTable = nullptr;

    if (view.m_outdoorSpriteObjectBillboardSet)
    {
        pSpriteFrameTable = &view.m_outdoorSpriteObjectBillboardSet->spriteFrameTable;
    }
    else if (view.m_outdoorActorPreviewBillboardSet)
    {
        pSpriteFrameTable = &view.m_outdoorActorPreviewBillboardSet->spriteFrameTable;
    }

    if (pSpriteFrameTable == nullptr)
    {
        return;
    }

    const bx::Vec3 cameraRight = {pViewMatrix[0], pViewMatrix[4], pViewMatrix[8]};
    const bx::Vec3 cameraUp = {pViewMatrix[1], pViewMatrix[5], pViewMatrix[9]};
    applyBillboardFogUniforms(view, view.m_viewDistanceCache.runtimeProjectileDistance);

    struct BillboardDrawItem
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        const SpriteFrameEntry *pFrame = nullptr;
        const OutdoorGameView::BillboardTextureHandle *pTexture = nullptr;
        bool mirrored = false;
        float distanceSquared = 0.0f;
        uint32_t lightContributionAbgr = 0xff000000u;
    };

    std::vector<BillboardDrawItem> drawItems;
    const std::vector<GameplayProjectilePresentationState> &projectiles =
        view.m_gameSession.gameplayFxService().activeProjectilePresentationStates();
    const std::vector<GameplayProjectileImpactPresentationState> &impacts =
        view.m_gameSession.gameplayFxService().activeProjectileImpactPresentationStates();
    drawItems.reserve(
        projectiles.size()
        + impacts.size()
        + view.m_pOutdoorWorldRuntime->fireSpikeTrapCount());

    auto appendRuntimeDrawItem =
        [&](uint32_t runtimeId,
            const char *kind,
            float x,
            float y,
            float z,
            float velocityX,
            float velocityY,
            uint16_t cachedSpriteFrameIndex,
            uint16_t spriteId,
            const std::string &spriteName,
            uint32_t timeTicks,
            bool forceLog = false)
        {
            uint16_t spriteFrameIndex = cachedSpriteFrameIndex;
            const bool shouldLog = forceLog || (DebugProjectileDrawLogging && timeTicks <= 16);
            const char *pResolutionSource = cachedSpriteFrameIndex != 0 ? "cached" : "none";
            const float deltaX = x - cameraPosition.x;
            const float deltaY = y - cameraPosition.y;
            const float deltaZ = z - cameraPosition.z;
            const float distanceSquared = deltaX * deltaX + deltaY * deltaY + deltaZ * deltaZ;

            if (distanceSquared > view.m_viewDistanceCache.runtimeProjectileDistanceSquared)
            {
                if (shouldLog)
                {
                    std::cout << "Projectile draw skipped kind=" << kind
                              << " id=" << runtimeId
                              << " sprite=\"" << (spriteName.empty() ? "<none>" : spriteName) << "\""
                              << " spriteId=" << spriteId
                              << " distanceSquared=" << distanceSquared
                              << " reason=too_far\n";
                }
                return;
            }

            if (spriteFrameIndex == 0 && !spriteName.empty())
            {
                const std::optional<uint16_t> spriteFrameIndexByName =
                    pSpriteFrameTable->findFrameIndexBySpriteName(spriteName);

                if (spriteFrameIndexByName)
                {
                    spriteFrameIndex = *spriteFrameIndexByName;
                    pResolutionSource = "name";
                }
            }

            if (spriteFrameIndex == 0)
            {
                spriteFrameIndex = spriteId;
                pResolutionSource = "id";
            }

            if (spriteFrameIndex == 0)
            {
                if (shouldLog)
                {
                    std::cout << "Projectile draw skipped kind=" << kind
                              << " id=" << runtimeId
                              << " sprite=\"" << (spriteName.empty() ? "<none>" : spriteName) << "\""
                              << " spriteId=" << spriteId
                              << " reason=no_frame_index\n";
                }
                return;
            }

            const SpriteFrameEntry *pFrame = pSpriteFrameTable->getFrame(spriteFrameIndex, timeTicks);

            if (pFrame == nullptr)
            {
                if (shouldLog)
                {
                    std::cout << "Projectile draw skipped kind=" << kind
                              << " id=" << runtimeId
                              << " sprite=\"" << (spriteName.empty() ? "<none>" : spriteName) << "\""
                              << " spriteId=" << spriteId
                              << " frameIndex=" << spriteFrameIndex
                              << " source=" << pResolutionSource
                              << " reason=frame_missing\n";
                }
                return;
            }

            const float velocityLengthSquared = velocityX * velocityX + velocityY * velocityY;
            int octant = 0;

            if (velocityLengthSquared > 0.000001f)
            {
                const float angleToCamera = std::atan2(y - cameraPosition.y, x - cameraPosition.x);
                const float projectileYawRadians = std::atan2(velocityY, velocityX);
                const float octantAngle = projectileYawRadians - angleToCamera + Pi + (Pi / 8.0f);
                octant = static_cast<int>(std::floor(octantAngle / (Pi / 4.0f))) & 7;
            }

            const ResolvedSpriteTexture resolvedTexture = SpriteFrameTable::resolveTexture(*pFrame, octant);
            const OutdoorGameView::BillboardTextureHandle *pTexture =
                ensureSpriteBillboardTexture(view, resolvedTexture.textureName, pFrame->paletteId);

            if (pTexture == nullptr || !bgfx::isValid(pTexture->textureHandle))
            {
                if (shouldLog)
                {
                    std::cout << "Projectile draw skipped kind=" << kind
                              << " id=" << runtimeId
                              << " sprite=\"" << (spriteName.empty() ? "<none>" : spriteName) << "\""
                              << " spriteId=" << spriteId
                              << " frameIndex=" << spriteFrameIndex
                              << " source=" << pResolutionSource
                              << " texture=\"" << resolvedTexture.textureName << "\""
                              << " palette=" << pFrame->paletteId
                              << " reason=texture_missing\n";
                }
                return;
            }

            const float spriteScale = std::max(pFrame->scale, 0.01f);
            const float worldWidth = static_cast<float>(pTexture->width) * spriteScale;
            const float worldHeight = static_cast<float>(pTexture->height) * spriteScale;
            const bool centerAnchored = SpriteFrameTable::hasFlag(pFrame->flags, SpriteFrameFlag::Center);

            if (shouldLog)
            {
                std::cout << "Projectile draw kind=" << kind
                          << " id=" << runtimeId
                          << " sprite=\"" << (spriteName.empty() ? "<none>" : spriteName) << "\""
                          << " spriteId=" << spriteId
                          << " frameIndex=" << spriteFrameIndex
                          << " source=" << pResolutionSource
                          << " texture=\"" << resolvedTexture.textureName << "\""
                          << " palette=" << pFrame->paletteId
                          << " texSize=(" << pTexture->width << ", " << pTexture->height << ")"
                          << " scale=" << spriteScale
                          << " worldSize=(" << worldWidth << ", " << worldHeight << ")"
                          << " pos=(" << x << ", " << y << ", " << z << ")"
                          << " ageTicks=" << timeTicks
                          << '\n';
            }

            BillboardDrawItem drawItem = {};
            drawItem.x = x;
            drawItem.y = y;
            drawItem.z = z;
            drawItem.pFrame = pFrame;
            drawItem.pTexture = pTexture;
            drawItem.mirrored = resolvedTexture.mirrored;
            drawItem.distanceSquared = distanceSquared;
            drawItem.lightContributionAbgr = computeBillboardLightContributionAbgr(
                view,
                x,
                y,
                centerAnchored ? z : z + worldHeight * 0.5f);
            drawItems.push_back(drawItem);
        };

    for (const GameplayProjectilePresentationState &projectile : projectiles)
    {
        if (projectile.visualMode == GameplayProjectileVisualMode::FxOnly)
        {
            continue;
        }

        appendRuntimeDrawItem(
            projectile.projectileId,
            "projectile",
            projectile.x,
            projectile.y,
            projectile.z,
            projectile.velocityX,
            projectile.velocityY,
            projectile.objectSpriteFrameIndex,
            projectile.objectSpriteId,
            projectile.objectSpriteName,
            projectile.timeSinceCreatedTicks);
    }

    for (const GameplayProjectileImpactPresentationState &impact : impacts)
    {
        const FxRecipes::ProjectileRecipe recipe = FxRecipes::classifyProjectileRecipe(
            impact.sourceSpellId,
            impact.sourceObjectName,
            impact.sourceObjectSpriteName,
            impact.sourceObjectFlags);

        if (FxRecipes::projectileRecipeUsesDedicatedImpactFx(recipe)
            && !FxRecipes::projectileRecipeShowsImpactBillboard(recipe))
        {
            if (impact.objectName.find("Trap") != std::string::npos)
            {
                std::cout << "Projectile draw skipped kind=impact"
                          << " id=" << impact.effectId
                          << " objectName=\"" << impact.objectName << "\""
                          << " reason=dedicated_fx_recipe\n";
            }
            continue;
        }

        appendRuntimeDrawItem(
            impact.effectId,
            "impact",
            impact.x,
            impact.y,
            impact.z,
            0.0f,
            0.0f,
            impact.objectSpriteFrameIndex,
            impact.objectSpriteId,
            impact.objectSpriteName,
            impact.freezeAnimation ? 0u : impact.timeSinceCreatedTicks,
            impact.objectName.find("Trap") != std::string::npos);
    }

    for (size_t trapIndex = 0; trapIndex < view.m_pOutdoorWorldRuntime->fireSpikeTrapCount(); ++trapIndex)
    {
        const OutdoorWorldRuntime::FireSpikeTrapState *pTrap = view.m_pOutdoorWorldRuntime->fireSpikeTrapState(trapIndex);

        if (pTrap == nullptr)
        {
            continue;
        }

        appendRuntimeDrawItem(
            pTrap->trapId,
            "fire_spike",
            pTrap->x,
            pTrap->y,
            pTrap->z,
            0.0f,
            0.0f,
            pTrap->objectSpriteFrameIndex,
            pTrap->objectSpriteId,
            pTrap->objectSpriteName,
            pTrap->timeSinceCreatedTicks);
    }

#if defined(OPENYAMM_VITA_LOADER)
    unsigned projectileCandidates = 0, projectileOutside = 0;
    const bx::Vec3 forward = {pViewMatrix[2], pViewMatrix[6], pViewMatrix[10]};
    std::erase_if(drawItems, [&](const BillboardDrawItem &item)
    {
        ++projectileCandidates;
        const float radius = std::max(item.pFrame->scale, 0.01f)
            * std::hypot(float(item.pTexture->width), float(item.pTexture->height));
        const bx::Vec3 delta = bx::sub(bx::Vec3{item.x, item.y, item.z}, cameraPosition);
        // Wide enclosing frustum is conservative for the Vita's 960x544 viewport and narrower UI viewports.
        const bool outside = !vitaSphereVisible(bx::dot(delta, cameraRight), bx::dot(delta, cameraUp),
            bx::dot(delta, forward), radius, 2.0f, view.m_viewDistanceCache.runtimeProjectileDistance);
        projectileOutside += outside;
        return vitaPerformanceSettings().extraFrustum && outside;
    });
#if defined(OPENYAMM_VITA_PROFILE)
    if (Engine::VitaProfile::frame.active)
    Engine::VitaProfile::print("PROFILE CULL category=projectile candidates=%u outside=%u culled=%u drawn=%u\n",
        projectileCandidates, projectileOutside, vitaPerformanceSettings().extraFrustum ? projectileOutside : 0,
        projectileCandidates - (vitaPerformanceSettings().extraFrustum ? projectileOutside : 0));
#endif
#endif
    std::sort(
        drawItems.begin(),
        drawItems.end(),
        [](const BillboardDrawItem &left, const BillboardDrawItem &right)
        {
            const uint16_t leftTextureId = left.pTexture != nullptr ? left.pTexture->textureHandle.idx : 0;
            const uint16_t rightTextureId = right.pTexture != nullptr ? right.pTexture->textureHandle.idx : 0;

            if (leftTextureId != rightTextureId)
            {
                return leftTextureId < rightTextureId;
            }

            return left.distanceSquared > right.distanceSquared;
        });

    if (OY_VITA_TRACE(view.m_gameSettings.performanceTrace))
    {
        view.m_outdoorSpriteRenderDiagnostics.runtimeProjectileItems += drawItems.size();
    }

    size_t groupBegin = 0;

    while (groupBegin < drawItems.size())
    {
        const OutdoorGameView::BillboardTextureHandle *pTexture = drawItems[groupBegin].pTexture;

        if (pTexture == nullptr || !bgfx::isValid(pTexture->textureHandle))
        {
            ++groupBegin;
            continue;
        }

        size_t groupEnd = groupBegin + 1;

        while (groupEnd < drawItems.size() && drawItems[groupEnd].pTexture == pTexture)
        {
            ++groupEnd;
        }

        const uint32_t vertexCount = static_cast<uint32_t>((groupEnd - groupBegin) * 6);

        if (bgfx::getAvailTransientVertexBuffer(vertexCount, OutdoorGameView::LitBillboardVertex::ms_layout) < vertexCount)
        {
            groupBegin = groupEnd;
            continue;
        }

        std::vector<OutdoorGameView::LitBillboardVertex> vertices;
        vertices.reserve(static_cast<size_t>(vertexCount));

        for (size_t index = groupBegin; index < groupEnd; ++index)
        {
            const BillboardDrawItem &drawItem = drawItems[index];
            const SpriteFrameEntry &frame = *drawItem.pFrame;
            const float spriteScale = std::max(frame.scale, 0.01f);
            const float worldWidth = static_cast<float>(pTexture->width) * spriteScale;
            const float worldHeight = static_cast<float>(pTexture->height) * spriteScale;
            const float halfWidth = worldWidth * 0.5f;
            const bool centerAnchored = SpriteFrameTable::hasFlag(frame.flags, SpriteFrameFlag::Center);
            const bx::Vec3 center = centerAnchored
                ? bx::Vec3{drawItem.x, drawItem.y, drawItem.z}
                : bottomAnchoredBillboardCenter(
                    drawItem.x,
                    drawItem.y,
                    drawItem.z,
                    cameraUp,
                    worldHeight);
            const bx::Vec3 right = {cameraRight.x * halfWidth, cameraRight.y * halfWidth, cameraRight.z * halfWidth};
            const bx::Vec3 up = {
                cameraUp.x * worldHeight * 0.5f,
                cameraUp.y * worldHeight * 0.5f,
                cameraUp.z * worldHeight * 0.5f
            };
            const float u0 = drawItem.mirrored ? 1.0f : 0.0f;
            const float u1 = drawItem.mirrored ? 0.0f : 1.0f;

            appendLitBillboardVertices(
                vertices,
                center,
                right,
                up,
                u0,
                u1,
                drawItem.lightContributionAbgr);
        }

        bgfx::TransientVertexBuffer transientVertexBuffer = {};
        bgfx::allocTransientVertexBuffer(
            &transientVertexBuffer,
            static_cast<uint32_t>(vertices.size()),
            OutdoorGameView::LitBillboardVertex::ms_layout);
        std::memcpy(
            transientVertexBuffer.data,
            vertices.data(),
            static_cast<size_t>(vertices.size() * sizeof(OutdoorGameView::LitBillboardVertex)));

        float modelMatrix[16] = {};
        bx::mtxIdentity(modelMatrix);
        bgfx::setTransform(modelMatrix);
        bgfx::setVertexBuffer(0, &transientVertexBuffer, 0, static_cast<uint32_t>(vertices.size()));
        bindTexture(
            0,
            view.m_terrainTextureSamplerHandle,
            pTexture->textureHandle,
            TextureFilterProfile::Billboard,
            BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
        applyBillboardAmbientUniform(view);
        applyBillboardOverrideColorUniform(view, 0, 0.0f);
        bgfx::setState(BillboardAlphaRenderState);
        bgfx::submit(viewId,
            !view.settingsSnapshot().spriteOutline && bgfx::isValid(view.m_outdoorFastBillboardProgramHandle)
                ? view.m_outdoorFastBillboardProgramHandle : view.m_outdoorLitBillboardProgramHandle);

        if (OY_VITA_TRACE(view.m_gameSettings.performanceTrace))
        {
            ++view.m_outdoorSpriteRenderDiagnostics.runtimeProjectileBatchSubmits;
            view.m_outdoorSpriteRenderDiagnostics.runtimeProjectileBatchedItems += groupEnd - groupBegin;
            ++view.m_outdoorSpriteRenderDiagnostics.runtimeProjectileTextureGroups;
        }

        groupBegin = groupEnd;
    }
}

void OutdoorBillboardRenderer::renderFxContactShadows(
    OutdoorGameView &view,
    uint16_t viewId)
{
    Engine::VitaProfile::Scope vitaScope(Engine::VitaProfile::WorldShadows);
    if (!bgfx::isValid(view.m_outdoorLitBillboardProgramHandle)
        || !bgfx::isValid(view.m_terrainTextureSamplerHandle)
        || !bgfx::isValid(view.m_outdoorBillboardOverrideColorUniformHandle)
        || !bgfx::isValid(view.m_outdoorBillboardOutlineParamsUniformHandle)
        || !bgfx::isValid(view.m_outdoorBillboardAmbientUniformHandle))
    {
        return;
    }

    const std::vector<WorldFxContactShadow> &shadows = view.m_worldFxSystem.contactShadows();

    if (shadows.empty())
    {
        return;
    }

    const OutdoorGameView::BillboardTextureHandle *pShadowTexture =
        view.findBillboardTexture(ContactShadowTextureName, 0);

    if (pShadowTexture == nullptr)
    {
        return;
    }

    applyBillboardFogUniforms(view, view.m_viewDistanceCache.actorBillboardDistance);

    std::vector<OutdoorGameView::LitBillboardVertex> vertices;
    vertices.reserve(shadows.size() * 6);

    if (OY_VITA_TRACE(view.m_gameSettings.performanceTrace))
    {
        view.m_outdoorSpriteRenderDiagnostics.fxContactShadowItems += shadows.size();
    }

    for (const WorldFxContactShadow &shadow : shadows)
    {
        const bx::Vec3 center = {shadow.x, shadow.y, shadow.z};
        const bx::Vec3 right = {shadow.radius, 0.0f, 0.0f};
        const bx::Vec3 up = {0.0f, shadow.radius, 0.0f};
        appendLitBillboardVertices(vertices, center, right, up, 0.0f, 1.0f, shadow.colorAbgr);
    }

    if (vertices.empty())
    {
        return;
    }

    if (bgfx::getAvailTransientVertexBuffer(
            static_cast<uint32_t>(vertices.size()),
            OutdoorGameView::LitBillboardVertex::ms_layout) < vertices.size())
    {
        return;
    }

    bgfx::TransientVertexBuffer transientVertexBuffer = {};
    bgfx::allocTransientVertexBuffer(
        &transientVertexBuffer,
        static_cast<uint32_t>(vertices.size()),
        OutdoorGameView::LitBillboardVertex::ms_layout);
    std::memcpy(
        transientVertexBuffer.data,
        vertices.data(),
        static_cast<size_t>(vertices.size() * sizeof(OutdoorGameView::LitBillboardVertex)));

    float modelMatrix[16] = {};
    bx::mtxIdentity(modelMatrix);
    bgfx::setTransform(modelMatrix);
    bgfx::setVertexBuffer(0, &transientVertexBuffer, 0, static_cast<uint32_t>(vertices.size()));
    bindTexture(
        0,
        view.m_terrainTextureSamplerHandle,
        pShadowTexture->textureHandle,
        TextureFilterProfile::Billboard,
        BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
    applyBillboardAmbientUniform(view);
    applyBillboardOverrideColorUniform(view, 0, 0.0f);
    applyBillboardOutlineParamsUniform(view, 0.0f, 0.0f, 0.0f);
    bgfx::setState(BillboardAlphaRenderState);
    bgfx::submit(viewId,
            !view.settingsSnapshot().spriteOutline && bgfx::isValid(view.m_outdoorFastBillboardProgramHandle)
                ? view.m_outdoorFastBillboardProgramHandle : view.m_outdoorLitBillboardProgramHandle);

    if (OY_VITA_TRACE(view.m_gameSettings.performanceTrace))
    {
        ++view.m_outdoorSpriteRenderDiagnostics.fxContactShadowSubmits;
    }
}

void OutdoorBillboardRenderer::renderFxGlowBillboards(
    OutdoorGameView &view,
    uint16_t viewId,
    const float *pViewMatrix)
{
    Engine::VitaProfile::Scope vitaScope(Engine::VitaProfile::WorldParticles);
    const std::vector<WorldFxGlowBillboard> &glowBillboards = view.m_worldFxSystem.glowBillboards();

    if (glowBillboards.empty())
    {
        return;
    }

    const bx::Vec3 cameraRight = {pViewMatrix[0], pViewMatrix[4], pViewMatrix[8]};
    const bx::Vec3 cameraUp = {pViewMatrix[1], pViewMatrix[5], pViewMatrix[9]};
    std::vector<OutdoorGameView::TerrainVertex> vertices;
    vertices.reserve(glowBillboards.size() * 6);
    size_t visibleGlowCount = 0;

    for (const WorldFxGlowBillboard &glow : glowBillboards)
    {
        if (!glow.renderVisibleBillboard)
        {
            continue;
        }

        ++visibleGlowCount;
        const bx::Vec3 center = {glow.x, glow.y, glow.z};
        const bx::Vec3 right = {
            cameraRight.x * glow.radius,
            cameraRight.y * glow.radius,
            cameraRight.z * glow.radius
        };
        const bx::Vec3 up = {
            cameraUp.x * glow.radius,
            cameraUp.y * glow.radius,
            cameraUp.z * glow.radius
        };
        appendWorldQuadVertices(vertices, center, right, up, glow.colorAbgr);
    }

    if (OY_VITA_TRACE(view.m_gameSettings.performanceTrace))
    {
        view.m_outdoorSpriteRenderDiagnostics.fxGlowItems += visibleGlowCount;

        if (!vertices.empty())
        {
            ++view.m_outdoorSpriteRenderDiagnostics.fxGlowSubmits;
        }
    }

    submitColoredVertices(view, viewId, vertices, ColoredAdditiveRenderState);
}

void OutdoorBillboardRenderer::renderFxSegmentProjectiles(
    OutdoorGameView &view,
    uint16_t viewId,
    const float *pViewMatrix)
{
    Engine::VitaProfile::Scope vitaScope(Engine::VitaProfile::WorldParticles);
    const std::vector<WorldFxSegmentProjectile> &segments = view.m_worldFxSystem.segmentProjectiles();

    if (segments.empty())
    {
        return;
    }

    const bx::Vec3 cameraForward = {pViewMatrix[2], pViewMatrix[6], pViewMatrix[10]};
    const bx::Vec3 cameraUp = {pViewMatrix[1], pViewMatrix[5], pViewMatrix[9]};
    std::vector<OutdoorGameView::TerrainVertex> vertices;
    vertices.reserve(segments.size() * 6);

    for (const WorldFxSegmentProjectile &segment : segments)
    {
        const bx::Vec3 start = {segment.startX, segment.startY, segment.startZ};
        const bx::Vec3 end = {segment.endX, segment.endY, segment.endZ};
        const bx::Vec3 delta = {end.x - start.x, end.y - start.y, end.z - start.z};
        const float deltaLength = std::sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);

        if (deltaLength <= 0.001f)
        {
            continue;
        }

        const bx::Vec3 direction = {delta.x / deltaLength, delta.y / deltaLength, delta.z / deltaLength};
        bx::Vec3 side = {
            direction.y * cameraForward.z - direction.z * cameraForward.y,
            direction.z * cameraForward.x - direction.x * cameraForward.z,
            direction.x * cameraForward.y - direction.y * cameraForward.x
        };
        float sideLength = std::sqrt(side.x * side.x + side.y * side.y + side.z * side.z);

        if (sideLength <= 0.001f)
        {
            side = cameraUp;
            sideLength = std::sqrt(side.x * side.x + side.y * side.y + side.z * side.z);
        }

        const float halfWidth = segment.width * 0.5f / std::max(sideLength, 0.001f);
        side.x *= halfWidth;
        side.y *= halfWidth;
        side.z *= halfWidth;

        vertices.push_back({start.x - side.x, start.y - side.y, start.z - side.z, segment.colorAbgr});
        vertices.push_back({start.x + side.x, start.y + side.y, start.z + side.z, segment.colorAbgr});
        vertices.push_back({end.x + side.x, end.y + side.y, end.z + side.z, segment.colorAbgr});
        vertices.push_back({start.x - side.x, start.y - side.y, start.z - side.z, segment.colorAbgr});
        vertices.push_back({end.x + side.x, end.y + side.y, end.z + side.z, segment.colorAbgr});
        vertices.push_back({end.x - side.x, end.y - side.y, end.z - side.z, segment.colorAbgr});
    }

    submitColoredVertices(view, viewId, vertices, ColoredAdditiveRenderState);
}

void OutdoorBillboardRenderer::renderSpriteObjectBillboards(
    OutdoorGameView &view,
    uint16_t viewId,
    const float *pViewMatrix,
    const bx::Vec3 &cameraPosition)
{
    Engine::VitaProfile::Scope vitaScope(Engine::VitaProfile::WorldParticles);
    if (!view.m_outdoorSpriteObjectBillboardSet)
    {
        return;
    }

    if (!bgfx::isValid(view.m_outdoorLitBillboardProgramHandle)
        || !bgfx::isValid(view.m_terrainTextureSamplerHandle)
        || !bgfx::isValid(view.m_outdoorBillboardOverrideColorUniformHandle)
        || !bgfx::isValid(view.m_outdoorBillboardOutlineParamsUniformHandle)
        || !bgfx::isValid(view.m_outdoorBillboardAmbientUniformHandle))
    {
        return;
    }

    const bx::Vec3 cameraRight = {pViewMatrix[0], pViewMatrix[4], pViewMatrix[8]};
    const bx::Vec3 cameraUp = {pViewMatrix[1], pViewMatrix[5], pViewMatrix[9]};
    applyBillboardFogUniforms(view, view.m_viewDistanceCache.actorBillboardDistance);

    struct BillboardDrawItem
    {
        const SpriteObjectBillboard *pBillboard = nullptr;
        const SpriteFrameEntry *pFrame = nullptr;
        const OutdoorGameView::BillboardTextureHandle *pTexture = nullptr;
        bool mirrored = false;
        float distanceSquared = 0.0f;
        uint32_t lightContributionAbgr = 0xff000000u;
    };

    std::vector<BillboardDrawItem> drawItems;
    drawItems.reserve(view.m_outdoorSpriteObjectBillboardSet->billboards.size());

    for (const SpriteObjectBillboard &billboard : view.m_outdoorSpriteObjectBillboardSet->billboards)
    {
        const SpriteFrameEntry *pFrame =
            view.m_outdoorSpriteObjectBillboardSet->spriteFrameTable.getFrame(
                billboard.objectSpriteId,
                billboard.timeSinceCreatedTicks);

        if (pFrame == nullptr)
        {
            continue;
        }

        const ResolvedSpriteTexture resolvedTexture = SpriteFrameTable::resolveTexture(*pFrame, 0);
        const OutdoorGameView::BillboardTextureHandle *pTexture =
            view.findBillboardTexture(resolvedTexture.textureName, pFrame->paletteId);

        if (pTexture == nullptr || !bgfx::isValid(pTexture->textureHandle))
        {
            continue;
        }

        const float deltaX = static_cast<float>(billboard.x) - cameraPosition.x;
        const float deltaY = static_cast<float>(billboard.y) - cameraPosition.y;
        const float deltaZ = static_cast<float>(billboard.z) - cameraPosition.z;

        BillboardDrawItem drawItem = {};
        drawItem.pBillboard = &billboard;
        drawItem.pFrame = pFrame;
        drawItem.pTexture = pTexture;
        drawItem.mirrored = resolvedTexture.mirrored;
        drawItem.distanceSquared = deltaX * deltaX + deltaY * deltaY + deltaZ * deltaZ;
        drawItem.lightContributionAbgr = computeBillboardLightContributionAbgr(
            view,
            static_cast<float>(billboard.x),
            static_cast<float>(billboard.y),
            static_cast<float>(billboard.z) + static_cast<float>(pTexture->height) * std::max(pFrame->scale, 0.01f) * 0.5f);
        drawItems.push_back(drawItem);
    }

    std::sort(
        drawItems.begin(),
        drawItems.end(),
        [](const BillboardDrawItem &left, const BillboardDrawItem &right)
        {
            return left.distanceSquared > right.distanceSquared;
        });

    if (OY_VITA_TRACE(view.m_gameSettings.performanceTrace))
    {
        view.m_outdoorSpriteRenderDiagnostics.staticSpriteObjectItems += drawItems.size();

        const OutdoorGameView::BillboardTextureHandle *pLastTexture = nullptr;
        for (const BillboardDrawItem &drawItem : drawItems)
        {
            if (drawItem.pTexture != pLastTexture)
            {
                ++view.m_outdoorSpriteRenderDiagnostics.staticSpriteObjectTextureSwitches;
                pLastTexture = drawItem.pTexture;
            }
        }
    }

    struct StaticSpriteObjectBillboardBatch
    {
        const OutdoorGameView::BillboardTextureHandle *pTexture = nullptr;
        std::vector<OutdoorGameView::LitBillboardVertex> vertices;
        size_t itemCount = 0;
    };

    StaticSpriteObjectBillboardBatch billboardBatch = {};
    billboardBatch.vertices.reserve(drawItems.size() * 6);

    const auto flushBillboardBatch =
        [&]()
        {
            if (billboardBatch.pTexture == nullptr || billboardBatch.vertices.empty())
            {
                billboardBatch.vertices.clear();
                billboardBatch.pTexture = nullptr;
                billboardBatch.itemCount = 0;
                return;
            }

            const uint32_t batchVertexCount = static_cast<uint32_t>(billboardBatch.vertices.size());

            if (bgfx::getAvailTransientVertexBuffer(
                    batchVertexCount,
                    OutdoorGameView::LitBillboardVertex::ms_layout) >= batchVertexCount)
            {
                bgfx::TransientVertexBuffer transientVertexBuffer = {};
                bgfx::allocTransientVertexBuffer(
                    &transientVertexBuffer,
                    batchVertexCount,
                    OutdoorGameView::LitBillboardVertex::ms_layout);
                std::memcpy(
                    transientVertexBuffer.data,
                    billboardBatch.vertices.data(),
                    static_cast<size_t>(
                        billboardBatch.vertices.size() * sizeof(OutdoorGameView::LitBillboardVertex)));

                float modelMatrix[16] = {};
                bx::mtxIdentity(modelMatrix);
                bgfx::setTransform(modelMatrix);
                bgfx::setVertexBuffer(0, &transientVertexBuffer, 0, batchVertexCount);
                bindTexture(
                    0,
                    view.m_terrainTextureSamplerHandle,
                    billboardBatch.pTexture->textureHandle,
                    TextureFilterProfile::Billboard,
                    BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
                applyBillboardAmbientUniform(view);
                applyBillboardOverrideColorUniform(view, 0, 0.0f);
                applyBillboardOutlineParamsUniform(view, 0.0f, 0.0f, 0.0f);
                bgfx::setState(BillboardAlphaRenderState);
                bgfx::submit(viewId,
            !view.settingsSnapshot().spriteOutline && bgfx::isValid(view.m_outdoorFastBillboardProgramHandle)
                ? view.m_outdoorFastBillboardProgramHandle : view.m_outdoorLitBillboardProgramHandle);

                if (OY_VITA_TRACE(view.m_gameSettings.performanceTrace))
                {
                    ++view.m_outdoorSpriteRenderDiagnostics.staticSpriteObjectBatchSubmits;
                    ++view.m_outdoorSpriteRenderDiagnostics.staticSpriteObjectSubmits;
                    view.m_outdoorSpriteRenderDiagnostics.staticSpriteObjectBatchedItems +=
                        billboardBatch.itemCount;
                }
            }

            billboardBatch.vertices.clear();
            billboardBatch.pTexture = nullptr;
            billboardBatch.itemCount = 0;
        };

    for (const BillboardDrawItem &drawItem : drawItems)
    {
        const SpriteObjectBillboard &billboard = *drawItem.pBillboard;
        const SpriteFrameEntry &frame = *drawItem.pFrame;
        const OutdoorGameView::BillboardTextureHandle &texture = *drawItem.pTexture;
        const float spriteScale = std::max(frame.scale, 0.01f);
        const float worldWidth = static_cast<float>(texture.width) * spriteScale;
        const float worldHeight = static_cast<float>(texture.height) * spriteScale;
        const float halfWidth = worldWidth * 0.5f;
        const bx::Vec3 center = bottomAnchoredBillboardCenter(
            static_cast<float>(billboard.x),
            static_cast<float>(billboard.y),
            static_cast<float>(billboard.z),
            cameraUp,
            worldHeight);
        const bx::Vec3 right = {cameraRight.x * halfWidth, cameraRight.y * halfWidth, cameraRight.z * halfWidth};
        const bx::Vec3 up = {cameraUp.x * worldHeight * 0.5f, cameraUp.y * worldHeight * 0.5f, cameraUp.z * worldHeight * 0.5f};
        const float u0 = drawItem.mirrored ? 1.0f : 0.0f;
        const float u1 = drawItem.mirrored ? 0.0f : 1.0f;

        std::array<OutdoorGameView::LitBillboardVertex, 6> vertices = {};
        writeLitBillboardVertices(
            vertices.data(),
            center,
            right,
            up,
            u0,
            u1,
            drawItem.lightContributionAbgr);

        if (billboardBatch.pTexture != &texture)
        {
            flushBillboardBatch();
            billboardBatch.pTexture = &texture;
        }

        billboardBatch.vertices.insert(billboardBatch.vertices.end(), vertices.begin(), vertices.end());
        ++billboardBatch.itemCount;
    }

    flushBillboardBatch();
}

} // namespace OpenYAMM::Game
