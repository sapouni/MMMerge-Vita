#pragma once

#include <SDL3/SDL.h>
#include "VitaStall.h"
#include "VitaAdaptiveResolution.h"
#include "VitaAsyncCapture.h"
#include "VitaCaptureSettings.h"
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>

extern "C" void oy_capture_snapshot(uint64_t *, unsigned) __attribute__((weak));
extern "C" uint64_t oy_thread_run_ticks() __attribute__((weak));
extern "C" void oy_capture_cpu_probe(uint64_t *) __attribute__((weak));
namespace OpenYAMM::Engine::VitaIncident
{
// Coarse opt-in incident recording. These elapsed intervals are not independent CPU/GPU costs.
// Two early hitches cannot consume the four slots reserved for severe/sustained failures.
enum Child { Ai, Facts, Decisions, Apply, Projectiles, Recovery, Colliders, Movement, Physics, Path, PathBuild, PathResolve, PlayerMove, Reveal, Minimap, HudSubmit, Visibility, Lighting, MoveSweep, MovePost, MoveFacts, Melee, Corpse, GeometryDraw, BloodDraw, DecorationDraw, SpriteDraw, ActorDraw, ParticleDraw, SharedState, PartyPresentation, PartyAudio, SharedInput, MovementFrame, CombatDispatch, Interaction, GameplayTail, HudState, HudAssets, HudDraw, HudArt, HudLabels, HudDelivery, HudFollowers, HudAggro, HudBaseAssets, HudFxEvents, HudLayoutSetup, HudParty, HudElements, TextureMaintenance, InteractionHover, InteractionContext, StreamLockWait, StreamDelivery, MechanismUpdate, ApplySetup, ApplyActorLoop, ApplyDispatch, MoveFloorQuery, MoveSectorQuery, MoveCollisionQuery, PathQueueWait, FactCandidates, FactTargetResolve, FactIdentityStats, FactStatus, FactTargetCopy, FactMovementWorld, ChildCount };
struct Work
{
    unsigned outColliderBuilds = 0, outColliderVisits = 0, outColliderUpdates = 0;
    unsigned outColliderMembershipRebuilds = 0, outColliderAttackInvalidations = 0;
    unsigned uiAsyncRequests = 0, uiAsyncInputBlocked = 0, uiSyncLoads = 0;
    uint64_t uiSyncNs = 0;
    std::array<uint64_t, ChildCount> ns{};
    unsigned simulationHz = 0;
    unsigned steps = 0, actors = 0, activeSum = 0, backgroundSum = 0, activatedSectors = 0;
    unsigned projectiles = 0, impacts = 0, portalBuilds = 0, portalNodes = 0;
    unsigned textures = 0, requests = 0, uploads = 0;
    uint64_t resident = 0, evictions = 0, deferred = 0;
    unsigned recoveryCalls = 0, recoveryCandidates = 0, recoveryCatalogue = 0, recoveryFallbacks = 0;
    unsigned movementCalls = 0, physicsCalls = 0, hudTextures = 0, hudFonts = 0;
    int footSector = -1, eyeSector = -1;
    float partyX = 0, partyY = 0, partyZ = 0;
    unsigned supportFace = UINT32_MAX, minimapRebuilds = 0, minimapLines = 0, revealUpdates = 0;
    unsigned invalidSectorMasks = 0, pathPending = 0, pathRunning = 0;
    unsigned staticHits = 0, staticMisses = 0, bloodDraws = 0, bloodCulled = 0, bloodUploads = 0, bloodVertices = 0, actorPrepared = 0, actorDrawn = 0, geometryDraws = 0, geometryLights = 0, fxLights = 0, snapshotItems = 0;
    unsigned fineSample = 0, directChecks = 0, directCacheHits = 0, longChecksDeferred = 0;
    unsigned waypointChecks = 0;
    unsigned waypointCertificateHits = 0, pathPartialRejected = 0, pathShortcutsDeferred = 0;
    unsigned waypointCacheHits = 0;
    unsigned waypointLongDeferred = 0;
    unsigned physicsWorkCalls = 0;
    unsigned floorQueries = 0;
    unsigned sectorQueries = 0;
    unsigned collisionQueries = 0;
    unsigned pathQueueAcquires = 0;
    uint64_t sectorCatalogue = 0;
    uint64_t sectorCandidates = 0;
    uint64_t applyCpuTicks = 0;
    unsigned applyCpuProbes = 0;
    unsigned applyCpuProbeFailures = 0;
    uint64_t floorFaceCatalogue = 0, floorFaceCandidates = 0;
    unsigned combatTargetCatalogue = 0, combatTargetEligible = 0;
    unsigned selectedSameSector = 0, selectedOtherSector = 0, selectedVertical256 = 0;
    unsigned combatQueryCalls = 0, combatQueryHits = 0;
    unsigned floorProjectionChecks = 0, floorProjectionHits = 0, pointSectorChecks = 0, pointSectorHits = 0;
    unsigned supportMemoChecks = 0, supportMemoHits = 0, terrainFastDraws = 0;

    uint64_t ceilingFaceCatalogue = 0, ceilingFaceCandidates = 0, sweepFaceCatalogue = 0, sweepFaceCandidates = 0;
    unsigned pathPrefixUsed = 0, pathPrefixBlocked = 0;
    uint64_t pathCancelled = 0, pathOldestPendingMs = 0;
    uint64_t pathPartial = 0, pathSuccess = 0, pathBudget = 0, pathRefinement = 0, pathMaxQueueMs = 0, pathMaxSearchMs = 0;
    uint64_t pathCompleted = 0, pathNodes = 0, pathMaxDeliveryMs = 0, pathRefreshed = 0;

    uint64_t terrainPrepareNs = 0, terrainUploadNs = 0, terrainCacheBytes = 0, terrainCachePeak = 0, terrainCacheHits = 0, terrainCacheMisses = 0, terrainCacheEvictions = 0, terrainSlots = 0, terrainVisibleSlots = 0, terrainUpdatedSlots = 0, terrainUploadCalls = 0, terrainUploadBytes = 0;

    unsigned targetPolicyBuilds = 0, overrideEvaluations = 0, factResets = 0, outTerrainDraws = 0, outBuildingDraws = 0, outHighlightDraws = 0, outActorDraws = 0, outDecorationDraws = 0;
};
struct Row
{
    uint32_t frame = 0, dead = 0, scene = 0, house = 0;
    std::array<uint64_t, 7> game{};
    // events/window, callback, post-callback, bgfx::frame, remaining loop work.
    std::array<uint64_t, 5> outer{};
    uint64_t renderNs = 0, submitWaitNs = 0, renderWaitNs = 0;
    uint32_t bgfxFrame = 0, draws = 0;
    Work work;
    unsigned worldResolutionMode = 0, resolutionTransitions = 0;
};
struct State
{
    std::array<Row, 35> ring{};
    Row row;
    char map[128]{};
    unsigned count = 0, next = 0, frame = 0, early = 0, severe = 0, extreme = 0, tail = 0, slowStreak = 0, cooldown = 0;
    unsigned scene = 0, house = 0, settingsId = 0, gameplayFrames = 0;
    std::array<uint64_t,4> cpuProbe{};
    bool enabled = false, active = false, changed = false, detailNext = false;
    uint64_t last = 0, nextHouseSummary = 0, nextIndoorSummary = 0;
};
inline State &state() { static State s; return s; }
inline unsigned houseVideoSessions = 0; // Owned by the game thread, not the decoder worker.
inline void flush(const char *reason);
inline void configure(bool enabled, unsigned scene, const char *map, unsigned house)
{
    State &s = state();
    if (!map) { map = ""; }
    if (s.settingsId != VitaCaptureSettings::current.id)
    {
        if (s.count) { flush("settings-changed"); }
        s.settingsId = VitaCaptureSettings::current.id;
        s.early = s.severe = s.extreme = s.tail = 0;
    }
    if (s.scene != scene || std::strncmp(s.map, map, sizeof(s.map) - 1) != 0 || s.house != house)
    {
        if (s.count) { flush("context-ended"); }
        s.scene = scene; s.house = house;
        std::snprintf(s.map, sizeof(s.map), "%s", map);
        s.count = s.next = s.early = s.severe = s.extreme = s.tail = s.slowStreak = 0;
        s.frame = s.gameplayFrames = 0; s.changed = true; s.nextHouseSummary = 0; s.nextIndoorSummary = 0;
    }
    if (enabled) {
        VitaAsyncCapture::start(); VitaCaptureSettings::queue(); }
    if (s.enabled && !enabled && s.count) { flush("capture-disabled"); }
    s.enabled = enabled;
}
inline bool active() { return state().active; }
inline bool detailedFrame() { return false; } // 00.70: coarse timing only.
inline uint64_t tick() { return active() ? SDL_GetTicksNS() : 0; }
inline void begin()
{
    State &s = state();
    VitaStall::begin(s.enabled);
    s.active = s.enabled;
    if (s.cooldown) { --s.cooldown; s.active = false; }
    if (s.active) { s.row = {}; s.last = SDL_GetTicksNS(); }
}
inline void mark(unsigned phase)
{
    State &s = state();
    const OyStallPhase next[] = {OyStallCallback, OyStallPostCallback, OyStallBgfx, OyStallFrame, OyStallFrame};
    if (phase < 5) { VitaStall::set(next[phase]); }
    if (!s.active || phase >= s.row.outer.size()) { return; }
    const uint64_t now = SDL_GetTicksNS(); s.row.outer[phase] += now - s.last; s.last = now;
}
inline void game(const std::array<uint64_t, 7> &times, const Work &work, unsigned dead)
{
    State &s = state();
    if (s.active) { s.row.game = times;
        // UI producers run outside the gameplay counter object. Preserve their totals.
        const unsigned requests = s.row.work.uiAsyncRequests;
        const unsigned blocked = s.row.work.uiAsyncInputBlocked;
        const unsigned loads = s.row.work.uiSyncLoads;
        const uint64_t loadNs = s.row.work.uiSyncNs;
        s.row.work = work;
        s.row.work.uiAsyncRequests += requests;
        s.row.work.uiAsyncInputBlocked += blocked;
        s.row.work.uiSyncLoads += loads;
        s.row.work.uiSyncNs += loadNs;
        s.row.dead = dead; }
}
inline void renderer(uint32_t frame, uint32_t draws, uint64_t render, uint64_t waitSubmit, uint64_t waitRender)
{
    State &s = state();
    s.row.worldResolutionMode = VitaAdaptiveResolution::mode;
    s.row.resolutionTransitions = VitaAdaptiveResolution::controller.transitions; if (!s.active) { return; }
    s.row.bgfxFrame = frame; s.row.draws = draws;
    s.row.renderNs = render; s.row.submitWaitNs = waitSubmit; s.row.renderWaitNs = waitRender;
}
#if defined(OPENYAMM_VITA_LOADER)
extern "C" int oy_capture_open() __attribute__((weak));
extern "C" int oy_capture_write(const void *, unsigned) __attribute__((weak));
extern "C" void oy_capture_health(uint64_t *, unsigned) __attribute__((weak));
#endif
inline void writeSnapshot(const void *snapshot, const char *reason)
{
#if defined(OPENYAMM_VITA_LOADER)
    if (!oy_capture_write) { return; }
    const State &s = *static_cast<const State *>(snapshot);
    static std::array<uint64_t, 64 + 64 * 128> wire;
    wire.fill(0);
    wire[0] = 0x3037304f59414d4dull; wire[1] = 15; wire[2] = sizeof(wire);
    wire[3] = 224; wire[4] = s.count; wire[5] = s.scene; wire[6] = s.house;
    wire[7] = VitaAsyncCapture::completed.load(std::memory_order_relaxed);
    wire[8] = VitaAsyncCapture::dropped.load(std::memory_order_relaxed);
    wire[9] = VitaAsyncCapture::lastWriteMilliseconds.load(std::memory_order_relaxed);
    wire[10] = SDL_GetTicksNS();
    wire[11] = s.settingsId;
    for (unsigned i=0;i<4;++i) wire[12+i]=s.cpuProbe[i];
    std::memcpy(&wire[16], s.map, sizeof(s.map));
    std::strncpy(reinterpret_cast<char *>(&wire[32]), reason, 63);
    if (oy_capture_health) { oy_capture_health(&wire[40], 23); }
    if (oy_capture_snapshot) oy_capture_snapshot(&wire[64], 192);
    const unsigned first = (s.next + s.ring.size() - s.count) % s.ring.size();
    for (unsigned i = 0; i < s.count; ++i)
    {
        const Row &r = s.ring[(first + i) % s.ring.size()];
        uint64_t *v = wire.data() + 256 + i * 224;
        *v++ = r.frame; *v++ = r.bgfxFrame; *v++ = r.draws; *v++ = r.dead;
        for (auto n : r.outer) { *v++ = n; }
        for (auto n : r.game) { *v++ = n; }
        for (auto n : r.work.ns) { *v++ = n; }
        *v++ = r.renderNs; *v++ = r.submitWaitNs; *v++ = r.renderWaitNs;
        *v++ = uint64_t(r.work.simulationHz);
        *v++ = uint64_t(r.work.steps);
        *v++ = uint64_t(r.work.actors);
        *v++ = uint64_t(r.work.activeSum);
        *v++ = uint64_t(r.work.backgroundSum);
        *v++ = uint64_t(r.work.activatedSectors);
        *v++ = uint64_t(r.work.projectiles);
        *v++ = uint64_t(r.work.impacts);
        *v++ = uint64_t(r.work.portalBuilds);
        *v++ = uint64_t(r.work.portalNodes);
        *v++ = uint64_t(r.work.textures);
        *v++ = uint64_t(r.work.requests);
        *v++ = uint64_t(r.work.uploads);
        *v++ = uint64_t(r.work.resident);
        *v++ = uint64_t(r.work.evictions);
        *v++ = uint64_t(r.work.deferred);
        *v++ = uint64_t(r.work.recoveryCalls);
        *v++ = uint64_t(r.work.recoveryCandidates);
        *v++ = uint64_t(r.work.recoveryCatalogue);
        *v++ = uint64_t(r.work.recoveryFallbacks);
        *v++ = uint64_t(r.work.movementCalls);
        *v++ = uint64_t(r.work.physicsCalls);
        *v++ = uint64_t(r.work.hudTextures);
        *v++ = uint64_t(r.work.hudFonts);
        *v++ = uint64_t(r.work.footSector);
        *v++ = uint64_t(r.work.eyeSector);
        { uint32_t bits; std::memcpy(&bits, &r.work.partyX, 4); *v++ = bits; }
        { uint32_t bits; std::memcpy(&bits, &r.work.partyY, 4); *v++ = bits; }
        { uint32_t bits; std::memcpy(&bits, &r.work.partyZ, 4); *v++ = bits; }
        *v++ = uint64_t(r.work.supportFace);
        *v++ = uint64_t(r.work.minimapRebuilds);
        *v++ = uint64_t(r.work.minimapLines);
        *v++ = uint64_t(r.work.revealUpdates);
        *v++ = uint64_t(r.work.invalidSectorMasks);
        *v++ = uint64_t(r.work.pathPending);
        *v++ = uint64_t(r.work.pathRunning);
        *v++ = uint64_t(r.work.staticHits);
        *v++ = uint64_t(r.work.staticMisses);
        *v++ = uint64_t(r.work.bloodDraws);
        *v++ = uint64_t(r.work.bloodCulled);
        *v++ = uint64_t(r.work.bloodUploads);
        *v++ = uint64_t(r.work.bloodVertices);
        *v++ = uint64_t(r.work.actorPrepared);
        *v++ = uint64_t(r.work.actorDrawn);
        *v++ = uint64_t(r.work.geometryDraws);
        *v++ = uint64_t(r.work.geometryLights);
        *v++ = uint64_t(r.work.fxLights);
        *v++ = uint64_t(r.work.snapshotItems);
        *v++ = uint64_t(r.work.fineSample);
        *v++ = uint64_t(r.work.directChecks);
        *v++ = uint64_t(r.work.directCacheHits);
        *v++ = uint64_t(r.work.longChecksDeferred);
        *v++ = uint64_t(r.work.targetPolicyBuilds);
        *v++ = uint64_t(r.work.overrideEvaluations);
        *v++ = uint64_t(r.work.factResets);
        *v++ = uint64_t(r.work.outTerrainDraws);
        *v++ = uint64_t(r.work.outBuildingDraws);
        *v++ = uint64_t(r.work.outHighlightDraws);
        *v++ = uint64_t(r.work.outActorDraws);
        *v++ = uint64_t(r.work.outDecorationDraws);
        *v++ = r.work.uiAsyncRequests;
        *v++ = r.work.uiAsyncInputBlocked;
        *v++ = r.work.uiSyncLoads;
        *v++ = r.work.uiSyncNs;
        *v++ = r.work.outColliderBuilds;
        *v++ = r.work.outColliderVisits;
        *v++ = r.work.outColliderUpdates;
        *v++ = r.work.outColliderMembershipRebuilds;
        *v++ = r.work.outColliderAttackInvalidations;
        *v++ = r.work.waypointChecks;
        *v++ = r.work.waypointCacheHits;
        *v++ = r.work.waypointLongDeferred;
        *v++ = r.work.physicsWorkCalls;
        *v++ = r.work.floorQueries;
        *v++ = r.work.sectorQueries;
        *v++ = r.work.collisionQueries;
        *v++ = r.work.pathQueueAcquires;
        *v++ = r.work.sectorCatalogue;
        *v++ = r.work.sectorCandidates;
        *v++ = r.work.applyCpuTicks;
        *v++ = r.work.applyCpuProbes;
        *v++ = r.work.applyCpuProbeFailures;
        *v++ = r.work.floorFaceCatalogue;
        *v++ = r.work.floorFaceCandidates;
        *v++ = r.work.combatTargetCatalogue;
        *v++ = r.work.combatTargetEligible;
        *v++ = r.work.selectedSameSector;
        *v++ = r.work.selectedOtherSector;
        *v++ = r.work.selectedVertical256;
        *v++ = r.work.floorProjectionChecks;
        *v++ = r.work.floorProjectionHits;
        *v++ = r.work.pointSectorChecks;
        *v++ = r.work.pointSectorHits;
        *v++ = r.work.combatQueryCalls;
        *v++ = r.work.combatQueryHits;
        *v++ = r.work.ceilingFaceCatalogue;
        *v++ = r.work.ceilingFaceCandidates;
        *v++ = r.work.sweepFaceCatalogue;
        *v++ = r.work.sweepFaceCandidates;
        *v++ = uint64_t(r.work.pathPrefixUsed) | (uint64_t(r.work.pathPrefixBlocked) << 32);
        *v++ = (r.work.pathCompleted & UINT32_MAX) | ((r.work.pathCancelled & UINT32_MAX) << 32);
        *v++ = r.work.pathNodes;
        *v++ = (r.work.pathMaxDeliveryMs & UINT32_MAX) | ((r.work.pathOldestPendingMs & UINT32_MAX) << 32);
        *v++ = r.work.pathRefreshed;
        *v++ = r.work.terrainPrepareNs;
        *v++ = r.work.terrainUploadNs;
        *v++ = r.work.terrainCacheBytes;
        *v++ = r.work.terrainCachePeak;
        *v++ = r.work.terrainCacheHits;
        *v++ = r.work.terrainCacheMisses;
        *v++ = r.work.terrainCacheEvictions;
        *v++ = r.work.terrainSlots;
        *v++ = r.work.terrainVisibleSlots;
        *v++ = r.work.terrainUpdatedSlots;
        *v++ = r.work.terrainUploadCalls;
        *v++ = r.work.terrainUploadBytes;
        *v++ = uint64_t(r.work.supportMemoChecks) | (uint64_t(r.work.supportMemoHits) << 32);
        *v++ = r.work.terrainFastDraws;
        *v++ = r.worldResolutionMode;
        *v++ = r.resolutionTransitions;
        *v++ = r.work.pathPartial; *v++ = r.work.pathSuccess; *v++ = r.work.pathBudget;
        *v++ = r.work.pathRefinement; *v++ = r.work.pathMaxQueueMs; *v++ = r.work.pathMaxSearchMs;
        *v++ = r.work.waypointCertificateHits; *v++ = r.work.pathPartialRejected; *v++ = r.work.pathShortcutsDeferred;


    }
    // Word 63 remains zero during checksum computation; truncated/corrupt records reject on PC.
    uint32_t checksum = 2166136261u;
    const auto *bytes = reinterpret_cast<const unsigned char *>(wire.data());
    for (size_t i = 0; i < sizeof(wire); ++i) { checksum = (checksum ^ bytes[i]) * 16777619u; }
    wire[63] = checksum;
    oy_capture_write(wire.data(), unsigned(sizeof(wire)));
#else
    (void)snapshot; (void)reason;
#endif
}
inline void flush(const char *reason)
{
    State &s = state();
    VitaAsyncCapture::enqueue(s, reason, writeSnapshot, std::strcmp(reason, "extreme-reserved-slot") == 0);
    s.count = 0;
    // Logging is asynchronous; no unrecorded cooldown frames are needed.
}
inline void end()
{
    State &s = state(); if (!s.active) { return; }
    mark(4); s.active = false;
    // Drop the callback which crossed a map/house boundary; it mixes two different workloads.
    if (s.changed) { s.changed = false; return; }
    s.row.frame = ++s.frame; s.row.scene = s.scene; s.row.house = s.house;
    const bool gameplay = !s.house && s.row.work.actors != 0 && s.row.work.steps != 0;
    if (gameplay) { ++s.gameplayFrames; }
    s.ring[s.next] = s.row; s.next = (s.next + 1) % s.ring.size();
    if (s.count < s.ring.size()) { ++s.count; }
    uint64_t total = 0; for (uint64_t n : s.row.outer) { total += n; }
    s.detailNext = false;
    s.slowStreak = total >= 125000000 ? s.slowStreak + 1 : 0;
    const bool severe = total >= 500000000 || s.slowStreak >= 4;
    if (total >= 3000000000ull && s.extreme < 4)
    {
        ++s.extreme; flush("extreme-reserved-slot"); s.tail = 16;
    }
    else if (severe && s.severe < 4)
    {
        ++s.severe; flush("severe-or-sustained-with-history"); s.tail = 16; s.slowStreak = 0;
    }
    else if (s.tail)
    {
        if (--s.tail == 0 || total >= 1000000000) { flush("after-incident"); s.tail = 0; }
    }
    else if (s.frame >= 16 && total >= (s.house ? 45000000ull : 125000000ull) && s.early < 2)
    {
        ++s.early; flush("early-hitch-with-history"); s.tail = 16;
    }
    // Menus/loading cannot spend or postpone the first gameplay window. Later windows
    // have an independent budget, for both the battle and cave even after early hitches.
    if (gameplay && s.gameplayFrames >= 41 && s.last >= s.nextIndoorSummary)
    {
        s.nextIndoorSummary = s.last + 10000000000ull;
        if (s.count) { flush(s.scene ? "indoor-gameplay-periodic" : "outdoor-gameplay-periodic"); }
    }
    // Coarse periodic house snapshots can see a frame-boundary stall even with little game work.
    else if (s.house && s.last >= s.nextHouseSummary)
    {
        s.nextHouseSummary = s.last + 15000000000ull;
        if (s.frame >= 64) { flush("house-window"); }
    }
}
}
