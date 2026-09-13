#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace PreSrMotionReset
{
// Screen widths/heights per second. Median rejects isolated object motion.
inline float PanSpeed(std::array<float, 9> x, std::array<float, 9> y)
{
    for (unsigned i = 0; i < 9; ++i)
        if (!std::isfinite(x[i]) || !std::isfinite(y[i])) return -1;
    std::sort(x.begin(), x.end());
    std::sort(y.begin(), y.end());
    return std::hypot(x[4], y[4]);
}

struct Gate
{
    bool armed = false, quiet = false, haveReset = false;
    uint64_t quietSince = 0, lastReset = 0, lastSample = 0;
    bool Update(float speed, uint64_t sampleMs, uint64_t nowMs)
    {
        if (!std::isfinite(speed) || speed < 0 || sampleMs > nowMs ||
            nowMs - sampleMs > 150 || sampleMs <= lastSample) return false;
        if (lastSample && sampleMs - lastSample > 150) { armed = false; quiet = false; }
        lastSample = sampleMs;
        if (speed <= 0.04f)
        {
            if (!quiet) { quiet = true; quietSince = sampleMs; }
            if (sampleMs - quietSince >= 250) armed = true;
            return false;
        }
        quiet = false;
        if (speed < 0.15f || !armed) return false;
        // Consume the onset even during cooldown: never trigger halfway through a pan.
        armed = false;
        if (haveReset && sampleMs - lastReset < 750) return false;
        haveReset = true; lastReset = sampleMs;
        return true;
    }
};
}
