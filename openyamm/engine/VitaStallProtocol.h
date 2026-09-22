#pragma once
#include <stdint.h>
// Shared ARM32 protocol. All cross-thread accesses use atomic 32-bit operations.
enum OyStallPhase
{
    OyStallFrame, OyStallEvents, OyStallCallback, OyStallPostCallback, OyStallBgfx,
    OyStallSimulation, OyStallFacts, OyStallDecisions, OyStallApply, OyStallActorState,
    OyStallMovement, OyStallPhysics, OyStallPathBuild, OyStallPathResolve, OyStallMoveSweep,
    OyStallMovePost, OyStallMoveFacts, OyStallMelee, OyStallCorpse, OyStallProjectiles,
    OyStallPlayer, OyStallWorldRender, OyStallHud, OyStallAudio, OyStallVisibility,
    OyStallLighting, OyStallRecovery, OyStallGeometry, OyStallPathClear, OyStallRecord,
    OyStallSharedState, OyStallPartyPresentation, OyStallPartyAudio, OyStallSharedInput, OyStallMovementFrame, OyStallCombatDispatch, OyStallInteraction, OyStallGameplayTail, OyStallHudState, OyStallHudAssets, OyStallHudDraw,
    OyStallPhaseCount
};
struct OyPoolSnapshot
{
    uint32_t sequence, instance, frame, limit, retained, peak, hits, misses, returned, rejected, maxRequest;
};
struct OyStallProgress
{
    uint32_t enabled, heartbeatUs, frame, context, progress, sector, step, hz, detail;
    struct OyPoolSnapshot pools[3]; // audio, sprites, path; game publishes, watchdog writes.
    uint32_t pickSequence, pickTotals[24]; // elapsed_ms, frames, hover/context refreshes,
    uint32_t opaqueSurfaceDraws, alphaSurfaceDraws, blendedSurfaceDraws;
    uint32_t streamSequence, streamTotals[20]; // instance, frame, queued, ready bytes, hits, decodes, failures, consumed, expired, queue full, read us, ready head
    // context keyboard fallbacks, forced context refreshes, turning frames, hover/context us, direct query count.
    // Two stationary groups (none/targetable): ms, frames, hover count/us, context count/us, fallback count.
};
