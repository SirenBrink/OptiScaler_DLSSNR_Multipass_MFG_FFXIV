#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <array>
#include <mutex>
#include <atomic>
#include <cmath>
#include <algorithm>
#include <limits>
#include "FfxivCharaViewTrace.h"

// Exact captured opaque preview variants write previous-UV minus current-UV
// (+ native jitter compensation) to SV_Target3.xy. The native BGRA8 MRT clamps
// negative values. Replay only that output into FP16; keep all native pixels
// and depth/stencil writes in the one original draw.
namespace FfxivPreviewMotion {
using Microsoft::WRL::ComPtr;
inline constexpr GUID tag{0x6ae4dd56,0x8db2,0x4e11,{0xb2,0x63,0x71,0x1a,0x8c,0x27,0x9e,0x44}};
inline thread_local bool replaying=false;
inline std::atomic<UINT64> draws{0},clears{0};
inline std::atomic<UINT> activeQueries{0};
inline bool BlocksReplay(D3D11_QUERY kind) {
    // Timing queries should measure the real cost of our work. Replaying under
    // an occlusion/statistics query would change the game's visibility/counts.
    return kind!=D3D11_QUERY_EVENT && kind!=D3D11_QUERY_TIMESTAMP && kind!=D3D11_QUERY_TIMESTAMP_DISJOINT;
}
inline bool KnownMotion(UINT crc) {
    switch(crc) {
    case 3425462897u:case 1739391434u:case 3534667946u:case 90336753u:
    case 1200362900u:case 2790768613u:case 1745001980u:return true;
    default:return false;
    }
}
inline void Tag(ID3D11PixelShader* shader,UINT crc) {
    if(shader && KnownMotion(crc)){UINT yes=1;shader->SetPrivateData(tag,sizeof(yes),&yes);}
}
struct State {
    std::mutex mutex;ComPtr<ID3D11Device> device;
    ComPtr<ID3D11Texture2D> texture;ComPtr<ID3D11RenderTargetView> target;
    ComPtr<ID3D11BlendState> blend;
    ComPtr<ID3D11Texture2D> readback;
    UINT64 readAt=0;UINT reports=0;bool pending=false;
    struct Depth {ComPtr<ID3D11DepthStencilState> native,readOnly;};
    std::array<Depth,16> depth{};UINT depthCount=0,width=0,height=0;
    bool failed=false;
};
inline State& Data(){static auto* value=new State;return *value;}
inline bool Ensure(State& s,ID3D11DeviceContext* c,UINT width,UINT height) {
    if(s.failed || !width || !height || width>4096 || height>4096)return false;
    ComPtr<ID3D11Device> device;c->GetDevice(&device);
    if(s.device && s.device!=device)return false;
    s.device=device;
    if(!s.blend) {
        D3D11_BLEND_DESC desc{};desc.IndependentBlendEnable=true;
        desc.RenderTarget[3].RenderTargetWriteMask=15;
        if(FAILED(device->CreateBlendState(&desc,&s.blend))){s.failed=true;return false;}
    }
    if(!s.texture || s.width!=width || s.height!=height) {
        D3D11_TEXTURE2D_DESC desc{};desc.Width=width;desc.Height=height;
        desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;
        desc.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> texture;ComPtr<ID3D11RenderTargetView> target;
        if(FAILED(device->CreateTexture2D(&desc,nullptr,&texture)) ||
           FAILED(device->CreateRenderTargetView(texture.Get(),nullptr,&target)))return false;
        s.readback.Reset();s.pending=false;
        s.texture=texture;s.target=target;s.width=width;s.height=height;
        const FLOAT zero[4]{};c->ClearRenderTargetView(s.target.Get(),zero);
    }
    return true;
}
inline float Half(unsigned short bits) {
    const int exponent=(bits>>10)&31,mantissa=bits&1023;
    const float value=exponent==31?(mantissa?std::numeric_limits<float>::quiet_NaN():std::numeric_limits<float>::infinity()):
        exponent?std::ldexp(1.0f+mantissa/1024.0f,exponent-15):std::ldexp(float(mantissa),-24);
    return bits&0x8000?-value:value;
}
// Bounded asynchronous diagnostic: sample the previous geometry buffer before
// clearing it. Never wait for the GPU, and never feed these CPU values to DLSS.
inline void Report(State& s,ID3D11DeviceContext* c) {
    if(s.reports>=8 || !s.texture)return;
    if(s.pending) {
        D3D11_MAPPED_SUBRESOURCE map{};
        if(c->Map(s.readback.Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&map)!=S_OK)return;
        float loX=0,hiX=0,loY=0,hiY=0;UINT valid=0,nonzero=0,invalid=0;
        for(UINT y=0;y<s.height;++y) {
            auto row=reinterpret_cast<const unsigned short*>(static_cast<const unsigned char*>(map.pData)+y*map.RowPitch);
            for(UINT x=0;x<s.width;++x) {
                const float mx=Half(row[x*4]),my=Half(row[x*4+1]);
                if(!std::isfinite(mx) || !std::isfinite(my)){++invalid;continue;}
                ++valid;if(mx!=0 || my!=0)++nonzero;
                loX=(std::min)(loX,mx);hiX=(std::max)(hiX,mx);loY=(std::min)(loY,my);hiY=(std::max)(hiY,my);
            }
        }
        c->Unmap(s.readback.Get(),0);s.pending=false;++s.reports;
        LOG_INFO("CharaView signed motion sample {}: x [{}, {}], y [{}, {}], nonzero {}/{}, invalid {} (shared geometry; not a DLSS history)",s.reports,loX,hiX,loY,hiY,nonzero,valid,invalid);
    }
    const auto now=GetTickCount64();
    if(s.reports>=8 || now-s.readAt<5000 || !draws.load())return;
    if(!s.readback) {
        D3D11_TEXTURE2D_DESC desc{};s.texture->GetDesc(&desc);
        desc.BindFlags=0;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        if(FAILED(s.device->CreateTexture2D(&desc,nullptr,&s.readback)))return;
    }
    c->CopyResource(s.readback.Get(),s.texture.Get());s.pending=true;s.readAt=now;
}
inline void Clear(ID3D11DeviceContext* c,ID3D11RenderTargetView* native,const FfxivCharaViewTrace::Snapshot& snapshot=FfxivCharaViewTrace::Current()) {
    if(replaying || c->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE || !native)return;
    ComPtr<ID3D11Resource> resource;ComPtr<ID3D11Texture2D> texture;
    native->GetResource(&resource);if(FAILED(resource.As(&texture)))return;
    D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);
    if(FfxivCharaViewTrace::Match(snapshot,texture.Get(),desc.Width,desc.Height)!=8)return;
    auto& s=Data();std::lock_guard lock(s.mutex);
    if(!Ensure(s,c,desc.Width,desc.Height))return;
    Report(s,c);
    struct Guard {Guard(){replaying=true;}~Guard(){replaying=false;}} guard;
    const FLOAT zero[4]{};c->ClearRenderTargetView(s.target.Get(),zero);
    if(++clears==1)LOG_INFO("CharaView signed motion: native geometry clear observed, {}x{}",desc.Width,desc.Height);
}
template<class F> inline void Around(ID3D11DeviceContext* c,F&& nativeDraw,const FfxivCharaViewTrace::Snapshot& snapshot=FfxivCharaViewTrace::Current()) {
    if(replaying || activeQueries.load() || c->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE){nativeDraw();return;}
    ComPtr<ID3D11PixelShader> shader;c->PSGetShader(&shader,nullptr,nullptr);
    UINT yes=0,size=sizeof(yes);
    if(!shader){nativeDraw();return;}
    const bool exact=SUCCEEDED(shader->GetPrivateData(tag,&size,&yes)) && yes;
    ID3D11RenderTargetView* raw[8]{};ComPtr<ID3D11DepthStencilView> dsv;
    c->OMGetRenderTargets(8,raw,&dsv);
    std::array<ComPtr<ID3D11RenderTargetView>,8> targets;for(UINT i=0;i<8;++i)targets[i].Attach(raw[i]);
    if(!targets[0] || !targets[3] || !dsv){nativeDraw();return;}
    ComPtr<ID3D11Resource> resource;ComPtr<ID3D11Texture2D> texture;
    targets[0]->GetResource(&resource);if(FAILED(resource.As(&texture))){nativeDraw();return;}
    D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);
    if(desc.SampleDesc.Count!=1 || FfxivCharaViewTrace::Match(snapshot,texture.Get(),desc.Width,desc.Height)!=8){nativeDraw();return;}
    if(!exact) {
        // Early-created native shaders can predate bytecode tagging. Recover
        // only the complete captured opaque MRT/camera contract; transparent
        // geometry has three MRTs and must never be interpreted as motion.
        const auto& current=snapshot;bool contract=true;
        for(UINT i=0;i<5;++i){
            if(!targets[i]){contract=false;break;}
            ComPtr<ID3D11Resource> r;ComPtr<ID3D11Texture2D> t;targets[i]->GetResource(&r);
            if(FAILED(r.As(&t)) || FfxivCharaViewTrace::Match(current,t.Get(),desc.Width,desc.Height)!=int(8+i)){contract=false;break;}
        }
        ComPtr<ID3D11Buffer> vsCamera,psCamera;c->VSGetConstantBuffers(0,1,&vsCamera);c->PSGetConstantBuffers(2,1,&psCamera);
        D3D11_BUFFER_DESC v{},p{};if(vsCamera)vsCamera->GetDesc(&v);if(psCamera)psCamera->GetDesc(&p);
        if(!contract || v.ByteWidth!=944 || p.ByteWidth!=944){nativeDraw();return;}
        UINT recovered=1;shader->SetPrivateData(tag,sizeof(recovered),&recovered);
    }
    // UAV counters/stream output would have side effects on a replay.
    ID3D11UnorderedAccessView* uav[8]{};c->OMGetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,0,8,uav);
    ID3D11Buffer* so[4]{};c->SOGetTargets(4,so);bool sideEffects=false;
    for(auto p:uav)if(p){sideEffects=true;p->Release();}
    for(auto p:so)if(p){sideEffects=true;p->Release();}
    if(sideEffects){nativeDraw();return;}
    ComPtr<ID3D11DepthStencilState> depth;UINT ref=0;c->OMGetDepthStencilState(&depth,&ref);
    ComPtr<ID3D11BlendState> blend;FLOAT factors[4]{};UINT mask=0;c->OMGetBlendState(&blend,factors,&mask);
    {
        auto& s=Data();std::unique_lock lock(s.mutex);
        if(!Ensure(s,c,desc.Width,desc.Height)){lock.unlock();nativeDraw();return;}
        ComPtr<ID3D11DepthStencilState> readOnly;
        for(UINT i=0;i<s.depthCount;++i)if(s.depth[i].native==depth){readOnly=s.depth[i].readOnly;break;}
        if(!readOnly) {
            if(s.depthCount==s.depth.size()){lock.unlock();nativeDraw();return;}
            D3D11_DEPTH_STENCIL_DESC ds{};
            if(depth)depth->GetDesc(&ds);
            else {ds.DepthEnable=true;ds.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;ds.DepthFunc=D3D11_COMPARISON_LESS;
                ds.StencilReadMask=ds.StencilWriteMask=255;
                ds.FrontFace=ds.BackFace={D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_KEEP,D3D11_COMPARISON_ALWAYS};}
            ds.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ZERO;ds.StencilWriteMask=0;
            if(FAILED(s.device->CreateDepthStencilState(&ds,&readOnly))){lock.unlock();nativeDraw();return;}
            s.depth[s.depthCount++]={depth,readOnly};
        }
        ID3D11RenderTargetView* capture[4]{nullptr,nullptr,nullptr,s.target.Get()};
        c->OMSetRenderTargets(4,capture,dsv.Get());c->OMSetBlendState(s.blend.Get(),nullptr,mask);
        c->OMSetDepthStencilState(readOnly.Get(),ref);
        {struct Guard {Guard(){replaying=true;}~Guard(){replaying=false;}} guard;nativeDraw();}
        c->OMSetRenderTargets(8,raw,dsv.Get());c->OMSetBlendState(blend.Get(),factors,mask);
        c->OMSetDepthStencilState(depth.Get(),ref);
        if(++draws==1 || draws.load()%600==0)LOG_INFO("CharaView signed motion: {} replayed geometry draws, native targets and depth restored",draws.load());
    }
    nativeDraw();
}
inline void Shutdown(){auto& s=Data();std::lock_guard lock(s.mutex);s.target.Reset();s.texture.Reset();s.readback.Reset();s.blend.Reset();s.device.Reset();for(auto& d:s.depth)d={};s.depthCount=s.width=s.height=0;s.failed=s.pending=false;s.readAt=0;s.reports=0;}
}
