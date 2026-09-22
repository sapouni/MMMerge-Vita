#pragma once
#include <cstdint>

namespace OpenYAMM::Game
{
struct VitaPickGate
{
    uint64_t lastQueryNs = 0;
    uint32_t idleFrames = 0;
    uint64_t lastFastTurnNs = 0;
    bool settling = false;
    uint64_t lastViewNs = 0;
    uint64_t lastFrameNs = 0;
    uint64_t averageFrameNs = 100000000;
    float forwardX = 0, forwardY = 0, forwardZ = 0;

    bool observeTurn(uint64_t nowNs, float x, float y, float z)
    {
        const float dx = x - forwardX, dy = y - forwardY, dz = z - forwardZ;
        const float seconds = nowNs > lastViewNs ? float(nowNs - lastViewNs) * 1.0e-9f : 0;
        // Unit-forward chord length approximates angular speed, without an acos per frame.
        const bool fast = lastViewNs && seconds > 0 && dx * dx + dy * dy + dz * dz > seconds * seconds;
        lastViewNs = nowNs; forwardX = x; forwardY = y; forwardZ = z;
        return fast;
    }

    bool update(uint64_t nowNs, bool changed, bool interaction, bool cached, bool fastTurn = false)
    {
        if (lastFrameNs && nowNs > lastFrameNs)
        {
            const uint64_t dt = nowNs-lastFrameNs;
            // Ignore loading/suspend gaps; budget about one refresh per three frames.
            if (dt < 1000000000) averageFrameNs = (7*averageFrameNs+dt)/8;
        }
        lastFrameNs = nowNs;
        ++idleFrames;
        const bool clockReset = nowNs < lastQueryNs;
        const uint64_t elapsed = clockReset ? 0 : nowNs - lastQueryNs;
        if (fastTurn) { lastFastTurnNs = nowNs; settling = true; }
        const bool settled = settling && !fastTurn && nowNs >= lastFastTurnNs
            && nowNs - lastFastTurnNs >= 100000000;
        const bool suppress = (fastTurn || (settling && !settled)) && cached && !interaction && !clockReset;
        const bool due = !suppress && (!cached || interaction || clockReset || settled
            || (changed && idleFrames >= 3 && elapsed >= changedIntervalNs())
            || (!changed && idleFrames >= 4 && elapsed >= 1000000000));
        if (due)
        {
            lastQueryNs = nowNs;
            idleFrames = 0;
            if (!fastTurn) { settling = false; }
        }
        return due;
    }

    uint64_t changedIntervalNs() const
    {
        const uint64_t interval=3*averageFrameNs;
        return interval<250000000 ? 250000000 : (interval>1000000000 ? 1000000000 : interval);
    }
};
}
