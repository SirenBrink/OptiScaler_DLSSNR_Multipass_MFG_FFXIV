#pragma once
#include <array>
#include <algorithm>
#include <cstdint>

namespace DlssNr
{
// Existing same-queue After completion timestamps, consumed only when their
// slots are complete. Measures real-frame GPU completion spacing, NOT scanout
// FPS or latency. Includes queue starvation and work between the two markers.
class PreSrCadence
{
    struct Entry { double ms = 0; unsigned kind = 0; };
    std::array<Entry, 128> window {};
    unsigned next = 0, count = 0;
    uint64_t last = 0;
public:
    enum { Anchor = 1, Skipped = 2, Ordinary = 3 };
    struct Stats { double anchor = 0, skipped = 0, median = 0, p95 = 0, maximum = 0; unsigned anchors = 0, skips = 0, samples = 0; };
    void Record(uint64_t tick, uint64_t frequency, unsigned kind)
    {
        if (!frequency || !tick || kind < Anchor || kind > Ordinary) return;
        if (last && tick <= last) return;
        const auto previous = last; last = tick;
        if (!previous) return;
        window[next] = { double(tick - previous) * 1000.0 / double(frequency), kind };
        next = (next + 1) % window.size(); count = std::min(count + 1, unsigned(window.size()));
    }
    Stats Get() const
    {
        Stats result; result.samples = count;
        if (!count) return result;
        std::array<double, 128> sorted {};
        for (unsigned i = 0; i < count; ++i)
        {
            const auto sample = window[i]; sorted[i] = sample.ms;
            if (sample.kind == Anchor) { result.anchor += sample.ms; ++result.anchors; }
            if (sample.kind == Skipped) { result.skipped += sample.ms; ++result.skips; }
        }
        if (result.anchors) result.anchor /= result.anchors;
        if (result.skips) result.skipped /= result.skips;
        std::sort(sorted.begin(), sorted.begin() + count);
        result.median = (sorted[(count - 1) / 2] + sorted[count / 2]) * 0.5;
        result.p95 = sorted[(95 * count + 99) / 100 - 1]; result.maximum = sorted[count - 1];
        return result;
    }
};
}
