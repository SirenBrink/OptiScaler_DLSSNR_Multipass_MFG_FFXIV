#define NOMINMAX
#include <misc/Dx11AutomaticMipBias.h>
#include <cassert>
#include <iostream>
#ifdef TEST_PRODUCTION_HOOKS
#include <detours/detours.h>
#include <string>
namespace SamplerMipBias {inline std::atomic<bool> automaticDx11Available{false}, automaticDx11Active{false};}
struct Config {struct Flag{bool value_or_default()const{return true;}} FfxivAutomaticMipBias;static Config* Instance(){static Config c;return &c;}};
struct State {std::string gameExe="ffxiv_dx11.exe";static State& Instance(){static State s;return s;}};
class D3D11Hooks{public: static void UpdateAutomaticMipBias(ID3D11DeviceContext*,unsigned,unsigned);static void ResetAutomaticMipBias();};
#define LOG_INFO(...) ((void)0)
#define LOG_ERROR(...) ((void)0)
#include "automatic_mip_hooks.inl"
#endif
using namespace Dx11AutomaticMipBias;
template<unsigned S> void STDMETHODCALLTYPE SetNative(ID3D11DeviceContext* c, UINT start, UINT n, ID3D11SamplerState* const* p) {
    if constexpr(S==0)c->PSSetSamplers(start,n,p);
    if constexpr(S==1)c->VSSetSamplers(start,n,p);
    if constexpr(S==2)c->GSSetSamplers(start,n,p);
    if constexpr(S==3)c->HSSetSamplers(start,n,p);
    if constexpr(S==4)c->DSSetSamplers(start,n,p);
    if constexpr(S==5)c->CSSetSamplers(start,n,p);
}
template<unsigned S> void STDMETHODCALLTYPE GetNative(ID3D11DeviceContext* c, UINT start, UINT n, ID3D11SamplerState** p) {
    if constexpr(S==0)c->PSGetSamplers(start,n,p);
    if constexpr(S==1)c->VSGetSamplers(start,n,p);
    if constexpr(S==2)c->GSGetSamplers(start,n,p);
    if constexpr(S==3)c->HSGetSamplers(start,n,p);
    if constexpr(S==4)c->DSGetSamplers(start,n,p);
    if constexpr(S==5)c->CSGetSamplers(start,n,p);
}
int main() {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    assert(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,
        D3D11_SDK_VERSION,&device,nullptr,&context)));
    const std::array<Set,6> sets {SetNative<0>,SetNative<1>,SetNative<2>,SetNative<3>,SetNative<4>,SetNative<5>};
    const std::array<Get,6> gets {GetNative<0>,GetNative<1>,GetNative<2>,GetNative<3>,GetNative<4>,GetNative<5>};
    Binder b; b.Attach(context.Get(),sets,gets);
    D3D11_SAMPLER_DESC desc {};
    desc.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    desc.AddressU=desc.AddressV=desc.AddressW=D3D11_TEXTURE_ADDRESS_WRAP;
    desc.MipLODBias=-.75f; desc.MaxLOD=D3D11_FLOAT32_MAX;desc.ComparisonFunc=D3D11_COMPARISON_NEVER;
    ComPtr<ID3D11SamplerState> world, ui, shadow;
    assert(SUCCEEDED(device->CreateSamplerState(&desc,&world)));
    desc.MaxLOD=0;assert(SUCCEEDED(device->CreateSamplerState(&desc,&ui)));
    desc.MaxLOD=D3D11_FLOAT32_MAX;desc.Filter=D3D11_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR;
    assert(SUCCEEDED(device->CreateSamplerState(&desc,&shadow)));
    ID3D11SamplerState* input[] {world.Get(),nullptr,ui.Get(),shadow.Get()};
    for(unsigned s=0;s<6;++s)sets[s](context.Get(),2,4,input);
    auto inspect=[&](unsigned s,float expected) {
        ComPtr<ID3D11SamplerState> physical, logical;
        gets[s](context.Get(),2,1,&physical);D3D11_SAMPLER_DESC d {};physical->GetDesc(&d);
        assert(std::abs(d.MipLODBias-expected)<.00001f);
        b.Read(s,context.Get(),2,1,&logical);assert(logical.Get()==world.Get());
        // Querying state for a renderer's backup must return the game's original objects.
        std::array<ID3D11SamplerState*,4> backup {};
        b.Read(s,context.Get(),2,4,backup.data());
        for(unsigned i=0;i<4;++i)assert(backup[i]==input[i]);
        b.Bind(s,context.Get(),2,4,backup.data());
        for(auto p:backup)if(p)p->Release();
    };
    b.Update(context.Get(),true,1920,3840);
    for(unsigned s=0;s<6;++s)inspect(s,-1);
    auto count=b.Creations();
    for(unsigned f=0;f<1000;++f){b.Update(context.Get(),true,1920,3840);for(unsigned s=0;s<6;++s)b.Bind(s,context.Get(),2,4,input);}
    assert(b.Creations()==count); // no steady-state CreateSamplerState churn
    b.Update(context.Get(),true,2560,3840);
    for(unsigned s=0;s<6;++s)inspect(s,std::log2(2.f/3));
    b.Update(context.Get(),true,3840,3840);
    for(unsigned s=0;s<6;++s)inspect(s,0);
    b.Update(context.Get(),false,3840,3840);
    for(unsigned s=0;s<6;++s)inspect(s,-.75f);
    b.Update(context.Get(),true,1920,3840);
    context->ClearState();
    b.Update(context.Get(),true,2560,3840);
    for(unsigned s=0;s<6;++s){ComPtr<ID3D11SamplerState> empty;b.Read(s,context.Get(),2,1,&empty);assert(!empty);}
    // Another context's state is never substituted.
    ComPtr<ID3D11DeviceContext> deferred;assert(SUCCEEDED(device->CreateDeferredContext(0,&deferred)));
    b.Bind(0,deferred.Get(),2,4,input);
    ComPtr<ID3D11SamplerState> other;deferred->PSGetSamplers(2,1,&other);assert(other.Get()==world.Get());
    b.Bind(0,context.Get(),2,4,input);
    b.Update(context.Get(),true,0,3840);inspect(0,-.75f);
    b.Clear(context.Get());context->ClearState();deferred->ClearState();
#ifdef TEST_PRODUCTION_HOOKS
    State::Instance().gameExe="another_game.exe";InstallAutomaticMipBias(device.Get());assert(!autoMipReady.load());
    State::Instance().gameExe="ffxiv_dx11.exe";
    InstallAutomaticMipBias(device.Get());assert(autoMipReady.load());
    context->PSSetSamplers(2,4,input);
    D3D11Hooks::UpdateAutomaticMipBias(context.Get(),1920,3840);
    for(unsigned stage=0;stage<6;++stage) {
        sets[stage](context.Get(),2,4,input);
        ComPtr<ID3D11SamplerState> physical,logical;
        autoMipGet[stage](context.Get(),2,1,&physical);
        D3D11_SAMPLER_DESC d {};physical->GetDesc(&d);assert(d.MipLODBias==-1);
        gets[stage](context.Get(),2,1,&logical);assert(logical.Get()==world.Get());
    }
    ComPtr<ID3D11SamplerState> native,visible;
    autoMipGet[0](context.Get(),2,1,&native);
    D3D11_SAMPLER_DESC physicalDesc {};native->GetDesc(&physicalDesc);assert(physicalDesc.MipLODBias==-1);
    context->PSGetSamplers(2,1,&visible);assert(visible.Get()==world.Get());
    context->PSSetSamplers(2,1,visible.GetAddressOf());
    // NGX may notify with a proxy identity. Bind on the captured native context.
    D3D11Hooks::UpdateAutomaticMipBias(deferred.Get(),2560,3840);
    native.Reset();autoMipGet[0](context.Get(),2,1,&native);
    native->GetDesc(&physicalDesc);assert(std::abs(physicalDesc.MipLODBias-std::log2(2.f/3))<.00001f);
    D3D11Hooks::ResetAutomaticMipBias();
    visible.Reset();context->PSGetSamplers(2,1,&visible);assert(visible.Get()==world.Get());
    RemoveAutomaticMipBias();assert(!autoMipReady.load());assert(!SamplerMipBias::automaticDx11Active.load());
    std::cout<<"PASS production Detours hooks install, native binding, logical Get/restore, live ratio, shutdown/unhook\n";
#endif
    std::cout<<"PASS automatic mip bias: cached live scale changes, six stages, logical state restore, scope, native fallback\n";
}
