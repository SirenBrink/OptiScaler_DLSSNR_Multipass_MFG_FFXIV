#pragma once
#include <cstdint>
namespace DlssNr
{
// Three clean frames retain current, previous, and previous-previous scene.
// A stages NR; B resolves its SR/FG. In steady state both output frame n-2:
// A uses the last resolved anchor; B uses the midpoint just reconstructed.
struct PreSrSplitSchedule
{
    unsigned write = 0, captured = 0, resolved = 0;
    uint64_t queuedEpoch = 0;
    bool queued = false;
    static bool Evaluate(bool split, bool skipNr) { return split ? skipNr : !skipNr; }
    void Reset() { *this = {}; }
    void Queue(uint64_t epoch) { queuedEpoch = epoch; queued = true; }
    bool CanResolve(uint64_t epoch) const { return queued && epoch > queuedEpoch && epoch - queuedEpoch == 1; }
    void Resolved() { queued = false; if (resolved < 2) ++resolved; }
    unsigned Output() const { return captured < 2 ? 0 : (write + 1) % 3; }
    bool HasEdit() const { return resolved != 0; }
    bool UseMidpoint(bool resolveFrame) const { return resolveFrame && resolved >= 2; }
    void Advance() { if (captured < 2) ++captured; write = (write + 1) % 3; }
};
}
