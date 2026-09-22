#pragma once
// Controller buttons must not become a different action halfway through a press.
typedef struct OyInputModeLatch { unsigned blocked; int initialized, pointerMode; } OyInputModeLatch;
static unsigned oy_input_mode_buttons(OyInputModeLatch *state, int pointerMode,
    unsigned held, unsigned remappedButtons)
{
    if (state->initialized && state->pointerMode != pointerMode)
        state->blocked |= held & remappedButtons;
    state->initialized = 1;
    state->pointerMode = pointerMode;
    state->blocked &= held; // A physical release rearms immediately; no arbitrary delay.
    return held & ~state->blocked;
}
