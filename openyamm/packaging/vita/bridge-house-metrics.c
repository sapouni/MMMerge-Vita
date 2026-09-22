// House-only, opt-in counters. The general GL profiler remains compiled out.
static unsigned oy_house_active;
static unsigned oy_house_values[8];
#include "vita-thread-state.h"

void oy_house_metrics(unsigned active, uint32_t *output)
{
    if (active == 1)
    {
        for (unsigned i = 0; i < 8; ++i) { __atomic_store_n(&oy_house_values[i], 0, __ATOMIC_RELAXED); }
        __atomic_store_n(&oy_house_active, 1, __ATOMIC_RELAXED);
    }
    else if (active == 0) { __atomic_store_n(&oy_house_active, 0, __ATOMIC_RELAXED); }
    if (output)
    {
        for (unsigned i = 0; i < 8; ++i) { output[i] = __atomic_load_n(&oy_house_values[i], __ATOMIC_RELAXED); }
    }
}

void oy_house_texture_copy(unsigned bytes)
{
    if (__atomic_load_n(&oy_house_active, __ATOMIC_RELAXED))
    {
        __atomic_fetch_add(&oy_house_values[3], bytes, __ATOMIC_RELAXED);
    }
}

void oy_house_recovery(unsigned phase, unsigned failed)
{
    if (!__atomic_load_n(&oy_house_active, __ATOMIC_RELAXED)) { return; }
    struct OyNativeThreadState *state = oy_native_thread_state();
    if (!state) { return; }
    if (!phase)
    {
        state->houseRecoveryBegin = __atomic_load_n(&oy_house_active, __ATOMIC_RELAXED)
            ? sceKernelGetProcessTimeWide() : 0;
    }
    else if (state->houseRecoveryBegin)
    {
        __atomic_fetch_add(&oy_house_values[4], 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&oy_house_values[5], (unsigned)(sceKernelGetProcessTimeWide() - state->houseRecoveryBegin), __ATOMIC_RELAXED);
        __atomic_fetch_add(&oy_house_values[6], failed != 0, __ATOMIC_RELAXED);
        state->houseRecoveryBegin = 0;
    }
}

static void oy_house_glTexSubImage2D(GLenum target, GLint level, GLint x, GLint y,
    GLsizei width, GLsizei height, GLenum format, GLenum type, const GLvoid *pixels)
{
    const int active = __atomic_load_n(&oy_house_active, __ATOMIC_RELAXED);
    const uint64_t begin = active ? sceKernelGetProcessTimeWide() : 0;
    glTexSubImage2D(target, level, x, y, width, height, format, type, pixels);
    if (active)
    {
        const unsigned elapsed = (unsigned)(sceKernelGetProcessTimeWide() - begin);
        __atomic_fetch_add(&oy_house_values[0], 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&oy_house_values[1], elapsed, __ATOMIC_RELAXED);
        unsigned maximum = __atomic_load_n(&oy_house_values[2], __ATOMIC_RELAXED);
        while (elapsed > maximum && !__atomic_compare_exchange_n(&oy_house_values[2], &maximum, elapsed,
            0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
    }
}
