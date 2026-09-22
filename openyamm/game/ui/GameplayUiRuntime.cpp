#include "game/render/VitaIndoorDiagnostics.h"
#include "game/render/VitaIndoorDiagnostics.h"
#include "game/ui/GameplayUiRuntime.h"
#include "game/render/VitaPerformanceConfig.h"

#include "engine/BgfxContext.h"
#include "game/StringUtils.h"
#include "game/render/TextureFiltering.h"
#include "game/tables/MapStats.h"
#include "game/tables/MergedBaseTables.h"

#include <SDL3/SDL.h>
#include <bgfx/bgfx.h>
#include <bx/math.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cctype>
#include <functional>
#include <optional>
#include <string_view>
#include <utility>

namespace OpenYAMM::Game
{
namespace
{
template <typename T>
void hashCombine(size_t &seed, const T &value)
{
    seed ^= std::hash<T>{}(value) + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2);
}

bool canUseBgfxResources()
{
    return Engine::BgfxContext::isBgfxInitialized();
}

std::filesystem::path getShaderPath(bgfx::RendererType::Enum rendererType, const char *pShaderName)
{
    const std::filesystem::path configuredShaderRoot = OPENYAMM_BGFX_SHADER_DIR;
    std::string rendererDirectory;

    switch (rendererType)
    {
    case bgfx::RendererType::Direct3D11:
        rendererDirectory = "dxbc";
        break;

    case bgfx::RendererType::OpenGL:
        rendererDirectory = "glsl";
        break;

    case bgfx::RendererType::OpenGLES:
        rendererDirectory = "essl";
        break;

    case bgfx::RendererType::Vulkan:
        rendererDirectory = "spirv";
        break;

    default:
        return {};
    }

    const std::filesystem::path shaderName =
        std::filesystem::path(rendererDirectory) / (std::string(pShaderName) + ".bin");

    if (configuredShaderRoot.is_absolute())
    {
        return configuredShaderRoot / shaderName;
    }

    if (const char *pBasePath = SDL_GetBasePath())
    {
        const std::filesystem::path executableRoot = pBasePath;
        const std::filesystem::path packagedPath = executableRoot / configuredShaderRoot / shaderName;

        if (std::filesystem::exists(packagedPath))
        {
            return packagedPath;
        }

        const std::filesystem::path buildTreePath = executableRoot / ".." / configuredShaderRoot / shaderName;

        if (std::filesystem::exists(buildTreePath))
        {
            return buildTreePath;
        }

        return packagedPath;
    }

    return configuredShaderRoot / shaderName;
}

std::vector<uint8_t> readBinaryFile(const std::filesystem::path &path)
{
    std::ifstream input(path, std::ios::binary);

    if (!input)
    {
        return {};
    }

    input.seekg(0, std::ios::end);
    const std::streamsize size = input.tellg();
    input.seekg(0, std::ios::beg);

    if (size <= 0)
    {
        return {};
    }

    std::vector<uint8_t> bytes(static_cast<size_t>(size));

    if (!input.read(reinterpret_cast<char *>(bytes.data()), size))
    {
        return {};
    }

    return bytes;
}
bgfx::ShaderHandle loadShader(const char *pShaderName)
{
    const std::filesystem::path shaderPath = getShaderPath(bgfx::getRendererType(), pShaderName);

    if (shaderPath.empty())
    {
        return BGFX_INVALID_HANDLE;
    }

    const std::vector<uint8_t> shaderBytes = readBinaryFile(shaderPath);
#if defined(OPENYAMM_VITA_PROFILE)
    std::cerr << "PROFILE SHADER_FILE path=" << shaderPath << " bytes=" << shaderBytes.size() << '\n';
#endif

    if (shaderBytes.empty())
    {
        std::cerr << "Gameplay HUD could not read shader: " << shaderPath << '\n';
        return BGFX_INVALID_HANDLE;
    }

    const bgfx::Memory *pMemory = bgfx::copy(shaderBytes.data(), static_cast<uint32_t>(shaderBytes.size()));
    return bgfx::createShader(pMemory);
}

bgfx::ProgramHandle loadProgram(const char *pVertexShaderName, const char *pFragmentShaderName)
{
    const bgfx::ShaderHandle vertexShader = loadShader(pVertexShaderName);
    const bgfx::ShaderHandle fragmentShader = loadShader(pFragmentShaderName);

    if (!bgfx::isValid(vertexShader) || !bgfx::isValid(fragmentShader))
    {
        if (bgfx::isValid(vertexShader))
        {
            bgfx::destroy(vertexShader);
        }

        if (bgfx::isValid(fragmentShader))
        {
            bgfx::destroy(fragmentShader);
        }

        return BGFX_INVALID_HANDLE;
    }

    return bgfx::createProgram(vertexShader, fragmentShader, true);
}

constexpr int32_t MergedYawUnitsPerTurn = 2048;
constexpr int32_t DegreesPerTurn = 360;
constexpr size_t MaxPerformanceAssetLoadEvents = 32;
constexpr std::string_view EagerGameplayHudScreen = "outdoorhud";
struct TownPortalDestinationOverride
{
    uint32_t mapId = 0;
    std::string iconName;
    bool useMapStartPosition = false;
    bool clearUnlockQBit = false;
    bool clearDirection = false;
    int32_t hitX = 0;
    int32_t hitY = 0;
    uint32_t hitWidth = 0;
    uint32_t hitHeight = 0;
};

int32_t directionDegreesFromMergedYawUnits(int32_t yawUnits)
{
    const int32_t normalizedYawUnits =
        ((yawUnits % MergedYawUnitsPerTurn) + MergedYawUnitsPerTurn) % MergedYawUnitsPerTurn;
    return normalizedYawUnits * DegreesPerTurn / MergedYawUnitsPerTurn;
}

std::string townPortalIconTextureName(const std::string &iconName)
{
    const std::string normalizedIconName = toLowerCopy(iconName);

    if (normalizedIconName == "tpharmndy")
    {
        return "tp_alvar";
    }

    if (normalizedIconName == "tpelf")
    {
        return "tp_ravenshore";
    }

    if (normalizedIconName == "tpwarlock")
    {
        return "tp_balthazarlair";
    }

    if (normalizedIconName == "tpisland")
    {
        return "tp_regna";
    }

    if (normalizedIconName == "tpheaven")
    {
        return "tp_shadowspire";
    }

    if (normalizedIconName == "tphell")
    {
        return "tp_daggerwoundislands";
    }

    return normalizedIconName;
}

const MapStatsEntry *findMapEntryById(const std::vector<MapStatsEntry> &mapEntries, uint32_t mapId)
{
    for (const MapStatsEntry &entry : mapEntries)
    {
        if (entry.id == static_cast<int>(mapId))
        {
            return &entry;
        }
    }

    return nullptr;
}

const MergedTownPortalSwitchGroup *findTownPortalGroup(
    const MergedTownPortalSwitchTable &townPortalSwitchTable,
    const std::string &groupName)
{
    const std::string normalizedGroupName = toLowerCopy(groupName);

    for (const MergedTownPortalSwitchGroup &group : townPortalSwitchTable.groups())
    {
        if (toLowerCopy(group.name) == normalizedGroupName)
        {
            return &group;
        }
    }

    return nullptr;
}

std::string townPortalGroupNameForMap(const MapStatsEntry &mapEntry)
{
    if (mapEntry.id >= 62 && mapEntry.id <= 136)
    {
        return "Antagrich";
    }

    if (mapEntry.id >= 137 && mapEntry.id <= 203)
    {
        return "TPEnroth";
    }

    return "townport";
}

bool appendTownPortalDestination(
    std::vector<GameplayTownPortalDestination> &destinations,
    const MergedTownPortalSwitchGroup &group,
    const MergedTownPortalDestination &sourceDestination,
    const std::vector<MapStatsEntry> &mapEntries,
    const TownPortalDestinationOverride *pOverride = nullptr)
{
    const std::string &iconName =
        pOverride != nullptr && !pOverride->iconName.empty() ? pOverride->iconName : sourceDestination.iconName;

    if (iconName.empty() || toLowerCopy(iconName) == "none")
    {
        return false;
    }

    const uint32_t mapId =
        pOverride != nullptr && pOverride->mapId != 0 ? pOverride->mapId : sourceDestination.mapId;
    const MapStatsEntry *pDestinationMap = findMapEntryById(mapEntries, mapId);

    if (pDestinationMap == nullptr)
    {
        return false;
    }

    GameplayTownPortalDestination destination = {};
    destination.id = group.name + ":" + std::to_string(sourceDestination.id);
    destination.label =
        sourceDestination.description.empty() ? pDestinationMap->name : sourceDestination.description;
    destination.iconTextureName = townPortalIconTextureName(iconName);
    destination.mapName = pDestinationMap->fileName;
    destination.x = sourceDestination.x;
    destination.y = sourceDestination.y;
    destination.z = sourceDestination.z;
    if (pOverride == nullptr || !pOverride->clearDirection)
    {
        destination.directionDegrees = directionDegreesFromMergedYawUnits(sourceDestination.direction);
    }
    destination.useMapStartPosition = pOverride != nullptr && pOverride->useMapStartPosition;
    destination.unlockQBitId =
        pOverride != nullptr && pOverride->clearUnlockQBit ? 0 : sourceDestination.qbitIndex;
    destination.iconX = sourceDestination.iconX;
    destination.iconY = sourceDestination.iconY;
    destination.iconWidth = sourceDestination.iconWidth.value_or(0);
    destination.iconHeight = sourceDestination.iconHeight.value_or(0);
    destination.hitX = pOverride != nullptr && pOverride->hitWidth != 0
        ? pOverride->hitX
        : destination.iconX;
    destination.hitY = pOverride != nullptr && pOverride->hitHeight != 0
        ? pOverride->hitY
        : destination.iconY;
    destination.hitWidth = pOverride != nullptr && pOverride->hitWidth != 0
        ? pOverride->hitWidth
        : destination.iconWidth;
    destination.hitHeight = pOverride != nullptr && pOverride->hitHeight != 0
        ? pOverride->hitHeight
        : destination.iconHeight;
    destinations.push_back(std::move(destination));
    return true;
}

bool resolveDimensionDoorDestinationOverride(
    const MergedTownPortalDestination &sourceDestination,
    uint32_t /*dayIndex*/,
    TownPortalDestinationOverride &destinationOverride)
{
    const std::string description = toLowerCopy(sourceDestination.description);

    destinationOverride = {};
    destinationOverride.clearUnlockQBit = true;
    destinationOverride.clearDirection = true;

    if (description == "jadame")
    {
        destinationOverride.mapId = 1;
        destinationOverride.iconName = sourceDestination.iconName;
        destinationOverride.hitX = 0;
        destinationOverride.hitY = 0;
        destinationOverride.hitWidth = 213;
        destinationOverride.hitHeight = 480;
        return true;
    }

    if (description == "antagarich")
    {
        destinationOverride.mapId = 62;
        destinationOverride.iconName = sourceDestination.iconName;
        destinationOverride.hitX = 213;
        destinationOverride.hitY = 0;
        destinationOverride.hitWidth = 214;
        destinationOverride.hitHeight = 480;
        return true;
    }

    if (description == "enroth")
    {
        destinationOverride.mapId = 151;
        destinationOverride.iconName = sourceDestination.iconName;
        destinationOverride.hitX = 427;
        destinationOverride.hitY = 0;
        destinationOverride.hitWidth = 213;
        destinationOverride.hitHeight = 480;
        return true;
    }

    return false;
}

uint32_t currentAnimationTicks()
{
    return (static_cast<uint64_t>(SDL_GetTicks()) * 128ULL) / 1000ULL;
}

std::string portraitTextureNameForPictureFrame(uint32_t pictureId, uint16_t frameIndex)
{
    char buffer[16] = {};
    std::snprintf(buffer, sizeof(buffer), "PC%02u-%02u", pictureId + 1, std::max<uint16_t>(1, frameIndex));
    return buffer;
}

std::string portraitTextureNameForTextureFrame(const std::string &textureName, uint16_t frameIndex)
{
    if (textureName.empty())
    {
        return {};
    }

    std::string frameTextureName = textureName;

    if (frameTextureName.size() >= 2
        && std::isdigit(static_cast<unsigned char>(frameTextureName[frameTextureName.size() - 2])) != 0
        && std::isdigit(static_cast<unsigned char>(frameTextureName[frameTextureName.size() - 1])) != 0)
    {
        char frameSuffix[8] = {};
        std::snprintf(frameSuffix, sizeof(frameSuffix), "%02u", std::max<uint16_t>(1, frameIndex));
        frameTextureName.replace(frameTextureName.size() - 2, 2, frameSuffix);
    }

    return frameTextureName;
}

std::string portraitTextureNameForCharacterFrame(const Character &character, uint16_t frameIndex)
{
    const std::string textureFrameName =
        portraitTextureNameForTextureFrame(character.portraitTextureName, frameIndex);

    if (!textureFrameName.empty())
    {
        return textureFrameName;
    }

    return portraitTextureNameForPictureFrame(character.portraitPictureId, frameIndex);
}

const bgfx::VertexLayout &gameplayHudQuadVertexLayout()
{
    static const bgfx::VertexLayout layout = []()
    {
        bgfx::VertexLayout result;
        result.begin()
            .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
            .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
            .end();
        return result;
    }();

    return layout;
}
}

size_t GameplayUiRuntime::HudLayoutResolutionCacheKeyHash::operator()(
    const HudLayoutResolutionCacheKey &key) const
{
    size_t seed = 0;
    hashCombine(seed, key.layoutId);
    hashCombine(seed, key.screenWidth);
    hashCombine(seed, key.screenHeight);
    hashCombine(seed, key.fallbackWidth);
    hashCombine(seed, key.fallbackHeight);
    return seed;
}

void GameplayUiRuntime::trimOptionalHudTextures(bool pressure)
{
    const uint64_t now = SDL_GetTicks();
    if (!pressure && now < m_nextHudTrim) { return; }
    m_nextHudTrim = now + 1000;
    size_t bytes = 0;
    for (const auto &texture : m_hudTextureHandles)
    {
        if (texture.dynamicTexture && bgfx::isValid(texture.textureHandle))
            bytes += size_t(texture.physicalWidth) * texture.physicalHeight * 4 + texture.bgraPixels.capacity();
    }
    const size_t limit = pressure ? 0 : 8 * 1024 * 1024;
    while (bytes > limit)
    {
        GameplayHudTextureData *oldest = nullptr;
        for (auto &texture : m_hudTextureHandles)
        {
            if (texture.dynamicTexture && bgfx::isValid(texture.textureHandle)
                && now > texture.dynamicLastUse + (pressure ? 1000 : 5000)
                && (!oldest || texture.dynamicLastUse < oldest->dynamicLastUse)) { oldest = &texture; }
        }
        if (!oldest) { break; } // Working set is pinned, never replace visible content mid-frame.
        bytes -= size_t(oldest->physicalWidth) * oldest->physicalHeight * 4 + oldest->bgraPixels.capacity();
        bgfx::destroy(oldest->textureHandle);
        oldest->textureHandle = BGFX_INVALID_HANDLE;
        oldest->bgraPixels = std::vector<uint8_t>();
        m_dynamicHudTextureContentSignatures.erase(oldest->textureName);
    }
}

void GameplayUiRuntime::beginPerformanceFrame(bool enabled)
{
    trimOptionalHudTextures(false);
    m_performanceFrameEnabled = enabled;

    if (!enabled)
    {
        return;
    }

    m_performanceAssetLoadEvents.clear();
    m_performanceAssetLoadEventOverflowCount = 0;
    m_lastOverlayFramePerformanceDiagnostics = {};
}

const std::vector<GameplayUiAssetLoadPerformanceEvent> &GameplayUiRuntime::performanceAssetLoadEvents() const
{
    return m_performanceAssetLoadEvents;
}

size_t GameplayUiRuntime::performanceAssetLoadEventOverflowCount() const
{
    return m_performanceAssetLoadEventOverflowCount;
}

void GameplayUiRuntime::setLastOverlayFramePerformanceDiagnostics(
    const GameplayUiOverlayFramePerformanceDiagnostics &diagnostics)
{
    m_lastOverlayFramePerformanceDiagnostics = diagnostics;
}

const GameplayUiOverlayFramePerformanceDiagnostics &
GameplayUiRuntime::lastOverlayFramePerformanceDiagnostics() const
{
    return m_lastOverlayFramePerformanceDiagnostics;
}

void GameplayUiRuntime::recordPerformanceAssetLoad(
    GameplayUiAssetLoadKind kind,
    const std::string &name,
    uint64_t loadNanoseconds,
    bool success,
    int width,
    int height,
    bool hasColor,
    uint32_t colorAbgr)
{
    if (!m_performanceFrameEnabled)
    {
        return;
    }

    if (m_performanceAssetLoadEvents.size() >= MaxPerformanceAssetLoadEvents)
    {
        ++m_performanceAssetLoadEventOverflowCount;
        return;
    }

    m_performanceAssetLoadEvents.push_back(GameplayUiAssetLoadPerformanceEvent{
        .kind = kind,
        .name = name,
        .loadNanoseconds = loadNanoseconds,
        .colorAbgr = colorAbgr,
        .width = width,
        .height = height,
        .success = success,
        .hasColor = hasColor,
    });
}

void GameplayUiRuntime::clearHudLayoutLookupCaches() const
{
    m_hudLayoutElementLookupCache.clear();
    m_staticHudLayoutElementLookupCache.clear();
    m_hudLayoutResolutionCache.clear();
}

void GameplayUiRuntime::clear()
{
#if defined(OPENYAMM_VITA_LOADER)
    // Lifecycle-only ownership report; no timers or per-frame collection.
    size_t pixelCapacity = 0, fontCapacity = 0;
    for (const auto &texture : m_hudTextureHandles) { pixelCapacity += texture.bgraPixels.capacity(); }
    for (const auto &font : m_hudFontHandles) { fontCapacity += font.mainAtlasPixels.capacity(); }
    size_t rawEquivalent = 0, indexedImages = 0;
    for (const auto &texture : m_hudTextureHandles)
    { rawEquivalent += texture.bgraPixels.size(); indexedImages += texture.bgraPixels.indexed(); }
    for (const auto &font : m_hudFontHandles)
    { rawEquivalent += font.mainAtlasPixels.size(); indexedImages += font.mainAtlasPixels.indexed(); }
    std::fprintf(stderr, "VITA HUD_CPU_STORAGE retained=%u raw_equivalent=%u indexed=%u\n",
        unsigned(pixelCapacity + fontCapacity), unsigned(rawEquivalent), unsigned(indexedImages));
    if (pixelCapacity || fontCapacity)
    {
        std::fprintf(stderr, "VITA HUD_RELEASE textures=%u cpu_pixels=%u font_pixels=%u tinted_textures=%u\n",
            unsigned(m_hudTextureHandles.size()), unsigned(pixelCapacity), unsigned(fontCapacity),
            unsigned(m_hudTextureColorTextureHandles.size() + m_hudFontColorTextureHandles.size()));
    }
#endif

    shutdownHouseVideoPlayer();
    clearPortraitRuntime();
    clearHudResources();
    m_layoutManager.clear();
    clearHudLayoutLookupCaches();
    m_hudLayoutRuntimeWidthOverrides.clear();
    m_hudLayoutRuntimeHeightOverrides.clear();
    m_renderedInspectableHudItems.clear();
    m_renderedInspectableHudScreenState = GameplayHudScreenState::Gameplay;
    m_townPortalDestinationsMapFileName.clear();
    m_townPortalBackgroundTextureName.clear();
    m_townPortalDestinations.clear();
    m_townPortalDestinationsLoaded = false;
    m_hudRenderBackend = {};
    m_flyBuffIconAnimationState = {};
    m_assetLoadCache = {};
    m_performanceFrameEnabled = false;
    m_performanceAssetLoadEvents.clear();
    m_performanceAssetLoadEventOverflowCount = 0;
    m_lastOverlayFramePerformanceDiagnostics = {};
    m_pAssetFileSystem = nullptr;
    m_assetWorldId.clear();
    m_uiAssetScaleTier = Engine::AssetScaleTier::X1;
    m_iconAssetScaleTier = Engine::AssetScaleTier::X1;
    m_fontAssetScaleTier = Engine::AssetScaleTier::X1;
    m_layoutsLoaded = false;
    m_assetsPreloaded = false;
}

void GameplayUiRuntime::bindDataRepository(const GameDataRepository *pDataRepository)
{
    if (m_pDataRepository == pDataRepository)
    {
        return;
    }

    clearPortraitRuntime();
    m_townPortalDestinationsMapFileName.clear();
    m_townPortalBackgroundTextureName.clear();
    m_townPortalDestinations.clear();
    m_townPortalDestinationsLoaded = false;
    m_pDataRepository = pDataRepository;
}

void GameplayUiRuntime::bindAssetFileSystem(const Engine::AssetFileSystem *pAssetFileSystem)
{
    const bool bindingsMatch =
        pAssetFileSystem != nullptr
        && m_pAssetFileSystem == pAssetFileSystem
        && m_assetWorldId == pAssetFileSystem->getActiveWorldId()
        && m_uiAssetScaleTier == pAssetFileSystem->getAssetScaleTier(Engine::AssetScaleCategory::Ui)
        && m_iconAssetScaleTier == pAssetFileSystem->getAssetScaleTier(Engine::AssetScaleCategory::Icons)
        && m_fontAssetScaleTier == pAssetFileSystem->getAssetScaleTier(Engine::AssetScaleCategory::Fonts);

    if (bindingsMatch || (pAssetFileSystem == nullptr && m_pAssetFileSystem == nullptr))
    {
        return;
    }

    clear();
    m_pAssetFileSystem = pAssetFileSystem;

    if (pAssetFileSystem != nullptr)
    {
        m_assetWorldId = pAssetFileSystem->getActiveWorldId();
        m_uiAssetScaleTier = pAssetFileSystem->getAssetScaleTier(Engine::AssetScaleCategory::Ui);
        m_iconAssetScaleTier = pAssetFileSystem->getAssetScaleTier(Engine::AssetScaleCategory::Icons);
        m_fontAssetScaleTier = pAssetFileSystem->getAssetScaleTier(Engine::AssetScaleCategory::Fonts);
    }
}

const Engine::AssetFileSystem *GameplayUiRuntime::assetFileSystem() const
{
    return m_pAssetFileSystem;
}

bool GameplayUiRuntime::ensureGameplayLayoutsLoaded(const GameplayUiController &uiController)
{
    if (m_layoutsLoaded)
    {
        return true;
    }

    if (m_pAssetFileSystem == nullptr)
    {
        return false;
    }

    m_layoutManager.clear();
    clearHudLayoutLookupCaches();

    const bool loaded = uiController.loadGameplayLayouts(
        *m_pAssetFileSystem,
        [this](const std::string &path) -> bool
        {
            return m_layoutManager.loadLayoutFile(*m_pAssetFileSystem, path);
        });

    if (!loaded)
    {
        clearHudLayoutLookupCaches();
        return false;
    }

    clearHudLayoutLookupCaches();
    m_layoutsLoaded = true;
    return true;
}

void GameplayUiRuntime::preloadReferencedAssets()
{
#if defined(OPENYAMM_VITA_LOADER)
    struct BootstrapScope
    {
        BootstrapScope() { SDL_SetHint("openyamm_vita_hud_bootstrap", "1"); }
        ~BootstrapScope() { SDL_SetHint("openyamm_vita_hud_bootstrap", "0"); }
    } bootstrapScope;
#endif
    if (m_assetsPreloaded)
    {
        return;
    }

    if (!canUseBgfxResources())
    {
        return;
    }

    unsigned textureRequests = 0, textureReady = 0, fontRequests = 0, fontReady = 0;
    for (const auto &[id, element] : m_layoutManager.elements())
    {
        (void)id;

        if (element.normalizedScreen != EagerGameplayHudScreen)
        {
            continue;
        }

        for (const std::string *pAssetName : {
                 &element.primaryAsset,
                 &element.hoverAsset,
                 &element.pressedAsset,
                 &element.secondaryAsset,
                 &element.tertiaryAsset,
                 &element.quaternaryAsset,
                 &element.quinaryAsset})
        {
#if defined(OPENYAMM_VITA_LOADER)
            if (pAssetName != &element.primaryAsset) { continue; }
#endif
            if (!pAssetName->empty())
            {
                // MINIMAP is generated from the active world, not an on-disk bitmap.
                if (toLowerCopy(*pAssetName) == "minimap") { continue; }
                // Spell-state icons are requested when their buff is active, not base HUD readiness.
                if (element.normalizedId.starts_with("outdoorbuffskull_")) { continue; }
                ++textureRequests;
                const bool ready = loadHudTexture(*pAssetName);
                textureReady += ready ? 1u : 0u;
#if defined(OPENYAMM_VITA_LOADER)
                if (!ready)
                {
                    std::fprintf(stderr, "VITA HUD_ASSET_MISSING element=%s asset=%s\n",
                        id.c_str(), pAssetName->c_str());
                }
#endif
            }
        }

        if (!element.fontName.empty())
        {
            ++fontRequests;
            fontReady += loadHudFont(element.fontName) ? 1u : 0u;
        }
    }

#if defined(OPENYAMM_VITA_LOADER)
    // Bounded built-in action names. Metadata-defined custom icons remain async.
    for (const char *icon : {"context_talk", "context_pickup_item", "context_loot_corpse", "context_open_chest",
         "context_enter_house", "context_open_door", "context_press_button", "context_use_lever",
         "context_generic_event", "context_drop_item", "use", "interact"})
    {
        loadHudTexture(std::string(icon) + "_default");
        loadHudTexture(std::string(icon) + "_pressed");
    }
    for (const char *screen : {"Menu", "SaveGame", "LoadGame"})
    {
        for (const auto &id : sortedHudLayoutIdsForScreen(screen))
        {
            const auto *layout = findHudLayoutElement(id);
            if (!layout || !layout->visible) { continue; }
            for (const auto *asset : {&layout->primaryAsset, &layout->hoverAsset, &layout->pressedAsset})
                if (!asset->empty()) { loadHudTexture(*asset); }
            if (!layout->fontName.empty()) { loadHudFont(layout->fontName); }
        }
    }
    loadHudFont("Create");
    m_assetLoadCache.binaryFilesByPath.clear();
    loadHudTexture("DEAD");
    loadHudTexture("ERADCATE");
    for (unsigned direction = 1; direction <= 8; ++direction)
    {
        loadHudTexture("MAPDIR" + std::to_string(direction));
    }
#endif
    m_assetsPreloaded = textureRequests != 0 && textureReady == textureRequests && fontReady == fontRequests;
#if defined(OPENYAMM_VITA_LOADER)
    ensureHudTextureLoaded("vita_button_up");
    ensureHudTextureLoaded("vita_button_ht");
    std::fprintf(stderr, "VITA HUD_BOOTSTRAP world=%s textures=%u/%u fonts=%u/%u ready=%u resident=%u\n",
        m_assetWorldId.c_str(), textureReady, textureRequests, fontReady, fontRequests,
        m_assetsPreloaded ? 1u : 0u, unsigned(m_hudTextureHandles.size()));
#endif
}

GameplayAssetLoadCache &GameplayUiRuntime::assetLoadCache()
{
    return m_assetLoadCache;
}

std::optional<GameplayHudTextureHandle> GameplayUiRuntime::ensureMinimapPaletteLoaded()
{
    auto palette = findCachedDynamicHudTexture("__minimap_palette__", "grey-blue-v1");
    if (!palette)
    {
        std::array<uint8_t, 256 * 2 * 4> pixels = {};
        for (unsigned value = 0; value < 256; ++value)
        {
            for (unsigned channel = 0; channel < 3; ++channel) { pixels[value * 4 + channel] = value; }
            pixels[value * 4 + 3] = 255;
            pixels[(256 + value) * 4] = 255;
            pixels[(256 + value) * 4 + 3] = 255;
        }
        palette = ensureDynamicHudTexture("__minimap_palette__", 256, 2,
            std::vector<uint8_t>(pixels.begin(), pixels.end()));
        if (palette) { setDynamicHudTextureContentSignature("__minimap_palette__", "grey-blue-v1"); }
    }
    return palette;
}

void GameplayUiRuntime::preloadMinimapAssets(const GameplayMinimapState &state)
{
#if defined(OPENYAMM_VITA_LOADER)
    // The world-selected map is not a literal asset in the HUD layout. Prepare
    // it during loading too, instead of first queuing it after gameplay begins.
    const char *oldHint = SDL_GetHint("openyamm_vita_hud_bootstrap");
    const std::string savedHint = oldHint != nullptr ? oldHint : "0";
    SDL_SetHint("openyamm_vita_hud_bootstrap", "1");
    if (state.vectorBackground)
    {
        ensureSolidHudTextureLoaded("__indoor_minimap_background__", state.backgroundColorAbgr);
        ensureMinimapPaletteLoaded();
    }
    else if (!state.textureName.empty())
    {
        loadHudTexture(state.textureName);
    }
    SDL_SetHint("openyamm_vita_hud_bootstrap", savedHint.c_str());
#endif
}

const GameplayAssetLoadCache &GameplayUiRuntime::assetLoadCache() const
{
    return m_assetLoadCache;
}

UiLayoutManager &GameplayUiRuntime::layoutManager()
{
    return m_layoutManager;
}

const UiLayoutManager &GameplayUiRuntime::layoutManager() const
{
    return m_layoutManager;
}

std::vector<GameplayHudTextureData> &GameplayUiRuntime::hudTextureHandles()
{
    return m_hudTextureHandles;
}

const std::vector<GameplayHudTextureData> &GameplayUiRuntime::hudTextureHandles() const
{
    return m_hudTextureHandles;
}

std::unordered_map<std::string, size_t> &GameplayUiRuntime::hudTextureIndexByName()
{
    return m_hudTextureIndexByName;
}

const std::unordered_map<std::string, size_t> &GameplayUiRuntime::hudTextureIndexByName() const
{
    return m_hudTextureIndexByName;
}

std::vector<GameplayHudFontData> &GameplayUiRuntime::hudFontHandles()
{
    return m_hudFontHandles;
}

const std::vector<GameplayHudFontData> &GameplayUiRuntime::hudFontHandles() const
{
    return m_hudFontHandles;
}

std::vector<GameplayHudFontColorTextureData> &GameplayUiRuntime::hudFontColorTextureHandles()
{
    return m_hudFontColorTextureHandles;
}

const std::vector<GameplayHudFontColorTextureData> &GameplayUiRuntime::hudFontColorTextureHandles() const
{
    return m_hudFontColorTextureHandles;
}

std::vector<GameplayHudTextureColorTextureData> &GameplayUiRuntime::hudTextureColorTextureHandles()
{
    return m_hudTextureColorTextureHandles;
}

const std::vector<GameplayHudTextureColorTextureData> &GameplayUiRuntime::hudTextureColorTextureHandles() const
{
    return m_hudTextureColorTextureHandles;
}

std::unordered_map<std::string, float> &GameplayUiRuntime::hudLayoutRuntimeHeightOverrides()
{
    return m_hudLayoutRuntimeHeightOverrides;
}

const std::unordered_map<std::string, float> &GameplayUiRuntime::hudLayoutRuntimeHeightOverrides() const
{
    return m_hudLayoutRuntimeHeightOverrides;
}

std::vector<GameplayRenderedInspectableHudItem> &GameplayUiRuntime::renderedInspectableHudItems()
{
    return m_renderedInspectableHudItems;
}

const std::vector<GameplayRenderedInspectableHudItem> &GameplayUiRuntime::renderedInspectableHudItems() const
{
    return m_renderedInspectableHudItems;
}

void GameplayUiRuntime::clearRenderedInspectableHudItems()
{
    m_renderedInspectableHudItems.clear();
}

GameplayHudScreenState GameplayUiRuntime::renderedInspectableHudScreenState() const
{
    return m_renderedInspectableHudScreenState;
}

void GameplayUiRuntime::setRenderedInspectableHudScreenState(GameplayHudScreenState state)
{
    m_renderedInspectableHudScreenState = state;
}

bool GameplayUiRuntime::ensureTownPortalDestinationsLoaded(const std::string &currentMapFileName)
{
    if (m_townPortalDestinationsLoaded
        && toLowerCopy(m_townPortalDestinationsMapFileName) == toLowerCopy(currentMapFileName))
    {
        return !m_townPortalDestinations.empty();
    }

    m_townPortalDestinationsMapFileName = currentMapFileName;
    m_townPortalBackgroundTextureName.clear();
    m_townPortalDestinations.clear();
    m_townPortalDestinationsLoaded = false;

    if (m_pDataRepository == nullptr || !m_pDataRepository->isBound())
    {
        return false;
    }

    const MapStatsEntry *pCurrentMap = m_pDataRepository->mapStats().findByFileName(currentMapFileName);

    if (pCurrentMap == nullptr)
    {
        return false;
    }

    const MergedTownPortalSwitchGroup *pGroup = findTownPortalGroup(
        m_pDataRepository->mergedTownPortalSwitchTable(),
        townPortalGroupNameForMap(*pCurrentMap));

    if (pGroup == nullptr)
    {
        return false;
    }

    std::vector<GameplayTownPortalDestination> destinations;
    const std::vector<MapStatsEntry> &mapEntries = m_pDataRepository->mapEntries();

    for (const MergedTownPortalDestination &sourceDestination : pGroup->destinations)
    {
        appendTownPortalDestination(destinations, *pGroup, sourceDestination, mapEntries);
    }

    m_townPortalBackgroundTextureName = townPortalIconTextureName(pGroup->name);
    m_townPortalDestinations = std::move(destinations);
    m_townPortalDestinationsLoaded = true;
    return !m_townPortalDestinations.empty();
}

bool GameplayUiRuntime::ensureDimensionDoorDestinationsLoaded(uint32_t dayIndex, bool crossContinentsUnlocked)
{
    const std::string cacheKey = "__dimension_door:" + std::to_string(dayIndex)
        + (crossContinentsUnlocked ? ":unlocked" : ":locked");

    if (m_townPortalDestinationsLoaded
        && toLowerCopy(m_townPortalDestinationsMapFileName) == toLowerCopy(cacheKey))
    {
        return !m_townPortalDestinations.empty();
    }

    m_townPortalDestinationsMapFileName = cacheKey;
    m_townPortalBackgroundTextureName.clear();
    m_townPortalDestinations.clear();
    m_townPortalDestinationsLoaded = false;

    if (!crossContinentsUnlocked)
    {
        m_townPortalDestinationsLoaded = true;
        return false;
    }

    if (m_pDataRepository == nullptr || !m_pDataRepository->isBound())
    {
        return false;
    }

    const MergedTownPortalSwitchGroup *pGroup = findTownPortalGroup(
        m_pDataRepository->mergedTownPortalSwitchTable(),
        "TPGlobal");

    if (pGroup == nullptr)
    {
        return false;
    }

    std::vector<GameplayTownPortalDestination> destinations;
    const std::vector<MapStatsEntry> &mapEntries = m_pDataRepository->mapEntries();

    for (const MergedTownPortalDestination &sourceDestination : pGroup->destinations)
    {
        TownPortalDestinationOverride destinationOverride = {};
        const bool hasOverride =
            resolveDimensionDoorDestinationOverride(sourceDestination, dayIndex, destinationOverride);
        appendTownPortalDestination(
            destinations,
            *pGroup,
            sourceDestination,
            mapEntries,
            hasOverride ? &destinationOverride : nullptr);
    }

    m_townPortalBackgroundTextureName = townPortalIconTextureName(pGroup->name);
    m_townPortalDestinations = std::move(destinations);
    m_townPortalDestinationsLoaded = true;
    return !m_townPortalDestinations.empty();
}

const std::string &GameplayUiRuntime::townPortalBackgroundTextureName() const
{
    return m_townPortalBackgroundTextureName;
}

const std::vector<GameplayTownPortalDestination> &GameplayUiRuntime::townPortalDestinations() const
{
    return m_townPortalDestinations;
}

bool GameplayUiRuntime::loadHudTexture(const std::string &textureName)
try
{
    if (m_hudAllocationRetries.blocked(textureName.c_str(), SDL_GetTicksNS())) { return false; }
    if (!canUseBgfxResources())
    {
        return false;
    }

    VitaIndoorStage assetStage(Engine::VitaIncident::HudAssets, OyStallHudAssets);
    const bool cacheMiss =
        m_performanceFrameEnabled
        && GameplayHudCommon::findHudTexture(m_hudTextureHandles, m_hudTextureIndexByName, textureName) == nullptr;
    const uint64_t loadBeginTickCount = cacheMiss ? SDL_GetTicksNS() : 0;
    const bool loaded = GameplayHudCommon::loadHudTexture(
        m_pAssetFileSystem,
        m_assetLoadCache,
        textureName,
        m_hudTextureHandles,
        m_hudTextureIndexByName);

    if (cacheMiss)
    {
        const GameplayHudTextureData *pTexture =
            GameplayHudCommon::findHudTexture(m_hudTextureHandles, m_hudTextureIndexByName, textureName);
        recordPerformanceAssetLoad(
            GameplayUiAssetLoadKind::HudTexture,
            textureName,
            SDL_GetTicksNS() - loadBeginTickCount,
            loaded,
            pTexture != nullptr ? pTexture->physicalWidth : 0,
            pTexture != nullptr ? pTexture->physicalHeight : 0);
    }

    return loaded;
}
catch (const std::bad_alloc &)
{
    m_hudAllocationRetries.begin(textureName.c_str(), SDL_GetTicksNS());
    std::fprintf(stderr, "VITA_UI_ALLOCATION_DEFER kind=loadHudTexture name=%s\n", textureName.c_str());
    trimOptionalHudTextures(true);
    return false;
}


bool GameplayUiRuntime::loadItemIconTexture(const std::string &textureName)
try
{
    if (m_hudAllocationRetries.blocked(textureName.c_str(), SDL_GetTicksNS())) { return false; }
    if (!canUseBgfxResources())
    {
        return false;
    }

    VitaIndoorStage assetStage(Engine::VitaIncident::HudAssets, OyStallHudAssets);
    const bool cacheMiss =
        m_performanceFrameEnabled
        && GameplayHudCommon::findHudTexture(m_hudTextureHandles, m_hudTextureIndexByName, textureName) == nullptr;
    const uint64_t loadBeginTickCount = cacheMiss ? SDL_GetTicksNS() : 0;
    const bool loaded = GameplayHudCommon::loadHudTexture(
        m_pAssetFileSystem,
        m_assetLoadCache,
        textureName,
        m_hudTextureHandles,
        m_hudTextureIndexByName,
        GameplayHudBitmapTransparencyMode::ItemIcon);

    if (cacheMiss)
    {
        const GameplayHudTextureData *pTexture =
            GameplayHudCommon::findHudTexture(m_hudTextureHandles, m_hudTextureIndexByName, textureName);
        recordPerformanceAssetLoad(
            GameplayUiAssetLoadKind::ItemIcon,
            textureName,
            SDL_GetTicksNS() - loadBeginTickCount,
            loaded,
            pTexture != nullptr ? pTexture->physicalWidth : 0,
            pTexture != nullptr ? pTexture->physicalHeight : 0);
    }

    return loaded;
}
catch (const std::bad_alloc &)
{
    m_hudAllocationRetries.begin(textureName.c_str(), SDL_GetTicksNS());
    std::fprintf(stderr, "VITA_UI_ALLOCATION_DEFER kind=loadItemIconTexture name=%s\n", textureName.c_str());
    trimOptionalHudTextures(true);
    return false;
}


bool GameplayUiRuntime::loadHudFont(const std::string &fontName)
try
{
    if (m_hudAllocationRetries.blocked(fontName.c_str(), SDL_GetTicksNS())) { return false; }
    if (!canUseBgfxResources())
    {
        return false;
    }

    VitaIndoorStage assetStage(Engine::VitaIncident::HudAssets, OyStallHudAssets);
    const bool cacheMiss =
        m_performanceFrameEnabled && GameplayHudCommon::findHudFont(m_hudFontHandles, fontName) == nullptr;
    const uint64_t loadBeginTickCount = cacheMiss ? SDL_GetTicksNS() : 0;
    const bool loaded = GameplayHudCommon::loadHudFont(
        m_pAssetFileSystem,
        m_assetLoadCache,
        fontName,
        m_hudFontHandles);

    if (cacheMiss)
    {
        const GameplayHudFontData *pFont = GameplayHudCommon::findHudFont(m_hudFontHandles, fontName);
        recordPerformanceAssetLoad(
            GameplayUiAssetLoadKind::HudFont,
            fontName,
            SDL_GetTicksNS() - loadBeginTickCount,
            loaded,
            pFont != nullptr ? pFont->atlasWidth : 0,
            pFont != nullptr ? pFont->atlasHeight : 0);
    }

    return loaded;
}
catch (const std::bad_alloc &)
{
    m_hudAllocationRetries.begin(fontName.c_str(), SDL_GetTicksNS());
    std::fprintf(stderr, "VITA_UI_ALLOCATION_DEFER kind=loadHudFont name=%s\n", fontName.c_str());
    trimOptionalHudTextures(true);
    return false;
}


std::optional<std::vector<uint8_t>> GameplayUiRuntime::loadHudBitmapPixelsBgraCached(
    const std::string &textureName,
    int &width,
    int &height)
{
    return GameplayHudCommon::loadHudBitmapPixelsBgraCached(
        m_pAssetFileSystem,
        m_assetLoadCache,
        textureName,
        width,
        height);
}

std::optional<std::vector<uint8_t>> GameplayUiRuntime::loadItemIconBitmapPixelsBgraCached(
    const std::string &textureName,
    int &width,
    int &height)
{
    return GameplayHudCommon::loadHudBitmapPixelsBgraCached(
        m_pAssetFileSystem,
        m_assetLoadCache,
        textureName,
        width,
        height,
        GameplayHudBitmapTransparencyMode::ItemIcon);
}

std::optional<std::vector<uint8_t>> GameplayUiRuntime::loadSpriteBitmapPixelsBgraCached(
    const std::string &textureName,
    int16_t paletteId,
    int &width,
    int &height)
{
    return GameplayHudCommon::loadSpriteBitmapPixelsBgraCached(
        m_pAssetFileSystem,
        m_assetLoadCache,
        textureName,
        paletteId,
        width,
        height);
}

void GameplayUiRuntime::clearHudLayoutRuntimeHeightOverrides()
{
    m_hudLayoutRuntimeWidthOverrides.clear();
    m_hudLayoutRuntimeHeightOverrides.clear();
    m_hudLayoutResolutionCache.clear();
}

void GameplayUiRuntime::setHudLayoutRuntimeWidthOverride(const std::string &layoutId, float width)
{
    m_hudLayoutRuntimeWidthOverrides[toLowerCopy(layoutId)] = width;
    m_hudLayoutResolutionCache.clear();
}

void GameplayUiRuntime::setHudLayoutRuntimeHeightOverride(const std::string &layoutId, float height)
{
    m_hudLayoutRuntimeHeightOverrides[toLowerCopy(layoutId)] = height;
    m_hudLayoutResolutionCache.clear();
}

const UiLayoutManager::LayoutElement *GameplayUiRuntime::findHudLayoutElement(const std::string &layoutId) const
{
    const std::unordered_map<std::string, const UiLayoutManager::LayoutElement *>::const_iterator cachedIterator =
        m_hudLayoutElementLookupCache.find(layoutId);

    if (cachedIterator != m_hudLayoutElementLookupCache.end())
    {
        return cachedIterator->second;
    }

    const UiLayoutManager::LayoutElement *pElement = m_layoutManager.findElement(layoutId);
    m_hudLayoutElementLookupCache.emplace(layoutId, pElement);
    return pElement;
}

const UiLayoutManager::LayoutElement *GameplayUiRuntime::findStaticHudLayoutElement(const char *pLayoutId) const
{
    const std::unordered_map<const char *, const UiLayoutManager::LayoutElement *>::const_iterator cachedIterator =
        m_staticHudLayoutElementLookupCache.find(pLayoutId);

    if (cachedIterator != m_staticHudLayoutElementLookupCache.end())
    {
        return cachedIterator->second;
    }

    const UiLayoutManager::LayoutElement *pElement = m_layoutManager.findElement(pLayoutId);
    m_staticHudLayoutElementLookupCache.emplace(pLayoutId, pElement);
    return pElement;
}

int GameplayUiRuntime::defaultHudLayoutZIndexForScreen(const std::string &screen) const
{
    return UiLayoutManager::defaultZIndexForScreen(screen);
}

const std::vector<std::string> &GameplayUiRuntime::sortedHudLayoutIdsForScreen(const std::string &screen) const
{
    return m_layoutManager.sortedLayoutIdsForScreenCached(screen);
}

std::optional<GameplayResolvedHudLayoutElement> GameplayUiRuntime::resolveHudLayoutElement(
    const std::string &layoutId,
    int screenWidth,
    int screenHeight,
    float fallbackWidth,
    float fallbackHeight) const
{
    HudLayoutResolutionCacheKey cacheKey = {};
    cacheKey.layoutId = layoutId;
    cacheKey.screenWidth = screenWidth;
    cacheKey.screenHeight = screenHeight;
    cacheKey.fallbackWidth = fallbackWidth;
    cacheKey.fallbackHeight = fallbackHeight;

    const std::unordered_map<
        HudLayoutResolutionCacheKey,
        std::optional<GameplayResolvedHudLayoutElement>,
        HudLayoutResolutionCacheKeyHash>::const_iterator cachedIterator =
            m_hudLayoutResolutionCache.find(cacheKey);

    if (cachedIterator != m_hudLayoutResolutionCache.end())
    {
        return cachedIterator->second;
    }

    std::optional<GameplayResolvedHudLayoutElement> resolved =
        GameplayHudCommon::resolveHudLayoutElement(
            m_layoutManager,
            m_hudLayoutRuntimeWidthOverrides,
            m_hudLayoutRuntimeHeightOverrides,
            layoutId,
            screenWidth,
            screenHeight,
            fallbackWidth,
            fallbackHeight);
    m_hudLayoutResolutionCache.emplace(std::move(cacheKey), resolved);
    return resolved;
}

std::optional<GameplayHudTextureHandle> GameplayUiRuntime::ensurePortraitTextureLoaded(
    const Character &character, size_t slot)
{
    if (slot >= m_lastPortraits.size()) { m_lastPortraits.resize(slot + 1); }
    LastPortrait &last = m_lastPortraits[slot];
    const std::string identity = character.portraitTextureName + ":" + std::to_string(character.portraitPictureId);
    if (last.identity != identity) { last = {identity, {}}; }
    const std::string requested = resolvePortraitTextureName(character);
    const auto ready = ensureHudTextureLoaded(requested);
    if (ready) { last.texture = requested; return ready; }
    return last.texture.empty() ? std::nullopt : ensureHudTextureLoaded(last.texture);
}

std::optional<GameplayHudTextureHandle> GameplayUiRuntime::ensureHudTextureLoaded(const std::string &textureName)
{
    const GameplayHudTextureData *pTexture = nullptr;
#if defined(OPENYAMM_VITA_LOADER)
    if (vitaPerformanceSettings().hudReadyCache && canUseBgfxResources())
    { pTexture = GameplayHudCommon::findHudTexture(m_hudTextureHandles, m_hudTextureIndexByName, textureName); }
#endif
    if (pTexture == nullptr)
    {
        if (textureName.empty() || !loadHudTexture(textureName)) { return std::nullopt; }
        pTexture = GameplayHudCommon::findHudTexture(m_hudTextureHandles, m_hudTextureIndexByName, textureName);
    }

    if (pTexture == nullptr)
    {
        return std::nullopt;
    }

    GameplayHudTextureHandle result = {};
    result.textureName = pTexture->textureName;
    result.width = pTexture->width;
    result.height = pTexture->height;
    result.textureHandle = pTexture->textureHandle;
    return result;
}

std::optional<GameplayHudTextureHandle> GameplayUiRuntime::ensureItemIconTextureLoaded(const std::string &textureName)
{
    if (textureName.empty() || !loadItemIconTexture(textureName))
    {
        return std::nullopt;
    }

    const GameplayHudTextureData *pTexture =
        GameplayHudCommon::findHudTexture(m_hudTextureHandles, m_hudTextureIndexByName, textureName);

    if (pTexture == nullptr)
    {
        return std::nullopt;
    }

    GameplayHudTextureHandle result = {};
    result.textureName = pTexture->textureName;
    result.width = pTexture->width;
    result.height = pTexture->height;
    result.textureHandle = pTexture->textureHandle;
    return result;
}

std::optional<std::string> GameplayUiRuntime::iconAnimationFrameTextureName(
    const std::string &animationName,
    uint32_t elapsedTicks) const
{
    if (m_pDataRepository == nullptr || animationName.empty())
    {
        return std::nullopt;
    }

    const std::optional<size_t> animationId = m_pDataRepository->iconFrameTable().findAnimationIdByName(animationName);

    if (!animationId)
    {
        return std::nullopt;
    }

    const IconFrameEntry *pFrame = m_pDataRepository->iconFrameTable().getFrame(*animationId, elapsedTicks);

    if (pFrame == nullptr || pFrame->textureName.empty())
    {
        return std::nullopt;
    }

    return pFrame->textureName;
}

std::optional<std::string> GameplayUiRuntime::flyBuffIconAnimationFrameTextureName(
    bool active,
    uint32_t tickDivisor)
{
    constexpr const char *pFlyBuffAnimationName = "spell21";
    const uint32_t nowTicks = currentAnimationTicks();

    if (!m_flyBuffIconAnimationState.initialized
        || m_flyBuffIconAnimationState.animationName != pFlyBuffAnimationName)
    {
        m_flyBuffIconAnimationState = {};
        m_flyBuffIconAnimationState.initialized = true;
        m_flyBuffIconAnimationState.animationName = pFlyBuffAnimationName;
        m_flyBuffIconAnimationState.lastSourceTicks = nowTicks;
    }

    if (active && m_flyBuffIconAnimationState.advancing)
    {
        m_flyBuffIconAnimationState.accumulatedSourceTicks +=
            nowTicks - m_flyBuffIconAnimationState.lastSourceTicks;
    }

    m_flyBuffIconAnimationState.advancing = active;
    m_flyBuffIconAnimationState.lastSourceTicks = nowTicks;

    const uint32_t safeTickDivisor = std::max<uint32_t>(tickDivisor, 1);
    return iconAnimationFrameTextureName(
        m_flyBuffIconAnimationState.animationName,
        m_flyBuffIconAnimationState.accumulatedSourceTicks / safeTickDivisor);
}

std::optional<GameplayHudTextureHandle> GameplayUiRuntime::ensureSolidHudTextureLoaded(
    const std::string &textureName,
    uint32_t abgrColor)
{
    if (textureName.empty())
    {
        return std::nullopt;
    }

    const std::string cacheTextureName =
        toLowerCopy(textureName)
        + "_"
        + std::to_string((abgrColor >> 24) & 0xffu)
        + "_"
        + std::to_string((abgrColor >> 16) & 0xffu)
        + "_"
        + std::to_string((abgrColor >> 8) & 0xffu)
        + "_"
        + std::to_string(abgrColor & 0xffu);
    const auto existingIterator = m_hudTextureIndexByName.find(cacheTextureName);

    if (existingIterator != m_hudTextureIndexByName.end() && existingIterator->second < m_hudTextureHandles.size())
    {
        const GameplayHudTextureData &existing = m_hudTextureHandles[existingIterator->second];
        GameplayHudTextureHandle result = {};
        result.textureName = existing.textureName;
        result.width = existing.width;
        result.height = existing.height;
        result.textureHandle = existing.textureHandle;
        return result;
    }

    const uint64_t loadBeginTickCount = m_performanceFrameEnabled ? SDL_GetTicksNS() : 0;

    const std::array<uint8_t, 4> pixel = {
        static_cast<uint8_t>((abgrColor >> 16) & 0xffu),
        static_cast<uint8_t>((abgrColor >> 8) & 0xffu),
        static_cast<uint8_t>(abgrColor & 0xffu),
        static_cast<uint8_t>((abgrColor >> 24) & 0xffu)
    };

    GameplayHudTextureData textureHandle = {};
    textureHandle.textureName = cacheTextureName;
    textureHandle.width = 1;
    textureHandle.height = 1;
    textureHandle.physicalWidth = 1;
    textureHandle.physicalHeight = 1;
    textureHandle.bgraPixels = std::vector<uint8_t>(pixel.begin(), pixel.end());
    textureHandle.textureHandle = createBgraTexture2D(
        1,
        1,
        pixel.data(),
        uint32_t(pixel.size()),
        TextureFilterProfile::Ui,
        BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);

    if (!bgfx::isValid(textureHandle.textureHandle))
    {
        if (m_performanceFrameEnabled)
        {
            recordPerformanceAssetLoad(
                GameplayUiAssetLoadKind::SolidTexture,
                cacheTextureName,
                SDL_GetTicksNS() - loadBeginTickCount,
                false,
                1,
                1,
                true,
                abgrColor);
        }

        return std::nullopt;
    }

    const size_t index = m_hudTextureHandles.size();
    m_hudTextureHandles.push_back(std::move(textureHandle));
    m_hudTextureIndexByName[m_hudTextureHandles.back().textureName] = index;

    GameplayHudTextureHandle result = {};
    result.textureName = m_hudTextureHandles.back().textureName;
    result.width = m_hudTextureHandles.back().width;
    result.height = m_hudTextureHandles.back().height;
    result.textureHandle = m_hudTextureHandles.back().textureHandle;

    if (m_performanceFrameEnabled)
    {
        recordPerformanceAssetLoad(
            GameplayUiAssetLoadKind::SolidTexture,
            cacheTextureName,
            SDL_GetTicksNS() - loadBeginTickCount,
            true,
            1,
            1,
            true,
            abgrColor);
    }

    return result;
}

std::optional<GameplayHudTextureHandle> GameplayUiRuntime::ensureDynamicHudTexture(
    const std::string &textureName,
    int width,
    int height,
    const std::vector<uint8_t> &bgraPixels)
try
{
    if (m_hudAllocationRetries.blocked(textureName.c_str(), SDL_GetTicksNS())) { return std::nullopt; }
    const GameplayHudTextureData *pExistingTexture =
        m_performanceFrameEnabled
            ? GameplayHudCommon::findHudTexture(m_hudTextureHandles, m_hudTextureIndexByName, textureName)
            : nullptr;
    const bool createsResource =
        m_performanceFrameEnabled
        && (pExistingTexture == nullptr
            || pExistingTexture->physicalWidth != width
            || pExistingTexture->physicalHeight != height);
    const uint64_t loadBeginTickCount = createsResource ? SDL_GetTicksNS() : 0;
    const std::optional<GameplayHudTextureHandle> result = GameplayHudCommon::ensureDynamicHudTexture(
        textureName,
        width,
        height,
        bgraPixels,
        m_hudTextureHandles,
        m_hudTextureIndexByName);

    if (createsResource)
    {
        recordPerformanceAssetLoad(
            GameplayUiAssetLoadKind::DynamicTexture,
            textureName,
            SDL_GetTicksNS() - loadBeginTickCount,
            result.has_value(),
            width,
            height);
    }

    return result;
}
catch (const std::bad_alloc &)
{
    m_hudAllocationRetries.begin(textureName.c_str(), SDL_GetTicksNS());
    std::fprintf(stderr, "VITA_UI_ALLOCATION_DEFER kind=ensureDynamicHudTexture name=%s\n", textureName.c_str());
    trimOptionalHudTextures(true);
    return std::nullopt; // Optional preview missing; the save UI and live game remain usable.
}

std::optional<GameplayHudTextureHandle> GameplayUiRuntime::findCachedDynamicHudTexture(
    const std::string &textureName,
    const std::string &contentSignature) const
{
    const std::string normalizedTextureName = toLowerCopy(textureName);
    const auto signatureIterator = m_dynamicHudTextureContentSignatures.find(normalizedTextureName);

    if (signatureIterator == m_dynamicHudTextureContentSignatures.end()
        || signatureIterator->second != contentSignature)
    {
        return std::nullopt;
    }

    const GameplayHudTextureData *pTexture = GameplayHudCommon::findHudTexture(
        m_hudTextureHandles,
        m_hudTextureIndexByName,
        normalizedTextureName);

    if (pTexture == nullptr || !bgfx::isValid(pTexture->textureHandle))
    {
        return std::nullopt;
    }

    pTexture->dynamicLastUse = SDL_GetTicks();
    return GameplayHudTextureHandle{
        .textureName = pTexture->textureName,
        .width = pTexture->width,
        .height = pTexture->height,
        .textureHandle = pTexture->textureHandle,
    };
}

void GameplayUiRuntime::setDynamicHudTextureContentSignature(
    const std::string &textureName,
    const std::string &contentSignature)
{
    m_dynamicHudTextureContentSignatures[toLowerCopy(textureName)] = contentSignature;
}

const HudCpuPixels *GameplayUiRuntime::hudTexturePixels(
    const std::string &textureName,
    int &width,
    int &height)
{
    const std::optional<GameplayHudTextureHandle> texture = ensureHudTextureLoaded(textureName);

    if (!texture)
    {
        width = 0;
        height = 0;
        return nullptr;
    }

    const auto textureIt = m_hudTextureIndexByName.find(toLowerCopy(textureName));

    if (textureIt == m_hudTextureIndexByName.end() || textureIt->second >= m_hudTextureHandles.size())
    {
        width = 0;
        height = 0;
        return nullptr;
    }

    const GameplayHudTextureData &sourceTexture = m_hudTextureHandles[textureIt->second];
    width = sourceTexture.width;
    height = sourceTexture.height;
    return &sourceTexture.bgraPixels;
}

bool GameplayUiRuntime::ensureHudTextureDimensions(const std::string &textureName, int &width, int &height)
{
    const std::optional<GameplayHudTextureHandle> texture = ensureHudTextureLoaded(textureName);

    if (!texture)
    {
        width = 0;
        height = 0;
        return false;
    }

    width = texture->width;
    height = texture->height;
    return true;
}

bool GameplayUiRuntime::ensureItemIconTextureDimensions(const std::string &textureName, int &width, int &height)
{
    const std::optional<GameplayHudTextureHandle> texture = ensureItemIconTextureLoaded(textureName);

    if (!texture)
    {
        width = 0;
        height = 0;
        return false;
    }

    width = texture->width;
    height = texture->height;
    return true;
}

bool GameplayUiRuntime::tryGetOpaqueHudTextureBounds(
    const std::string &textureName,
    int &width,
    int &height,
    int &opaqueMinX,
    int &opaqueMinY,
    int &opaqueMaxX,
    int &opaqueMaxY)
{
    const std::optional<GameplayHudTextureHandle> texture = ensureHudTextureLoaded(textureName);

    if (!texture)
    {
        width = 0;
        height = 0;
        opaqueMinX = -1;
        opaqueMinY = -1;
        opaqueMaxX = -1;
        opaqueMaxY = -1;
        return false;
    }

    const GameplayHudTextureData *pTexture =
        GameplayHudCommon::findHudTexture(m_hudTextureHandles, m_hudTextureIndexByName, textureName);

    if (pTexture == nullptr)
    {
        return false;
    }

    const Engine::AssetScaleTier assetScaleTier =
        m_pAssetFileSystem != nullptr ? m_pAssetFileSystem->getAssetScaleTier() : Engine::AssetScaleTier::X1;
    return GameplayHudCommon::tryGetOpaqueHudTextureBounds(
        *pTexture,
        assetScaleTier,
        width,
        height,
        opaqueMinX,
        opaqueMinY,
        opaqueMaxX,
        opaqueMaxY);
}

std::optional<GameplayHudFontHandle> GameplayUiRuntime::findHudFont(const std::string &fontName)
{
    if (fontName.empty() || !loadHudFont(fontName))
    {
        return std::nullopt;
    }

    const GameplayHudFontData *pFont = GameplayHudCommon::findHudFont(m_hudFontHandles, fontName);

    if (pFont == nullptr)
    {
        return std::nullopt;
    }

    GameplayHudFontHandle result = {};
    result.fontName = pFont->fontName;
    result.fontHeight = pFont->fontHeight;
    result.mainTextureHandle = pFont->mainTextureHandle;
    result.shadowTextureHandle = pFont->shadowTextureHandle;
    return result;
}

float GameplayUiRuntime::measureHudTextWidth(const GameplayHudFontHandle &font, const std::string &text) const
{
    const GameplayHudFontData *pFont = GameplayHudCommon::findHudFont(m_hudFontHandles, font.fontName);
    return pFont != nullptr ? GameplayHudCommon::measureHudTextWidth(*pFont, text) : 0.0f;
}

std::vector<std::string> GameplayUiRuntime::wrapHudTextToWidth(
    const GameplayHudFontHandle &font,
    const std::string &text,
    float maxWidth) const
{
    const GameplayHudFontData *pFont = GameplayHudCommon::findHudFont(m_hudFontHandles, font.fontName);
    return pFont != nullptr ? GameplayHudCommon::wrapHudTextToWidth(*pFont, text, maxWidth)
                            : std::vector<std::string>{text};
}

GameplayHudTextureHandle GameplayUiRuntime::ensureHudTextureColor(
    const GameplayHudTextureHandle &texture, uint32_t colorAbgr)
{
    GameplayHudTextureHandle result = texture;
    result.colorAbgr = colorAbgr;
    result.tintMode = colorAbgr == 0xffffffffu ? 0.0f : 1.0f;
    return result;
}
GameplayHudTextureHandle GameplayUiRuntime::ensureHudTextureColorModulated(
    const GameplayHudTextureHandle &texture, uint32_t colorAbgr)
{
    GameplayHudTextureHandle result = texture;
    result.colorAbgr = colorAbgr;
    result.tintMode = colorAbgr == 0xffffffffu ? 0.0f : 2.0f;
    return result;
}
GameplayHudFontColor GameplayUiRuntime::ensureHudFontMainTextureColor(
    const GameplayHudFontHandle &font, uint32_t colorAbgr)
{
    return {font.mainTextureHandle, colorAbgr | 0xff000000u, colorAbgr == 0xffffffffu ? 0.0f : 1.0f};
}

void GameplayUiRuntime::addRenderedInspectableHudItem(const GameplayRenderedInspectableHudItem &item)
{
    m_renderedInspectableHudItems.push_back(item);
}

bool GameplayUiRuntime::isOpaqueHudPixelAtPoint(const GameplayRenderedInspectableHudItem &item, float x, float y)
{
    if (item.textureName.empty()
        || item.width <= 0.0f
        || item.height <= 0.0f
        || x < item.x
        || x >= item.x + item.width
        || y < item.y
        || y >= item.y + item.height)
    {
        return false;
    }

#if defined(OPENYAMM_VITA_LOADER)
    // The rendering cache already owns the prepared pixels. A pick must not read/decode the image again.
    const auto *texture = GameplayHudCommon::findHudTexture(m_hudTextureHandles, m_hudTextureIndexByName, item.textureName);
    const int textureWidth = texture ? texture->physicalWidth : 0;
    const int textureHeight = texture ? texture->physicalHeight : 0;
    const HudCpuPixels *pixels = texture ? &texture->bgraPixels : nullptr;
#else
    int textureWidth = 0;
    int textureHeight = 0;
    const std::optional<std::vector<uint8_t>> pixels =
        GameplayHudCommon::loadHudBitmapPixelsBgraCached(
            m_pAssetFileSystem,
            m_assetLoadCache,
            item.textureName,
            textureWidth,
            textureHeight,
            item.textureUsesItemIconTransparency
                ? GameplayHudBitmapTransparencyMode::ItemIcon
                : GameplayHudBitmapTransparencyMode::HudColorKey);

#endif

    if (!pixels || textureWidth <= 0 || textureHeight <= 0)
    {
        return true;
    }

    float normalizedX = std::clamp((x - item.x) / item.width, 0.0f, 0.999999f);
    float normalizedY = std::clamp((y - item.y) / item.height, 0.0f, 0.999999f);

    if (item.rotatedCounterClockwise)
    {
        const float rotatedX = std::clamp(1.0f - ((y - item.y) / item.height), 0.0f, 0.999999f);
        const float rotatedY = normalizedX;
        normalizedX = rotatedX;
        normalizedY = rotatedY;
    }

    const int pixelX = std::clamp(int(normalizedX * textureWidth), 0, textureWidth - 1);
    const int pixelY = std::clamp(int(normalizedY * textureHeight), 0, textureHeight - 1);
    const size_t pixelOffset =
        (static_cast<size_t>(pixelY) * static_cast<size_t>(textureWidth) + static_cast<size_t>(pixelX)) * 4;

    if (pixelOffset + 3 >= pixels->size())
    {
        return false;
    }

    return (*pixels)[pixelOffset + 3] > 0;
}

void GameplayUiRuntime::releaseHudGpuResources(bool destroyBgfxResources)
{
    m_assetsPreloaded = false;
    if (!destroyBgfxResources)
    {
        m_hudTintProgram = BGFX_INVALID_HANDLE;
        m_hudTintUniform = BGFX_INVALID_HANDLE;
        m_hudTintModeUniform = BGFX_INVALID_HANDLE;
    }
    if (destroyBgfxResources)
    {
        clearHudResources();
        return;
    }

    for (GameplayHudTextureData &textureHandle : m_hudTextureHandles)
    {
        textureHandle.textureHandle = BGFX_INVALID_HANDLE;
    }

    for (GameplayHudFontData &fontHandle : m_hudFontHandles)
    {
        fontHandle.mainTextureHandle = BGFX_INVALID_HANDLE;
        fontHandle.shadowTextureHandle = BGFX_INVALID_HANDLE;
    }

    for (GameplayHudFontColorTextureData &textureHandle : m_hudFontColorTextureHandles)
    {
        textureHandle.textureHandle = BGFX_INVALID_HANDLE;
    }

    for (GameplayHudTextureColorTextureData &textureHandle : m_hudTextureColorTextureHandles)
    {
        textureHandle.textureHandle = BGFX_INVALID_HANDLE;
    }

    m_hudTextureHandles.clear();
    m_hudTextureIndexByName.clear();
    m_hudFontHandles.clear();
    m_hudFontColorTextureHandles.clear();
    m_hudTextureColorTextureHandles.clear();
}

void GameplayUiRuntime::bindHudRenderBackend(const GameplayHudRenderBackend &backend)
{
    m_hudRenderBackend = backend;
    if (canUseBgfxResources() && !bgfx::isValid(m_hudTintProgram))
    {
        m_hudTintProgram = loadProgram("vs_shadowmaps_texture", "fs_hud_tint");
        m_hudTintUniform = bgfx::createUniform("u_hudTint", bgfx::UniformType::Vec4);
        m_hudTintModeUniform = bgfx::createUniform("u_hudTintMode", bgfx::UniformType::Vec4);
    }
}

void GameplayUiRuntime::clearHudRenderBackend()
{
    m_hudRenderBackend = {};
}

bool GameplayUiRuntime::hasHudRenderResources() const
{
    return bgfx::isValid(m_hudRenderBackend.texturedProgramHandle)
        && bgfx::isValid(m_hudRenderBackend.textureSamplerHandle);
}

void GameplayUiRuntime::prepareHudView(int width, int height) const
{
    if (!hasHudRenderResources() || width <= 0 || height <= 0)
    {
        return;
    }

    float projectionMatrix[16] = {};
    bx::mtxOrtho(
        projectionMatrix,
        0.0f,
        static_cast<float>(width),
        static_cast<float>(height),
        0.0f,
        0.0f,
        1000.0f,
        0.0f,
        bgfx::getCaps()->homogeneousDepth);
    bgfx::setViewRect(m_hudRenderBackend.viewId, 0, 0, static_cast<uint16_t>(width), static_cast<uint16_t>(height));
    bgfx::setViewMode(m_hudRenderBackend.viewId, bgfx::ViewMode::Sequential);
    bgfx::setViewTransform(m_hudRenderBackend.viewId, nullptr, projectionMatrix);
    bgfx::touch(m_hudRenderBackend.viewId);
}

void GameplayUiRuntime::submitHudDraw(uint32_t colorAbgr, float tintMode) const
{
    if (tintMode == 0.0f || !bgfx::isValid(m_hudTintProgram))
    {
        bgfx::submit(m_hudRenderBackend.viewId, m_hudRenderBackend.texturedProgramHandle);
        return;
    }
    const float color[4] = {float(colorAbgr & 255) / 255.0f, float((colorAbgr >> 8) & 255) / 255.0f,
        float((colorAbgr >> 16) & 255) / 255.0f, float(colorAbgr >> 24) / 255.0f};
    const float mode[4] = {tintMode, 0, 0, 0};
    // Every submit carries its own values: bgfx draw ordering cannot inherit another tint.
    bgfx::setUniform(m_hudTintUniform, color);
    bgfx::setUniform(m_hudTintModeUniform, mode);
    bgfx::submit(m_hudRenderBackend.viewId, m_hudTintProgram);
}

void GameplayUiRuntime::submitHudTexturedQuad(
    bgfx::TextureHandle textureHandle,
    float x,
    float y,
    float quadWidth,
    float quadHeight,
    float u0,
    float v0,
    float u1,
    float v1,
    TextureFilterProfile filterProfile, uint32_t colorAbgr, float tintMode) const
{
    if (!hasHudRenderResources()
        || !bgfx::isValid(textureHandle)
        || quadWidth <= 0.0f
        || quadHeight <= 0.0f)
    {
        return;
    }

    bgfx::TransientVertexBuffer vertexBuffer = {};
    bgfx::TransientIndexBuffer indexBuffer = {};

    if (!bgfx::allocTransientBuffers(
            &vertexBuffer,
            gameplayHudQuadVertexLayout(),
            4,
            &indexBuffer,
            6))
    {
        return;
    }

    struct HudQuadVertex
    {
        float x;
        float y;
        float z;
        float u;
        float v;
    };

    HudQuadVertex *pVertices = reinterpret_cast<HudQuadVertex *>(vertexBuffer.data);
    pVertices[0] = {x, y, 0.0f, u0, v0};
    pVertices[1] = {x + quadWidth, y, 0.0f, u1, v0};
    pVertices[2] = {x + quadWidth, y + quadHeight, 0.0f, u1, v1};
    pVertices[3] = {x, y + quadHeight, 0.0f, u0, v1};

    uint16_t *pIndices = reinterpret_cast<uint16_t *>(indexBuffer.data);
    pIndices[0] = 0;
    pIndices[1] = 1;
    pIndices[2] = 2;
    pIndices[3] = 0;
    pIndices[4] = 2;
    pIndices[5] = 3;

    float modelMatrix[16] = {};
    bx::mtxIdentity(modelMatrix);
    bgfx::setTransform(modelMatrix);
    bgfx::setVertexBuffer(0, &vertexBuffer);
    bgfx::setIndexBuffer(&indexBuffer);
    bindTexture(
        0,
        m_hudRenderBackend.textureSamplerHandle,
        textureHandle,
        filterProfile,
        BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
    bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_BLEND_ALPHA);
    submitHudDraw(colorAbgr, tintMode);
}

void GameplayUiRuntime::submitHudTexturedQuadRotatedCounterClockwise(
    bgfx::TextureHandle textureHandle,
    float x,
    float y,
    float quadWidth,
    float quadHeight,
    TextureFilterProfile filterProfile, uint32_t colorAbgr, float tintMode) const
{
    if (!hasHudRenderResources()
        || !bgfx::isValid(textureHandle)
        || quadWidth <= 0.0f
        || quadHeight <= 0.0f)
    {
        return;
    }

    bgfx::TransientVertexBuffer vertexBuffer = {};
    bgfx::TransientIndexBuffer indexBuffer = {};

    if (!bgfx::allocTransientBuffers(
            &vertexBuffer,
            gameplayHudQuadVertexLayout(),
            4,
            &indexBuffer,
            6))
    {
        return;
    }

    struct HudQuadVertex
    {
        float x;
        float y;
        float z;
        float u;
        float v;
    };

    HudQuadVertex *pVertices = reinterpret_cast<HudQuadVertex *>(vertexBuffer.data);
    pVertices[0] = {x, y, 0.0f, 1.0f, 0.0f};
    pVertices[1] = {x + quadWidth, y, 0.0f, 1.0f, 1.0f};
    pVertices[2] = {x + quadWidth, y + quadHeight, 0.0f, 0.0f, 1.0f};
    pVertices[3] = {x, y + quadHeight, 0.0f, 0.0f, 0.0f};

    uint16_t *pIndices = reinterpret_cast<uint16_t *>(indexBuffer.data);
    pIndices[0] = 0;
    pIndices[1] = 1;
    pIndices[2] = 2;
    pIndices[3] = 0;
    pIndices[4] = 2;
    pIndices[5] = 3;

    float modelMatrix[16] = {};
    bx::mtxIdentity(modelMatrix);
    bgfx::setTransform(modelMatrix);
    bgfx::setVertexBuffer(0, &vertexBuffer);
    bgfx::setIndexBuffer(&indexBuffer);
    bindTexture(
        0,
        m_hudRenderBackend.textureSamplerHandle,
        textureHandle,
        filterProfile,
        BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
    bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_BLEND_ALPHA);
    submitHudDraw(colorAbgr, tintMode);
}

void GameplayUiRuntime::submitHudQuadBatch(
    const std::vector<GameplayHudBatchQuad> &quads,
    int screenWidth,
    int screenHeight) const
{
    VitaIndoorScope submitScope(VitaIndoorWork::HudSubmit);
    if (!hasHudRenderResources() || quads.empty())
    {
        return;
    }

    const auto canBatchRun =
        [](const GameplayHudBatchQuad &left, const GameplayHudBatchQuad &right) -> bool
        {
            if (left.textureHandle.idx != right.textureHandle.idx || left.clipped != right.clipped
                || left.colorAbgr != right.colorAbgr || left.tintMode != right.tintMode || left.text != right.text)
            {
                return false;
            }

            if (!left.clipped)
            {
                return true;
            }

            return left.scissorX == right.scissorX
                && left.scissorY == right.scissorY
                && left.scissorWidth == right.scissorWidth
                && left.scissorHeight == right.scissorHeight;
        };

    struct HudQuadVertex
    {
        float x;
        float y;
        float z;
        float u;
        float v;
    };

    float modelMatrix[16] = {};
    bx::mtxIdentity(modelMatrix);
    size_t quadIndex = 0;

    while (quadIndex < quads.size())
    {
        size_t runEnd = quadIndex + 1;

        while (runEnd < quads.size() && canBatchRun(quads[runEnd - 1], quads[runEnd]))
        {
            ++runEnd;
        }

        while (quadIndex < runEnd)
        {
            const uint32_t availableVertexCount = bgfx::getAvailTransientVertexBuffer(
                static_cast<uint32_t>((runEnd - quadIndex) * 6),
                gameplayHudQuadVertexLayout());

            if (availableVertexCount < 6)
            {
                return;
            }

            const size_t quadCount = std::min<size_t>(runEnd - quadIndex, availableVertexCount / 6);
            bgfx::TransientVertexBuffer transientVertexBuffer = {};
            bgfx::allocTransientVertexBuffer(
                &transientVertexBuffer,
                static_cast<uint32_t>(quadCount * 6),
                gameplayHudQuadVertexLayout());
            HudQuadVertex *pVertices = reinterpret_cast<HudQuadVertex *>(transientVertexBuffer.data);

            for (size_t localIndex = 0; localIndex < quadCount; ++localIndex)
            {
                const GameplayHudBatchQuad &quad = quads[quadIndex + localIndex];
                HudQuadVertex *pQuadVertices = pVertices + localIndex * 6;

                if (quad.line)
                {
                    const float dx = quad.x2 - quad.x;
                    const float dy = quad.y2 - quad.y;
                    const float length = std::sqrt(dx * dx + dy * dy);
                    float normalX = 0.0f;
                    float normalY = 0.0f;

                    if (length > 0.0001f)
                    {
                        const float halfThickness = std::max(1.0f, quad.width) * 0.5f;
                        normalX = -dy / length * halfThickness;
                        normalY = dx / length * halfThickness;
                    }

                    pQuadVertices[0] = {quad.x + normalX, quad.y + normalY, 0.0f, quad.u0, quad.v0};
                    pQuadVertices[1] = {quad.x2 + normalX, quad.y2 + normalY, 0.0f, quad.u1, quad.v0};
                    pQuadVertices[2] = {quad.x2 - normalX, quad.y2 - normalY, 0.0f, quad.u1, quad.v1};
                    pQuadVertices[3] = {quad.x + normalX, quad.y + normalY, 0.0f, quad.u0, quad.v0};
                    pQuadVertices[4] = {quad.x2 - normalX, quad.y2 - normalY, 0.0f, quad.u1, quad.v1};
                    pQuadVertices[5] = {quad.x - normalX, quad.y - normalY, 0.0f, quad.u0, quad.v1};
                    continue;
                }

                pQuadVertices[0] = {quad.x, quad.y, 0.0f, quad.u0, quad.v0};
                pQuadVertices[1] = {quad.x + quad.width, quad.y, 0.0f, quad.u1, quad.v0};
                pQuadVertices[2] = {quad.x + quad.width, quad.y + quad.height, 0.0f, quad.u1, quad.v1};
                pQuadVertices[3] = {quad.x, quad.y, 0.0f, quad.u0, quad.v0};
                pQuadVertices[4] = {quad.x + quad.width, quad.y + quad.height, 0.0f, quad.u1, quad.v1};
                pQuadVertices[5] = {quad.x, quad.y + quad.height, 0.0f, quad.u0, quad.v1};
            }

            const GameplayHudBatchQuad &firstQuad = quads[quadIndex];
            bgfx::setTransform(modelMatrix);
            bgfx::setVertexBuffer(0, &transientVertexBuffer);
            bindTexture(
                0,
                m_hudRenderBackend.textureSamplerHandle,
                firstQuad.textureHandle,
                firstQuad.text ? TextureFilterProfile::Text : TextureFilterProfile::Ui,
                BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);

            if (firstQuad.clipped)
            {
                bgfx::setScissor(
                    firstQuad.scissorX,
                    firstQuad.scissorY,
                    firstQuad.scissorWidth,
                    firstQuad.scissorHeight);
            }

            bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_BLEND_ALPHA);
            submitHudDraw(firstQuad.colorAbgr, firstQuad.tintMode);

            if (firstQuad.clipped)
            {
                bgfx::setScissor(0, 0, static_cast<uint16_t>(screenWidth), static_cast<uint16_t>(screenHeight));
            }

            quadIndex += quadCount;
        }
    }
}

void GameplayUiRuntime::renderHudFontLayer(
    const GameplayHudFontHandle &font,
    GameplayHudFontColor textureHandle,
    const std::string &text,
    float textX,
    float textY,
    float fontScale) const
{
    const GameplayHudFontData *pFont = GameplayHudCommon::findHudFont(m_hudFontHandles, font.fontName);

    if (pFont == nullptr)
    {
        return;
    }

    m_fontGlyphQuads.clear();
    GameplayHudCommon::renderHudFontLayer(
        *pFont,
        textureHandle,
        text,
        textX,
        textY,
        fontScale,
        [this, textureHandle](bgfx::TextureHandle submittedTextureHandle,
               float x,
               float y,
               float quadWidth,
               float quadHeight,
               float u0,
               float v0,
               float u1,
               float v1,
               TextureFilterProfile filterProfile)
        {
            GameplayHudBatchQuad quad;
            quad.textureHandle = submittedTextureHandle;
            quad.colorAbgr = textureHandle.colorAbgr;
            quad.tintMode = textureHandle.tintMode;
            quad.text = true;
            quad.x = x; quad.y = y; quad.width = quadWidth; quad.height = quadHeight;
            quad.u0 = u0; quad.v0 = v0; quad.u1 = u1; quad.v1 = v1;
            m_fontGlyphQuads.push_back(quad);
        });
    submitHudQuadBatch(m_fontGlyphQuads, 0, 0);
}

bool GameplayUiRuntime::ensurePortraitRuntimeLoaded()
{
    if (m_portraitRuntimeLoaded)
    {
        return true;
    }

    if (m_pDataRepository == nullptr)
    {
        return false;
    }

    m_portraitRuntimeLoaded = true;
    return true;
}

void GameplayUiRuntime::resetPortraitFxStates(size_t memberCount)
{
    m_portraitFxStates.assign(memberCount, {});
}

void GameplayUiRuntime::resetPortraitPresentationState(size_t memberCount)
{
    m_portraitPresentationState.lastAnimationUpdateTicks = 0;
    m_portraitPresentationState.memberSpeechCooldownUntilTicks.assign(memberCount, 0);
    m_portraitPresentationState.memberCombatSpeechCooldownUntilTicks.assign(memberCount, 0);
}

GameplayPortraitPresentationState &GameplayUiRuntime::portraitPresentationState()
{
    return m_portraitPresentationState;
}

const GameplayPortraitPresentationState &GameplayUiRuntime::portraitPresentationState() const
{
    return m_portraitPresentationState;
}

std::string GameplayUiRuntime::resolvePortraitTextureName(const Character &character) const
{
    if (character.portraitState == PortraitId::Eradicated)
    {
        return "ERADCATE";
    }

    if (character.portraitState == PortraitId::Dead)
    {
        return "DEAD";
    }

    if (!m_portraitRuntimeLoaded)
    {
        return character.portraitTextureName;
    }

    if (character.portraitState == PortraitId::Normal)
    {
        return portraitTextureNameForCharacterFrame(character, 1);
    }

    const PortraitFrameEntry *pFrame =
        m_pDataRepository->portraitFrameTable().getFrame(character.portraitState, character.portraitElapsedTicks);

    if (pFrame != nullptr && pFrame->textureIndex > 0)
    {
        return portraitTextureNameForCharacterFrame(character, pFrame->textureIndex);
    }

    return character.portraitTextureName;
}

bool GameplayUiRuntime::triggerPortraitFxAnimation(
    const std::string &animationName,
    const std::vector<size_t> &memberIndices)
{
    if (!ensurePortraitRuntimeLoaded() || memberIndices.empty())
    {
        return false;
    }

    const std::optional<size_t> animationId = m_pDataRepository->iconFrameTable().findAnimationIdByName(animationName);

    if (!animationId)
    {
        return false;
    }

    const uint32_t startedTicks = currentAnimationTicks();
    bool triggered = false;

    for (size_t memberIndex : memberIndices)
    {
        if (memberIndex >= m_portraitFxStates.size())
        {
            // Party members can be recruited without reinitializing the current map UI.
            m_portraitFxStates.resize(memberIndex + 1);
        }

        GameplayPortraitFxState &state = m_portraitFxStates[memberIndex];
        state.active = true;
        state.animationId = *animationId;
        state.startedTicks = startedTicks;
        triggered = true;
    }

    return triggered;
}

void GameplayUiRuntime::triggerPortraitSpellFx(const PartySpellCastResult &result)
{
    if (!ensurePortraitRuntimeLoaded())
    {
        return;
    }

    const SpellFxEntry *pSpellFxEntry = m_pDataRepository->spellFxTable().findBySpellId(result.spellId);

    if (pSpellFxEntry == nullptr)
    {
        return;
    }

    triggerPortraitFxAnimation(pSpellFxEntry->animationName, result.affectedCharacterIndices);
}

const PortraitFxEventEntry *GameplayUiRuntime::findPortraitFxEvent(PortraitFxEventKind kind) const
{
    return m_pDataRepository != nullptr ? m_pDataRepository->portraitFxEventTable().findByKind(kind) : nullptr;
}

const FaceAnimationEntry *GameplayUiRuntime::findFaceAnimation(FaceAnimationId animationId) const
{
    return m_portraitRuntimeLoaded ? m_pDataRepository->faceAnimationTable().find(animationId) : nullptr;
}

uint32_t GameplayUiRuntime::defaultPortraitAnimationLengthTicks(PortraitId portraitId) const
{
    const int32_t animationLengthTicks = m_pDataRepository->portraitFrameTable().animationLengthTicks(portraitId);

    if (animationLengthTicks > 0)
    {
        return animationLengthTicks;
    }

    return 48u;
}

void GameplayUiRuntime::renderPortraitFx(
    size_t memberIndex,
    float portraitX,
    float portraitY,
    float portraitWidth,
    float portraitHeight) const
{
    if (memberIndex >= m_portraitFxStates.size() || !m_portraitRuntimeLoaded)
    {
        return;
    }

    const GameplayPortraitFxState &state = m_portraitFxStates[memberIndex];

    if (!state.active)
    {
        return;
    }

    const int32_t animationLengthTicks = m_pDataRepository->iconFrameTable().animationLengthTicks(state.animationId);
    const uint32_t nowTicks = currentAnimationTicks();
    const uint32_t elapsedTicks = nowTicks - state.startedTicks;

    if (animationLengthTicks <= 0 || elapsedTicks >= static_cast<uint32_t>(animationLengthTicks))
    {
        return;
    }

    const IconFrameEntry *pFrame = m_pDataRepository->iconFrameTable().getFrame(state.animationId, elapsedTicks);

    if (pFrame == nullptr || pFrame->textureName.empty())
    {
        return;
    }

    const std::optional<GameplayHudTextureHandle> texture =
        const_cast<GameplayUiRuntime *>(this)->ensureHudTextureLoaded(pFrame->textureName);

    if (!texture || texture->width <= 0 || texture->height <= 0)
    {
        return;
    }

    const float textureAspectRatio = static_cast<float>(texture->width) / static_cast<float>(texture->height);
    const float renderHeight = portraitHeight;
    const float renderWidth = renderHeight * textureAspectRatio;
    const float renderX = portraitX + (portraitWidth - renderWidth) * 0.5f;
    const float renderY = portraitY;
    submitHudTexturedQuad(texture->textureHandle, renderX, renderY, renderWidth, renderHeight);
}

bool GameplayUiRuntime::initializeHouseVideoPlayer()
{
    if (m_houseVideoPlayerInitialized)
    {
        return true;
    }

    m_houseVideoPlayerInitialized = m_houseVideoPlayer.initialize();
    return m_houseVideoPlayerInitialized;
}

void GameplayUiRuntime::shutdownHouseVideoPlayer()
{
    if (!m_houseVideoPlayerInitialized)
    {
        return;
    }

    m_houseVideoPlayer.shutdown();
    m_houseVideoPlayerInitialized = false;
}

void GameplayUiRuntime::stopHouseVideoPlayback()
{
    if (m_houseVideoPlayerInitialized)
    {
        m_houseVideoPlayer.stop();
    }
}

bool GameplayUiRuntime::playHouseVideo(const std::string &videoStem)
{
    return initializeHouseVideoPlayer()
        && m_pAssetFileSystem != nullptr
        && m_houseVideoPlayer.play(*m_pAssetFileSystem, videoStem);
}

bool GameplayUiRuntime::playHouseVideo(const std::string &videoStem, const std::string &videoDirectory)
{
    return initializeHouseVideoPlayer()
        && m_pAssetFileSystem != nullptr
        && m_houseVideoPlayer.play(*m_pAssetFileSystem, videoStem, videoDirectory);
}

void GameplayUiRuntime::setHouseVideoAudioVolume(float volume)
{
    if (initializeHouseVideoPlayer())
    {
        m_houseVideoPlayer.setAudioVolume(volume);
    }
}

void GameplayUiRuntime::updateHouseVideoPlayback(float deltaSeconds)
{
    if (m_houseVideoPlayerInitialized)
    {
        m_houseVideoPlayer.update(deltaSeconds);
    }
}

bool GameplayUiRuntime::renderHouseVideoFrame(float x, float y, float quadWidth, float quadHeight) const
{
    if (!m_houseVideoPlayerInitialized || !m_houseVideoPlayer.hasActiveFrame())
    {
        return false;
    }

    submitHudTexturedQuad(m_houseVideoPlayer.textureHandle(), x, y, quadWidth, quadHeight);
    return true;
}

void GameplayUiRuntime::clearPortraitRuntime()
{
    m_lastPortraits.clear();
    m_portraitRuntimeLoaded = false;
    m_portraitFxStates.clear();
    m_portraitPresentationState = {};
}

void GameplayUiRuntime::clearHudResources()
{
    if (canUseBgfxResources())
    {
        if (bgfx::isValid(m_hudTintProgram)) { bgfx::destroy(m_hudTintProgram); }
        if (bgfx::isValid(m_hudTintUniform)) { bgfx::destroy(m_hudTintUniform); }
        if (bgfx::isValid(m_hudTintModeUniform)) { bgfx::destroy(m_hudTintModeUniform); }
    }
    m_hudTintProgram = BGFX_INVALID_HANDLE;
    m_hudTintUniform = BGFX_INVALID_HANDLE;
    m_hudTintModeUniform = BGFX_INVALID_HANDLE;
    m_assetsPreloaded = false;
    m_hudAllocationRetries.clear();
    if (!canUseBgfxResources())
    {
        for (GameplayHudTextureData &textureHandle : m_hudTextureHandles)
        {
            textureHandle.textureHandle = BGFX_INVALID_HANDLE;
        }

        for (GameplayHudFontData &fontHandle : m_hudFontHandles)
        {
            fontHandle.mainTextureHandle = BGFX_INVALID_HANDLE;
            fontHandle.shadowTextureHandle = BGFX_INVALID_HANDLE;
        }

        for (GameplayHudFontColorTextureData &textureHandle : m_hudFontColorTextureHandles)
        {
            textureHandle.textureHandle = BGFX_INVALID_HANDLE;
        }

        for (GameplayHudTextureColorTextureData &textureHandle : m_hudTextureColorTextureHandles)
        {
            textureHandle.textureHandle = BGFX_INVALID_HANDLE;
        }

        m_hudTextureHandles.clear();
        m_hudTextureIndexByName.clear();
        m_dynamicHudTextureContentSignatures.clear();
        m_hudFontHandles.clear();
        m_hudFontColorTextureHandles.clear();
        m_hudTextureColorTextureHandles.clear();
        return;
    }

    for (GameplayHudTextureData &textureHandle : m_hudTextureHandles)
    {
        if (bgfx::isValid(textureHandle.textureHandle))
        {
            bgfx::destroy(textureHandle.textureHandle);
            textureHandle.textureHandle = BGFX_INVALID_HANDLE;
        }
    }

    for (GameplayHudFontData &fontHandle : m_hudFontHandles)
    {
        if (bgfx::isValid(fontHandle.mainTextureHandle))
        {
            bgfx::destroy(fontHandle.mainTextureHandle);
            fontHandle.mainTextureHandle = BGFX_INVALID_HANDLE;
        }

        if (bgfx::isValid(fontHandle.shadowTextureHandle))
        {
            bgfx::destroy(fontHandle.shadowTextureHandle);
            fontHandle.shadowTextureHandle = BGFX_INVALID_HANDLE;
        }
    }

    for (GameplayHudFontColorTextureData &textureHandle : m_hudFontColorTextureHandles)
    {
        if (bgfx::isValid(textureHandle.textureHandle))
        {
            bgfx::destroy(textureHandle.textureHandle);
            textureHandle.textureHandle = BGFX_INVALID_HANDLE;
        }
    }

    for (GameplayHudTextureColorTextureData &textureHandle : m_hudTextureColorTextureHandles)
    {
        if (bgfx::isValid(textureHandle.textureHandle))
        {
            bgfx::destroy(textureHandle.textureHandle);
            textureHandle.textureHandle = BGFX_INVALID_HANDLE;
        }
    }

    m_hudTextureHandles.clear();
    m_hudTextureIndexByName.clear();
    m_dynamicHudTextureContentSignatures.clear();
    m_hudFontHandles.clear();
    m_hudFontColorTextureHandles.clear();
    m_hudTextureColorTextureHandles.clear();
}
}

namespace OpenYAMM::Game
{
void GameplayUiRuntime::prefetchMenuStates(const std::string &screen)
{
#if defined(OPENYAMM_VITA_LOADER)
    // Only the currently open menu. The existing layout index avoids a catalogue scan.
    for (const std::string &id : sortedHudLayoutIdsForScreen(screen))
    {
        const auto *layout = findHudLayoutElement(id);
        if (!layout || !layout->visible || !layout->interactive) { continue; }
        for (const std::string *asset : {&layout->primaryAsset, &layout->hoverAsset, &layout->pressedAsset})
            if (!asset->empty()) { ensureHudTextureLoaded(*asset); }
    }
#else
    (void)screen;
#endif
}
}
