#pragma once
#include "VitaAsyncCapture.h"
#include <cstdint>
#include <cstring>

extern "C" int oy_capture_settings(unsigned, const char *, unsigned) __attribute__((weak));
namespace OpenYAMM::Engine::VitaCaptureSettings
{
struct Snapshot { unsigned id = 0, length = 0; char text[8192]{}; };
inline Snapshot current;
inline unsigned queued = 0;
inline void publish(const char *text)
{
    current.length = unsigned(std::strlen(text));
    if (current.length >= sizeof(current.text)) { current.length = sizeof(current.text) - 1; }
    std::memcpy(current.text, text, current.length); current.text[current.length] = 0;
    current.id = 2166136261u;
    for (unsigned i = 0; i < current.length; ++i) current.id = (current.id ^ uint8_t(current.text[i])) * 16777619u;
}
inline void write(const void *data, const char *)
{
    const auto &s = *static_cast<const Snapshot *>(data);
    if (oy_capture_settings) { oy_capture_settings(s.id, s.text, s.length); }
}
inline void queue()
{
    if (current.id && current.id != queued
        && VitaAsyncCapture::enqueue(current, "active-settings", write)) queued = current.id;
}
}
