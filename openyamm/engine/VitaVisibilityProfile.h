#pragma once

// Game-thread aggregate counters only. No per-object clocks, locks or dynamic allocation.
#if defined(OPENYAMM_VITA_PROFILE) && defined(OPENYAMM_VITA_LOADER)
#define OY_VIS(...) __VA_ARGS__
#include "engine/VitaFrameProfile.h"
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>

namespace OpenYAMM::Engine::VitaVisibility
{
enum Category { Actor, Decoration, Building, CategoryCount };
struct Entry { uint64_t seen = 0; uint32_t texture = 0; bool retained = false; };
inline std::array<Entry, 4096> actors;
inline std::array<Entry, 8192> decorations;
inline std::array<Entry, 16384> buildings;
struct Counts
{
    uint32_t visible[CategoryCount]{}, entered[CategoryCount]{}, changedTexture[CategoryCount]{};
    uint32_t spriteMisses = 0, spritePending = 0, actorFallbacks = 0;
    // Origins: 0 other, 1 candidate absent last frame, 2 candidate present last frame.
    uint32_t queued[3]{}, uploads[3]{};
    uint32_t setup[3]{}, lighting[3]{};
    uint64_t uploadBytes = 0;
    uint32_t pickChanged = 0, pickPressed = 0, pickRefresh = 0;
    uint32_t buildingRebuilds = 0, pickIndexRebuilds = 0, billboardDraws = 0, untracked = 0;
    float yaw = 0, pitch = 0, yawDelta = 0, pitchDelta = 0, translationSquared = 0;
    bool world = false, baseline = false, turning = false;
    uint32_t fastTurn = 0, pickSuppressed = 0, pickFaces = 0, pickFacesRejected = 0;
    uint32_t pickOcclusionQueries = 0, pickOcclusionFaces = 0, dialogueChecks = 0;
    uint32_t proximitySteps = 0, within256 = 0, within512 = 0, within2048 = 0;
    uint32_t nearestActor = ~0u, targetCandidates = 0, targetPoliciesBuilt = 0;
    uint32_t partyVeryNearTicks = 0;
    uint32_t factVectorGrowth = 0;
    float nearestDistanceSquared = 1.0e30f;
    uint64_t factSampleUs[6]{};
    uint32_t factSamples[6]{};
};
inline Counts counts;
inline uint64_t sequence = 0;
inline std::array<char, 128> mapKey;
inline bool cameraValid = false;
inline float lastYaw = 0, lastPitch = 0, lastX = 0, lastY = 0, lastZ = 0;
inline uint32_t consecutiveTurns = 0, origin = 0;
inline std::array<Counts, 20> records;
inline bool pendingFlush = false;

template <size_t N>
inline void clearEntries(std::array<Entry, N> &entries)
{
    // Reset in place: assigning {} to an entire array creates a temporary as
    // large as the Vita game thread's 256 KiB stack, even in rarely taken paths.
    for (Entry &value : entries) { value = {}; }
}

inline Entry *entry(Category category, size_t id)
{
    switch (category)
    {
    case Actor: return id < actors.size() ? &actors[id] : nullptr;
    case Decoration: return id < decorations.size() ? &decorations[id] : nullptr;
    case Building: return id < buildings.size() ? &buildings[id] : nullptr;
    default: return nullptr;
    }
}
inline void begin()
{
    counts = {};
    origin = 0;
    if (++sequence == 0)
    {
        sequence = 1; clearEntries(actors); clearEntries(decorations); clearEntries(buildings); cameraValid = false;
    }
}
inline void camera(const char *key, float yaw, float pitch, float x, float y, float z)
{
    if (counts.world) { return; } // Once per game callback, independent of render passes.
    if (std::strncmp(mapKey.data(), key, mapKey.size()) != 0)
    {
        std::strncpy(mapKey.data(), key, mapKey.size() - 1);
        cameraValid = false; clearEntries(actors); clearEntries(decorations); clearEntries(buildings);
    }
    counts.world = true;
    counts.baseline = !cameraValid;
    counts.yaw = yaw; counts.pitch = pitch;
    if (cameraValid)
    {
        float delta = yaw - lastYaw;
        constexpr float pi = 3.14159265358979323846f;
        if (std::isfinite(delta))
        {
            delta = std::fmod(delta, 2 * pi);
            if (delta > pi) { delta -= 2 * pi; }
            else if (delta < -pi) { delta += 2 * pi; }
            counts.yawDelta = delta; counts.pitchDelta = pitch - lastPitch;
            counts.turning = std::abs(delta) > 0.002f || std::abs(counts.pitchDelta) > 0.002f;
        }
        const float dx = x - lastX, dy = y - lastY, dz = z - lastZ;
        counts.translationSquared = dx * dx + dy * dy + dz * dz;
    }
    lastYaw = yaw; lastPitch = pitch; lastX = x; lastY = y; lastZ = z; cameraValid = true;
    consecutiveTurns = counts.turning ? consecutiveTurns + 1 : 0;
    // Request expires as soon as turning stops. Capture rearming occurs at the next frame boundary.
    VitaProfile::frame.rotationPending = consecutiveTurns >= 3;
}
inline void visible(Category category, size_t id, uint32_t texture)
{
    Entry *pEntry = entry(category, id);
    if (!pEntry) { ++counts.untracked; return; }
    if (pEntry->seen == sequence) { return; }
    const bool previous = pEntry->seen != 0 && pEntry->seen + 1 == sequence;
    ++counts.visible[category];
    counts.entered[category] += !previous;
    counts.changedTexture[category] += previous && pEntry->texture != texture;
    *pEntry = {sequence, texture, previous};
}
class Origin
{
public:
    Origin(Category category, size_t id) : m_previous(origin)
    {
        const Entry *pEntry = entry(category, id);
        const bool retained = pEntry && (pEntry->seen == sequence ? pEntry->retained
            : (pEntry->seen != 0 && pEntry->seen + 1 == sequence));
        origin = pEntry ? (retained ? 2 : 1) : 0;
        ++counts.setup[origin];
    }
    ~Origin() { origin = m_previous; }
private:
    uint32_t m_previous;
};
inline void queued() { ++counts.queued[origin]; }
inline void upload(uint64_t bytes) { ++counts.uploads[origin]; counts.uploadBytes += bytes; }
inline void pick(bool changed, bool pressed, bool refresh)
{
    counts.pickChanged += changed; counts.pickPressed += pressed; counts.pickRefresh += refresh;
}
inline void buildingRebuild() { clearEntries(buildings); ++counts.buildingRebuilds; }
inline void record()
{
    VitaProfile::frame.nearActorPending = counts.world && counts.nearestDistanceSquared <= 256 * 256;
    VitaProfile::frame.farActorPending = counts.world && counts.proximitySteps && counts.nearestActor != ~0u && counts.nearestDistanceSquared >= 512 * 512;
    if (!counts.world) { cameraValid = false; consecutiveTurns = 0; VitaProfile::frame.rotationPending = false; }
    if (VitaProfile::frame.active && VitaProfile::frame.index < records.size())
    {
        records[VitaProfile::frame.index] = counts;
        pendingFlush = VitaProfile::frame.index + 1 == records.size();
    }
}
inline void flush()
{
    if (!pendingFlush) { return; }
    pendingFlush = false;
    VitaProfile::print("PROFILE VISIBILITY_POLICY bounded=1 per_object_timers=0 buffered_until_window_end=1 "
        "visible_stage=post_cull_pre_submit building_scope=outdoor_resolved_groups "
        "new_candidate_origin=absent_previous_frame_not_proof_of_submission\n");
    for (size_t i = 0; i < records.size(); ++i)
    {
        const Counts &c = records[i];
        const char *factNames[] = {"candidate_gather", "target_resolve", "identity_stats", "runtime_status", "target_copy", "movement"};
        for (unsigned part = 0; part < 6; ++part)
            VitaProfile::print("PROFILE FACT_SAMPLE n=%u part=%s samples=%u raw_us=%llu stride=64 extrapolated=0\n",
                unsigned(i + 1), factNames[part], c.factSamples[part], (unsigned long long)c.factSampleUs[part]);
        VitaProfile::print("PROFILE PROXIMITY n=%u nearest_actor=%u nearest_distance_squared=%.2f "
            "steps=%u within256_ticks=%u within512_ticks=%u within2048_ticks=%u dialogue_checks=%u "
            "fast_turn=%u pick_suppressed=%u pick_faces=%u pick_faces_rejected=%u occlusion_queries=%u "
            "occlusion_faces=%u target_candidates=%u target_policies_built=%u party_very_near_ticks=%u fact_vector_growth=%u\n", unsigned(i + 1),
            c.nearestActor, c.nearestDistanceSquared, c.proximitySteps, c.within256, c.within512, c.within2048,
            c.dialogueChecks, c.fastTurn, c.pickSuppressed, c.pickFaces, c.pickFacesRejected,
            c.pickOcclusionQueries, c.pickOcclusionFaces, c.targetCandidates, c.targetPoliciesBuilt, c.partyVeryNearTicks, c.factVectorGrowth);
        VitaProfile::print("PROFILE ROTATION n=%u world=%u baseline=%u turning=%u yaw=%.4f pitch=%.4f "
            "yaw_delta=%.4f pitch_delta=%.4f translation_squared=%.2f untracked=%u\n",
            unsigned(i + 1), c.world, c.baseline, c.turning, c.yaw, c.pitch, c.yawDelta, c.pitchDelta,
            c.translationSquared, c.untracked);
        const char *names[] = {"actor", "decoration", "building_group"};
        for (unsigned category = 0; category < CategoryCount; ++category)
        {
            VitaProfile::print("PROFILE VISIBLE n=%u category=%s visible=%u newly_visible=%u "
                "retained_texture_changed=%u\n", unsigned(i + 1), names[category], c.visible[category],
                c.entered[category], c.changedTexture[category]);
        }
        VitaProfile::print("PROFILE ROTATION_WORK n=%u sprite_misses=%u pending=%u fallbacks=%u "
            "queued_other=%u queued_new_candidate=%u queued_retained_candidate=%u "
            "uploads_other=%u uploads_new_candidate=%u uploads_retained_candidate=%u upload_bytes=%llu "
            "pick_changed=%u pick_pressed=%u pick_refresh=%u building_rebuilds=%u pick_index_rebuilds=%u "
            "billboard_batch_draws=%u setup_other=%u setup_new=%u setup_retained=%u "
            "lighting_other=%u lighting_new=%u lighting_retained=%u\n",
            unsigned(i + 1), c.spriteMisses, c.spritePending, c.actorFallbacks,
            c.queued[0], c.queued[1], c.queued[2], c.uploads[0], c.uploads[1], c.uploads[2],
            (unsigned long long)c.uploadBytes, c.pickChanged, c.pickPressed, c.pickRefresh,
            c.buildingRebuilds, c.pickIndexRebuilds, c.billboardDraws,
            c.setup[0], c.setup[1], c.setup[2], c.lighting[0], c.lighting[1], c.lighting[2]);
    }
}
}
#else
#define OY_VIS(...) ((void)0)
namespace OpenYAMM::Engine::VitaVisibility
{
inline void begin() {}
inline void record() {}
inline void flush() {}
}
#endif
