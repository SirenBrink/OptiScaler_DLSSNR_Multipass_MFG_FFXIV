#pragma once
#include "CompanionPackets.h"
#include "CompanionCore.h"
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <atomic>
#include <memory>
#include <string>
#include <algorithm>

// The native producer renders only the exact Companion-delimited commands.
// The worker opens shared colour textures; it never touches the game context.
namespace FfxivCompanion::Visibility
{
using Microsoft::WRL::ComPtr;
struct Slot
{
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11RenderTargetView> view;
    ComPtr<IDXGIKeyedMutex> keyed;
    ComPtr<ID3D11Query> begin,end,disjoint;
    HANDLE handle=nullptr; // Legacy shared handle belongs to the texture; do not CloseHandle.
    std::atomic<int> state{0}; // free, producer, available, consumer
    uint64_t serial=0;
    ULONGLONG time=0;
    bool timing=false;
    std::shared_ptr<const Snapshot> snapshot;
};
struct Ring
{
    std::array<Slot,2> slots;
    UINT width=0,height=0;
    LUID adapter{};
    std::atomic<uint64_t> validSerial{0};
    std::atomic<double> gpuMs{0};
    uint64_t nextSerial=0; // Producer only.
    bool Init(ID3D11Device* device,UINT w,UINT h)
    {
        if(!w || !h || w>8192 || h>8192 || uint64_t(w)*h>32*1024*1024)return false;
        width=w;height=h;
        ComPtr<IDXGIDevice> dxgi;ComPtr<IDXGIAdapter> a;DXGI_ADAPTER_DESC ad{};
        if(FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgi))) || FAILED(dxgi->GetAdapter(&a)) || FAILED(a->GetDesc(&ad)))return false;
        adapter=ad.AdapterLuid;
        D3D11_TEXTURE2D_DESC d{};d.Width=w;d.Height=h;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;
        d.Format=DXGI_FORMAT_B8G8R8A8_UNORM;d.BindFlags=D3D11_BIND_RENDER_TARGET;d.MiscFlags=D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
        for(auto& s:slots)
        {
            ComPtr<IDXGIResource> resource;
            if(FAILED(device->CreateTexture2D(&d,nullptr,&s.texture)) || FAILED(device->CreateRenderTargetView(s.texture.Get(),nullptr,&s.view)) ||
               FAILED(s.texture.As(&s.keyed)) || FAILED(s.texture.As(&resource)) || FAILED(resource->GetSharedHandle(&s.handle)))return false;
            D3D11_QUERY_DESC q{D3D11_QUERY_TIMESTAMP,0};
            if(FAILED(device->CreateQuery(&q,&s.begin)) || FAILED(device->CreateQuery(&q,&s.end)))return false;
            q.Query=D3D11_QUERY_TIMESTAMP_DISJOINT;if(FAILED(device->CreateQuery(&q,&s.disjoint)))return false;
        }
        return true;
    }
    Slot* Acquire(ID3D11DeviceContext* c)
    {
        for(auto& s:slots)
        {
            int expected=0;if(!s.state.compare_exchange_strong(expected,1))continue;
            // WAIT_TIMEOUT is positive: SUCCEEDED is NOT a valid keyed-mutex test.
            if(s.keyed->AcquireSync(0,0)!=S_OK){s.state=0;continue;}
            if(s.timing)
            {
                D3D11_QUERY_DATA_TIMESTAMP_DISJOINT d{};UINT64 a=0,b=0;
                if(c->GetData(s.disjoint.Get(),&d,sizeof(d),D3D11_ASYNC_GETDATA_DONOTFLUSH)==S_OK && !d.Disjoint && d.Frequency &&
                   c->GetData(s.begin.Get(),&a,sizeof(a),D3D11_ASYNC_GETDATA_DONOTFLUSH)==S_OK &&
                   c->GetData(s.end.Get(),&b,sizeof(b),D3D11_ASYNC_GETDATA_DONOTFLUSH)==S_OK && b>=a)
                    gpuMs=double(b-a)*1000/d.Frequency;
            }
            s.timing=false;s.snapshot.reset();return &s;
        }
        return nullptr;
    }
};
// Scope per selected native batch. Output redirection is per draw, after the game
// has bound the live DSV, shaders, constants and alpha state. No frozen materials.
struct Producer
{
    std::shared_ptr<Ring>& ring;
    Slot* slot=nullptr;
    ID3D11DeviceContext* context=nullptr;
    bool failed=false;
    UINT draws=0;
    std::string reason;
    explicit Producer(std::shared_ptr<Ring>& r):ring(r){}
    bool Reject(const char* message){failed=true;reason=message;return false;}
    bool Draw(ID3D11DeviceContext* c,void(*original)(void*),void* args)
    {
        if(failed)return false;
        if(c->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE || (context && context!=c))return Reject("Unexpected native draw context");
        // Native depth writes, stencil writes, UAVs and extra colour targets would
        // have side effects if a rejected attempt must fall back to native drawing.
        ComPtr<ID3D11DepthStencilState> depth;UINT ref=0;c->OMGetDepthStencilState(&depth,&ref);
        D3D11_DEPTH_STENCIL_DESC dd{};if(depth)depth->GetDesc(&dd);
        ID3D11RenderTargetView* raw[8]{};ComPtr<ID3D11DepthStencilView> dsv;c->OMGetRenderTargets(8,raw,&dsv);
        std::array<ComPtr<ID3D11RenderTargetView>,8> rt;for(UINT i=0;i<8;++i)rt[i].Attach(raw[i]);
        ID3D11UnorderedAccessView* uavs[8]{};c->OMGetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,0,8,uavs);
        bool hasUav=false;for(auto u:uavs)if(u){hasUav=true;u->Release();}
        if(hasUav || !rt[0])return Reject("UAV or missing native target");
        for(UINT i=1;i<8;++i)if(rt[i])return Reject("Multiple native colour targets");
        if(dsv && (!depth || dd.StencilEnable || (dd.DepthEnable && dd.DepthWriteMask!=D3D11_DEPTH_WRITE_MASK_ZERO)))return Reject("Depth/stencil write side effects");
        ComPtr<ID3D11Resource> resource;rt[0]->GetResource(&resource);ComPtr<ID3D11Texture2D> target;
        if(FAILED(resource.As(&target)))return Reject("Unsupported native target");
        D3D11_TEXTURE2D_DESC td{};target->GetDesc(&td);D3D11_RENDER_TARGET_VIEW_DESC rd{};rt[0]->GetDesc(&rd);
        if(td.SampleDesc.Count!=1 || td.ArraySize!=1 || rd.ViewDimension!=D3D11_RTV_DIMENSION_TEXTURE2D || rd.Texture2D.MipSlice!=0 ||
           (rd.Format!=DXGI_FORMAT_B8G8R8A8_UNORM && rd.Format!=DXGI_FORMAT_R8G8B8A8_UNORM))return Reject("Native target is not single-sample SDR");
        ComPtr<ID3D11BlendState> blend;FLOAT factors[4]{};UINT sampleMask=0;c->OMGetBlendState(&blend,factors,&sampleMask);
        D3D11_BLEND_DESC bd{};if(blend)blend->GetDesc(&bd);auto& b=bd.RenderTarget[0];
        if(!blend || bd.AlphaToCoverageEnable || bd.IndependentBlendEnable || !b.BlendEnable || b.SrcBlend!=D3D11_BLEND_SRC_ALPHA ||
           b.DestBlend!=D3D11_BLEND_INV_SRC_ALPHA || b.BlendOp!=D3D11_BLEND_OP_ADD || b.SrcBlendAlpha!=D3D11_BLEND_ONE ||
           b.DestBlendAlpha!=D3D11_BLEND_INV_SRC_ALPHA || b.BlendOpAlpha!=D3D11_BLEND_OP_ADD || b.RenderTargetWriteMask!=15)
            return Reject("Unsupported native transparency blend");
        if(!ring)
        {
            ComPtr<ID3D11Device> device;c->GetDevice(&device);auto next=std::make_shared<Ring>();
            if(!next->Init(device.Get(),td.Width,td.Height))return Reject("Shared visibility texture creation failed");ring=std::move(next);
        }
        if(td.Width!=ring->width || td.Height!=ring->height)return Reject("Native target size changed");
        if(!slot)
        {
            slot=ring->Acquire(c);if(!slot)return Reject("Visibility consumer busy; native fallback");context=c;
            c->Begin(slot->disjoint.Get());c->End(slot->begin.Get());FLOAT clear[4]{};c->ClearRenderTargetView(slot->view.Get(),clear);
        }
        auto output=slot->view.Get();c->OMSetRenderTargets(1,&output,dsv.Get());
        struct Restore {ID3D11DeviceContext* c;ID3D11RenderTargetView** rt;ID3D11DepthStencilView* d;~Restore(){c->OMSetRenderTargets(8,rt,d);}} restore{c,raw,dsv.Get()};
        original(args);++draws;return true;
    }
    bool Finish(bool publish)
    {
        if(!slot)return false;
        context->End(slot->end.Get());context->End(slot->disjoint.Get());slot->timing=true;
        bool ok=publish && !failed && draws;
        slot->serial=++ring->nextSerial;slot->time=GetTickCount64();
        HRESULT hr=slot->keyed->ReleaseSync(ok?1:0);
        if(hr!=S_OK){ring->validSerial=0;slot->state=4;slot=nullptr;return false;}
        if(ok){ring->validSerial=slot->serial;slot->state=2;}else{ring->validSerial=0;slot->state=0;}
        slot=nullptr;return ok;
    }
    ~Producer(){if(slot)Finish(false);}
};
// Consumer keeps a private last image so a slot can return to the game immediately.
struct Consumer
{
    std::array<ComPtr<ID3D11Texture2D>,2> textures;
    std::array<ComPtr<IDXGIKeyedMutex>,2> mutexes;
    ComPtr<ID3D11Texture2D> image;
    uint64_t serial=0;ULONGLONG time=0;
    uint64_t previousSerial=0;ULONGLONG previousTime=0;
    std::shared_ptr<const Snapshot> snapshot,previousSnapshot;
    bool Init(ID3D11Device* d,const Ring& r)
    {
        for(size_t i=0;i<textures.size();++i)
            if(FAILED(d->OpenSharedResource(r.slots[i].handle,IID_PPV_ARGS(&textures[i]))) || FAILED(textures[i].As(&mutexes[i])))return false;
        D3D11_TEXTURE2D_DESC td{};textures[0]->GetDesc(&td);td.MiscFlags=0;
        return SUCCEEDED(d->CreateTexture2D(&td,nullptr,&image));
    }
    bool Update(ID3D11DeviceContext* c,Ring& r)
    {
        if(!r.validSerial.load()){serial=0;time=0;previousSerial=0;snapshot.reset();previousSnapshot.reset();}
        for(size_t i=0;i<textures.size();++i)
        {
            auto& s=r.slots[i];int expected=2;if(!s.state.compare_exchange_strong(expected,3))continue;
            if(mutexes[i]->AcquireSync(1,0)!=S_OK){s.state=2;continue;}
            if(s.serial==r.validSerial.load() && GetTickCount64()-s.time<=100)
            {
                previousSerial=serial;previousTime=time;previousSnapshot=std::move(snapshot);
                c->CopyResource(image.Get(),textures[i].Get());serial=s.serial;time=s.time;snapshot=s.snapshot;
            }
            if(mutexes[i]->ReleaseSync(0)!=S_OK){s.state=4;return false;}
            s.state=0;
        }
        return serial && r.validSerial.load()!=0 && GetTickCount64()-time<=100;
    }
};
}
