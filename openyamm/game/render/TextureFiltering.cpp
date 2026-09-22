#include <memory>
#include "game/render/VitaPerformanceConfig.h"
#include "engine/VitaLoadProfile.h"
#include <cstring>
#include "engine/VitaFrameProfile.h"
#include "game/render/TextureFiltering.h"
#include "game/render/VitaTextureUploadBatch.h"

#include <algorithm>
#include <new>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace OpenYAMM::Game
{
namespace
{
bool g_textureFilteringEnabled = true;
TextureFilteringConfig g_textureFilteringConfig = {};

bool profileUsesMipmaps(TextureFilterProfile profile)
{
    return profile == TextureFilterProfile::Terrain
        || profile == TextureFilterProfile::BModel
        || profile == TextureFilterProfile::Sky;
}

bool profileNeedsTransparentEdgeBleed(TextureFilterProfile profile)
{
    switch (profile)
    {
        case TextureFilterProfile::Billboard:
        case TextureFilterProfile::Ui:
            return true;

        case TextureFilterProfile::Terrain:
        case TextureFilterProfile::BModel:
        case TextureFilterProfile::Lightmap:
        case TextureFilterProfile::Sky:
        case TextureFilterProfile::Text:
            return false;
    }

    return false;
}

TextureFilterMode textureFilterModeForProfile(TextureFilterProfile profile)
{
#if defined(OPENYAMM_VITA_LOADER)
    if (vitaPerformanceSettings().lowTextureFiltering
        && (profile == TextureFilterProfile::Terrain || profile == TextureFilterProfile::BModel
            || profile == TextureFilterProfile::Lightmap || profile == TextureFilterProfile::Sky))
    {
        return TextureFilterMode::Linear;
    }
#endif
    switch (profile)
    {
        case TextureFilterProfile::Terrain:
            return g_textureFilteringConfig.terrain;

        case TextureFilterProfile::BModel:
            return g_textureFilteringConfig.bmodel;

        case TextureFilterProfile::Lightmap:
            return g_textureFilteringConfig.bmodel;

        case TextureFilterProfile::Sky:
            return g_textureFilteringConfig.sky;

        case TextureFilterProfile::Billboard:
            return g_textureFilteringConfig.billboard;

        case TextureFilterProfile::Ui:
            return g_textureFilteringConfig.ui;

        case TextureFilterProfile::Text:
            return g_textureFilteringConfig.text;
    }

    return TextureFilterMode::Linear;
}

uint64_t textureFilterSamplerFlagsForMode(TextureFilterMode mode)
{
    switch (mode)
    {
        case TextureFilterMode::Nearest:
            return BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT | BGFX_SAMPLER_MIP_POINT;

        case TextureFilterMode::Linear:
            return BGFX_SAMPLER_NONE;

        case TextureFilterMode::Anisotropic:
            return BGFX_SAMPLER_MIN_ANISOTROPIC | BGFX_SAMPLER_MAG_ANISOTROPIC;
    }

    return BGFX_SAMPLER_NONE;
}

bool hasMixedTransparency(const uint8_t *pPixels, size_t pixelCount)
{
    bool foundTransparent = false;
    bool foundOpaque = false;

    for (size_t pixelIndex = 0; pixelIndex < pixelCount; ++pixelIndex)
    {
        const uint8_t alpha = pPixels[pixelIndex * 4 + 3];

        if (alpha == 0)
        {
            foundTransparent = true;
        }
        else
        {
            foundOpaque = true;
        }

        if (foundTransparent && foundOpaque)
        {
            return true;
        }
    }

    return false;
}

void bleedTransparentEdgeColors(uint16_t width, uint16_t height, std::vector<uint8_t> &pixels)
{
    OY_LOAD(Engine::VitaLoad::Scope loadBleed(Engine::VitaLoad::EdgeBleed));
    Engine::VitaProfile::Scope timing(Engine::VitaProfile::EdgeBleed);
    const size_t pixelCount = static_cast<size_t>(width) * static_cast<size_t>(height);

    if (pixelCount == 0 || pixels.size() < pixelCount * 4)
    {
        return;
    }

    std::vector<bool> visited(pixelCount, false);
    std::vector<uint32_t> queue;
    queue.reserve(pixelCount);

    for (uint32_t pixelIndex = 0; pixelIndex < pixelCount; ++pixelIndex)
    {
        if (pixels[static_cast<size_t>(pixelIndex) * 4 + 3] != 0)
        {
            visited[pixelIndex] = true;
            queue.push_back(pixelIndex);
        }
    }

    size_t queueHead = 0;

    while (queueHead < queue.size())
    {
        const uint32_t currentIndex = queue[queueHead++];
        const uint32_t x = currentIndex % width;
        const uint32_t y = currentIndex / width;

        for (int dy = -1; dy <= 1; ++dy)
        {
            for (int dx = -1; dx <= 1; ++dx)
            {
                if (dx == 0 && dy == 0)
                {
                    continue;
                }

                const int neighborX = static_cast<int>(x) + dx;
                const int neighborY = static_cast<int>(y) + dy;

                if (neighborX < 0
                    || neighborY < 0
                    || neighborX >= static_cast<int>(width)
                    || neighborY >= static_cast<int>(height))
                {
                    continue;
                }

                const uint32_t neighborIndex =
                    static_cast<uint32_t>(neighborY) * static_cast<uint32_t>(width) + static_cast<uint32_t>(neighborX);
                const size_t neighborOffset = static_cast<size_t>(neighborIndex) * 4;

                if (visited[neighborIndex])
                {
                    continue;
                }

                visited[neighborIndex] = true;
                const size_t sourceOffset = size_t(currentIndex) * 4;
                pixels[neighborOffset] = pixels[sourceOffset];
                pixels[neighborOffset + 1] = pixels[sourceOffset + 1];
                pixels[neighborOffset + 2] = pixels[sourceOffset + 2];
                queue.push_back(neighborIndex);
            }
        }
    }


}

std::vector<uint8_t> downsampleBgraPixels(
    const uint8_t *sourcePixels,
    uint16_t sourceWidth,
    uint16_t sourceHeight,
    uint16_t targetWidth,
    uint16_t targetHeight)
{
    std::vector<uint8_t> targetPixels(static_cast<size_t>(targetWidth) * static_cast<size_t>(targetHeight) * 4, 0);

    if (sourceWidth == 0 || sourceHeight == 0 || targetWidth == 0 || targetHeight == 0)
    {
        return targetPixels;
    }

    for (uint16_t targetY = 0; targetY < targetHeight; ++targetY)
    {
        for (uint16_t targetX = 0; targetX < targetWidth; ++targetX)
        {
            uint32_t blue = 0;
            uint32_t green = 0;
            uint32_t red = 0;
            uint32_t alpha = 0;

            for (uint16_t offsetY = 0; offsetY < 2; ++offsetY)
            {
                const uint16_t sourceY =
                    std::min<uint16_t>(static_cast<uint16_t>(targetY * 2 + offsetY), sourceHeight - 1);

                for (uint16_t offsetX = 0; offsetX < 2; ++offsetX)
                {
                    const uint16_t sourceX =
                        std::min<uint16_t>(static_cast<uint16_t>(targetX * 2 + offsetX), sourceWidth - 1);
                    const size_t sourceOffset =
                        (static_cast<size_t>(sourceY) * static_cast<size_t>(sourceWidth) + sourceX) * 4;

                    blue += sourcePixels[sourceOffset + 0];
                    green += sourcePixels[sourceOffset + 1];
                    red += sourcePixels[sourceOffset + 2];
                    alpha += sourcePixels[sourceOffset + 3];
                }
            }

            const size_t targetOffset =
                (static_cast<size_t>(targetY) * static_cast<size_t>(targetWidth) + targetX) * 4;
            targetPixels[targetOffset + 0] = static_cast<uint8_t>((blue + 2) / 4);
            targetPixels[targetOffset + 1] = static_cast<uint8_t>((green + 2) / 4);
            targetPixels[targetOffset + 2] = static_cast<uint8_t>((red + 2) / 4);
            targetPixels[targetOffset + 3] = static_cast<uint8_t>((alpha + 2) / 4);
        }
    }

    return targetPixels;
}

bgfx::TextureHandle createBgraTextureWithMipChain(
    uint16_t width,
    uint16_t height,
    const uint8_t *pPixels,
    uint32_t pixelBytes,
    TextureFilterProfile profile,
    uint64_t extraFlags)
{
    if (pPixels == nullptr || !width || !height
        || uint64_t(width) * height * 4 > pixelBytes)
    {
        return BGFX_INVALID_HANDLE;
    }

    const bgfx::TextureHandle textureHandle = bgfx::createTexture2D(
        width, height, true, 1, bgraTextureUploadFormat(), textureFilterSamplerFlags(profile) | extraFlags);
    if (!bgfx::isValid(textureHandle))
    {
        return BGFX_INVALID_HANDLE;
    }

    try
    {
    uint16_t mipWidth = width;
    uint16_t mipHeight = height;
    uint8_t mipIndex = 0;
    const uint8_t *pMipPixels = pPixels;
    std::vector<uint8_t> currentMip;
    for (;;)
    {
        const uint32_t mipBytes = static_cast<uint32_t>(mipWidth) * mipHeight * 4;
#if defined(OPENYAMM_VITA_LOADER)
        for (uint16_t row = 0; row < mipHeight;)
            row += uploadBgraTextureRows(textureHandle, mipIndex, mipWidth, mipHeight, row, pMipPixels);
#else
        bgfx::updateTexture2D(textureHandle, 0, mipIndex, 0, 0, mipWidth, mipHeight,
                             copyBgraTextureUploadMemory(pMipPixels, mipBytes));
        accountVitaTextureUpload(mipBytes);
#endif
        if (mipWidth == 1 && mipHeight == 1)
        {
            break;
        }
        const uint16_t nextWidth = std::max<uint16_t>(1, mipWidth / 2);
        const uint16_t nextHeight = std::max<uint16_t>(1, mipHeight / 2);
        std::vector<uint8_t> nextMip = downsampleBgraPixels(
            pMipPixels, mipWidth, mipHeight, nextWidth, nextHeight);
        currentMip = std::move(nextMip);
        pMipPixels = currentMip.data();
        mipWidth = nextWidth;
        mipHeight = nextHeight;
        ++mipIndex;
    }

    }
    catch (...) { bgfx::destroy(textureHandle); throw; }
    return textureHandle;
}
}

uint64_t textureFilterSamplerFlags(TextureFilterProfile profile)
{
    return textureFilterSamplerFlagsForMode(textureFilterModeForProfile(profile));
}

bool textureFilteringEnabled()
{
    return g_textureFilteringEnabled;
}

void setTextureFilteringEnabled(bool enabled)
{
    g_textureFilteringEnabled = enabled;
    g_textureFilteringConfig.enabled = enabled;
}

void setTextureFilteringConfig(const TextureFilteringConfig &config)
{
    g_textureFilteringConfig = config;
    g_textureFilteringEnabled = config.enabled;
}

bool toggleTextureFilteringEnabled()
{
    g_textureFilteringEnabled = !g_textureFilteringEnabled;
    g_textureFilteringConfig.enabled = g_textureFilteringEnabled;
    return g_textureFilteringEnabled;
}

uint32_t textureBindingSamplerFlags(TextureFilterProfile profile, uint32_t extraFlags)
{
    uint64_t profileFlags = textureFilterSamplerFlags(profile);

    if (!g_textureFilteringEnabled)
    {
        profileFlags = BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT | BGFX_SAMPLER_MIP_POINT;
    }

    return uint32_t(profileFlags) | extraFlags;
}

bgfx::TextureFormat::Enum bgraTextureUploadFormat()
{
#if defined(__ANDROID__)
    return bgfx::TextureFormat::RGBA8;
#else
    return bgfx::TextureFormat::BGRA8;
#endif
}

const bgfx::Memory *copyBgraTextureUploadMemory(const uint8_t *pPixels, uint32_t pixelBytes)
{
    if (pPixels == nullptr || pixelBytes == 0)
    {
        return nullptr;
    }

#if defined(__ANDROID__)
    // Convert directly into upload-owned memory instead of allocating a second
    // full-sized temporary image before bgfx::copy.
    const bgfx::Memory *pMemory = bgfx::alloc(pixelBytes);
    if (pMemory == nullptr) { throw std::bad_alloc(); }
    uint32_t byteOffset = 0;
    for (; byteOffset + 3 < pixelBytes; byteOffset += 4)
    {
        pMemory->data[byteOffset + 0] = pPixels[byteOffset + 2];
        pMemory->data[byteOffset + 1] = pPixels[byteOffset + 1];
        pMemory->data[byteOffset + 2] = pPixels[byteOffset + 0];
        pMemory->data[byteOffset + 3] = pPixels[byteOffset + 3];
    }
    for (; byteOffset < pixelBytes; ++byteOffset)
    {
        pMemory->data[byteOffset] = 0;
    }
    return pMemory;
#else
    return bgfx::copy(pPixels, pixelBytes);
#endif
}

bool prepareBgraTexturePixelsForUploadInPlace(
    uint16_t width,
    uint16_t height,
    std::vector<uint8_t> &pixels,
    TextureFilterProfile profile)
{
    const size_t pixelCount = static_cast<size_t>(width) * static_cast<size_t>(height);
    const size_t expectedPixelBytes = pixelCount * 4;

    if (!profileNeedsTransparentEdgeBleed(profile)
        || pixelCount == 0
        || pixels.size() < expectedPixelBytes
        || !hasMixedTransparency(pixels.data(), pixelCount))
    {
        return false;
    }

#if defined(OPENYAMM_VITA_LOADER)
    if (profile == TextureFilterProfile::Ui)
    {
        // UI textures have no mipmaps: bilinear filtering only needs the adjacent transparent texel.
        // Preserve alpha and opaque colors; avoid an image-sized BFS queue (9.6 MB for Arcomage).
        for (int y = 0; y < height; ++y)
        {
            for (int x = 0; x < width; ++x)
            {
                const size_t dst = (size_t(y) * width + x) * 4;
                if (pixels[dst + 3]) { continue; }
                bool found = false;
                for (int dy = -1; dy <= 1 && !found; ++dy)
                {
                    for (int dx = -1; dx <= 1; ++dx)
                    {
                        const int sx = x + dx, sy = y + dy;
                        if (sx < 0 || sy < 0 || sx >= width || sy >= height) { continue; }
                        const size_t from = (size_t(sy) * width + sx) * 4;
                        if (!pixels[from + 3]) { continue; }
                        std::copy_n(pixels.data() + from, 3, pixels.data() + dst);
                        found = true; break;
                    }
                }
            }
        }
        return true;
    }
#endif
    bleedTransparentEdgeColors(width, height, pixels);
    return true;
}

void bindTexture(
    uint8_t stage,
    bgfx::UniformHandle sampler,
    bgfx::TextureHandle textureHandle,
    TextureFilterProfile profile,
    uint32_t extraFlags)
{
    bgfx::setTexture(stage, sampler, textureHandle, textureBindingSamplerFlags(profile, extraFlags));
}

bgfx::TextureHandle createBgraTexture2D(
    uint16_t width,
    uint16_t height,
    const uint8_t *pPixels,
    uint32_t pixelBytes,
    TextureFilterProfile profile,
    uint64_t extraFlags,
    BgraTexturePixelPreparation pixelPreparation,
    const char *pDiagnosticName)
{
    OY_LOAD(Engine::VitaLoad::Scope loadUpload(Engine::VitaLoad::TexturePrepareSubmit));
    OY_LOAD(loadUpload.bytes(pixelBytes));
    if (pPixels == nullptr || !width || !height
        || uint64_t(width) * height * 4 > pixelBytes)
    {
        return BGFX_INVALID_HANDLE;
    }

    std::vector<uint8_t> uploadPixels;

    if (pixelPreparation == BgraTexturePixelPreparation::Required
        && profileNeedsTransparentEdgeBleed(profile))
    {
        const size_t pixelCount = static_cast<size_t>(width) * static_cast<size_t>(height);

        if (pixelBytes >= pixelCount * 4 && hasMixedTransparency(pPixels, pixelCount))
        {
            uploadPixels.assign(pPixels, pPixels + pixelBytes);
            prepareBgraTexturePixelsForUploadInPlace(width, height, uploadPixels, profile);
        }
    }

    const uint8_t *pUploadPixels = uploadPixels.empty() ? pPixels : uploadPixels.data();

    if (profileUsesMipmaps(profile))
    {
        const bgfx::TextureHandle mipTextureHandle =
            createBgraTextureWithMipChain(width, height, pUploadPixels, pixelBytes, profile, extraFlags);

        if (bgfx::isValid(mipTextureHandle))
        {
            return mipTextureHandle;
        }
    }

#if defined(OPENYAMM_VITA_LOADER)
    if (pixelBytes > 64 * 1024)
    {
        const bgfx::TextureHandle texture = createEmptyBgraTexture2D(width, height, profile, extraFlags);
        if (!bgfx::isValid(texture)) { return texture; }
        try
        {
            for (uint16_t row = 0; row < height;)
                row += uploadBgraTextureRows(texture, 0, width, height, row, pUploadPixels);
        }
        catch (...) { bgfx::destroy(texture); throw; }
        return texture;
    }
#endif
    const bgfx::Memory *pUploadMemory = copyBgraTextureUploadMemory(pUploadPixels, pixelBytes);
#if defined(OPENYAMM_VITA_PROFILE)
    static unsigned foliageAudits = 0;
    if (pDiagnosticName != nullptr && std::strstr(pDiagnosticName, "tree") != nullptr && foliageAudits < 64)
    {
        ++foliageAudits;
        unsigned alphaZero = 0, zeroAlphaCyan = 0, visibleCyan = 0;
        uint32_t hash = 2166136261u;
        for (uint32_t i = 0; i < pixelBytes; ++i) { hash = (hash ^ pUploadMemory->data[i]) * 16777619u; }
        for (uint32_t i = 0; i + 3 < pixelBytes; i += 4)
        {
            const uint8_t *p = pUploadMemory->data + i; // Actual RGBA upload bytes on Android/Vita.
            const bool cyan = p[0] <= 8 && p[1] >= 248 && p[2] >= 248;
            alphaZero += p[3] == 0;
            zeroAlphaCyan += cyan && p[3] == 0;
            visibleCyan += cyan && p[3] != 0;
        }
        Engine::VitaProfile::print(
            "PROFILE FOLIAGE_UPLOAD name=%s w=%u h=%u format=RGBA8 mips=1 prepared=%u "
            "alpha_zero=%u zero_alpha_cyan=%u visible_cyan=%u rgba_fnv=%08x\n",
            pDiagnosticName, width, height, unsigned(pixelPreparation),
            alphaZero, zeroAlphaCyan, visibleCyan, hash);
    }
#else
    (void)pDiagnosticName;
#endif
    const bgfx::TextureHandle handle = bgfx::createTexture2D(
        width, height, false, 1, bgraTextureUploadFormat(),
        textureFilterSamplerFlags(profile) | extraFlags, pUploadMemory);
    accountVitaTextureUpload(pixelBytes);
    return handle;
}

bgfx::TextureHandle createOwnedUiTexture2D(uint16_t width, uint16_t height, std::vector<uint8_t> pixels)
{
    if (!width || !height || pixels.size() != size_t(width) * height * 4) { return BGFX_INVALID_HANDLE; }
    prepareBgraTexturePixelsForUploadInPlace(width, height, pixels, TextureFilterProfile::Ui);
#if defined(__ANDROID__)
    for (size_t i = 0; i < pixels.size(); i += 4) { std::swap(pixels[i], pixels[i + 2]); }
#endif
    auto owner = std::make_unique<std::vector<uint8_t>>(std::move(pixels));
    const bgfx::Memory *memory = bgfx::makeRef(owner->data(), uint32_t(owner->size()),
        [](void *, void *user) { delete static_cast<std::vector<uint8_t> *>(user); }, owner.get());
    if (!memory) { return BGFX_INVALID_HANDLE; }
    owner.release(); // bgfx releases the upload even if texture creation fails.
    return bgfx::createTexture2D(width, height, false, 1, bgraTextureUploadFormat(),
        textureFilterSamplerFlags(TextureFilterProfile::Ui) | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP, memory);
}

bgfx::TextureHandle createEmptyBgraTexture2D(
    uint16_t width,
    uint16_t height,
    TextureFilterProfile profile,
    uint64_t extraFlags)
{
    return bgfx::createTexture2D(
        width,
        height,
        false,
        1,
        bgraTextureUploadFormat(),
        textureFilterSamplerFlags(profile) | extraFlags);
}
}

namespace OpenYAMM::Game
{
uint16_t uploadBgraTextureRows(bgfx::TextureHandle texture, uint8_t mip, uint16_t width,
    uint16_t height, uint16_t firstRow, const uint8_t *pixels)
{
    if (!width || firstRow >= height || !pixels) { return 0; }
    const uint32_t stride = uint32_t(width) * 4;
    const uint16_t rows = uint16_t(std::min<uint32_t>(height - firstRow,
        std::max<uint32_t>(1, (64 * 1024) / stride)));
    const uint32_t bytes = stride * rows;
    bgfx::updateTexture2D(texture, 0, mip, 0, firstRow, width, rows,
        copyBgraTextureUploadMemory(pixels + size_t(firstRow) * stride, bytes));
    accountVitaTextureUpload(bytes);
    return rows;
}
}
