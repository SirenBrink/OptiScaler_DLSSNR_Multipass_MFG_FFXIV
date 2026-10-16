#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace DynamicMfgPolicy {
// Zero selects the current display. Reject invalid persisted or injected values.
inline float Target(float fps) { return std::isfinite(fps) && fps > 0.0f ? fps : 0.0f; }
inline uint32_t LimitUs(float fps) {
    if (!std::isfinite(fps) || fps <= 0.0f) return 0;
    return static_cast<uint32_t>(std::clamp(std::round(1000000.0 / double(fps)),
                                          1.0, double(std::numeric_limits<uint32_t>::max())));
}
// Called only after the runtime accepted both option sets. Presentation-factor
// changes are telemetry, never a reason to reconfigure the base-frame limiter.
inline bool OwnsPacing(bool dynamic, bool dlssAccepted, bool reflexAccepted) {
    return dynamic && dlssAccepted && reflexAccepted;
}
}
