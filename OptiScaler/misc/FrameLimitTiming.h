#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace FrameLimitTiming {
inline uint64_t Interval(float fps, bool fgActive)
{
    // Validate before converting to an unsigned integer: negative, NaN and
    // infinite INI values previously produced undefined/out-of-range casts.
    if (!std::isfinite(fps) || fps <= 0) return 0;
    const double ns = std::clamp(1'000'000'000.0 / double(fps), 1.0, 100'000'000'000.0);
    return uint64_t(ns) * (fgActive ? 2 : 1);
}
}
