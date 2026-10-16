#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <array>
#include <mutex>
#include <algorithm>
#include "FfxivCharaViewTrace.h"

// Discovery only. Native draws, pixels and resource ownership remain unchanged.
namespace FfxivPreviewTrace {
using Microsoft::WRL::ComPtr;
inline constexpr GUID producerTag{0xbb3c42a6,0x18f3,0x4b6c,{0x90,0x5b,0xa3,0x18,0x35,0x28,0x17,0x2e}};
inline constexpr GUID shaderTag{0xd2b564c4,0x7708,0x48f0,{0xa7,0xb5,0x55,0x22,0x19,0x42,0x31,0xf0}};
struct Input {
    UINT width=0,height=0,format=0,binds=0,samples=0,shader=0;
    UINT depthWidth=0,depthHeight=0,depthFormat=0;
    int charaIndex=-1;
};
struct DrawStage {
    UINT shader=0,depthWidth=0,depthHeight=0,depthFormat=0;
    UINT64 time=0;
};
struct Producer {
    UINT shader=0, depthWidth=0, depthHeight=0, depthFormat=0;
    UINT mip=0, draws=0, inheritedDepth=0, deferred=0;
    UINT64 time=0;
    int charaIndex=-1;
    D3D11_VIEWPORT viewport{};
    std::array<Input,32> inputs{};
    // Keep a bounded record of earlier writes to the same texture. Finishing
    // passes often remove depth; do not misreport that as a depth-free scene.
    std::array<DrawStage,4> stages{};
};
inline void ReportProducer(const D3D11_TEXTURE2D_DESC& d,const Producer& p);
inline void Shader(ID3D11PixelShader* shader, UINT crc) {
    if(shader)shader->SetPrivateData(shaderTag,sizeof(crc),&crc);
}
inline void Observe(ID3D11DeviceContext* c) {
    ComPtr<ID3D11RenderTargetView> target;ComPtr<ID3D11DepthStencilView> depth;
    c->OMGetRenderTargets(1,&target,&depth);
    if(!target)return;
    ComPtr<ID3D11Resource> resource;ComPtr<ID3D11Texture2D> colour,depthTexture;
    target->GetResource(&resource);if(FAILED(resource.As(&colour)))return;
    D3D11_TEXTURE2D_DESC cd{};colour->GetDesc(&cd);
    if(!(cd.BindFlags&D3D11_BIND_SHADER_RESOURCE))return;
    D3D11_TEXTURE2D_DESC dd{};
    if(depth){depth->GetResource(&resource);if(SUCCEEDED(resource.As(&depthTexture)))depthTexture->GetDesc(&dd);}
    ComPtr<ID3D11PixelShader> shader;c->PSGetShader(&shader,nullptr,nullptr);
    Producer p{};UINT bytes=sizeof(p);colour->GetPrivateData(producerTag,&bytes,&p);
    p.charaIndex=FfxivCharaViewTrace::Match(FfxivCharaViewTrace::Current(),colour.Get(),cd.Width,cd.Height);
    bytes=sizeof(p.shader);p.shader=0;if(shader)shader->GetPrivateData(shaderTag,&bytes,&p.shader);
    D3D11_RENDER_TARGET_VIEW_DESC view{};target->GetDesc(&view);
    p.mip=view.ViewDimension==D3D11_RTV_DIMENSION_TEXTURE2D?view.Texture2D.MipSlice:0;
    D3D11_DEPTH_STENCIL_VIEW_DESC depthView{};if(depth)depth->GetDesc(&depthView);
    p.depthWidth=dd.Width;p.depthHeight=dd.Height;p.depthFormat=depthView.Format;
    p.inheritedDepth=0;p.deferred=c->GetType()==D3D11_DEVICE_CONTEXT_DEFERRED;
    const DrawStage stage{p.shader,dd.Width,dd.Height,UINT(depthView.Format),GetTickCount64()};
    if(p.stages[0].shader!=stage.shader || p.stages[0].depthWidth!=stage.depthWidth ||
       p.stages[0].depthHeight!=stage.depthHeight || p.stages[0].depthFormat!=stage.depthFormat) {
        for(size_t i=p.stages.size()-1;i>0;--i)p.stages[i]=p.stages[i-1];
    }
    p.stages[0]=stage;
    {
        // Follow same-extent finishing inputs. This is lineage evidence, not a
        // claim that their depth is valid for a future NR evaluation.
        const UINT slots=(p.charaIndex>=0 || p.shader==2503081625u || p.shader==242988824u)?32:8;
        std::array<ComPtr<ID3D11ShaderResourceView>,32> inputs;ID3D11ShaderResourceView* raw[32]{};
        c->PSGetShaderResources(0,slots,raw);for(UINT i=0;i<slots;++i)inputs[i].Attach(raw[i]);
        p.inputs={};
        for(UINT slot=0;slot<inputs.size();++slot)if(auto& input=inputs[slot]) {
            input->GetResource(&resource);ComPtr<ID3D11Texture2D> t;if(FAILED(resource.As(&t)))continue;
            D3D11_TEXTURE2D_DESC d{};t->GetDesc(&d);
            D3D11_SHADER_RESOURCE_VIEW_DESC inputView{};input->GetDesc(&inputView);
            auto& edge=p.inputs[slot];edge.width=d.Width;edge.height=d.Height;edge.format=inputView.Format;edge.binds=d.BindFlags;edge.samples=d.SampleDesc.Count;
            edge.charaIndex=FfxivCharaViewTrace::Match(FfxivCharaViewTrace::Current(),t.Get(),d.Width,d.Height);
            Producer parent{};bytes=sizeof(parent);
            if(t!=colour && SUCCEEDED(t->GetPrivateData(producerTag,&bytes,&parent)) && GetTickCount64()-parent.time<1000) {
                edge.shader=parent.shader;edge.depthWidth=parent.depthWidth;edge.depthHeight=parent.depthHeight;edge.depthFormat=parent.depthFormat;
                if(!depthTexture && !p.inheritedDepth && d.Width==cd.Width && d.Height==cd.Height && parent.depthWidth) {
                    p.depthWidth=parent.depthWidth;p.depthHeight=parent.depthHeight;p.depthFormat=parent.depthFormat;p.inheritedDepth=1;
                }
            }
        }
    }
    UINT count=1;c->RSGetViewports(&count,&p.viewport);
    p.time=GetTickCount64();++p.draws;
    colour->SetPrivateData(producerTag,sizeof(p),&p);
    if(p.charaIndex>=0)ReportProducer(cd,p);
}
inline void Copy(ID3D11Resource* dest,ID3D11Resource* source,bool full) {
    if(!full || !dest || !source)return;
    Producer p{};UINT bytes=sizeof(p);
    if(SUCCEEDED(source->GetPrivateData(producerTag,&bytes,&p)))dest->SetPrivateData(producerTag,sizeof(p),&p);
    else dest->SetPrivateData(producerTag,0,nullptr);
}
struct Key {
    UINT shader,slot,width,height,format,producer,depthWidth,depthHeight,depthFormat,inheritedDepth,deferred,inputSignature;
    bool operator==(const Key&)const=default;
};
inline void ReportProducer(const D3D11_TEXTURE2D_DESC& d,const Producer& p) {
    UINT signature=2166136261u;
    signature=(signature^UINT(p.charaIndex))*16777619u;
    for(const auto& edge:p.inputs)for(UINT field:{edge.width,edge.height,edge.format,edge.binds,edge.samples,edge.shader,edge.depthWidth,edge.depthHeight,edge.depthFormat})signature=(signature^field)*16777619u;
    for(const auto& stage:p.stages)for(UINT field:{stage.shader,stage.depthWidth,stage.depthHeight,stage.depthFormat})signature=(signature^field)*16777619u;
    const Key key{p.shader,0,d.Width,d.Height,UINT(d.Format),p.shader,p.depthWidth,p.depthHeight,p.depthFormat,p.inheritedDepth,p.deferred,signature};
    static std::mutex mutex;static std::array<Key,256> seen{};static size_t count=0;
    std::lock_guard lock(mutex);
    if(count==seen.size() || std::find(seen.begin(),seen.begin()+count,key)!=seen.begin()+count)return;
    seen[count++]=key;
    if(p.charaIndex>=0)LOG_INFO("CharaView verified producer: table entry {} ({}) shader {} target {}x{}",p.charaIndex,FfxivCharaViewTrace::Role(p.charaIndex),p.shader,d.Width,d.Height);
    LOG_INFO("Preview NR producer: shader {} target {}x{} format {} samples {}, depth {}x{} format {}, deferred {}",
        p.shader,d.Width,d.Height,UINT(d.Format),d.SampleDesc.Count,p.depthWidth,p.depthHeight,p.depthFormat,p.deferred);
    for(UINT slot=0;slot<p.inputs.size();++slot)if(const auto& edge=p.inputs[slot];edge.width)
        LOG_INFO("Preview NR producer input: shader {} t{} {}x{} format {} binds {} samples {}, parent {}, depth {}x{} format {}",
            p.shader,slot,edge.width,edge.height,edge.format,edge.binds,edge.samples,edge.shader,edge.depthWidth,edge.depthHeight,edge.depthFormat);
    for(const auto& stage:p.stages)if(stage.time && GetTickCount64()-stage.time<1000)
        LOG_INFO("Preview NR prior draw: target {}x{} shader {}, native depth {}x{} format {}, age {} ms",
            d.Width,d.Height,stage.shader,stage.depthWidth,stage.depthHeight,stage.depthFormat,GetTickCount64()-stage.time);
}
inline void Report(ID3D11DeviceContext* c,bool sceneOnly=false) {
    ComPtr<ID3D11PixelShader> shader;c->PSGetShader(&shader,nullptr,nullptr);
    UINT crc=0,bytes=sizeof(crc);
    if(!shader || FAILED(shader->GetPrivateData(shaderTag,&bytes,&crc)))return;
    // Separate catalogs prevent ordinary UI sources from exhausting the scene
    // discovery budget before the equipment window is opened.
    static std::mutex mutex;static std::array<std::array<Key,128>,2> catalogs{};
    static std::array<size_t,2> counts{};
    auto& seen=catalogs[sceneOnly?1:0];auto& count=counts[sceneOnly?1:0];
    std::lock_guard lock(mutex);if(count==seen.size())return;
    std::array<ComPtr<ID3D11ShaderResourceView>,8> inputs;ID3D11ShaderResourceView* raw[8]{};
    c->PSGetShaderResources(0,8,raw);for(UINT i=0;i<8;++i)inputs[i].Attach(raw[i]);
    for(UINT i=0;i<8 && count<seen.size();++i)if(inputs[i]) {
        ComPtr<ID3D11Resource> resource;ComPtr<ID3D11Texture2D> texture;
        inputs[i]->GetResource(&resource);if(FAILED(resource.As(&texture)))continue;
        D3D11_TEXTURE2D_DESC d{};texture->GetDesc(&d);
        if(!(d.BindFlags&D3D11_BIND_RENDER_TARGET))continue; // Exclude UI atlases.
        Producer p{};bytes=sizeof(p);const bool produced=SUCCEEDED(texture->GetPrivateData(producerTag,&bytes,&p));
        const int chara=FfxivCharaViewTrace::Match(FfxivCharaViewTrace::Current(),texture.Get(),d.Width,d.Height);
        if(sceneOnly && chara<0)continue;
        D3D11_SHADER_RESOURCE_VIEW_DESC view{};inputs[i]->GetDesc(&view);
        UINT signature=2166136261u;
        signature=(signature^UINT(chara))*16777619u;
        for(const auto& edge:p.inputs)for(UINT field:{edge.width,edge.height,edge.format,edge.binds,edge.samples,edge.shader,edge.depthWidth,edge.depthHeight,edge.depthFormat})signature=(signature^field)*16777619u;
        Key key{crc,i,d.Width,d.Height,UINT(view.Format),p.shader,p.depthWidth,p.depthHeight,p.depthFormat,p.inheritedDepth,p.deferred,signature};
        if(std::find(seen.begin(),seen.begin()+count,key)!=seen.begin()+count)continue;
        seen[count++]=key;
        if(chara>=0)LOG_INFO("CharaView verified consumer: table entry {} ({}) shader {} t{} source {}x{}, producer {}",chara,FfxivCharaViewTrace::Role(chara),crc,i,d.Width,d.Height,p.shader);
        LOG_INFO("Preview NR trace: UI shader {} t{} colour {}x{} format {} binds {} samples {}, producer {} mip {} draws {}, depth {}x{} format {}, viewport {},{} {}x{}, age {} ms (metadata {}, inherited depth {}, deferred {})",
            crc,i,d.Width,d.Height,UINT(view.Format),d.BindFlags,d.SampleDesc.Count,p.shader,p.mip,p.draws,
            p.depthWidth,p.depthHeight,p.depthFormat,p.viewport.TopLeftX,p.viewport.TopLeftY,p.viewport.Width,p.viewport.Height,
            produced?GetTickCount64()-p.time:0,produced,p.inheritedDepth,p.deferred);
        for(UINT slot=0;slot<p.inputs.size();++slot)if(const auto& edge=p.inputs[slot];edge.width)
            LOG_INFO("Preview NR input: UI {} source t{} producer {} reads t{} {}x{} format {} binds {} samples {}, parent shader {}, parent depth {}x{} format {}",
                crc,i,p.shader,slot,edge.width,edge.height,edge.format,edge.binds,edge.samples,edge.shader,edge.depthWidth,edge.depthHeight,edge.depthFormat);
    }
}
}
