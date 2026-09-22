#include <psp2/ctrl.h>
#include <psp2/kernel/threadmgr.h>
#include "vita-input-clock.h"
#include "vita-input-latch.h"
#include "vita-controls.h"
static OyVitaControls oy_controls;
static OyVitaChord oy_chord;
static void oy_load_input_rates(void);
static int oy_modifier_available(void)
{
    oy_load_input_rates();
    const char *v = SDL_GetHint("openyamm_vita_realtime");
    return oy_vita_modifier_enabled(&oy_controls, v && v[0] == '1');
}
static const unsigned oy_binding_buttons[] = {SCE_CTRL_UP,SCE_CTRL_DOWN,SCE_CTRL_LEFT,SCE_CTRL_RIGHT,
    SCE_CTRL_UP,SCE_CTRL_DOWN,SCE_CTRL_LEFT,SCE_CTRL_RIGHT,SCE_CTRL_CROSS,SCE_CTRL_CIRCLE,
    SCE_CTRL_SQUARE,SCE_CTRL_TRIANGLE,SCE_CTRL_RTRIGGER};
#define OY_CHORD_BUTTONS (SCE_CTRL_UP|SCE_CTRL_DOWN|SCE_CTRL_LEFT|SCE_CTRL_RIGHT|SCE_CTRL_CROSS|SCE_CTRL_CIRCLE|SCE_CTRL_SQUARE|SCE_CTRL_TRIANGLE|SCE_CTRL_RTRIGGER)
static int vita_gameplay_input(void);
static OyInputModeLatch oy_input_mode_latch;
static void oy_filter_remapped_buttons(SceCtrlData *pad)
{
    if (oy_ime_blocks_input()) { pad->buttons = 0; pad->lx = pad->ly = pad->rx = pad->ry = 128; return; }
    const int l = (pad->buttons & SCE_CTRL_LTRIGGER) != 0;
    oy_vita_chord_inspect(&oy_chord, l, vita_gameplay_input() && l &&
        ((oy_modifier_available() && (pad->buttons & OY_CHORD_BUTTONS)) || (pad->buttons & SCE_CTRL_SELECT)));
    const int pointerMode = !vita_gameplay_input() ? 1 : l ? 2 : 0;
    pad->buttons = oy_input_mode_buttons(&oy_input_mode_latch, pointerMode, pad->buttons,
        OY_CHORD_BUTTONS);
}


static int oy_select_chord_latched;
static float oy_camera_multiplier = 1.5f, oy_cursor_multiplier = 1.0f;
static void oy_load_input_rates(void)
{
    static int loaded;
    if (loaded) return;
    loaded = 1;
    oy_vita_controls_init(&oy_controls);
    FILE *file = fopen("ux0:data/openyamm/vita-performance.ini", "r");
    if (!file) return;
    char line[160];
    float value;
    char key[64], binding[32];
    while (fgets(line, sizeof(line), file))
    {
        if (sscanf(line, " %63[^= ] = %31s", key, binding) == 2)
            oy_vita_control_value(&oy_controls, key, binding);
        if (sscanf(line, " camera_multiplier = %f", &value) == 1 && isfinite(value) && value >= 0.1f && value <= 8)
            oy_camera_multiplier = value;
        if (sscanf(line, " cursor_multiplier = %f", &value) == 1 && isfinite(value) && value >= 0.1f && value <= 8)
            oy_cursor_multiplier = value;
    }
    fclose(file);
}

static int vita_gameplay_input(void)
{
    const char *value = SDL_GetHint("openyamm_vita_gameplay");
    return value && value[0] == '1';
}

const bool *oy_SDL_GetKeyboardState(int *pCount)
{
    oy_load_input_rates();
    static bool merged[SDL_SCANCODE_COUNT], previous[SDL_SCANCODE_COUNT];
    bool virtualKeys[SDL_SCANCODE_COUNT] = {0};
    int count = 0;
    const bool *physical = SDL_GetKeyboardState(&count);
    memset(merged, 0, sizeof(merged));
    if (physical && count > 0)
        memcpy(merged, physical, (count < SDL_SCANCODE_COUNT ? count : SDL_SCANCODE_COUNT) * sizeof(bool));
    if (oy_ime_blocks_input()) memset(merged, 0, sizeof(merged));
    SceCtrlData pad = {0};
    if (sceCtrlPeekBufferPositive(0, &pad, 1) > 0)
    {
        oy_filter_remapped_buttons(&pad);
        if ((pad.buttons & (SCE_CTRL_LTRIGGER | SCE_CTRL_SELECT)) == (SCE_CTRL_LTRIGGER | SCE_CTRL_SELECT))
            oy_select_chord_latched = 1;
        if (!(pad.buttons & SCE_CTRL_SELECT)) oy_select_chord_latched = 0;
        virtualKeys[SDL_SCANCODE_ESCAPE] = (pad.buttons & SCE_CTRL_START) != 0;
        // Spellbook consumes Q as quick-spell assignment; Circle remains right-click inspect.
        if (!vita_gameplay_input())
            virtualKeys[SDL_SCANCODE_Q] = (pad.buttons & SCE_CTRL_TRIANGLE) != 0;
        if (vita_gameplay_input() && !(pad.buttons & SCE_CTRL_LTRIGGER))
        {
            virtualKeys[SDL_SCANCODE_W] = pad.ly < 96;
            virtualKeys[SDL_SCANCODE_S] = pad.ly > 160;
            virtualKeys[SDL_SCANCODE_A] = pad.lx < 96;
            virtualKeys[SDL_SCANCODE_D] = pad.lx > 160;
            virtualKeys[SDL_SCANCODE_SPACE] = (pad.buttons & SCE_CTRL_CROSS) != 0;
            virtualKeys[SDL_SCANCODE_X] = (pad.buttons & SCE_CTRL_TRIANGLE) != 0;
            virtualKeys[SDL_SCANCODE_Q] = (pad.buttons & SCE_CTRL_CIRCLE) != 0;
            virtualKeys[SDL_SCANCODE_F] = (pad.buttons & SCE_CTRL_SQUARE) != 0;
            virtualKeys[SDL_SCANCODE_I] = (pad.buttons & SCE_CTRL_RTRIGGER) != 0;
            virtualKeys[SDL_SCANCODE_RETURN] = !oy_select_chord_latched && (pad.buttons & SCE_CTRL_SELECT) != 0;
        }
        if (vita_gameplay_input() && !oy_select_chord_latched)
        {
            const int alternate = (pad.buttons & SCE_CTRL_LTRIGGER) != 0;
            if (!alternate || oy_modifier_available())
                for (int i=alternate ? 4 : 0; i<(alternate ? OY_VITA_BINDING_COUNT : 4); ++i)
                    if (pad.buttons & oy_binding_buttons[i])
                    {
                        const SDL_Scancode key = oy_vita_action_keys[oy_controls.bindings[i]];
                        if (key != SDL_SCANCODE_UNKNOWN) virtualKeys[key] = true;
                    }
        }
    }
    for (int i = 1; i < SDL_SCANCODE_COUNT; ++i)
    {
        merged[i] = merged[i] || virtualKeys[i];
        if (virtualKeys[i] != previous[i])
        {
            SDL_Event event = {0};
            event.type = virtualKeys[i] ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
            event.key.scancode = (SDL_Scancode)i;
            event.key.down = virtualKeys[i];
            SDL_PushEvent(&event);
        }
        previous[i] = virtualKeys[i];
    }
    if (pCount) *pCount = SDL_SCANCODE_COUNT;
    return merged;
}

SDL_MouseButtonFlags oy_SDL_GetRelativeMouseState(float *pX, float *pY)
{
    static OyInputClock inputClock;
    oy_load_input_rates();
    float x = 0.0f, y = 0.0f;
    SDL_MouseButtonFlags buttons = SDL_GetRelativeMouseState(&x, &y);
    // A native mouse flush discards physical motion; it must not consume this frame's stick interval.
    if (!pX && !pY) return buttons;
    SceCtrlData pad = {0};
    int available = sceCtrlPeekBufferPositive(0, &pad, 1) > 0;
    if (available) oy_filter_remapped_buttons(&pad);
    const int imeBlocked = oy_ime_blocks_input();
    if (imeBlocked) { x = y = 0; buttons = 0; }
    int active = available && !imeBlocked && vita_gameplay_input() && !(pad.buttons & SCE_CTRL_LTRIGGER);
    float seconds = oy_input_seconds(&inputClock, sceKernelGetSystemTimeWide(), active);
    if (active)
    {
        float ax = ((int)pad.rx - 128) / 127.0f;
        float ay = ((int)pad.ry - 128) / 127.0f;
        if (fabsf(ax) > 0.2f) x += ax * seconds * 400.0f * oy_camera_multiplier;
        if (fabsf(ay) > 0.2f) y += ay * seconds * 400.0f * oy_camera_multiplier;
    }
    if (pX) *pX = x;
    if (pY) *pY = y;
    return buttons;
}

// L inspects the gameplay crosshair. Only menus use the movable pointer.
SDL_MouseButtonFlags oy_SDL_GetMouseState(float *pX, float *pY)
{
    oy_load_input_rates();
    static float pointerX = 480.0f, pointerY = 272.0f;
    static float lastMouseX, lastMouseY;
    static OyInputClock inputClock;
    float mouseX, mouseY;
    SDL_MouseButtonFlags buttons = SDL_GetMouseState(&mouseX, &mouseY);
    const int imeBlocked = oy_ime_blocks_input();
    if (!imeBlocked && (mouseX != lastMouseX || mouseY != lastMouseY))
    {
        pointerX = mouseX;
        pointerY = mouseY;
    }
    lastMouseX = mouseX;
    lastMouseY = mouseY;
    // A native mouse flush discards physical motion; it must not consume this frame's stick interval.
    if (!pX && !pY) return imeBlocked ? 0 : buttons;
    SceCtrlData pad = {0};
    int available = sceCtrlPeekBufferPositive(0, &pad, 1) > 0;
    if (available) oy_filter_remapped_buttons(&pad);
    if (imeBlocked) buttons = 0;
    int active = available && !imeBlocked && !vita_gameplay_input();
    float seconds = oy_input_seconds(&inputClock, sceKernelGetSystemTimeWide(), active);
    // A blocked menu did not sample the stick during the stall. Do not apply the newly sampled
    // direction retroactively across that gap; ordinary frame-rate-independent motion is unchanged.
    if (seconds > 0.25f) seconds = 0.0f;
    if (active)
    {
        float axisX = ((int)pad.lx - 128) / 127.0f;
        float axisY = ((int)pad.ly - 128) / 127.0f;
        if (fabsf(axisX) < 0.2f) axisX = 0.0f;
        if (fabsf(axisY) < 0.2f) axisY = 0.0f;
        if (pad.buttons & SCE_CTRL_LEFT) axisX = -1.0f;
        if (pad.buttons & SCE_CTRL_RIGHT) axisX = 1.0f;
        if (pad.buttons & SCE_CTRL_UP) axisY = -1.0f;
        if (pad.buttons & SCE_CTRL_DOWN) axisY = 1.0f;
        pointerX += axisX * seconds * 400.0f * oy_cursor_multiplier;
        pointerY += axisY * seconds * 400.0f * oy_cursor_multiplier;
        if (pad.buttons & SCE_CTRL_CROSS) buttons |= SDL_BUTTON_LMASK;
        if (pad.buttons & SCE_CTRL_CIRCLE) buttons |= SDL_BUTTON_RMASK;
    }
    if (available && !imeBlocked && vita_gameplay_input() && (pad.buttons & SCE_CTRL_LTRIGGER)
        && !(pad.buttons & SCE_CTRL_SELECT) && !oy_select_chord_latched && !oy_chord.used)
    {
        buttons |= SDL_BUTTON_RMASK;
    }
    pointerX = fminf(959.0f, fmaxf(0.0f, pointerX));
    pointerY = fminf(543.0f, fmaxf(0.0f, pointerY));
    if (pX) *pX = pointerX;
    if (pY) *pY = pointerY;
    return buttons;
}

#include "vita-affinity.h"
bool oy_SDL_SetHint(const char *name, const char *value)
{
    if (strcmp(name, "openyamm_vita_thread_role") == 0 && strcmp(value, "audio_decode") == 0)
    {
        oy_background_thread("audio_decode");
        return true;
    }
    if (strcmp(name, "openyamm_vita_thread_role") == 0 && strcmp(value, "audio_mix") == 0)
    {
        // The mixer shares core 1 with rendering, below its priority. It must
        // sleep after each chunk; it must not outrank asset/path work on core 2.
        const int render = __atomic_load_n(&oy_render_thread_priority, __ATOMIC_ACQUIRE);
        const int changed = sceKernelChangeThreadPriority(sceKernelGetThreadId(), render + 8);
        oy_pin_thread("audio_mix", SCE_KERNEL_CPU_MASK_USER_1);
        fprintf(stderr, "VITA AUDIO_MIX priority=%d result=%d yield_after_chunk_ms=1\n", render + 8, changed);
        return true;
    }
    if (strcmp(name, "openyamm_vita_thread_role") == 0
        && (strcmp(value, "video") == 0 || strcmp(value, "video-observe") == 0))
    {
        const SceUID thread = sceKernelGetThreadId();
        SceKernelThreadInfo info = {0};
        info.size = sizeof(info);
        const int infoResult = sceKernelGetThreadInfo(thread, &info);
        fprintf(stderr, "VITA VIDEO_WORKER before_affinity=0x%x priority=%d\n",
            sceKernelGetThreadCpuAffinityMask(thread), infoResult == 0 ? info.currentPriority : infoResult);
        if (strcmp(value, "video") == 0) { oy_background_thread("video"); }
        return true;
    }
    if (strcmp(name, "openyamm_vita_thread_role") == 0 && strcmp(value, "path") == 0)
    {
        oy_background_thread("path");
        return true;
    }
    if (strcmp(name, "openyamm_vita_thread_role") == 0 && strcmp(value, "capture") == 0)
    {
        oy_background_thread("capture");
        return true;
    }
    if (strcmp(name, "openyamm_vita_thread_role") == 0 && strcmp(value, "asset") == 0)
    {
        // These workers share allocation and stream locks with path/capture workers.
        // Do not let a decoder strictly outrank the worker holding such a lock.
        oy_background_thread("asset");
        return true;
    }
    if (strcmp(name, "openyamm_vita_thread_role") == 0 && strncmp(value, "actor", 5) == 0)
    {
        const int masks[] = {SCE_KERNEL_CPU_MASK_USER_0, SCE_KERNEL_CPU_MASK_USER_2, SCE_KERNEL_CPU_MASK_USER_1};
        unsigned index = (unsigned)(value[5] - '0');
        if (index < 3) { oy_pin_thread(value, masks[index]); return true; }
    }
    return SDL_SetHint(name, value);
}
