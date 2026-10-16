#pragma once
#include <algorithm>
#include <cstdint>

namespace FfxivPreviewPresentation
{
// Native MFG presents are submitted in bursts, not at display cadence.
// Plan one distinct preview phase per output, committing only submitted work.
class Clock
{
  public:
    struct Step
    {
        uint64_t batch = 0;
        unsigned output = 0;
        unsigned count = 1;
        float deficit = 0.0f;
        float Phase() const
        {
            const float phase = float(output) / float(count);
            return std::clamp(phase - deficit * (1.0f - phase), 0.0f, 1.0f);
        }
    };

    Step Plan(uint64_t batch, unsigned multiplier, bool continuousPair = false) const
    {
        if (!batch || batch < last.batch)
            return {};
        if (batch != last.batch)
        {
            // SL reports the previous batch's factor. If a shorter burst left
            // its endpoint unfinished, ease that small remainder into the next
            // adjacent pose pair. Pay it off by this batch's endpoint: never
            // accumulate delay or infer a new factor from wall-clock bursts.
            const float deficit = continuousPair && last.batch && batch == last.batch + 1 ?
                std::clamp(1.0f - last.Phase(), 0.0f, 0.25f) : 0.0f;
            return {batch, 1, std::clamp(multiplier, 1u, 8u), deficit};
        }
        return {batch, std::min(last.output + 1, last.count), last.count, last.deficit};
    }
    void Commit(const Step& step) { last = step; }
    uint64_t Batch() const { return last.batch; }
    void Reset() { last = {}; }

  private:
    Step last;
};
}
