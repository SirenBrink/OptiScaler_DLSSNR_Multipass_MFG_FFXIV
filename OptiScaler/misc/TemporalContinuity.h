#pragma once
#include <cstdint>

namespace TemporalContinuity
{
// Track successful submissions, not attempted dispatches. A skipped input cannot
// become the previous temporal frame merely because its slot was selected.
struct SuccessfulFrames
{
    bool valid = false;
    std::uint64_t last = 0;
    bool NeedsReset(std::uint64_t frame) const
    {
        return !valid || frame <= last || frame - last != 1;
    }
    void Commit(std::uint64_t frame) { last = frame; valid = true; }
    void Invalidate() { valid = false; }
};
}
