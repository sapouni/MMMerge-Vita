#pragma once

#include <SDL3/SDL.h>
#include <array>
#include <cstddef>
#include <new>
#include <atomic>
#include <cstring>
#include <thread>
#include <type_traits>

extern "C" void oy_capture_heartbeat() __attribute__((weak));
extern "C" int oy_capture_open() __attribute__((weak));

namespace OpenYAMM::Engine::VitaAsyncCapture
{
// Fixed storage, no waiting or file access on producers. Slot 3 is reserved for extreme stalls.
using Writer = void (*)(const void *, const char *);
struct Slot
{
    std::atomic<unsigned> state{0}; // free, producer, ready, consumer
    Writer writer = nullptr;
    char tag[128]{};
    alignas(std::max_align_t) std::array<unsigned char, 65536> payload{};
};
inline std::array<Slot, 4> slots;
inline std::atomic<unsigned> dropped{0};
inline std::atomic<unsigned> completed{0};
inline std::atomic<unsigned> lastWriteMilliseconds{0};
inline std::atomic<bool> started{false};
inline void start()
{
    bool expected = false;
    if (!started.compare_exchange_strong(expected, true)) { return; }
    try
    {
        std::thread([]
        {
#if defined(OPENYAMM_VITA_LOADER)
            SDL_SetHint("openyamm_vita_thread_role", "capture");
            if (oy_capture_open) { oy_capture_open(); }
#endif
            for (;;)
            {
                for (Slot &slot : slots)
                {
                    unsigned ready = 2;
                    if (!slot.state.compare_exchange_strong(ready, 3, std::memory_order_acquire)) { continue; }
                    const uint64_t begin = SDL_GetTicksNS();
                    slot.writer(slot.payload.data(), slot.tag);
                    lastWriteMilliseconds.store(unsigned((SDL_GetTicksNS() - begin) / 1000000), std::memory_order_relaxed);
                    completed.fetch_add(1, std::memory_order_relaxed);
                    slot.state.store(0, std::memory_order_release);
                }
                if (oy_capture_heartbeat) oy_capture_heartbeat();
                SDL_Delay(10);
            }
        }).detach();
    }
    catch (...) { started.store(false); }
}
template<class T, class Fill> bool enqueueBuild(const char *tag, Writer writer, Fill fill, bool extreme = false)
{
    static_assert(std::is_trivially_copyable_v<T> && sizeof(T) <= 65536);
    if (!started.load(std::memory_order_relaxed)) { ++dropped; return false; }
    for (unsigned index = extreme ? 3 : 0; index < (extreme ? 4 : 3); ++index)
    {
        Slot &slot = slots[index];
        unsigned free = 0;
        if (!slot.state.compare_exchange_strong(free, 1, std::memory_order_acquire)) { continue; }
        T *copy = ::new (slot.payload.data()) T;
        fill(*copy);
        std::strncpy(slot.tag, tag ? tag : "", sizeof(slot.tag) - 1);
        slot.tag[sizeof(slot.tag) - 1] = 0;
        slot.writer = writer;
        slot.state.store(2, std::memory_order_release);
        return true;
    }
    ++dropped;
    return false;
}
template<class T> bool enqueue(const T &value, const char *tag, Writer writer, bool extreme = false)
{
    return enqueueBuild<T>(tag, writer, [&](T &copy) { std::memcpy(&copy, &value, sizeof(T)); }, extreme);
}

}
