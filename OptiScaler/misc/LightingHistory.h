#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace LightingHistory
{
// Compare the same neutral scene values through the captured tone curve, not
// image brightness (which changes when the camera looks at a different object).
using Curve = std::array<float, 4>;
inline constexpr uint64_t MaxAgeMs = 350;
inline bool Valid(const Curve& curve)
{
    for (float v : curve) if (!std::isfinite(v) || v <= 1e-6f || v > 100.0f) return false;
    return true;
}
inline float Difference(const Curve& a, const Curve& b)
{
    float result = 0;
    for (size_t i = 0; i < a.size(); ++i)
        result = std::max(result, std::abs(std::log2(a[i] / b[i])));
    return result;
}
struct Gate
{
    Curve previous {};
    uint64_t time = 0, quietSince = 0, lastEvent = 0;
    bool havePrevious = false, armed = true;
    float lastDifference = 0;

    bool Observe(const Curve& curve, uint64_t sampleTime)
    {
        if (!Valid(curve)) { havePrevious = false; return false; }
        if (havePrevious && sampleTime <= time) return false;
        if (!havePrevious || sampleTime - time > MaxAgeMs)
        {
            previous = curve; time = sampleTime; havePrevious = true;
            armed = true; quietSince = 0; lastDifference = 0;
            return false; // Enabling, a loading gap or missing data is not a lighting cut.
        }
        const auto dt = sampleTime - time;
        lastDifference = Difference(curve, previous);
        previous = curve; time = sampleTime;
        if (lastDifference <= 0.04f)
        {
            if (!quietSince) quietSince = sampleTime;
            if (sampleTime - quietSince >= 300 && (!lastEvent || sampleTime - lastEvent >= 750)) armed = true;
        }
        else quietSince = 0;
        // Approximately 23% in the displayed tone response within 200 ms.
        // No cumulative drift threshold: ordinary eye adaptation must not keep flushing history.
        if (armed && dt <= 200 && lastDifference >= 0.30f &&
            (!lastEvent || sampleTime - lastEvent >= 750))
        {
            armed = false; lastEvent = sampleTime; return true;
        }
        return false;
    }
};

// Each temporal consumer acknowledges independently (NR and PreSR's private SR).
inline bool Consume(uint64_t event, uint64_t eventTime, uint64_t now, uint64_t& cursor)
{
    if (event == cursor) return false;
    cursor = event;
    return event != 0 && now >= eventTime && now - eventTime <= MaxAgeMs;
}
}
