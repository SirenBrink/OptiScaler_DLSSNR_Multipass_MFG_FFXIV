#pragma once
#include "FfxivNameplatePackets.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <algorithm>
#include <map>

// Diagnostic reconstruction only. No engine commands are replayed, and native
// nameplates are never suppressed. Shaders/resources come from live API bindings;
// no extracted game assets are shipped. All rendering uses a deferred context.
namespace FfxivNameplateRenderer
{
using Microsoft::WRL::ComPtr;
inline constexpr GUID vertexCodeTag {0x2ddb1677,0x8cc9,0x4116,{0xb8,0x01,0x34,0x95,0x80,0x11,0x27,0x19}};
inline thread_local bool executing=false;
inline UINT Crc(const void* ptr,size_t size)
{
    UINT crc=~0u; auto bytes=static_cast<const unsigned char*>(ptr);
    for(size_t i=0;i<size;++i) { crc^=bytes[i]; for(int b=0;b<8;++b) crc=(crc>>1)^(0xedb88320u& (0u-(crc&1))); }
    return ~crc;
}
inline int Variant(UINT crc)
{
    switch(crc) { case 3865947726u:return 0; case 1823062889u:return 4;
        case 3759127293u:return 5; case 2966694105u:return 6; default:return -1; }
}
inline constexpr GUID layoutTag {0xb8b5a74c,0x6319,0x46fe,{0xbb,0xf3,0x1d,0xad,0x36,0x50,0xc4,0x84}};
struct Element { char semantic[32]{}; UINT index=0; DXGI_FORMAT format=DXGI_FORMAT_UNKNOWN; UINT offset=0; };
struct Layout { UINT count=0; std::array<Element,16> elements{}; };
inline UINT FormatBytes(DXGI_FORMAT f)
{
    switch(f)
    {
    case DXGI_FORMAT_R32G32B32A32_FLOAT:return 16;
    case DXGI_FORMAT_R32G32B32_FLOAT:return 12;
    case DXGI_FORMAT_R32G32_FLOAT:case DXGI_FORMAT_R16G16B16A16_SINT:case DXGI_FORMAT_R16G16B16A16_UINT:return 8;
    case DXGI_FORMAT_R32_FLOAT:case DXGI_FORMAT_R16G16_SINT:case DXGI_FORMAT_R16G16_UINT:
    case DXGI_FORMAT_R8G8B8A8_UNORM:case DXGI_FORMAT_R8G8B8A8_SINT:case DXGI_FORMAT_R8G8B8A8_UINT:return 4;
    default:return 0;
    }
}
inline void TagLayout(ID3D11InputLayout* object,const D3D11_INPUT_ELEMENT_DESC* elements,UINT count,const void* code,SIZE_T size)
{
    if(!object || !elements || !count || count>16 || !code || !size || size>256*1024) return;
    const auto crc=Crc(code,size);
    if(crc!=2964264238u && crc!=1445359818u && crc!=3153679471u) return;
    Layout layout; layout.count=count;
    std::array<UINT,32> ends{};
    for(UINT i=0;i<count;++i)
    {
        const auto& in=elements[i]; auto& out=layout.elements[i]; UINT bytes=FormatBytes(in.Format);
        if(!in.SemanticName || strlen(in.SemanticName)>=sizeof(out.semantic) || in.InputSlot>=ends.size() ||
           in.InputSlotClass!=D3D11_INPUT_PER_VERTEX_DATA || in.InstanceDataStepRate || !bytes) return;
        strcpy_s(out.semantic,in.SemanticName); out.index=in.SemanticIndex; out.format=in.Format;
        out.offset=in.AlignedByteOffset==D3D11_APPEND_ALIGNED_ELEMENT?ends[in.InputSlot]:in.AlignedByteOffset;
        if(out.offset>128 || out.offset+bytes>128) return;
        ends[in.InputSlot]=out.offset+bytes;
    }
    object->SetPrivateData(layoutTag,sizeof(layout),&layout);
    object->SetPrivateData(vertexCodeTag,static_cast<UINT>(size),code);
}
struct Material
{
    ComPtr<ID3D11VertexShader> vs; ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11InputLayout> input;
    ComPtr<ID3D11ShaderResourceView> texture;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11BlendState> blend;
    ComPtr<ID3D11RasterizerState> raster;
    std::array<ComPtr<ID3D11Buffer>,3> vertexConstants;
    std::array<ComPtr<ID3D11Buffer>,2> pixelConstants;
    D3D11_VIEWPORT viewport{};
    std::vector<D3D11_RECT> scissors;
    Layout layout{}; std::vector<unsigned char> vertexCode;
    FLOAT blendFactor[4]{}; UINT sampleMask=~0u,width=0,height=0; size_t textureBytes=0;
};
inline bool CloneConstant(ID3D11Device* d,ID3D11DeviceContext* c,ID3D11Buffer* source,ComPtr<ID3D11Buffer>& target)
{
    if(!source) return false;
    D3D11_BUFFER_DESC desc{}; source->GetDesc(&desc);
    if(!desc.ByteWidth || desc.ByteWidth>65536) return false;
    desc.Usage=D3D11_USAGE_DEFAULT; desc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    desc.CPUAccessFlags=desc.MiscFlags=desc.StructureByteStride=0;
    if(FAILED(d->CreateBuffer(&desc,nullptr,&target))) return false;
    c->CopyResource(target.Get(),source); return true;
}
inline bool CaptureMaterial(ID3D11DeviceContext* c,ID3D11ShaderResourceView* view,int variant,Material& m,size_t budget,std::string& reason)
{
    reason="Depth/stencil-dependent draw cannot be moved to a desktop layer";
    ComPtr<ID3D11DepthStencilState> depth; UINT stencilRef=0;
    c->OMGetDepthStencilState(&depth,&stencilRef);
    ComPtr<ID3D11DepthStencilView> depthView;c->OMGetRenderTargets(0,nullptr,&depthView);
    if(depthView)
    {
        if(!depth)return false; // Default state with an actual depth target enables testing.
        D3D11_DEPTH_STENCIL_DESC depthDesc{};depth->GetDesc(&depthDesc);
        if(depthDesc.DepthEnable || depthDesc.StencilEnable)return false;
    }
    reason="Unsupported shader variant";
    if(variant!=0 && variant!=4 && variant!=5 && variant!=6) return false;
    ComPtr<ID3D11Device> d; c->GetDevice(&d);
    c->VSGetShader(&m.vs,nullptr,nullptr); c->PSGetShader(&m.ps,nullptr,nullptr);
    if(!m.vs || !m.ps) return false;
    reason="Native input layout was not tagged (created before hooks, or unsupported input format)";
    ComPtr<ID3D11InputLayout> nativeLayout; c->IAGetInputLayout(&nativeLayout);
    if(!nativeLayout) return false;
    Layout layout; UINT size=sizeof(layout);
    if(FAILED(nativeLayout->GetPrivateData(layoutTag,&size,&layout)) || size!=sizeof(layout) || !layout.count || layout.count>16) return false;
    size=0;
    if(FAILED(nativeLayout->GetPrivateData(vertexCodeTag,&size,nullptr)) || !size || size>256*1024) return false;
    std::vector<unsigned char> code(size);
    if(FAILED(nativeLayout->GetPrivateData(vertexCodeTag,&size,code.data()))) return false;
    reason="Vertex shader signature does not match pixel shader variant";
    const auto crc=Crc(code.data(),code.size());
    if(crc!=(variant==0?2964264238u:variant==4?1445359818u:3153679471u)) return false;
    reason="Native input layout exceeds packet stride";
    std::array<D3D11_INPUT_ELEMENT_DESC,16> input{};
    UINT stride=variant==0?40:variant==4?24:32;
    for(UINT i=0;i<layout.count;++i)
    {
        const auto& e=layout.elements[i];
        if(!FormatBytes(e.format) || e.offset+FormatBytes(e.format)>stride) return false;
        input[i]={e.semantic,e.index,e.format,0,e.offset,D3D11_INPUT_PER_VERTEX_DATA,0};
    }
    if(FAILED(d->CreateInputLayout(input.data(),layout.count,code.data(),code.size(),&m.input))) return false;
    m.layout=layout; m.vertexCode=code;
    reason="Unsupported texture view or texture allocation failed";
    D3D11_SHADER_RESOURCE_VIEW_DESC sv{}; view->GetDesc(&sv);
    if(sv.ViewDimension!=D3D11_SRV_DIMENSION_TEXTURE2D || sv.Texture2D.MostDetailedMip!=0) return false;
    ComPtr<ID3D11Resource> resource; view->GetResource(&resource);
    ComPtr<ID3D11Texture2D> source; if(FAILED(resource.As(&source))) return false;
    D3D11_TEXTURE2D_DESC td{}; source->GetDesc(&td);
    if(td.ArraySize!=1 || td.SampleDesc.Count!=1 || !td.Width || !td.Height || td.Width>4096 || td.Height>4096) return false;
    if(td.Format!=DXGI_FORMAT_B8G8R8A8_UNORM && td.Format!=DXGI_FORMAT_B8G8R8A8_UNORM_SRGB && td.Format!=DXGI_FORMAT_B8G8R8A8_TYPELESS &&
       td.Format!=DXGI_FORMAT_R8G8B8A8_UNORM && td.Format!=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB && td.Format!=DXGI_FORMAT_R8G8B8A8_TYPELESS) return false;
    m.textureBytes=size_t(td.Width)*td.Height*8; // Conservative full-mip-chain budget for supported 32-bit formats.
    if(m.textureBytes>budget) return false;
    // Capture all mips: the original SRV/sampler may select them.
    td.Usage=D3D11_USAGE_DEFAULT; td.BindFlags=D3D11_BIND_SHADER_RESOURCE; td.CPUAccessFlags=td.MiscFlags=0;
    ComPtr<ID3D11Texture2D> owned;
    if(FAILED(d->CreateTexture2D(&td,nullptr,&owned)) || FAILED(d->CreateShaderResourceView(owned.Get(),&sv,&m.texture))) return false;
    c->CopyResource(owned.Get(),source.Get());
    reason="Required constant buffer is missing or could not be copied";
    ID3D11Buffer* cb[3]{}; c->VSGetConstantBuffers(0,3,cb);
    bool ok=true;
    for(UINT i=0;i<3;++i) { if(i==0 || i==(variant==0?1u:2u)) ok &= CloneConstant(d.Get(),c,cb[i],m.vertexConstants[i]); if(cb[i]) cb[i]->Release(); }
    if(variant==0)
    {
        c->PSGetConstantBuffers(0,2,cb);
        for(UINT i=0;i<2;++i) { ok &= CloneConstant(d.Get(),c,cb[i],m.pixelConstants[i]); if(cb[i]) cb[i]->Release(); }
    }
    if(!ok) return false;
    reason="Unsupported viewport, target or sampler";
    c->PSGetSamplers(0,1,&m.sampler); c->OMGetBlendState(&m.blend,m.blendFactor,&m.sampleMask);
    c->RSGetState(&m.raster);
    UINT n=1; c->RSGetViewports(&n,&m.viewport); if(n!=1) return false;
    n=0; c->RSGetScissorRects(&n,nullptr); if(n>16) return false;
    m.scissors.resize(n); if(n) c->RSGetScissorRects(&n,m.scissors.data());
    ComPtr<ID3D11RenderTargetView> rtv; c->OMGetRenderTargets(1,&rtv,nullptr);
    ComPtr<ID3D11Texture2D> target;
    if(!rtv) return false;
    rtv->GetResource(&resource); if(FAILED(resource.As(&target))) return false;
    target->GetDesc(&td); m.width=td.Width; m.height=td.Height;
    if(m.sampler && m.width && m.height && m.width<=8192 && m.height<=8192) reason.clear();
    return ok && m.sampler && m.width && m.height && m.width<=8192 && m.height<=8192;
}
struct Draw { FfxivNameplatePackets::Packet packet; uintptr_t identity=0; };
struct Session
{
    std::vector<Draw> draws;
    std::map<std::pair<uintptr_t,UINT>,Material> materials;
    std::map<std::pair<uintptr_t,UINT>,std::string> failures;
    ComPtr<ID3D11Texture2D> staging;
    ComPtr<ID3D11Query> fence;
    struct BufferCopy { std::string name; UINT bytes=0; ComPtr<ID3D11Buffer> staging; };
    std::vector<BufferCopy> buffers;
    size_t textureBytes=0;
    UINT width=0,height=0;
    bool valid=false,submitted=false,saved=false;
    std::string status="Not requested";
    void Arm(const FfxivNameplatePackets::Capture& capture)
    {
        *this={}; valid=capture.ready && !capture.rejected && !capture.packets.empty() && capture.packets.size()<=512;
        size_t total=0;
        for(const auto& p:capture.packets)
        {
            uintptr_t wrapper=0,identity=0; memcpy(&wrapper,p.header.data()+16,8);
            const UINT stride=p.layout==0?40:p.layout==4?24:32;
            const bool supported=p.layout==0 || p.layout==4 || p.layout==5 || p.layout==6;
            total+=p.vertices.size();
            uint16_t extension=0;memcpy(&extension,p.header.data()+4,2);
            if(!supported || p.error || p.kind!=0x22 || extension || !p.count || p.count%4 || p.count>65536 || p.stride!=stride ||
               p.vertices.size()!=size_t(p.count)*stride || total>8*1024*1024 ||
               !FfxivNameplatePackets::Read(wrapper+0x68,identity) || !identity)
            { valid=false; continue; }
            draws.push_back({p,identity});
        }
        std::stable_sort(draws.begin(),draws.end(),[](const Draw& a,const Draw& b){return a.packet.key<b.packet.key;});
        status=valid?"Waiting for live materials":"Unsupported or incomplete geometry; native drawing unchanged";
    }
    void Observe(ID3D11DeviceContext* c,ID3D11ShaderResourceView* view,uintptr_t identity,UINT crc)
    {
        if(!valid || submitted) return;
        int variant=Variant(crc); if(variant<0) return;
        auto key=std::make_pair(identity,static_cast<UINT>(variant));
        if(materials.contains(key) || materials.size()>=32) return;
        if(std::none_of(draws.begin(),draws.end(),[&](const Draw& draw){return draw.identity==identity && draw.packet.layout==static_cast<UINT>(variant);} )) return;
        Material m; if(CaptureMaterial(c,view,variant,m,128u*1024u*1024u-textureBytes,failures[key]))
        { textureBytes+=m.textureBytes; materials.emplace(key,std::move(m)); failures.erase(key); }
    }
    bool Render(ID3D11DeviceContext* immediate)
    {
        if(!valid || submitted) return false;
        for(const auto& draw:draws) if(!materials.contains({draw.identity,draw.packet.layout})) return false;
        const auto& first=materials.begin()->second; width=first.width; height=first.height;
        if(size_t(width)*height>32*1024*1024) { valid=false; status="Layer exceeds capture budget"; return false; }
        for(const auto& [key,m]:materials) if(m.width!=width || m.height!=height) { valid=false; status="Mismatched target dimensions"; return false; }
        ComPtr<ID3D11Device> d; immediate->GetDevice(&d); ComPtr<ID3D11DeviceContext> c;
        if(FAILED(d->CreateDeferredContext(0,&c))) return false;
        D3D11_TEXTURE2D_DESC td{}; td.Width=width; td.Height=height; td.ArraySize=td.MipLevels=1;
        td.Format=DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count=1; td.BindFlags=D3D11_BIND_RENDER_TARGET;
        ComPtr<ID3D11Texture2D> target; ComPtr<ID3D11RenderTargetView> rtv;
        if(FAILED(d->CreateTexture2D(&td,nullptr,&target)) || FAILED(d->CreateRenderTargetView(target.Get(),nullptr,&rtv))) return false;
        td.BindFlags=0; td.Usage=D3D11_USAGE_STAGING; td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        if(FAILED(d->CreateTexture2D(&td,nullptr,&staging))) return false;
        D3D11_QUERY_DESC q{D3D11_QUERY_EVENT,0}; if(FAILED(d->CreateQuery(&q,&fence))) return false;
        FLOAT clear[4]{}; auto rt=rtv.Get(); c->OMSetRenderTargets(1,&rt,nullptr); c->ClearRenderTargetView(rt,clear);
        D3D11_DEPTH_STENCIL_DESC dd{}; ComPtr<ID3D11DepthStencilState> depth;
        if(FAILED(d->CreateDepthStencilState(&dd,&depth))) return false;
        c->OMSetDepthStencilState(depth.Get(),0); // Isolated layer; world depth is not yet reproduced.
        for(const auto& draw:draws)
        {
            const auto& p=draw.packet; auto& m=materials.at({draw.identity,p.layout});
            std::vector<UINT> indices; indices.reserve(p.count/4*6);
            for(UINT i=0;i<p.count;i+=4) for(UINT off:{0u,1u,2u,0u,2u,3u}) indices.push_back(i+off);
            auto make=[&](const void* data,UINT bytes,UINT bind,ComPtr<ID3D11Buffer>& out)
            { D3D11_BUFFER_DESC b{}; b.ByteWidth=bytes; b.BindFlags=bind; b.Usage=D3D11_USAGE_IMMUTABLE; D3D11_SUBRESOURCE_DATA init{data,0,0}; return d->CreateBuffer(&b,&init,&out); };
            ComPtr<ID3D11Buffer> vb,ib;
            if(FAILED(make(p.vertices.data(),static_cast<UINT>(p.vertices.size()),D3D11_BIND_VERTEX_BUFFER,vb)) ||
               FAILED(make(indices.data(),static_cast<UINT>(indices.size()*4),D3D11_BIND_INDEX_BUFFER,ib))) return false;
            UINT offset=0; auto v=vb.Get(); c->IASetVertexBuffers(0,1,&v,&p.stride,&offset);
            c->IASetIndexBuffer(ib.Get(),DXGI_FORMAT_R32_UINT,0); c->IASetInputLayout(m.input.Get()); c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            c->VSSetShader(m.vs.Get(),nullptr,0); c->PSSetShader(m.ps.Get(),nullptr,0);
            ID3D11Buffer* vcs[3]{m.vertexConstants[0].Get(),m.vertexConstants[1].Get(),m.vertexConstants[2].Get()};
            ID3D11Buffer* pcs[2]{m.pixelConstants[0].Get(),m.pixelConstants[1].Get()};
            c->VSSetConstantBuffers(0,3,vcs); c->PSSetConstantBuffers(0,2,pcs);
            auto srv=m.texture.Get(); auto sampler=m.sampler.Get(); c->PSSetShaderResources(0,1,&srv); c->PSSetSamplers(0,1,&sampler);
            c->OMSetBlendState(m.blend.Get(),m.blendFactor,m.sampleMask); c->RSSetState(m.raster.Get());
            c->RSSetViewports(1,&m.viewport); c->RSSetScissorRects(static_cast<UINT>(m.scissors.size()),m.scissors.data());
            c->DrawIndexed(static_cast<UINT>(indices.size()),0,0);
        }
        buffers.clear();
        UINT materialIndex=0;
        for(const auto& [key,m]:materials)
        {
            auto snapshot=[&](const ComPtr<ID3D11Buffer>& source,const std::string& suffix)
            {
                if(!source) return true;
                D3D11_BUFFER_DESC bd{}; source->GetDesc(&bd); BufferCopy copy;
                copy.name="material-"+std::to_string(materialIndex)+suffix; copy.bytes=bd.ByteWidth;
                bd.Usage=D3D11_USAGE_STAGING; bd.BindFlags=0; bd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
                if(FAILED(d->CreateBuffer(&bd,nullptr,&copy.staging))) return false;
                c->CopyResource(copy.staging.Get(),source.Get()); buffers.push_back(std::move(copy)); return true;
            };
            for(UINT i=0;i<3;++i) if(!snapshot(m.vertexConstants[i],"-vs-cb"+std::to_string(i)+".bin")) return false;
            for(UINT i=0;i<2;++i) if(!snapshot(m.pixelConstants[i],"-ps-cb"+std::to_string(i)+".bin")) return false;
            ++materialIndex;
        }
        c->CopyResource(staging.Get(),target.Get());
        ComPtr<ID3D11CommandList> commands;
        if(FAILED(c->FinishCommandList(FALSE,&commands))) return false;
        struct Executing { Executing(){executing=true;} ~Executing(){executing=false;} } executingGuard;
        immediate->ExecuteCommandList(commands.Get(),TRUE); // Restore caller pipeline, including UAVs and shaders.
        immediate->End(fence.Get()); submitted=true; status="Reconstructed layer awaiting readback"; return true;
    }
    void Save(ID3D11DeviceContext* c,const std::filesystem::path& path)
    {
        if(!submitted || saved || path.empty() || c->GetData(fence.Get(),nullptr,0,D3D11_ASYNC_GETDATA_DONOTFLUSH)!=S_OK) return;
        for(const auto& b:buffers)
        {
            D3D11_MAPPED_SUBRESOURCE data{};
            if(FAILED(c->Map(b.staging.Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&data))) return;
            struct UnmapBuffer { ID3D11DeviceContext* c; ID3D11Buffer* b; ~UnmapBuffer(){c->Unmap(b,0);} } cleanup{c,b.staging.Get()};
            std::ofstream file(path/b.name,std::ios::binary); file.write(static_cast<const char*>(data.pData),b.bytes);
            file.close(); if(!file.good()) { status="Constant-buffer export failed"; return; }
        }
        std::ofstream report(path/"reconstruction.txt");
        report<<"Diagnostic reconstruction; native nameplates unchanged. Depth not reproduced. Material states sampled from shared resources.\n";
        report<<"width "<<width<<" height "<<height<<" records "<<draws.size()<<" materials "<<materials.size()<<'\n';
        UINT materialIndex=0;
        for(const auto& [key,material]:materials)
        {
            report<<"material "<<materialIndex<<" resource "<<key.first<<" variant "<<key.second<<" viewport "<<material.viewport.TopLeftX<<' '<<material.viewport.TopLeftY<<' '<<material.viewport.Width<<' '<<material.viewport.Height<<' '<<material.viewport.MinDepth<<' '<<material.viewport.MaxDepth<<'\n';
            for(const auto& rect:material.scissors) report<<"scissor "<<rect.left<<' '<<rect.top<<' '<<rect.right<<' '<<rect.bottom<<'\n';
            for(UINT i=0;i<material.layout.count;++i) { const auto& e=material.layout.elements[i]; report<<"input "<<e.semantic<<e.index<<" format "<<e.format<<" offset "<<e.offset<<'\n'; }
            std::ofstream shader(path/("material-"+std::to_string(materialIndex)+"-vs.dxbc"),std::ios::binary);
            shader.write(reinterpret_cast<const char*>(material.vertexCode.data()),material.vertexCode.size());
            ++materialIndex;
        }
        D3D11_MAPPED_SUBRESOURCE m{};
        if(FAILED(c->Map(staging.Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&m))) return;
        // RAII also unmaps on path/stream allocation failure.
        struct Unmap { ID3D11DeviceContext* c; ID3D11Texture2D* t; ~Unmap(){c->Unmap(t,0);} } unmap{c,staging.Get()};
        BITMAPFILEHEADER file{}; BITMAPV4HEADER h{};
        file.bfType=0x4d42; file.bfOffBits=sizeof(file)+sizeof(h); file.bfSize=file.bfOffBits+width*height*4;
        h.bV4Size=sizeof(h); h.bV4Width=width; h.bV4Height=-static_cast<LONG>(height); h.bV4Planes=1; h.bV4BitCount=32;
        h.bV4V4Compression=BI_BITFIELDS; h.bV4RedMask=0xff0000; h.bV4GreenMask=0xff00; h.bV4BlueMask=0xff; h.bV4AlphaMask=0xff000000;
        h.bV4CSType=LCS_sRGB;
        std::ofstream out(path/"reconstructed-nameplates.bmp",std::ios::binary);
        out.write(reinterpret_cast<const char*>(&file),sizeof(file)); out.write(reinterpret_cast<const char*>(&h),sizeof(h));
        std::vector<unsigned char> row(width*4);
        if(m.RowPitch<width*4) { status="Invalid layer row pitch"; return; }
        for(UINT y=0;y<height;++y)
        {
            memcpy(row.data(),static_cast<const unsigned char*>(m.pData)+y*m.RowPitch,row.size());
            for(UINT x=0;x<width;++x) { UINT a=row[x*4+3]; for(UINT k=0;k<3;++k) row[x*4+k]=a?static_cast<unsigned char>((std::min)(255u,UINT(row[x*4+k])*255/a)):0; }
            out.write(reinterpret_cast<const char*>(row.data()),row.size());
        }
        out.close(); saved=out.good(); status=saved?"Complete text/icon reconstruction saved (native drawing unchanged)":"Layer save failed";
    }
};
}
