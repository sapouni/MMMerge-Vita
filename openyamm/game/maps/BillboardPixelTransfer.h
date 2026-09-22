#pragma once

#include <cstdint>
#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

namespace OpenYAMM::Game
{
// Retain map metadata for runtime/saves, but transfer the large render-only buffers.
template <typename BillboardSet>
std::optional<BillboardSet> transferBillboardPixels(std::optional<BillboardSet> &source)
{
    if (!source)
    {
        return std::nullopt;
    }
    std::vector<std::vector<uint8_t>> pixels;
    pixels.reserve(source->textures.size());
    for (auto &texture : source->textures)
    {
        pixels.push_back(std::move(texture.pixels));
    }
    std::optional<BillboardSet> destination;
    try
    {
        destination = source;
    }
    catch (...)
    {
        for (size_t index = 0; index < pixels.size(); ++index)
        {
            source->textures[index].pixels = std::move(pixels[index]);
        }
        throw;
    }
    for (size_t index = 0; index < pixels.size(); ++index)
    {
        destination->textures[index].pixels = std::move(pixels[index]);
    }
    return destination;
}
}
