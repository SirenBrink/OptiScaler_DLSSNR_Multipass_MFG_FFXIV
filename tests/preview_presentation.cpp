#include "../OptiScaler/misc/FfxivPreviewPresentation.h"
#include <cassert>
#include <cmath>
#include <cstdio>

int main()
{
    using FfxivPreviewPresentation::Clock;
    Clock clock;
    // All six outputs may be submitted together: each still has a distinct
    // phase and the last reaches the current real pose exactly.
    for (unsigned output = 1; output <= 6; ++output)
    {
        const auto step = clock.Plan(10, 6);
        assert(step.output == output && step.count == 6);
        assert(std::abs(step.Phase() - float(output) / 6.0f) < 1e-6f);
        clock.Commit(step);
    }
    assert(clock.Plan(10, 6).Phase() == 1.0f);

    // A skipped command list must not consume a phase. Factor changes start
    // at the next real batch; a change within one burst cannot rewind it.
    const auto skipped = clock.Plan(11, 3);
    const auto retried = clock.Plan(11, 3);
    assert(skipped.output == retried.output && retried.Phase() == 1.0f / 3.0f);
    clock.Commit(retried);
    const auto changedMidBurst = clock.Plan(11, 8);
    assert(changedMidBurst.count == 3 && changedMidBurst.output == 2);
    clock.Commit(changedMidBurst);
    assert(clock.Plan(11, 1).Phase() == 1.0f);
    const auto newFactor = clock.Plan(12, 2);
    assert(newFactor.count == 2 && newFactor.Phase() == 0.5f);
    clock.Commit(newFactor);

    assert(clock.Plan(9, 6).batch == 0);
    clock.Reset();
    assert(clock.Plan(1, 1).Phase() == 1.0f);
    assert(clock.Plan(1, 99).count == 8);
    assert(clock.Plan(0, 6).batch == 0);

    // Dynamic MFG can shorten a six-output batch before GetState reports the
    // new count. Keep the following adjacent pair from adding a full new step
    // on top of the unfinished endpoint, without carrying delay past that pair.
    clock.Reset();
    for (unsigned i=0; i<5; ++i) clock.Commit(clock.Plan(20,6));
    assert(std::abs(clock.Plan(21,5,true).Phase()-1.f/15.f)<1e-6f);
    float lastPhase=0;
    for (unsigned i=0; i<5; ++i) {
        const auto step=clock.Plan(21,5,true);
        assert(step.Phase()>lastPhase && step.Phase()<=1);
        lastPhase=step.Phase();clock.Commit(step);
    }
    assert(lastPhase==1);
    assert(clock.Plan(22,5,true).Phase()==.2f); // no accumulated latency
    clock.Reset();clock.Commit(clock.Plan(30,8));
    assert(clock.Plan(31,4,false).Phase()==.25f); // unrelated/held pair
    assert(clock.Plan(32,4,true).Phase()==.25f); // missing batch

    // All multiplier transitions remain monotonic and reach their endpoint,
    // including a shortened burst and an increase up to eight outputs.
    for (unsigned from=1;from<=8;++from) for(unsigned to=1;to<=8;++to) {
        clock.Reset();for(unsigned i=0;i<from;++i)clock.Commit(clock.Plan(40,8));
        float previous=-1;
        for(unsigned i=0;i<to;++i){const auto s=clock.Plan(41,to,true);
            assert(s.Phase()>previous && s.Phase()>=0 && s.Phase()<=1);
            previous=s.Phase();clock.Commit(s);}
        assert(previous==1);
    }

    // Round-trip the UNORM guide representation: a stationary pixel stays
    // stationary, and signed motion remains within half a quantisation step.
    for (float motion : {-48.0f, -10.0f, 0.0f, 10.0f, 48.0f})
    {
        const float byte = std::round(128.0f + motion * (255.0f / 128.0f));
        const float decoded = (byte - 128.0f) * (128.0f / 255.0f);
        assert(std::abs(decoded - motion) <= 0.252f);
        if (motion == 0.0f) assert(decoded == 0.0f);
    }
    std::puts("PASS: burst phases, skipped submission, dynamic factors, reset and signed guide encoding");
}
