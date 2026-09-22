#pragma once
#if defined(OPENYAMM_VITA_CLEAN) || defined(OPENYAMM_VITA_LOADER)
#define OY_VITA_TRACE(...) false
#else
#define OY_VITA_TRACE(...) (__VA_ARGS__)
#endif
