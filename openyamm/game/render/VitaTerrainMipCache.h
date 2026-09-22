#pragma once
#include <algorithm>
#include <cstdint>
#include <map>
#include <vector>

namespace OpenYAMM::Game
{
struct VitaTerrainMip
{
    uint16_t x = 0, y = 0, width = 0, height = 0;
    uint8_t level = 0;
    std::vector<uint8_t> rgba;
};
struct VitaTerrainMipChain
{
    std::vector<VitaTerrainMip> mips;
    uint64_t bytes = 0;
};

// Match the existing atlas-region rounding, edge clamping and integer mip filter.
// Keep upload-ready RGBA so the Android path need not swap channels on every frame.
inline VitaTerrainMipChain buildVitaTerrainMips(int atlasWidth, int atlasHeight, int innerX, int innerY,
    int tileSize, int padding, const std::vector<uint8_t> &bgra)
{
    VitaTerrainMipChain result;
    if (atlasWidth <= 0 || atlasHeight <= 0 || tileSize <= 0 || padding < 0
        || bgra.size() < uint64_t(tileSize) * tileSize * 4) { return result; }
    int x = std::max(0, innerX - padding), y = std::max(0, innerY - padding);
    int width = std::min(atlasWidth, innerX + tileSize + padding) - x;
    int height = std::min(atlasHeight, innerY + tileSize + padding) - y;
    if (width <= 0 || height <= 0) { return result; }
    std::vector<uint8_t> pixels(size_t(width) * height * 4);
    for (int row = 0; row < height; ++row)
    {
        const int sourceY = std::clamp(y + row - innerY, 0, tileSize - 1);
        for (int column = 0; column < width; ++column)
        {
            const int sourceX = std::clamp(x + column - innerX, 0, tileSize - 1);
            const size_t from = (size_t(sourceY) * tileSize + sourceX) * 4;
            const size_t to = (size_t(row) * width + column) * 4;
            pixels[to] = bgra[from + 2]; pixels[to + 1] = bgra[from + 1];
            pixels[to + 2] = bgra[from]; pixels[to + 3] = bgra[from + 3];
        }
    }
    uint8_t level = 0;
    for (;;)
    {
        result.bytes += pixels.size();
        result.mips.push_back({uint16_t(x), uint16_t(y), uint16_t(width), uint16_t(height),
            level, std::move(pixels)});
        if (atlasWidth == 1 && atlasHeight == 1) { break; }
        const int nextAtlasWidth = std::max(1, atlasWidth / 2);
        const int nextAtlasHeight = std::max(1, atlasHeight / 2);
        const int nextX = x / 2, nextY = y / 2;
        const int nextWidth = std::min(nextAtlasWidth, (x + width + 1) / 2) - nextX;
        const int nextHeight = std::min(nextAtlasHeight, (y + height + 1) / 2) - nextY;
        if (nextWidth <= 0 || nextHeight <= 0) { break; }
        pixels.resize(size_t(nextWidth) * nextHeight * 4);
        const std::vector<uint8_t> &previous = result.mips.back().rgba;
        for (int row = 0; row < nextHeight; ++row)
        {
            for (int column = 0; column < nextWidth; ++column)
            {
                uint32_t sum[4] = {};
                for (int dy = 0; dy < 2; ++dy)
                {
                    const int sourceY = std::clamp((nextY + row) * 2 + dy, y, y + height - 1);
                    for (int dx = 0; dx < 2; ++dx)
                    {
                        const int sourceX = std::clamp((nextX + column) * 2 + dx, x, x + width - 1);
                        const size_t offset = (size_t(sourceY - y) * width + sourceX - x) * 4;
                        for (unsigned c = 0; c < 4; ++c) { sum[c] += previous[offset + c]; }
                    }
                }
                const size_t offset = (size_t(row) * nextWidth + column) * 4;
                for (unsigned c = 0; c < 4; ++c) { pixels[offset + c] = uint8_t((sum[c] + 2) / 4); }
            }
        }
        x = nextX; y = nextY; width = nextWidth; height = nextHeight;
        atlasWidth = nextAtlasWidth; atlasHeight = nextAtlasHeight; ++level;
    }
    return result;
}

class VitaTerrainMipCache
{
public:
    static constexpr uint64_t Limit = 8u * 1024 * 1024;
    explicit VitaTerrainMipCache(uint64_t limit = Limit) : m_limit(limit) {}
    void clear()
    {
        m_entries.clear(); bytes = peak = hits = misses = evictions = 0; m_tick = 0;
    }
    template<class Build> const VitaTerrainMipChain *get(uint64_t key, Build build)
    {
        ++m_tick;
        auto found = m_entries.find(key);
        if (found != m_entries.end())
        {
            ++hits; found->second.used = m_tick; return &found->second.chain;
        }
        ++misses;
        VitaTerrainMipChain chain = build();
        if (!chain.bytes || chain.bytes > m_limit) { return nullptr; }
        while (bytes + chain.bytes > m_limit)
        {
            auto oldest = std::min_element(m_entries.begin(), m_entries.end(),
                [](const auto &a, const auto &b) { return a.second.used < b.second.used; });
            bytes -= oldest->second.chain.bytes; m_entries.erase(oldest); ++evictions;
        }
        bytes += chain.bytes; peak = std::max(peak, bytes);
        return &m_entries.emplace(key, Entry{m_tick, std::move(chain)}).first->second.chain;
    }
    uint64_t bytes = 0, peak = 0, hits = 0, misses = 0, evictions = 0;
private:
    struct Entry { uint64_t used; VitaTerrainMipChain chain; };
    uint64_t m_limit, m_tick = 0;
    std::map<uint64_t, Entry> m_entries;
};
}
