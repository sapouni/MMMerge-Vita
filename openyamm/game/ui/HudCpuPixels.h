#pragma once

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <new>
#include <utility>
#include <vector>

namespace OpenYAMM::Game
{
// Lossless retained CPU copy. GPU uploads remain BGRA; picks read indexed alpha
// directly, and a new tint expands only its one temporary upload buffer.
class HudCpuPixels
{
public:
    HudCpuPixels &operator=(std::vector<uint8_t> pixels)
    {
        m_raw = std::move(pixels);
        m_wideIndices = false;
        std::vector<uint8_t>().swap(m_indices);
        std::vector<uint32_t>().swap(m_palette);
        return *this;
    }
    void assign(size_t count, uint8_t value) { *this = std::vector<uint8_t>(count, value); }
    bool empty() const { return m_raw.empty() && m_indices.empty(); }
    size_t size() const { return m_indices.empty() ? m_raw.size() : m_indices.size() / (m_wideIndices ? 2 : 1) * 4; }
    size_t capacity() const
    {
        return m_raw.capacity() + m_indices.capacity() + m_palette.capacity() * sizeof(uint32_t);
    }
    bool indexed() const { return !m_indices.empty(); }
    const uint8_t *data() const
    {
        assert(!indexed()); // Only initial upload may request a contiguous pointer.
        return m_raw.data();
    }
    uint8_t operator[](size_t offset) const
    {
        assert(offset < size());
        return indexed() ? uint8_t(m_palette[paletteIndex(offset / 4)] >> ((offset % 4) * 8)) : m_raw[offset];
    }
    std::vector<uint8_t> expanded() const
    {
        if (!indexed()) { return m_raw; }
        std::vector<uint8_t> pixels(size());
        for (size_t i = 0; i < pixels.size() / 4; ++i)
        {
            const uint32_t color = m_palette[paletteIndex(i)];
            for (unsigned channel = 0; channel < 4; ++channel)
            {
                pixels[i * 4 + channel] = uint8_t(color >> (channel * 8));
            }
        }
        return pixels;
    }
    bool compact()
    {
        if (indexed()) { return true; }
        if (m_raw.size() < 2048 || m_raw.size() % 4 != 0) { return false; }
        // Bounded stack table, no per-pixel allocation. Validate the palette before
        // allocating anything; true-colour assets retain their exact original bytes.
        std::array<uint32_t, 512> colors{};
        std::array<uint16_t, 1024> slots{};
        size_t count = 0;
        const auto colorAt = [&](size_t offset)
        {
            return uint32_t(m_raw[offset]) | (uint32_t(m_raw[offset + 1]) << 8)
                | (uint32_t(m_raw[offset + 2]) << 16) | (uint32_t(m_raw[offset + 3]) << 24);
        };
        const auto slotFor = [&](uint32_t color)
        {
            uint32_t hash = color;
            hash ^= hash >> 16;
            hash *= 0x7feb352du;
            hash ^= hash >> 15;
            size_t slot = hash & 1023;
            while (slots[slot] && colors[slots[slot] - 1] != color) { slot = (slot + 1) & 1023; }
            return slot;
        };
        for (size_t i = 0; i < m_raw.size(); i += 4)
        {
            const uint32_t color = colorAt(i);
            const size_t slot = slotFor(color);
            if (!slots[slot])
            {
                if (count == colors.size()) { return false; }
                colors[count++] = color;
                slots[slot] = uint16_t(count);
            }
        }
        // Edge bleed can give a paletted colour both opaque and transparent alpha.
        // Up to 512 exact colours use two-byte indices rather than falling back to BGRA.
        const size_t indexBytes = count > 256 ? 2 : 1;
        if (m_raw.size() / 4 * indexBytes + count * 4 >= m_raw.size()) { return false; }
        try
        {
            std::vector<uint8_t> indices(m_raw.size() / 4 * indexBytes);
            std::vector<uint32_t> palette(colors.begin(), colors.begin() + count);
            for (size_t i = 0; i < m_raw.size() / 4; ++i)
            {
                const uint16_t index = slots[slotFor(colorAt(i * 4))] - 1;
                indices[i * indexBytes] = uint8_t(index);
                if (indexBytes == 2) { indices[i * indexBytes + 1] = uint8_t(index >> 8); }
            }
            // Commit only after both allocations succeed; a failed compaction must
            // leave the original pixels usable, including transparent RGB bytes.
            m_wideIndices = indexBytes == 2;
            m_indices = std::move(indices);
            m_palette = std::move(palette);
            std::vector<uint8_t>().swap(m_raw);
            return true;
        }
        catch (const std::bad_alloc &)
        {
            return false;
        }
    }
private:
    size_t paletteIndex(size_t pixel) const
    {
        return m_wideIndices
            ? size_t(m_indices[pixel * 2]) | (size_t(m_indices[pixel * 2 + 1]) << 8) : m_indices[pixel];
    }
    bool m_wideIndices = false;
    std::vector<uint8_t> m_raw;
    std::vector<uint8_t> m_indices;
    std::vector<uint32_t> m_palette;
};
}
