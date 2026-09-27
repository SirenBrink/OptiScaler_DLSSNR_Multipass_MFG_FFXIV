// Real WARP copies, queries and Maps through the production scan scheduler.
// Synthetic verified-stage fixtures; this does not emulate FFXIV's draw graph.
#include <cassert>
#include <cstdio>
#define LOG_INFO(...) ((void)0)
#define LOG_WARN(...) ((void)0)
#define LOG_ERROR(...) ((void)0)
#include "../OptiScaler/misc/FfxivLightingScan.h"
#pragma comment(lib, "d3d11.lib")

int main()
{
    using namespace FfxivLightingScan;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    assert(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,
        D3D11_SDK_VERSION,&device,nullptr,&context)));
    auto texture = [&](UINT width,UINT height,DXGI_FORMAT format,const void* data,UINT pitch) {
        D3D11_TEXTURE2D_DESC desc {};
        desc.Width=width; desc.Height=height; desc.MipLevels=desc.ArraySize=1;
        desc.Format=format; desc.SampleDesc.Count=1;
        D3D11_SUBRESOURCE_DATA initial {data,pitch,0};
        ComPtr<ID3D11Texture2D> result;
        assert(SUCCEEDED(device->CreateTexture2D(&desc,data ? &initial : nullptr,&result)));
        return result;
    };
    float gain=1;
    auto gainSource=texture(1,1,DXGI_FORMAT_R32_FLOAT,&gain,sizeof(gain));
    std::array<uint16_t,4096> lookup; lookup.fill(0x3c00); // Half-float 1.0.
    auto lutSource=texture(1024,1,DXGI_FORMAT_R16G16B16A16_FLOAT,lookup.data(),sizeof(lookup));
    std::array<float,12> constants {};
    constants[1]=constants[6]=1; constants[8]=0.01f; constants[9]=0.99f;
    ComPtr<ID3D11Buffer> cb;
    D3D11_BUFFER_DESC bd {}; bd.ByteWidth=sizeof(constants);
    D3D11_SUBRESOURCE_DATA data {constants.data(),0,0};
    assert(SUCCEEDED(device->CreateBuffer(&bd,&data,&cb)));
    auto& s=State();
    auto begin = [&](ID3D11Resource* scene) {
        s.next=0; Tick(context.Get(),true);
        assert(s.collecting>=0 && armed && !s.failed);
        auto& slot=s.slots[s.collecting];
        context->CopyResource(slot.gain.Get(),gainSource.Get());
        context->CopyResource(slot.lut.Get(),lutSource.Get());
        context->CopyResource(slot.constants.Get(),cb.Get());
        s.haveGain=s.haveLut=s.haveTone=true;
        s.colorAliases[0]=scene; s.slotLinear[s.collecting]=false;
        return s.collecting;
    };
    auto drain = [&](int index) {
        // Test-only wait: production Tick remains nonblocking and never Flushes.
        context->Flush(); BOOL ready=FALSE; HRESULT hr=S_FALSE;
        const auto deadline=GetTickCount64()+3000;
        do {
            hr=context->GetData(s.slots[index].ready.Get(),&ready,sizeof(ready),0);
            if(hr==S_FALSE) Sleep(1);
        } while(hr==S_FALSE && GetTickCount64()<deadline);
        assert(hr==S_OK && ready);
        s.next=UINT64_MAX; Tick(context.Get(),true);
        assert(!s.slots[index].pending && !s.failed);
    };
    uint64_t accepted=0;
    for(unsigned i=0;i<48;++i)
    {
        const UINT scale=1+i%3;
        auto scene=texture(128*scale,72*scale,DXGI_FORMAT_R16G16B16A16_FLOAT,nullptr,0);
        auto other=texture(128*scale,72*scale,DXGI_FORMAT_R16G16B16A16_FLOAT,nullptr,0);
        if(i%8==0) // Missing scene link must recover on the next valid sample.
        {
            begin(scene.Get()); Boundary(context.Get(),other.Get(),false);
            assert(s.collecting==-1 && !s.failed && Latest().samples==accepted);
        }
        if(i%7==0) // Disable with a pending slot; never accept its old generation.
        {
            int index=begin(scene.Get()); Boundary(context.Get(),scene.Get(),false);
            TickWithPoll(context.Get(),false,[](auto*,auto*,BOOL*){return S_FALSE;});
            assert(!enabled && !Latest().valid && s.slots[index].pending);
            drain(index);
            assert(Latest().samples==accepted && !Latest().valid);
        }
        if(i%9==0) // Completed but stale GPU results cannot drive history resets.
        {
            int index=begin(scene.Get()); Boundary(context.Get(),scene.Get(),false);
            s.slots[index].time=GetTickCount64()-LightingHistory::MaxAgeMs-1;
            drain(index); assert(Latest().samples==accepted);
        }
        int index=begin(scene.Get()); Boundary(context.Get(),scene.Get(),false);
        drain(index);
        const auto reading=Latest(); ++accepted;
        assert(reading.samples==accepted && Fresh(reading,GetTickCount64()));
        assert(reading.events==0 && std::abs(reading.gain-1)<0.0001f);
        assert(LightingHistory::Valid(reading.curve));
        for(const auto& alias:s.colorAliases) assert(!alias);
    }
    Tick(context.Get(),false);
    assert(!enabled && !Latest().valid);
    puts("PASS: 48 real GPU readbacks across resource/size changes; missing-link recovery; disable/re-enable with pending work; stale-generation rejection; no false history cuts");
}
