#pragma once
#include <stdint.h>
typedef struct OyInputClock { uint64_t previous; int mode; } OyInputClock;
static float oy_input_seconds(OyInputClock *clock, uint64_t now, int mode)
{
    uint64_t elapsed = now >= clock->previous ? now - clock->previous : 0;
    float seconds = clock->previous && clock->mode == mode && elapsed <= 1000000
        ? elapsed / 1000000.0f : 0.0f;
    clock->previous = now; clock->mode = mode;
    return seconds;
}
