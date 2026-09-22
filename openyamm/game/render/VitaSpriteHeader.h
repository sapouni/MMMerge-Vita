#pragma once
#include "engine/AssetFileSystem.h"
#include "engine/ImageAssetLoader.h"
#include "engine/VitaFrameProfile.h"
#include <array>
#include <span>

namespace OpenYAMM::Game
{
inline bool vitaSpriteHeaderDimensions(std::span<const uint8_t> bytes, int &width, int &height)
{
    if (bytes.size() < 32) { return false; }
    const auto little = [&bytes](size_t offset, size_t count)
    {
        uint32_t result = 0;
        for (size_t i = 0; i < count; ++i) { result |= uint32_t(bytes[offset + i]) << (8 * i); }
        return result;
    };
    uint32_t w = 0, h = 0;
    if (bytes[0] == 'B' && bytes[1] == 'M')
    {
        if (little(14, 4) == 12) { w = little(18, 2); h = little(20, 2); }
        else
        {
            w = little(18, 4); h = little(22, 4);
            if (h & 0x80000000u) { h = ~h + 1; }
        }
    }
    else if (bytes[0] == 0x89 && bytes[1] == 'P' && bytes[2] == 'N' && bytes[3] == 'G')
    {
        for (size_t i = 0; i < 4; ++i) { w = (w << 8) | bytes[16 + i]; h = (h << 8) | bytes[20 + i]; }
    }
    else if (bytes[0] == 10 && bytes[2] == 1)
    {
        if (little(8, 2) < little(4, 2) || little(10, 2) < little(6, 2)) { return false; }
        w = little(8, 2) - little(4, 2) + 1; h = little(10, 2) - little(6, 2) + 1;
    }
    if (!w || !h || w > 4096 || h > 4096) { return false; }
    width = int(w); height = int(h);
    return true;
}

template<class Cache>
bool readVitaSpriteDimensions(const Engine::AssetFileSystem *pFileSystem, Cache &cache,
    const std::string &name, int &width, int &height)
{
    Engine::VitaProfile::Scope timing(Engine::VitaProfile::SpriteHeader);
    if (pFileSystem == nullptr) { return false; }
    const std::optional<std::string> path = Engine::findImageAssetPath(*pFileSystem, "Data/sprites", name,
        cache.directoryAssetPathsByPath, cache.assetPathByKey);
    if (!path) { return false; }
    const std::unique_ptr<Engine::AssetReadStream> stream = pFileSystem->openReadStream(*path);
    if (!stream) { return false; }
    std::array<uint8_t, 32> header{};
    size_t total = 0;
    while (total < header.size())
    {
        const int64_t count = stream->read(header.data() + total, header.size() - total);
        if (count <= 0) { return false; }
        total += size_t(count);
    }
    return vitaSpriteHeaderDimensions(header, width, height);
}

inline uint64_t vitaSpriteBackingBytes(int width, int height)
{
    uint64_t w = 1, h = 1;
    while (w < uint64_t(width)) { w *= 2; }
    while (h < uint64_t(height)) { h *= 2; }
    return w * h * 4;
}
}
