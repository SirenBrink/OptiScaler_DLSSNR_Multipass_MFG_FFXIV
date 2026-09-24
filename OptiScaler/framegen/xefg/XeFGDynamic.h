#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

// Chooses a factor from real-frame intervals, never generated FPS. Timing
// includes frame limiting: downsizing is deliberately conservative at a cap.
class XeFGDynamic
{
    double average = 0;
    uint64_t lastChange = 0, candidateSince = 0;
    int candidate = 0;
public:
    void Reset() { *this = {}; }
    int Select(double ms, double target, uint64_t now, int current, int maximum)
    {
        maximum = std::clamp(maximum, 2, 4);
        if (current < 2 || current > maximum) { Reset(); lastChange = now; return std::clamp(current, 2, maximum); }
        if (!std::isfinite(ms) || ms <= 0 || ms > 250 || !std::isfinite(target) || target <= 0)
        { average = 0; candidate = 0; lastChange = now; return current; }
        const double weight = 1.0 - std::exp(-ms / 500.0);
        average = average > 0 ? average + weight * (ms - average) : ms;
        if (lastChange == 0) { lastChange = now; return current; }
        int desired = current;
        const double realFps = 1000.0 / average;
        if (current < maximum && realFps * current < target * 0.93) desired = current + 1;
        else if (current > 2 && realFps * (current - 1) > target * 1.10) desired = current - 1;
        if (desired == current) { candidate = 0; return current; }
        if (candidate != desired) { candidate = desired; candidateSince = now; return current; }
        const uint64_t dwell = desired > current ? 1000 : 2000;
        if (now - lastChange < 3000 || now - candidateSince < dwell) return current;
        lastChange = now; candidate = 0; average = 0;
        return desired;
    }
};
