#include <SDL3/SDL.h>
#include <stddef.h>

// These constants are emitted by the Android compiler in an object section for cross-build comparison.
__attribute__((used, section(".sdl_abi"))) const unsigned int sdlAbi[] =
{
    sizeof(SDL_Event), sizeof(SDL_AudioSpec), sizeof(SDL_Surface), sizeof(SDL_AudioFormat),
    sizeof(SDL_DisplayMode), sizeof(SDL_GamepadAxis), sizeof(SDL_GamepadButton),
    offsetof(SDL_AudioSpec, channels), offsetof(SDL_Surface, pixels),
    offsetof(SDL_DisplayMode, refresh_rate), sizeof(SDL_FPoint), sizeof(SDL_Rect),
    sizeof(SDL_KeyboardEvent), sizeof(SDL_GamepadAxisEvent), sizeof(SDL_TouchFingerEvent)
};
