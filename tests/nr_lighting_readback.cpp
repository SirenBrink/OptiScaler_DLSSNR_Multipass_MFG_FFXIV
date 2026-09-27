// Exercise production scan scheduling with deterministic GPU query outcomes.
#include <cstdio>
#include <cassert>
#define LOG_INFO(...) ((void)0)
#define LOG_WARN(...) ((void)0)
#define LOG_ERROR(...) ((void)0)
#include "../OptiScaler/misc/FfxivLightingScan.h"
#pragma comment(lib, "d3d11.lib")
// Model ReShade's distinct COM identity plus its public original-object query.
struct IdentityProxy : IUnknown
{
    IUnknown* original; ULONG refs=1;
    explicit IdentityProxy(IUnknown* value):original(value){}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override
    {
        if(!out) return E_POINTER;
        *out=nullptr;
        if(id==IID_IUnknown) { *out=this; AddRef(); return S_OK; }
        if(id==FfxivLightingScan::UnwrappedObject && original) { *out=original; original->AddRef(); return S_OK; }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { return --refs; }
};
int main()
{
    using namespace FfxivLightingScan;
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
    assert(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,
        D3D11_SDK_VERSION,&device,nullptr,&context)));
    ComPtr<ID3D11DeviceContext1> context1;
    assert(SUCCEEDED(context.As(&context1)));
    assert(SameContext(context.Get(), context1.Get()));
    assert(!SameContext(context.Get(), nullptr));
    ComPtr<ID3D11Device> otherDevice; ComPtr<ID3D11DeviceContext> otherContext;
    assert(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,
        D3D11_SDK_VERSION,&otherDevice,nullptr,&otherContext)));
    assert(!SameContext(context.Get(), otherContext.Get()));
    IdentityProxy proxy(device.Get()), nested(&proxy), foreign(otherDevice.Get());
    assert(NativeIdentity(&proxy)==NativeIdentity(device.Get()));
    assert(NativeIdentity(&nested)==NativeIdentity(device.Get()));
    assert(NativeIdentity(&foreign)!=NativeIdentity(device.Get()));
    assert(proxy.refs==1 && nested.refs==1 && foreign.refs==1);
    IdentityProxy cycleA(nullptr), cycleB(&cycleA); cycleA.original=&cycleB;
    assert(!NativeIdentity(&cycleA));
    assert(cycleA.refs==1 && cycleB.refs==1);
    auto& s = State(); s.active = true; s.context = context.Get();
    // Different contexts on one device are allowed only with the exact captured
    // colour. Query completion must not depend on executing NGX's deferred list.
    ComPtr<ID3D11DeviceContext> deferred;
    assert(SUCCEEDED(device->CreateDeferredContext(0, &deferred)));
    assert(Allocate(context.Get(), s.slots[0]));
    auto prepare = [&] {
        s.collecting=0; s.haveTone=true; s.color=reinterpret_cast<uintptr_t>(s.slots[0].lut.Get());
        s.slots[0].pending=false; armed=true;
    };
    prepare(); Boundary(otherContext.Get(), s.slots[0].lut.Get(), false);
    assert(!s.slots[0].pending && s.collecting == -1);
    prepare(); Boundary(deferred.Get(), s.slots[0].gain.Get(), false);
    assert(!s.slots[0].pending && s.collecting == -1);
    prepare(); Boundary(deferred.Get(), s.slots[0].lut.Get(), true);
    assert(!s.slots[0].pending && s.collecting == -1);
    prepare(); Boundary(deferred.Get(), s.slots[0].lut.Get(), false);
    assert(s.slots[0].pending && s.collecting == -1 && !armed.load());
    context->Flush(); BOOL ready=FALSE; HRESULT query=S_FALSE;
    const auto deadline=GetTickCount64()+2000;
    do { query=context->GetData(s.slots[0].ready.Get(), &ready, sizeof(ready), 0); if(query==S_FALSE) Sleep(1); }
    while(query==S_FALSE && GetTickCount64()<deadline);
    assert(query==S_OK && ready);
    s.reading.valid = true; s.reading.eventTime = 1;
    auto& slot = s.slots[0]; slot.pending = true; slot.time = GetTickCount64()-3000;
    slot.generation = s.generation;
    auto pending=[](auto*,auto*,BOOL*){return S_FALSE;};
    TickWithPoll(context.Get(),true,pending);
    assert(s.waitingReadback && !s.failed && !s.reading.valid && !s.reading.failed);
    assert(slot.pending && s.collecting == -1 && !armed.load());
    const auto generation = s.generation;
    TickWithPoll(context.Get(),true,pending);
    assert(s.generation == generation && slot.pending); // No repeated invalidation/recycling.
    auto complete=[](auto*,auto*,BOOL*done){*done=TRUE;return S_OK;};
    TickWithPoll(context.Get(),true,complete);
    assert(!s.waitingReadback && !s.failed && !slot.pending);
    assert(s.reading.samples == 0 && s.collecting >= 0 && armed.load()); // Stale sample discarded.
    TickWithPoll(context.Get(),false,complete);
    assert(!armed.load() && !s.active && s.collecting == -1);
    // Actual query errors must still stop new work.
    slot.pending=true; slot.time=GetTickCount64();
    auto failure=[](auto*,auto*,BOOL*){return HRESULT(DXGI_ERROR_DEVICE_REMOVED);};
    TickWithPoll(context.Get(),true,failure);
    assert(s.failed && s.reading.failed && !enabled.load() && !armed.load());
    puts("PASS: cross-context query completes on capture context; wrong device/resource/HDR rejected; delayed-readback recovery intact");
}
