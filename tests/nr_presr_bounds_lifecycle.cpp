// Run the production ownership/barrier function with resource-state assertions.
// Actual shader behavior is executed separately on WARP.
#include <cassert>
#include <cstdio>
#include <utility>
#include "../OptiScaler/shaders/dlssnr/DlssNr_Common.h"
enum { D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE };
constexpr int DXGI_FORMAT_R16G16B16A16_FLOAT=10;
struct ID3D12Resource { int state=D3D12_RESOURCE_STATE_UNORDERED_ACCESS; };
struct ID3D12GraphicsCommandList {};
unsigned allocations=0, dispatches=0;
bool allocationOk=true, dispatchOk=true;
ID3D12Resource* CreateScratch(void*, int, unsigned, unsigned) {++allocations;return allocationOk?new ID3D12Resource:nullptr;}
void Barrier(ID3D12GraphicsCommandList*,ID3D12Resource* r,int before,int after) {assert(r && r->state==before);r->state=after;}
struct Codec {
    bool DispatchPass(ID3D12GraphicsCommandList*,const DlssNrConstants& c,ID3D12Resource* raw,ID3D12Resource* current,
                      void*,void*,void*,ID3D12Resource* target,void*) {
        ++dispatches;
        assert(c.Mode==DlssNrMode_BoundResidual && c.Width==16 && c.Height==8 && c.GuideWidth==8 && c.GuideHeight==4);
        assert(raw!=target && raw->state==D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        assert(current->state==D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE && target->state==D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        return dispatchOk;
    }
};
struct Generation {
    bool trailGuard=false;void* device=nullptr;unsigned w=8,h=4,outW=16,outH=8;
    ID3D12Resource *residualOutput=new ID3D12Resource,*boundedResidual=nullptr;
    ID3D12Resource* residualInput=new ID3D12Resource{D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
    Codec storage;Codec* codec=&storage;
    ~Generation(){delete residualOutput;delete boundedResidual;delete residualInput;}
};
#include "presr-bounds-production.inl"
int main() {
    Generation g;ID3D12GraphicsCommandList cmd;
    auto* first=g.residualOutput;
    assert(BoundResidual(g,&cmd) && allocations==0 && dispatches==0 && g.residualOutput==first);
    g.trailGuard=true;allocationOk=false;
    assert(!BoundResidual(g,&cmd) && g.residualOutput==first && !g.boundedResidual);
    allocationOk=true;
    assert(BoundResidual(g,&cmd));auto* second=g.residualOutput;
    assert(second!=first && g.boundedResidual==first);
    for(unsigned i=0;i<100;++i) {
        auto* input=g.residualOutput;auto* scratch=g.boundedResidual;
        assert(BoundResidual(g,&cmd) && g.residualOutput==scratch && g.boundedResidual==input);
        assert(input->state==D3D12_RESOURCE_STATE_UNORDERED_ACCESS && scratch->state==D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    assert(allocations==2); // Failed allocation plus one successful allocation, not per frame.
    auto* stable=g.residualOutput;dispatchOk=false;
    assert(!BoundResidual(g,&cmd) && g.residualOutput==stable && stable->state==D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    g.trailGuard=false;
    auto calls=dispatches;assert(BoundResidual(g,&cmd) && dispatches==calls && g.residualOutput==stable);
    g.trailGuard=true;dispatchOk=true;assert(BoundResidual(g,&cmd) && allocations==2);
    std::puts("PASS: production bounds ownership/state transitions, repeated anchors, bypass, allocation/dispatch failures, re-enable without reallocation");
}
