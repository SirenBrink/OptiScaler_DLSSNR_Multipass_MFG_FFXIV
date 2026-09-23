// Exercises the production motion-policy, reset parameter and successful-SR
// acknowledgement fragments with mock NGX. No game or NVIDIA runtime required.
#include <cassert>
#include <memory>
#include <string>
#include <cstdio>
#include <shaders/dlssnr/PreSrMotionReset.h>
#include <shaders/dlssnr/PreSrSplitSchedule.h>
constexpr unsigned NVSDK_NGX_DLSS_Feature_Flags_MVLowRes=1, NVSDK_NGX_DLSS_Feature_Flags_MVJittered=2;
constexpr int NVSDK_NGX_Parameter_Reset=1, NVSDK_NGX_Result_Success=0;
struct State { std::string gameExe="ffxiv_dx11.exe"; static State& Instance() { static State s; return s; } };
struct Parameters { unsigned reset=0; void Set(int, unsigned value) { reset=value; } };
struct Half { struct { DlssNr::PreSrSplitSchedule schedule; } split; bool havePrevious=true, failed=false; void Reset() { havePrevious=false; } };
struct Generation {
    bool privateHistoryResetPending=false, reset=false, failed=false, approximateCamera=true, trailGuard=false;
    Parameters storage; Parameters* parameters=&storage;
    std::unique_ptr<Half> half=std::make_unique<Half>();
    int timing=0, feature=0;
    PreSrMotionReset::Gate gate;
    float speed=0;
};
struct Pair { bool skipNr=false, half=true; };
struct Frame { bool Reset=false; };
unsigned panSamples=0;
bool ConsumePanOnset(Generation& g, unsigned long long now) { ++panSamples; return g.gate.Update(g.speed,now,now); }
unsigned UInt(Parameters* p, int) { return p->reset; }
void Say(const std::string&) {}
#define LOG_INFO(...) ((void)0)
struct PreSrTiming { enum { Sr, Guard }; struct Scope { Scope(int,void*,unsigned,int) {} void End() {} }; };
bool BoundResidual(Generation&, void*) { return true; } // Covered by the bounds lifecycle/shader tests.
unsigned calls=0;
bool succeed=true;
namespace NVNGXProxy {
int Evaluate(void*,int,Parameters*,void*) { ++calls; return succeed ? 0 : 1; }
auto D3D12_EvaluateFeature() { return &Evaluate; }
}
#include "nr_presr_pan_production.inl"
void Observe(Generation& g, unsigned long long now, float speed, bool wantsHalf=true) {
    g.speed=speed; RecordProduction(g,false,NVSDK_NGX_DLSS_Feature_Flags_MVLowRes,wantsHalf,now);
}
int main() {
    Generation g;
    for(unsigned t=100;t<=400;t+=50) Observe(g,t,0);
    Observe(g,450,.3f); // onset on the frame that deliberately skips NR/SR
    assert(g.privateHistoryResetPending);
    EvaluateProduction(g,{true,true});
    assert(calls==0 && g.privateHistoryResetPending);
    Observe(g,500,.3f); // sustained motion must not lose the pending onset
    ArmProduction(g,{});
    assert(g.parameters->reset==1);
    EvaluateProduction(g,{false,true});
    assert(calls==1 && !g.privateHistoryResetPending && g.half->havePrevious);
    assert(ResetFgProduction(g)); // residual FG restarts with its newly reset carrier
    for(unsigned t=550;t<=2000;t+=50) {
        Observe(g,t,.3f);
        const bool skip=(t/50)%2!=0;
        if(!skip) ArmProduction(g,{});
        EvaluateProduction(g,{skip,true});
        if(!skip) assert(!g.parameters->reset && !ResetFgProduction(g));
    }
    assert(!g.privateHistoryResetPending); // no repeated resets during a sustained pan
    for(unsigned t=2050;t<=2350;t+=50) Observe(g,t,0);
    Observe(g,2400,.3f);
    ArmProduction(g,{});
    succeed=false;
    EvaluateProduction(g,{false,true});
    assert(g.failed && g.privateHistoryResetPending); // failed evaluation cannot acknowledge it
    succeed=true;
    Generation ordinary;
    ordinary.approximateCamera=false;
    for(unsigned t=100;t<=400;t+=50) Observe(ordinary,t,0,false);
    Observe(ordinary,450,.3f,false);
    ArmProduction(ordinary,{});
    EvaluateProduction(ordinary,{false,false});
    assert(ordinary.parameters->reset && !ordinary.privateHistoryResetPending);
    ArmProduction(ordinary,{true});
    assert(ResetFgProduction(ordinary)); // explicit scene reset also invalidates residual FG
    Generation bounded;
    bounded.trailGuard=true;
    const auto priorSamples=panSamples;
    for(unsigned t=100;t<=2000;t+=50) {
        Observe(bounded,t,(t%500)<300 ? 0.f : .3f);
        ArmProduction(bounded,{});
        assert(!bounded.privateHistoryResetPending && !bounded.parameters->reset && !ResetFgProduction(bounded));
    }
    assert(panSamples==priorSamples); // No pan readback consumed while bounds protect the edit.
    ArmProduction(bounded,{true});
    assert(bounded.parameters->reset && ResetFgProduction(bounded)); // Real cut survives.
    bounded.reset=true; ArmProduction(bounded,{});
    assert(bounded.parameters->reset && ResetFgProduction(bounded)); // Lighting/failure reset survives.
    bounded.reset=false; bounded.trailGuard=false;
    for(unsigned t=2100;t<=2400;t+=50) Observe(bounded,t,0);
    Observe(bounded,2450,.3f); ArmProduction(bounded,{});
    assert(bounded.privateHistoryResetPending && bounded.parameters->reset && ResetFgProduction(bounded));
    std::puts("PreSR pan cadence: legacy pan policy retained, bounded mode avoids pan resets, cuts/lighting resets survive, skipped onset/failure retention passed");
}
