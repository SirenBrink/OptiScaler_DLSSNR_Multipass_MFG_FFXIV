#pragma once
#include "FfxivNameplatePackets.h"
#include "FfxivNameplateRenderer.h"
#include "FfxivNameplateLive.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <sstream>
#include <set>

// Match opaque engine texture identities against live, API-owned SRVs. Never
// dereference a captured COM pointer. Readback is bounded and asynchronously polled.
namespace FfxivNameplateGpu
{
using Microsoft::WRL::ComPtr;
inline constexpr GUID shaderCodeTag {0x5c5fdded,0x4e79,0x49ea,{0x80,0xf1,0x12,0x89,0x99,0x45,0xe1,0x05}};
struct Texture
{
    uintptr_t identity=0;
    ComPtr<ID3D11Texture2D> staging;
    ComPtr<ID3D11Query> fence;
    D3D11_TEXTURE2D_DESC desc{};
    bool attempted=false, saved=false;
};
inline std::mutex mutex;
inline std::atomic<bool> armed {false};
inline ULONGLONG deadline=0;
inline std::vector<Texture> textures;
inline std::set<std::pair<uintptr_t,uintptr_t>> seen;
inline std::vector<std::vector<unsigned char>> shaders;
inline std::filesystem::path directory;
inline std::ostringstream states;
inline size_t queuedBytes=0;
inline FfxivNameplateRenderer::Session reconstruction;
inline std::map<uintptr_t,UINT> shaderCrcs;
struct Blob { std::string name; std::vector<unsigned char> data; };
inline std::vector<Blob> blobs;
// One small, asynchronous projection snapshot per rejected material. This records
// the evidence needed to distinguish meaningful depth testing from an inert HUD
// state. It does not disable the visibility guard or read back the scene depth.
struct DepthProbe
{
    std::pair<uintptr_t,UINT> key;
    ComPtr<ID3D11Buffer> projection;
    ComPtr<ID3D11Query> fence;
    UINT bytes=0;
    bool saved=false;
};
inline std::vector<DepthProbe> depthProbes;
inline void ProbeDepth(ID3D11DeviceContext* c, uintptr_t identity, UINT variant)
{
    const auto key=std::make_pair(identity,variant);
    if(depthProbes.size()>=32 || std::any_of(depthProbes.begin(),depthProbes.end(),[&](const auto& p){return p.key==key;}))return;
    DepthProbe probe;probe.key=key;
    const auto prefix="depth-probe-"+std::to_string(depthProbes.size());
    ComPtr<ID3D11DepthStencilView> view;c->OMGetRenderTargets(0,nullptr,&view);
    D3D11_DEPTH_STENCIL_VIEW_DESC vd{};D3D11_TEXTURE2D_DESC td{};
    if(view)
    {
        view->GetDesc(&vd);ComPtr<ID3D11Resource> resource;view->GetResource(&resource);
        ComPtr<ID3D11Texture2D> texture;if(SUCCEEDED(resource.As(&texture)))texture->GetDesc(&td);
    }
    D3D11_VIEWPORT viewport{};UINT count=1;c->RSGetViewports(&count,&viewport);
    states<<prefix<<" resource "<<identity<<" variant "<<variant<<" dsv "<<(view!=nullptr)
          <<" format "<<vd.Format<<" flags "<<vd.Flags<<" width "<<td.Width<<" height "<<td.Height
          <<" samples "<<td.SampleDesc.Count<<" viewport "<<viewport.TopLeftX<<' '<<viewport.TopLeftY<<' '
          <<viewport.Width<<' '<<viewport.Height<<' '<<viewport.MinDepth<<' '<<viewport.MaxDepth<<'\n';
    ComPtr<ID3D11InputLayout> layout;c->IAGetInputLayout(&layout);UINT size=0;
    if(layout && SUCCEEDED(layout->GetPrivateData(FfxivNameplateRenderer::vertexCodeTag,&size,nullptr)) && size && size<=256*1024)
    {
        Blob shader;shader.name=prefix+"-vs.dxbc";shader.data.resize(size);
        if(SUCCEEDED(layout->GetPrivateData(FfxivNameplateRenderer::vertexCodeTag,&size,shader.data.data())))blobs.push_back(std::move(shader));
    }
    // CPU-owned packets are bounded by Session::Arm. Save geometry only once per
    // matching key; no borrowed engine pointer is retained or dereferenced here.
    UINT packetIndex=0;
    for(const auto& draw:reconstruction.draws)if(draw.identity==identity && draw.packet.layout==variant)
    {
        const auto& p=draw.packet;const auto name=prefix+"-packet-"+std::to_string(packetIndex++);
        blobs.push_back({name+".vertices.bin",p.vertices});
        states<<name<<" stride "<<p.stride<<" vertices "<<p.count<<'\n';
    }
    ComPtr<ID3D11Device> device;c->GetDevice(&device);
    ComPtr<ID3D11Buffer> source;c->VSGetConstantBuffers(0,1,&source);
    if(source)
    {
        D3D11_BUFFER_DESC bd{};source->GetDesc(&bd);probe.bytes=bd.ByteWidth;
        if(bd.ByteWidth>=64 && bd.ByteWidth<=65536)
        {
            bd.Usage=D3D11_USAGE_STAGING;bd.BindFlags=bd.MiscFlags=bd.StructureByteStride=0;bd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
            D3D11_QUERY_DESC q{D3D11_QUERY_EVENT,0};
            if(SUCCEEDED(device->CreateBuffer(&bd,nullptr,&probe.projection)) && SUCCEEDED(device->CreateQuery(&q,&probe.fence)))
            {c->CopyResource(probe.projection.Get(),source.Get());c->End(probe.fence.Get());}
        }
    }
    depthProbes.push_back(std::move(probe));
}
template<class T> inline void Store(const std::string& name,const T& data)
{
    Blob b; b.name=name; b.data.resize(sizeof(data)); std::memcpy(b.data.data(),&data,sizeof(data)); blobs.push_back(std::move(b));
}
inline bool Layout(DXGI_FORMAT f,UINT w,UINT h,UINT& row,UINT& rows);
inline void Arm(const FfxivNameplatePackets::Capture& capture)
{
    std::lock_guard lock(mutex);
    textures.clear(); seen.clear(); shaders.clear(); blobs.clear(); directory.clear(); states.str(""); states.clear(); queuedBytes=0;
    reconstruction.Arm(capture); shaderCrcs.clear(); depthProbes.clear();
    for(const auto& packet:capture.packets)
    {
        uintptr_t wrapper=0,identity=0;
        std::memcpy(&wrapper,packet.header.data()+0x10,8);
        // Offset is only read as an identity hint. Actual resource must be found
        // via PSGetShaderResources/GetResource on the immediate context.
        if(!FfxivNameplatePackets::Read(wrapper+0x68,identity) || !identity) continue;
        bool found=false; for(const auto& t:textures) found |= t.identity==identity;
        if(!found && textures.size()<8) { Texture t; t.identity=identity; textures.push_back(std::move(t)); }
        states<<"packet_texture "<<std::hex<<wrapper<<" resource "<<identity<<std::dec<<'\n';
    }
    deadline=GetTickCount64()+5000; armed.store(!textures.empty());
    LOG_INFO("FFXIV nameplate GPU: armed {} texture identities",textures.size());
}
inline void SetDirectory(const std::filesystem::path& path)
{
    std::lock_guard lock(mutex); directory=path;
}
inline void Observe(ID3D11DeviceContext* c)
{
    if(!armed.load(std::memory_order_relaxed)) return;
    std::lock_guard lock(mutex);
    if(!armed.load() || GetTickCount64()>=deadline) return;
    ID3D11ShaderResourceView* raw[16]{}; c->PSGetShaderResources(0,16,raw);
    std::array<ComPtr<ID3D11ShaderResourceView>,16> views;
    for(UINT slot=0;slot<16;++slot) views[slot].Attach(raw[slot]);
    for(UINT slot=0;slot<16;++slot)
    {
        if(!views[slot]) continue;
        ComPtr<ID3D11Resource> resource; views[slot]->GetResource(&resource);
        for(UINT ti=0;ti<textures.size();++ti)
        {
            auto& t=textures[ti];
            if(reinterpret_cast<uintptr_t>(resource.Get())!=t.identity) continue;
            ComPtr<ID3D11Device> device; c->GetDevice(&device);
            if(!t.attempted)
            {
                t.attempted=true;
                ComPtr<ID3D11Texture2D> source;
                if(SUCCEEDED(resource.As(&source)))
                {
                    source->GetDesc(&t.desc);
                    UINT rowBytes=0,rowCount=0;
                    const bool supported=Layout(t.desc.Format,t.desc.Width,t.desc.Height,rowBytes,rowCount);
                    const auto bytes=static_cast<size_t>(rowBytes)*rowCount*t.desc.ArraySize;
                    if(supported && t.desc.SampleDesc.Count==1 && t.desc.ArraySize<=16 && bytes<=128u*1024u*1024u && queuedBytes+bytes<=128u*1024u*1024u)
                    {
                        auto d=t.desc; d.MipLevels=1; d.BindFlags=0; d.MiscFlags=0;
                        d.Usage=D3D11_USAGE_STAGING; d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
                        D3D11_QUERY_DESC q{D3D11_QUERY_EVENT,0};
                        if(SUCCEEDED(device->CreateTexture2D(&d,nullptr,&t.staging)) && SUCCEEDED(device->CreateQuery(&q,&t.fence)))
                        {
                            for(UINT slice=0;slice<d.ArraySize;++slice)
                                c->CopySubresourceRegion(t.staging.Get(),slice,0,0,0,source.Get(),D3D11CalcSubresource(0,slice,t.desc.MipLevels),nullptr);
                            c->End(t.fence.Get()); queuedBytes+=bytes;
                        }
                    }
                }
                states<<"texture "<<ti<<" width "<<t.desc.Width<<" height "<<t.desc.Height<<" format "<<t.desc.Format<<" array "<<t.desc.ArraySize<<" queued "<<(t.fence!=nullptr)<<'\n';
            }
            ComPtr<ID3D11PixelShader> ps; c->PSGetShader(&ps,nullptr,nullptr);
            if(slot==0 && ps && reconstruction.valid && !reconstruction.submitted)
            {
                const auto psId=reinterpret_cast<uintptr_t>(ps.Get());
                auto found=shaderCrcs.find(psId);
                if(found==shaderCrcs.end() && shaderCrcs.size()<64)
                {
                    UINT codeSize=0,crc=0;
                    if(SUCCEEDED(ps->GetPrivateData(shaderCodeTag,&codeSize,nullptr)) && codeSize && codeSize<=256*1024)
                    {
                        std::vector<unsigned char> code(codeSize);
                        if(SUCCEEDED(ps->GetPrivateData(shaderCodeTag,&codeSize,code.data()))) crc=FfxivNameplateRenderer::Crc(code.data(),code.size());
                    }
                    found=shaderCrcs.emplace(psId,crc).first;
                }
                if(found!=shaderCrcs.end())
                {
                    reconstruction.Observe(c,views[slot].Get(),t.identity,found->second);
                    const int variant=FfxivNameplateRenderer::Variant(found->second);
                    if(variant>=0)
                    {
                        const auto failure=reconstruction.failures.find({t.identity,static_cast<UINT>(variant)});
                        if(failure!=reconstruction.failures.end() && failure->second.starts_with("Depth/stencil"))
                            ProbeDepth(c,t.identity,static_cast<UINT>(variant));
                    }
                }
            }
            if(seen.size()>=32 || !seen.emplace(t.identity,reinterpret_cast<uintptr_t>(ps.Get())).second) continue;
            const auto index=shaders.size(); shaders.emplace_back();
            UINT size=0;
            if(ps && SUCCEEDED(ps->GetPrivateData(shaderCodeTag,&size,nullptr)) && size && size<=256*1024)
            { shaders.back().resize(size); ps->GetPrivateData(shaderCodeTag,&size,shaders.back().data()); }
            D3D11_SHADER_RESOURCE_VIEW_DESC view{}; views[slot]->GetDesc(&view);
            ComPtr<ID3D11BlendState> blend; FLOAT factor[4]{}; UINT mask=0;
            c->OMGetBlendState(&blend,factor,&mask); D3D11_BLEND_DESC bd{}; if(blend) blend->GetDesc(&bd);
            ComPtr<ID3D11DepthStencilState> depth; UINT ref=0; c->OMGetDepthStencilState(&depth,&ref);
            D3D11_DEPTH_STENCIL_DESC dd{}; if(depth) depth->GetDesc(&dd);
            ComPtr<ID3D11RasterizerState> raster; c->RSGetState(&raster); D3D11_RASTERIZER_DESC rd{}; if(raster) raster->GetDesc(&rd);
            ID3D11Buffer* vb=nullptr; UINT stride=0,offset=0; c->IAGetVertexBuffers(0,1,&vb,&stride,&offset); if(vb) vb->Release();
            const auto prefix="state-"+std::to_string(index);
            Store(prefix+"-view.bin",view); Store(prefix+"-blend.bin",bd); Store(prefix+"-blend-factor.bin",factor);
            Store(prefix+"-depth.bin",dd); Store(prefix+"-raster.bin",rd);
            auto& b=bd.RenderTarget[0];
            states<<"state "<<index<<" texture "<<ti<<" slot "<<slot<<" viewFormat "<<view.Format<<" viewDimension "<<view.ViewDimension
                  <<" stride "<<stride<<" psBytes "<<shaders.back().size()<<" blendDefault "<<(!blend)<<" blend "<<b.BlendEnable
                  <<" src "<<b.SrcBlend<<" dst "<<b.DestBlend<<" op "<<b.BlendOp<<" srcAlpha "<<b.SrcBlendAlpha<<" dstAlpha "<<b.DestBlendAlpha
                  <<" alphaOp "<<b.BlendOpAlpha<<" writeMask "<<UINT(b.RenderTargetWriteMask)<<" sampleMask "<<mask
                  <<" depthDefault "<<(!depth)<<" depthEnable "<<dd.DepthEnable<<" depthWrite "<<dd.DepthWriteMask<<" depthFunc "<<dd.DepthFunc
                  <<" stencil "<<dd.StencilEnable<<" stencilRef "<<ref<<" rasterDefault "<<(!raster)<<" cull "<<rd.CullMode<<" scissor "<<rd.ScissorEnable<<'\n';
            ID3D11SamplerState* samplers[16]{}; c->PSGetSamplers(0,16,samplers);
            for(UINT s=0;s<16;++s) if(samplers[s])
            {
                D3D11_SAMPLER_DESC sd{}; samplers[s]->GetDesc(&sd); samplers[s]->Release();
                Store(prefix+"-sampler-"+std::to_string(s)+".bin",sd);
                states<<"sampler "<<index<<' '<<s<<" filter "<<sd.Filter<<" address "<<sd.AddressU<<' '<<sd.AddressV<<' '<<sd.AddressW<<" lod "<<sd.MinLOD<<' '<<sd.MaxLOD<<'\n';
            }
        }
    }
}
inline bool Layout(DXGI_FORMAT f,UINT w,UINT h,UINT& row,UINT& rows)
{
    rows=h; UINT bpp=0;
    switch(f)
    {
    case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_A8_UNORM:bpp=1;break;
    case DXGI_FORMAT_R8G8_UNORM:case DXGI_FORMAT_R16_UNORM:bpp=2;break;
    case DXGI_FORMAT_R8G8B8A8_UNORM:case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:case DXGI_FORMAT_B8G8R8A8_TYPELESS:bpp=4;break;
    case DXGI_FORMAT_BC1_UNORM:case DXGI_FORMAT_BC1_UNORM_SRGB:row=((w+3)/4)*8;rows=(h+3)/4;return true;
    case DXGI_FORMAT_BC2_UNORM:case DXGI_FORMAT_BC3_UNORM:case DXGI_FORMAT_BC3_UNORM_SRGB:case DXGI_FORMAT_BC7_UNORM:case DXGI_FORMAT_BC7_UNORM_SRGB:
        row=((w+3)/4)*16;rows=(h+3)/4;return true;
    default:return false;
    }
    row=w*bpp;return true;
}
inline void Tick(ID3D11DeviceContext* c)
{
    if(!armed.load()) return;
    std::lock_guard lock(mutex);
    if(directory.empty()) return;
    reconstruction.Render(c);
    reconstruction.Save(c,directory);
    for(UINT i=0;i<depthProbes.size();++i)
    {
        auto& probe=depthProbes[i];
        if(probe.saved || !probe.fence || c->GetData(probe.fence.Get(),nullptr,0,D3D11_ASYNC_GETDATA_DONOTFLUSH)!=S_OK)continue;
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if(FAILED(c->Map(probe.projection.Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&mapped)))continue;
        { // Always unmap, including allocation failures.
            struct Unmap { ID3D11DeviceContext* c;ID3D11Buffer* b;~Unmap(){c->Unmap(b,0);} } cleanup{c,probe.projection.Get()};
            Blob blob;blob.name="depth-probe-"+std::to_string(i)+"-projection.bin";blob.data.resize(probe.bytes);
            memcpy(blob.data.data(),mapped.pData,probe.bytes);blobs.push_back(std::move(blob));probe.saved=true;
        }
    }
    for(UINT i=0;i<textures.size();++i)
    {
        auto& t=textures[i]; if(t.saved || !t.fence) continue;
        if(c->GetData(t.fence.Get(),nullptr,0,D3D11_ASYNC_GETDATA_DONOTFLUSH)!=S_OK) continue;
        UINT row=0,rows=0;
        if(!Layout(t.desc.Format,t.desc.Width,t.desc.Height,row,rows)) { t.saved=true; states<<"unsupported texture format "<<i<<'\n'; continue; }
        bool all=true;
        for(UINT slice=0;slice<t.desc.ArraySize;++slice)
        {
            D3D11_MAPPED_SUBRESOURCE m{};
            if(FAILED(c->Map(t.staging.Get(),slice,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&m))) { all=false; break; }
            std::ofstream file(directory/("texture-"+std::to_string(i)+"-slice-"+std::to_string(slice)+".bin"),std::ios::binary);
            if(row<=m.RowPitch) for(UINT y=0;y<rows;++y) file.write(static_cast<const char*>(m.pData)+y*m.RowPitch,row);
            else all=false;
            all &= file.good(); c->Unmap(t.staging.Get(),slice);
        }
        if(all) { t.saved=true; states<<"texture_saved "<<i<<" rowBytes "<<row<<" rows "<<rows<<'\n'; }
    }
    if(GetTickCount64()<deadline) return;
    for(UINT i=0;i<shaders.size();++i) if(!shaders[i].empty())
    { std::ofstream file(directory/("pixel-shader-"+std::to_string(i)+".dxbc"),std::ios::binary); file.write(reinterpret_cast<const char*>(shaders[i].data()),shaders[i].size()); }
    for(const auto& b:blobs) { std::ofstream file(directory/b.name,std::ios::binary); file.write(reinterpret_cast<const char*>(b.data.data()),b.data.size()); }
    UINT saved=0;for(const auto& t:textures) saved+=t.saved && t.fence!=nullptr;
    states<<"reconstruction "<<reconstruction.status<<" materials "<<reconstruction.materials.size()<<" records "<<reconstruction.draws.size()<<" submitted "<<reconstruction.submitted<<" saved "<<reconstruction.saved<<'\n';
    for(const auto& [key,reason]:reconstruction.failures)
        states<<"reconstruction_missing resource "<<key.first<<" variant "<<key.second<<" reason "<<reason<<'\n';
    std::ofstream report(directory/"gpu-state.txt"); report<<states.str();
    LOG_INFO("FFXIV nameplate reconstruction: {} ({} materials, {} records)",reconstruction.status,reconstruction.materials.size(),reconstruction.draws.size());
    LOG_INFO("FFXIV nameplate GPU: completed, {} of {} textures processed, {} material observations",saved,textures.size(),seen.size());
    if(FfxivNameplateLive::preparing.load())
    {
        std::vector<FfxivNameplateLive::TextureSource> sources;
        for(UINT i=0;i<textures.size();++i) if(textures[i].saved)
            sources.push_back({textures[i].identity,textures[i].desc,directory/("texture-"+std::to_string(i)+"-slice-0.bin")});
        FfxivNameplateLive::Launch(c,reconstruction,sources,directory,shaderCodeTag);
    }
    armed.store(false); textures.clear(); shaders.clear(); blobs.clear(); depthProbes.clear(); reconstruction={};
}
inline void Stop() { armed.store(false); std::lock_guard lock(mutex); textures.clear(); shaders.clear(); blobs.clear(); depthProbes.clear(); reconstruction={}; }
}
