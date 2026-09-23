#include "../OptiScaler/misc/LightingHistory.h"
#include <cassert>
#include <iostream>
#include <limits>

int main()
{
    using LightingHistory::Curve;
    using LightingHistory::Gate;
    const Curve base {0.1f, 0.2f, 0.4f, 0.7f};
    Curve neutral = base;
    Gate gate;
    assert(!gate.Observe(neutral, 1000));
    // A large cumulative change from slow adaptation must not flush history.
    for (uint64_t t = 1100; t <= 4000; t += 100)
    {
        for (auto& value : neutral) value *= 1.01f;
        assert(!gate.Observe(neutral, t));
    }
    auto jump = neutral;
    for (auto& value : jump) value *= 1.5f;
    assert(gate.Observe(jump, 4100));
    assert(!gate.Observe(jump, 4100));
    assert(!gate.Observe(neutral, 4000)); // Out-of-order readings cannot rewind history.
    for (uint64_t t = 4200; t <= 5000; t += 100)
        assert(!gate.Observe(t % 200 ? jump : neutral, t)); // Oscillation cannot rearm.
    for (uint64_t t = 5100; t <= 5500; t += 100)
        assert(!gate.Observe(jump, t));
    assert(gate.Observe(neutral, 5600));
    assert(!gate.Observe(jump, 6100)); // Stale gaps establish a new baseline.

    for (float invalid : {0.0f, -1.0f, 101.0f, std::numeric_limits<float>::infinity(),
                          std::numeric_limits<float>::quiet_NaN()})
    {
        auto bad = base;
        bad[0] = invalid;
        assert(!gate.Observe(bad, 6200));
        assert(!gate.Observe(neutral, 6300));
    }

    Gate cooldown;
    assert(!cooldown.Observe(neutral, 1000));
    assert(cooldown.Observe(jump, 1100));
    for (uint64_t t = 1200; t <= 1500; t += 100)
        assert(!cooldown.Observe(jump, t));
    assert(!cooldown.Observe(neutral, 1600)); // Quiet interval alone is insufficient.
    for (uint64_t t = 1700; t <= 2100; t += 100)
        assert(!cooldown.Observe(neutral, t));
    assert(cooldown.Observe(jump, 2200));

    Gate slow;
    assert(!slow.Observe(neutral, 1000));
    assert(!slow.Observe(jump, 1250)); // Not an abrupt change within the 200 ms window.

    uint64_t nr = 0, presr = 0;
    assert(!LightingHistory::Consume(0, 0, 1000, nr));
    assert(LightingHistory::Consume(1, 1000, 1100, nr));
    assert(!LightingHistory::Consume(1, 1000, 1100, nr));
    assert(LightingHistory::Consume(1, 1000, 1100, presr));
    assert(!LightingHistory::Consume(2, 1000, 1500, nr)); // Acknowledge expired events.
    assert(!LightingHistory::Consume(2, 1000, 1500, nr));
    assert(!LightingHistory::Consume(3, 2000, 1900, nr));
    assert(LightingHistory::Consume(4, 2000, 2350, nr));
    assert(!LightingHistory::Consume(5, 2000, 2351, nr));
    std::cout << "Lighting history: cuts, drift, rearm, cooldown, invalid/stale readings and independent consumers passed\n";
}
