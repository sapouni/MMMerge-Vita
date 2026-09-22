#pragma once
#if defined(OPENYAMM_VITA_CLEAN)
#define OY_BGFX_STATS_COUNTER() int64_t(0)
#else
#define OY_BGFX_STATS_COUNTER() bx::getHPCounter()
#endif

// Coarse frame clocks are independently opt-in. Keep per-view/operation clocks off.
#include <stdint.h>
#if defined(OPENYAMM_VITA_CLEAN)
int64_t oyVitaCoarseFrameClock();
#define OY_BGFX_FRAME_COUNTER() oyVitaCoarseFrameClock()
#else
#define OY_BGFX_FRAME_COUNTER() OY_BGFX_STATS_COUNTER()
#endif
