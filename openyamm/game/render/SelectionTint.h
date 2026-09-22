#pragma once
#include <cstdint>
namespace OpenYAMM::Game
{
inline uint32_t selectionTintLighting(uint32_t color)
{
    const uint32_t red = (color & 255u) / 2u + 128u;
    const uint32_t green = ((color >> 8) & 255u) / 2u + 96u;
    const uint32_t blue = ((color >> 16) & 255u) / 2u;
    return (color & 0xff000000u) | red | (green << 8) | (blue << 16);
}
}
