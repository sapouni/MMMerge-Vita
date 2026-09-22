#pragma once
#include <cstdint>

namespace OpenYAMM::Game::VitaAnimationBudget
{
// 00.42's combat capture left at least 14.4 MiB of CDRAM free at the old 28 MiB limit.
// Spend 6 MiB of that headroom; retain room for UI, resource retirement and transitions.
// These limits include managed live textures and their delayed retirement accounting.
inline constexpr uint32_t Limit = 34u * 1024 * 1024;
inline constexpr uint32_t EvictionTarget = 30u * 1024 * 1024;
}
