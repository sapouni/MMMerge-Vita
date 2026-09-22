// Native keyboard for an externally presented SDL window. No SDL renderer or second swap.
#include <psp2/ime_dialog.h>
#include <psp2/touch.h>
#include <psp2/ctrl.h>
#include <vitaGL.h>
#include "vita-ime-text.h"

extern GLboolean vgl_has_cdlg_support;
static SDL_Mutex *oy_ime_mutex;
static int oy_ime_active;
static int oy_ime_release_guard;
static SceWChar16 oy_ime_text[SCE_IME_DIALOG_MAX_TEXT_LENGTH + 1];
static SceWChar16 oy_ime_initial[SCE_IME_DIALOG_MAX_TEXT_LENGTH + 1];
static SceWChar16 oy_ime_title[SCE_IME_DIALOG_MAX_TITLE_LENGTH + 1];
static char oy_ime_result_utf8[SCE_IME_DIALOG_MAX_TEXT_LENGTH * 3 + 1];

static const char *oy_ime_hint(const char *key, const char *fallback)
{
    const char *s = SDL_GetHint(key);
    return s ? s : fallback;
}

static void oy_ime_utf16(SceWChar16 *dest, size_t capacity, const char *text)
{
    oy_ime_to_utf16(dest, capacity, text);
}

static bool oy_SDL_StartTextInputWithProperties(SDL_Window *window, SDL_PropertiesID properties)
{
    (void)window; (void)properties;
    if (__atomic_load_n(&oy_ime_active, __ATOMIC_ACQUIRE)) return true;
    if (!oy_ime_mutex) oy_ime_mutex = SDL_CreateMutex();
    if (!oy_ime_mutex || !vgl_has_cdlg_support)
    {
        fprintf(stderr, "VITA IME unavailable dialog_memory=%u mutex=%d\n", vgl_has_cdlg_support, oy_ime_mutex != NULL);
        SDL_SetHint("openyamm_ime_state", "cancelled");
        return true; // Deliver an explicit cancellation; don't trap or retry every frame.
    }
    oy_ime_utf16(oy_ime_title, SCE_IME_DIALOG_MAX_TITLE_LENGTH + 1, oy_ime_hint("openyamm_ime_title", "Enter text"));
    oy_ime_utf16(oy_ime_initial, SCE_IME_DIALOG_MAX_TEXT_LENGTH + 1, oy_ime_hint("openyamm_ime_initial", ""));
    memset(oy_ime_text, 0, sizeof(oy_ime_text));
    unsigned limit = (unsigned)atoi(oy_ime_hint("openyamm_ime_limit", "128"));
    if (!limit || limit > SCE_IME_DIALOG_MAX_TEXT_LENGTH) limit = 128;
    oy_ime_initial[limit] = 0;
    if (limit && oy_ime_initial[limit-1] >= 0xd800 && oy_ime_initial[limit-1] <= 0xdbff)
        oy_ime_initial[limit-1] = 0;
    SceImeDialogParam param;
    sceImeDialogParamInit(&param);
    param.type = oy_ime_hint("openyamm_ime_numeric", "0")[0] == '1' ? SCE_IME_TYPE_NUMBER : SCE_IME_TYPE_DEFAULT;
    param.dialogMode = SCE_IME_DIALOG_DIALOG_MODE_WITH_CANCEL;
    param.textBoxMode = SCE_IME_DIALOG_TEXTBOX_MODE_WITH_CLEAR;
    param.title = oy_ime_title;
    param.initialText = oy_ime_initial;
    param.inputTextBuffer = oy_ime_text;
    param.maxTextLength = limit;
    SDL_LockMutex(oy_ime_mutex);
    const int result = sceImeDialogInit(&param);
    if (result >= 0) __atomic_store_n(&oy_ime_active, 1, __ATOMIC_RELEASE);
    SDL_UnlockMutex(oy_ime_mutex);
    oy_ime_release_guard = 1;
    SDL_SetHint("openyamm_ime_state", result >= 0 ? "running" : "cancelled");
    fprintf(stderr, "VITA IME open type=%u limit=%u result=0x%x\n", param.type, limit, result);
    return true;
}

static bool oy_SDL_StartTextInput(SDL_Window *window)
{
    return oy_SDL_StartTextInputWithProperties(window, 0);
}

static bool oy_SDL_StopTextInput(SDL_Window *window)
{
    (void)window;
    if (__atomic_load_n(&oy_ime_active, __ATOMIC_ACQUIRE))
    {
        SDL_LockMutex(oy_ime_mutex);
        __atomic_store_n(&oy_ime_active, 0, __ATOMIC_RELEASE);
        sceImeDialogAbort();
        sceImeDialogTerm();
        SDL_UnlockMutex(oy_ime_mutex);
        oy_ime_release_guard = 1;
    }
    SDL_SetHint("openyamm_ime_state", "idle");
    return true;
}

static void oy_ime_poll(void)
{
    if (!__atomic_load_n(&oy_ime_active, __ATOMIC_ACQUIRE)) return;
    if (sceImeDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED) return;
    SDL_LockMutex(oy_ime_mutex);
    SceImeDialogResult result = {0};
    const int rc = sceImeDialogGetResult(&result);
    const bool accepted = rc >= 0 && result.result >= 0 && result.button == SCE_IME_DIALOG_BUTTON_ENTER;
    size_t bytes = 0;
    if (accepted)
        bytes = oy_ime_to_utf8(oy_ime_result_utf8, sizeof(oy_ime_result_utf8), oy_ime_text,
            SCE_IME_DIALOG_MAX_TEXT_LENGTH);
    __atomic_store_n(&oy_ime_active, 0, __ATOMIC_RELEASE);
    sceImeDialogTerm();
    SDL_UnlockMutex(oy_ime_mutex);
    oy_ime_release_guard = 1;
    if (accepted)
    {
        SDL_SetHint("openyamm_ime_result", oy_ime_result_utf8);
    }
    SDL_SetHint("openyamm_ime_state", accepted ? "accepted" : "cancelled");
    fprintf(stderr, "VITA IME completed accepted=%d result=0x%x dialog=0x%x button=%d bytes=%u\n",
        accepted, rc, result.result, result.button, (unsigned)bytes);
}

static int oy_ime_blocks_input(void)
{
    if (__atomic_load_n(&oy_ime_active, __ATOMIC_ACQUIRE)) return 1;
    if (!oy_ime_release_guard) return 0;
    SceCtrlData pad = {0}; SceTouchData touch = {0};
    sceCtrlPeekBufferPositive(0, &pad, 1);
    sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1);
    if (!pad.buttons && !touch.reportNum) oy_ime_release_guard = 0;
    return 1; // Suppress the release frame as well.
}

static bool oy_SDL_PollEvent(SDL_Event *event)
{
    oy_ime_poll();
    if (!event) return SDL_PollEvent(NULL);
    const int blocked = oy_ime_blocks_input();
    while (SDL_PollEvent(event))
    {
        if (blocked && event && ((event->type >= SDL_EVENT_KEY_DOWN && event->type <= SDL_EVENT_TEXT_INPUT)
            || (event->type >= SDL_EVENT_MOUSE_MOTION && event->type <= SDL_EVENT_MOUSE_WHEEL)
            || (event->type >= SDL_EVENT_FINGER_DOWN && event->type <= SDL_EVENT_FINGER_CANCELED))) continue;
        return true;
    }
    return false;
}

static EGLBoolean oy_ime_eglSwapBuffers(EGLDisplay display, EGLSurface surface)
{
    if (!__atomic_load_n(&oy_ime_active, __ATOMIC_ACQUIRE)) return eglSwapBuffers(display, surface);
    SDL_LockMutex(oy_ime_mutex);
    vglSwapBuffers(__atomic_load_n(&oy_ime_active, __ATOMIC_ACQUIRE) ? GL_TRUE : GL_FALSE);
    SDL_UnlockMutex(oy_ime_mutex);
    return EGL_TRUE;
}
