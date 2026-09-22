#pragma once
#include <algorithm>
#include <cmath>

namespace OpenYAMM::Engine::VitaAdaptiveResolution
{
// Owned exclusively by the game thread. Render timings arrive through bgfx's
// completed-frame statistics, never by reading render-thread mutable storage.
struct Controller
{
    bool low = false;
    double elapsed = 0, renderTotal = 0, callbackTotal = 0, cooldown = 0, qualifying = 0;
    unsigned samples = 0, transitions = 0;
    void clearWindow() { elapsed = renderTotal = callbackTotal = 0; samples = 0; }
    void sample(double delta, double callbackMs, double renderMs, bool gameplay)
    {
        if (!gameplay || !std::isfinite(delta) || !std::isfinite(callbackMs) || !std::isfinite(renderMs)
            || delta <= 0 || delta > .25 || callbackMs <= 0 || callbackMs > 250 || renderMs <= 0 || renderMs > 250)
        { clearWindow(); qualifying = 0; cooldown = std::max(cooldown, 1.0); return; }
        cooldown = std::max(0.0, cooldown - delta);
        elapsed += delta; renderTotal += renderMs; callbackTotal += callbackMs; ++samples;
        if (elapsed < 1.0 || samples < 12) return;
        const double render = renderTotal / samples, callback = callbackTotal / samples, window = elapsed;
        clearWindow();
        // Do not sacrifice resolution to disguise a slow AI/update thread.
        const bool change = low ? render / .5625 < 32.0 : render > 44.0 && render > callback * 1.08;
        qualifying = change && cooldown == 0 ? qualifying + window : 0;
        if (qualifying >= (low ? 2.0 : 1.0))
        { low = !low; ++transitions; qualifying = 0; cooldown = 3.0; }
    }
};
inline Controller controller;
inline bool enabled = false, gameplayThisFrame = false, lowThisFrame = false;
inline unsigned mode = 0; // 0 native, 1 fixed low, 2 adaptive native, 3 adaptive low.
}
