#pragma once

// One load per process. Disabled before gameplay; no per-frame collection or log writes.
#if defined(OPENYAMM_VITA_PROFILE) && defined(OPENYAMM_VITA_LOADER)
#define OY_LOAD(...) __VA_ARGS__
#include "VitaFrameProfile.h"
#include <array>
#include <atomic>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>

namespace OpenYAMM::Engine::VitaLoad
{
enum Bucket
{
    AssetRead, AssetLookup, ImageDecode, EdgeBleed, TexturePrepareSubmit, TextureDrain,
    GlTextureUpload, MapAssetBuild, RuntimeBuild, RenderBuild, ScriptSetup, DerivedRead,
    DerivedWrite, YamlReplay, YamlParse, Count
};
struct Counter { uint64_t calls = 0, us = 0, selfUs = 0, bytes = 0; };
struct Stage { char scope[40]{}, name[104]{}; uint64_t us = 0, atUs = 0; };
inline std::atomic<bool> active{false};
inline std::mutex lock;
inline std::array<std::array<Counter, Count>, 2> counters;
inline std::array<Stage, 192> stages;
inline unsigned stageCount = 0, droppedStages = 0, inFlight = 0;
inline uint64_t started = 0, derivedHits = 0, derivedMisses = 0, derivedBc3Hits = 0;
inline uint64_t yamlHits = 0, yamlMisses = 0;
inline uint64_t mapSpriteDecodeCalls = 0, mapSpriteDecodeBytes = 0;
inline bool used = false;
inline thread_local bool owner = false;
class Scope;
inline thread_local Scope *top = nullptr;

class Scope
{
public:
    explicit Scope(Bucket bucket) : m_bucket(bucket)
    {
        if (!active.load(std::memory_order_acquire)) { return; }
        {
            std::lock_guard guard(lock);
            if (!active.load(std::memory_order_relaxed)) { return; }
            ++inFlight;
        }
        m_enabled = true; m_lane = owner ? 0 : 1;
        m_parent = top; top = this; m_start = VitaProfile::now();
    }
    ~Scope()
    {
        if (!m_enabled) { return; }
        const uint64_t elapsed = VitaProfile::now() - m_start;
        top = m_parent;
        if (m_parent) { m_parent->m_children += elapsed; }
        std::lock_guard guard(lock);
        --inFlight;
        if (!active.load(std::memory_order_relaxed)) { return; }
        Counter &counter = counters[m_lane][m_bucket];
        ++counter.calls; counter.us += elapsed;
        counter.selfUs += elapsed >= m_children ? elapsed - m_children : 0;
        counter.bytes += m_bytes;
    }
    void bytes(uint64_t count) { m_bytes += count; }
    Scope(const Scope &) = delete;
    Scope &operator=(const Scope &) = delete;
private:
    Bucket m_bucket;
    unsigned m_lane = 0;
    bool m_enabled = false;
    uint64_t m_start = 0, m_children = 0, m_bytes = 0;
    Scope *m_parent = nullptr;
};

inline void derived(bool hit, bool bc3)
{
    if (!active.load(std::memory_order_acquire)) { return; }
    std::lock_guard guard(lock);
    if (!active.load(std::memory_order_relaxed)) { return; }
    derivedHits += hit; derivedMisses += !hit; derivedBc3Hits += hit && bc3;
}
inline void mapSpriteDecode(uint64_t bytes)
{
    if (!active.load(std::memory_order_acquire)) { return; }
    std::lock_guard guard(lock);
    if (!active.load(std::memory_order_relaxed)) { return; }
    ++mapSpriteDecodeCalls; mapSpriteDecodeBytes += bytes;
}

class Stages
{
public:
    explicit Stages(const std::string &, const std::string &scope = "map_assets")
    {
        if (!active.load(std::memory_order_acquire)) { return; }
        m_last = VitaProfile::now(); std::strncpy(m_scope, scope.c_str(), sizeof(m_scope) - 1);
    }
    void beginStage(const std::string &) const {} // Completion boundaries already partition each logger's interval.
    void stage(const std::string &name)
    {
        if (!m_last || !active.load(std::memory_order_acquire)) { return; }
        const uint64_t end = VitaProfile::now();
        std::lock_guard guard(lock);
        if (!active.load(std::memory_order_relaxed)) { return; }
        if (stageCount < stages.size())
        {
            Stage &record = stages[stageCount++];
            std::strncpy(record.scope, m_scope, sizeof(record.scope) - 1);
            std::strncpy(record.name, name.c_str(), sizeof(record.name) - 1);
            record.us = end - m_last; record.atUs = end - started;
        }
        else { ++droppedStages; }
        m_last = end;
    }
private:
    uint64_t m_last = 0;
    char m_scope[40]{};
};

class Session
{
public:
    explicit Session(const char *path = nullptr)
    {
        if (used) { return; }
        used = true; m_enabled = true; owner = true;
        if (path) { std::strncpy(m_path, path, sizeof(m_path) - 1); }
        else { std::snprintf(m_path, sizeof(m_path), "/data/load-profile-00.52-%llu.log",
            (unsigned long long)std::time(nullptr)); }
        started = VitaProfile::now(); active.store(true, std::memory_order_release);
    }
    void success() { m_success = true; }
    ~Session()
    {
        if (!m_enabled) { return; }
        const uint64_t elapsed = VitaProfile::now() - started;
        unsigned unfinished;
        {
            std::lock_guard guard(lock);
            active.store(false, std::memory_order_release); unfinished = inFlight;
        }
        owner = false;
        // Counters/stages are now immutable. Late worker completions are excluded and reported as in-flight.
        FILE *file = std::fopen(m_path, "wb");
        FILE *out = file ? file : stderr;
        std::fprintf(out, "LOAD PROFILE version=00.52 trigger=new_game_character_OK success=%u wall_ms=%.3f "
            "in_flight_scopes=%u dropped_stages=%u completed_work_only=1 overhead_verified=0\n",
            unsigned(m_success), elapsed / 1000.0, unfinished, droppedStages);
        std::fprintf(out, "LOAD POLICY asset_read_includes_archive_inflate=1 async_overlaps_main=1 "
            "scope_inclusive_rows_overlap=1 stage_loggers_overlap=1 output_after_loading=1\n");
        const char *names[] = {"asset_stream_read", "asset_lookup_open", "image_decode", "edge_bleed",
            "texture_prepare_submit", "bgfx_upload_drain", "gl_texture_create_update", "map_asset_build",
            "world_runtime_build", "render_resource_build", "script_setup", "derived_read_validate", "derived_write", "yaml_event_replay", "yaml_parse"};
        uint64_t mainCovered = 0;
        for (unsigned lane = 0; lane < counters.size(); ++lane)
        {
            for (unsigned bucket = 0; bucket < Count; ++bucket)
            {
                const Counter &c = counters[lane][bucket];
                if (lane == 0) { mainCovered += c.selfUs; }
                std::fprintf(out, "LOAD PART thread=%s part=%s calls=%llu inclusive_ms=%.3f self_ms=%.3f bytes=%llu\n",
                    lane ? "async" : "game", names[bucket], (unsigned long long)c.calls,
                    c.us / 1000.0, c.selfUs / 1000.0, (unsigned long long)c.bytes);
            }
        }
        std::fprintf(out, "LOAD REMAINDER game_covered_ms=%.3f game_unclassified_ms=%.3f\n",
            mainCovered / 1000.0, (elapsed > mainCovered ? elapsed - mainCovered : 0) / 1000.0);
        std::fprintf(out, "LOAD CACHE hits=%llu misses=%llu bc3_hits=%llu direct_map_sprite_decodes=%llu "
            "direct_map_sprite_source_bytes=%llu\n", (unsigned long long)derivedHits,
            (unsigned long long)derivedMisses, (unsigned long long)derivedBc3Hits,
            (unsigned long long)mapSpriteDecodeCalls, (unsigned long long)mapSpriteDecodeBytes);
        std::fprintf(out, "LOAD YAML hits=%llu misses=%llu\n", (unsigned long long)yamlHits, (unsigned long long)yamlMisses);
        for (unsigned i = 0; i < stageCount; ++i)
        {
            const Stage &s = stages[i];
            std::fprintf(out, "LOAD STAGE scope=\"%s\" name=\"%s\" delta_ms=%.3f at_ms=%.3f\n",
                s.scope, s.name, s.us / 1000.0, s.atUs / 1000.0);
        }
        std::fprintf(out, "LOAD COMPLETE success=%u\n", unsigned(m_success));
        if (file) { std::fclose(file); }
        std::fprintf(stderr, "LOAD PROFILE 00.52 saved=%u wall_ms=%.3f path=%s\n",
            unsigned(file != nullptr), elapsed / 1000.0, m_path);
    }
    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;
private:
    bool m_enabled = false, m_success = false;
    char m_path[192]{};
};
}
#else
#define OY_LOAD(...) ((void)0)
#endif
