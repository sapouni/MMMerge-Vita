#pragma once

#include <SDL3/SDL.h>
#include "engine/VitaAsyncCapture.h"
#include "engine/VitaCaptureSettings.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>

#if defined(OPENYAMM_VITA_LOADER)
extern "C" int oy_capture_write(const void *, unsigned) __attribute__((weak));
extern "C" void oy_house_metrics(unsigned active, uint32_t *output) __attribute__((weak));
#endif

namespace OpenYAMM::Game
{
// Opt-in, separate from the frame profiler and sound-effect mixer. No writes during capture.
struct HouseVideoDiagnostics
{
    enum Section { Read, Demux, VideoDecode, AudioDecode, DecodeWait, LoopSeek, Upload, Queue, Update, Join, Count };
    struct Counter
    {
        std::atomic<uint32_t> calls{0}, microseconds{0}, maximum{0};
    };
    struct Row
    {
        uint32_t elapsedMs = 0, loops = 0, presented = 0, emptyVideo = 0, emptyAudio = 0;
        uint32_t minimumVideo = UINT32_MAX, minimumAudioFrames = UINT32_MAX, minimumQueuedBytes = UINT32_MAX;
        std::array<uint32_t, Count * 3> sections{};
        std::array<uint32_t, 8> graphics{};
    };
    std::array<Counter, Count> counters;
    std::atomic<uint32_t> loops{0};
    uint32_t presented = 0, emptyVideo = 0, emptyAudio = 0;
    uint32_t minimumVideo = UINT32_MAX, minimumAudioFrames = UINT32_MAX, minimumQueuedBytes = UINT32_MAX;
    uint64_t startTick = SDL_GetTicksNS(), nextTick = startTick;
    unsigned settingsId = Engine::VitaCaptureSettings::current.id;
    size_t rowCount = 0;
    std::array<Row, 181> rows{};

    void snapshot(bool force = false)
    {
        const uint64_t now = SDL_GetTicksNS();
        if ((!force && rowCount == rows.size()) || (!force && now < nextTick)) { return; }
        Row &row = rows[rowCount == rows.size() ? rowCount - 1 : rowCount++];
        row.elapsedMs = static_cast<uint32_t>((now - startTick) / 1000000);
        row.loops = loops.load(std::memory_order_relaxed);
        row.presented = presented; row.emptyVideo = emptyVideo; row.emptyAudio = emptyAudio;
        row.minimumVideo = minimumVideo; row.minimumAudioFrames = minimumAudioFrames;
        row.minimumQueuedBytes = minimumQueuedBytes;
        for (size_t i = 0; i < Count; ++i)
        {
            row.sections[i * 3] = counters[i].calls.load(std::memory_order_relaxed);
            row.sections[i * 3 + 1] = counters[i].microseconds.load(std::memory_order_relaxed);
            row.sections[i * 3 + 2] = counters[i].maximum.load(std::memory_order_relaxed);
        }
#if defined(OPENYAMM_VITA_LOADER)
        if (oy_house_metrics) { oy_house_metrics(2, row.graphics.data()); }
#endif
        nextTick = now + 1000000000;
    }

    struct Snapshot
    {
        std::array<Row, 181> rows{};
        size_t rowCount = 0;
        uint64_t startTick = 0;
        unsigned settingsId = 0;
        bool pausedPresentation = false;
    };
    void write(const char *clip, bool pausedPresentation)
    {
        snapshot(true);
        Engine::VitaAsyncCapture::start();
        // This bounded copy happens only on house exit, never inside the video update loop.
        Engine::VitaAsyncCapture::enqueueBuild<Snapshot>(clip, writeSnapshot, [&](Snapshot &copy)
        {
            copy.rows = rows; copy.rowCount = rowCount;
            copy.startTick = startTick; copy.pausedPresentation = pausedPresentation;
            copy.settingsId = settingsId;
        });
    }
    static void writeSnapshot(const void *data, const char *clip)
    {
#if defined(OPENYAMM_VITA_LOADER)
        if (!oy_capture_write) return;
        const Snapshot &copy = *static_cast<const Snapshot *>(data);
        static std::array<uint64_t, 64 + 64 * 120> wire;
        wire.fill(0);
        wire[0] = 0x3037304859414d4dull; wire[1] = 2; wire[2] = sizeof(wire);
        wire[3] = 46; wire[4] = copy.rowCount; wire[5] = copy.pausedPresentation;
        wire[6] = copy.startTick;
        wire[11] = copy.settingsId;
        std::strncpy(reinterpret_cast<char *>(&wire[16]), clip, 127);
        auto *v = reinterpret_cast<unsigned char *>(wire.data() + 64);
        auto put = [&](uint32_t n) { std::memcpy(v, &n, sizeof(n)); v += sizeof(n); };
        for (size_t i = 0; i < copy.rowCount; ++i) {
            const Row &row = copy.rows[i];
            put(row.elapsedMs); put(row.loops); put(row.presented);
            put(row.emptyVideo); put(row.emptyAudio); put(row.minimumVideo);
            put(row.minimumAudioFrames); put(row.minimumQueuedBytes);
            for (auto n : row.sections) put(n);
            for (auto n : row.graphics) put(n);
        }
        uint32_t checksum = 2166136261u;
        const auto *bytes = reinterpret_cast<const unsigned char *>(wire.data());
        for (size_t i=0; i<sizeof(wire); ++i) checksum = (checksum ^ bytes[i]) * 16777619u;
        wire[63] = checksum;
        oy_capture_write(wire.data(), unsigned(sizeof(wire)));
#else
        (void)data; (void)clip;
#endif
    }
};

class HouseVideoScope
{
public:
    HouseVideoScope(HouseVideoDiagnostics *diagnostics, HouseVideoDiagnostics::Section section)
        : m_counter(diagnostics ? &diagnostics->counters[section] : nullptr),
          m_start(m_counter ? SDL_GetTicksNS() : 0) {}
    ~HouseVideoScope()
    {
        if (!m_counter) { return; }
        const uint32_t elapsed = static_cast<uint32_t>((SDL_GetTicksNS() - m_start) / 1000);
        m_counter->calls.fetch_add(1, std::memory_order_relaxed);
        m_counter->microseconds.fetch_add(elapsed, std::memory_order_relaxed);
        uint32_t maximum = m_counter->maximum.load(std::memory_order_relaxed);
        while (maximum < elapsed && !m_counter->maximum.compare_exchange_weak(
            maximum, elapsed, std::memory_order_relaxed)) {}
    }
private:
    HouseVideoDiagnostics::Counter *m_counter;
    uint64_t m_start;
};
}
