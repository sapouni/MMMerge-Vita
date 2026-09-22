#pragma once
#include <SDL3/SDL.h>
#include <string>
#include <cstring>

namespace OpenYAMM::Engine::VitaTextInput
{
enum class Result { Pending, Accepted, Cancelled };

inline void prepare(const char *title, const std::string &value, unsigned limit, bool numeric = false)
{
#if defined(OPENYAMM_VITA_LOADER)
    const char *state = SDL_GetHint("openyamm_ime_state");
    if (state && std::strcmp(state, "idle") != 0) return;
    SDL_SetHint("openyamm_ime_title", title);
    SDL_SetHint("openyamm_ime_initial", value.c_str());
    SDL_SetHint("openyamm_ime_limit", std::to_string(limit).c_str());
    SDL_SetHint("openyamm_ime_numeric", numeric ? "1" : "0");
#endif
}

inline Result consume(std::string &value, size_t limit, bool numeric = false)
{
#if defined(OPENYAMM_VITA_LOADER)
    const char *state = SDL_GetHint("openyamm_ime_state");
    if (state && std::strcmp(state, "accepted") == 0)
    {
        const char *text = SDL_GetHint("openyamm_ime_result");
        std::string replacement = text ? text : "";
        if (numeric)
        {
            std::string digits;
            for (char c : replacement) if (c >= '0' && c <= '9' && digits.size() < limit) digits += c;
            // Bank transactions use signed 32-bit amounts in the shared game code.
            if (digits.size() == 10 && digits > "2147483647") digits = "2147483647";
            replacement = std::move(digits);
        }
        else if (replacement.size() > limit)
        {
            size_t end = limit;
            while (end && (static_cast<unsigned char>(replacement[end]) & 0xc0) == 0x80) --end;
            replacement.resize(end);
        }
        value = std::move(replacement);
        SDL_SetHint("openyamm_ime_state", "handled");
        return Result::Accepted;
    }
    if (state && std::strcmp(state, "cancelled") == 0)
    {
        SDL_SetHint("openyamm_ime_state", "handled");
        return Result::Cancelled;
    }
#endif
    return Result::Pending;
}
}
