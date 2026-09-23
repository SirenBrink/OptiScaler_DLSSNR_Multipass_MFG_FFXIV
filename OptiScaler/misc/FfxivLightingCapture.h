#pragma once
#include <d3d11_1.h>
#include <wrl/client.h>
#include <detours/detours.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>
#include "FfxivLightingScan.h"

// Shared native scan and opt-in diagnostic hooks. Signatures identify the game's
// shaders, extracted read-only from SqPack on 2026-09-23. No engine RVA is hooked.
// Verified tone response controls history rejection, never an exposure multiply.
namespace FfxivLightingCapture
{
using Microsoft::WRL::ComPtr;
struct Shader { const char* name; UINT bytes, crc; std::array<unsigned char,16> checksum; };
inline constexpr std::array<Shader,9> shaders {{
    {"ToneMapping", 1512u, 2246539247u, {0x47, 0x3c, 0x73, 0x0f, 0x5e, 0xe5, 0xea, 0xfb, 0x3e, 0x98, 0x65, 0xb7, 0xb3, 0x4a, 0x61, 0x71}},
    {"ToneAdjust", 1292u, 4142406171u, {0x99, 0xaa, 0x14, 0xeb, 0x1f, 0x12, 0xd7, 0xe1, 0x6f, 0xa5, 0xd6, 0x48, 0xe3, 0x28, 0xd2, 0xf7}},
    {"MeasureLumInitial", 1504u, 3549584390u, {0x99, 0xfb, 0xff, 0x98, 0xca, 0xc9, 0xac, 0x5c, 0x53, 0xe0, 0xe4, 0x90, 0x66, 0x1e, 0x6b, 0x2e}},
    {"MeasureLumIterative", 996u, 3365552696u, {0x57, 0xc0, 0x2e, 0x53, 0xea, 0x9c, 0xb8, 0x4d, 0xdb, 0x25, 0xa7, 0x88, 0x62, 0x93, 0x04, 0x8f}},
    {"MeasureLumFinal", 1120u, 3429323786u, {0xf7, 0xc4, 0x0e, 0x83, 0x23, 0x87, 0xd5, 0xe9, 0xb4, 0x76, 0x87, 0x24, 0x4f, 0x07, 0x9b, 0xc3}},
    {"AdaptLum", 1376u, 3046182588u, {0x7b, 0x59, 0x78, 0x4f, 0xed, 0xd5, 0xee, 0x80, 0x96, 0xf3, 0xce, 0xbc, 0x80, 0x2f, 0x1d, 0xb1}},
    {"BrightPassFilter", 1424u, 2198444491u, {0x19, 0xbd, 0x6f, 0x0e, 0x5d, 0xf4, 0xbb, 0x62, 0xd7, 0x83, 0x76, 0xe5, 0xf0, 0xa9, 0x17, 0x34}},
    {"BrightPassFilterUpdate", 1444u, 3541952783u, {0x54, 0x34, 0xf1, 0x09, 0x15, 0x76, 0xae, 0x70, 0x3a, 0x12, 0x64, 0x31, 0x03, 0x05, 0x7c, 0x47}},
    {"ToneMapLut", 1392u, 2137381618u, {0xf0,0x8f,0xb8,0xf9,0x1b,0x65,0xe2,0xe8,0x1a,0xd6,0x7e,0x8c,0x96,0xd4,0x3c,0x58}},
}};
inline constexpr GUID tag {0x032194ea,0x12b6,0x461f,{0x89,0x30,0xa0,0x22,0x93,0xc7,0xbe,0x51}};
inline std::array<std::atomic<UINT>,shaders.size()> created {};
inline std::atomic<bool> installed {false}, sampling {false}, service {false};
inline std::atomic<UINT> request {0};
inline ID3D11DeviceContext* context = nullptr; // Identity only; no idle COM ownership.
inline thread_local bool inside = false;
inline constexpr UINT durationMs = 12000, intervalMs = 500;
inline constexpr size_t maxBytes = 64u * 1024u * 1024u, maxPending = 32;
inline UINT Crc(const void* input, size_t size)
{
    UINT crc = ~0u;
    for (auto p = static_cast<const unsigned char*>(input); size--; ++p)
    {
        crc ^= *p;
        for (int i=0; i<8; ++i) crc = (crc>>1) ^ (0xedb88320u & (0u-(crc&1u)));
    }
    return ~crc;
}
inline UINT Identify(const void* input, size_t size)
{
    if (!input || size < 32 || std::memcmp(input,"DXBC",4)) return 0;
    for (UINT i=0; i<shaders.size(); ++i)
        if (size == shaders[i].bytes && !std::memcmp(static_cast<const char*>(input)+4,shaders[i].checksum.data(),16)
            && Crc(input,size) == shaders[i].crc) return i+1;
    return 0;
}
struct Readback
{
    ComPtr<ID3D11Resource> staging;
    std::string file;
    UINT rowBytes=0, rows=1, depth=1;
};
struct Shot
{
    ComPtr<ID3D11Query> fence;
    ComPtr<ID3D11Resource> target;
    UINT mip=0, slice=0, id=0, shader=0, window=0;
    DXGI_FORMAT targetFormat=DXGI_FORMAT_UNKNOWN;
    ULONGLONG queued=0;
    size_t bytes=0, saved=0;
    std::ostringstream meta;
    std::vector<Readback> reads;
};
struct CaptureState
{
    std::mutex mutex;
    std::filesystem::path directory;
    std::deque<std::unique_ptr<Shot>> pending;
    std::array<UINT,shaders.size()> thisFrame {}, captured {};
    bool active=false;
    ULONGLONG start=0, next=0;
    UINT window=0, serial=0, finished=0, dropped=0, sessions=0, commandLists=0;
    UINT upscaleWindow=0, upscaleSamples=0;
    size_t bytes=0;
    std::string status="Ready. Capture a bright view, a dark view, then a lighting transition.";
};
inline CaptureState& State()
{
    // Avoid running COM destructors under the loader lock at process exit.
    // Normal captures drain and release every staging resource on the render thread.
    static auto* value = new CaptureState;
    return *value;
}
inline std::filesystem::path Root()
{
    wchar_t exe[32768] {}; GetModuleFileNameW(nullptr,exe,32768);
    return std::filesystem::path(exe).parent_path()/L"OptiScaler_LightingCaptures";
}
inline void Request(UINT kind)
{
    if (installed && kind>=1 && kind<=3) { request.store(kind); service.store(true); }
}
inline std::string Status()
{
    auto& s=State(); std::lock_guard lock(s.mutex); return s.status;
}
inline bool Busy() { return service.load(); }
inline UINT Loaded()
{
    UINT n=0; for (auto& count:created) if (count.load()) ++n; return n;
}
inline UINT PixelBytes(DXGI_FORMAT f)
{
    switch(f)
    {
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: case DXGI_FORMAT_R32G32B32A32_FLOAT: return 16;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM: case DXGI_FORMAT_R32G32_TYPELESS: case DXGI_FORMAT_R32G32_FLOAT: return 8;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R11G11B10_FLOAT:
    case DXGI_FORMAT_R16G16_TYPELESS: case DXGI_FORMAT_R16G16_FLOAT: case DXGI_FORMAT_R16G16_UNORM:
    case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_R32_FLOAT: return 4;
    case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_R16_FLOAT: case DXGI_FORMAT_R16_UNORM: return 2;
    case DXGI_FORMAT_R8_UNORM: return 1;
    default: return 0;
    }
}
inline bool Budget(Shot& shot, size_t bytes)
{
    if (State().bytes + shot.bytes + bytes <= maxBytes) return true;
    shot.meta << "skip\tbyte-budget\n"; return false;
}
inline void AddRead(Shot& shot, const char* label, ID3D11Resource* staging, UINT row, UINT rows=1, UINT depth=1)
{
    Readback r; r.staging=staging; r.rowBytes=row; r.rows=rows; r.depth=depth;
    r.file=std::to_string(shot.id)+"-"+label+".bin";
    shot.meta << "blob\t" << label << '\t' << r.file << '\t' << row << '\t' << rows << '\t' << depth << '\n';
    shot.bytes+=size_t(row)*rows*depth; shot.reads.push_back(std::move(r));
}
inline void CopyTexture(ID3D11DeviceContext* c, Shot& shot, ID3D11Resource* resource,
                        UINT mip, UINT slice, DXGI_FORMAT viewFormat, const char* label)
{
    if (!resource) { shot.meta << "unbound\t" << label << '\n'; return; }
    ComPtr<ID3D11Device> d; c->GetDevice(&d);
    ComPtr<ID3D11Texture2D> tex; ComPtr<ID3D11Texture3D> volume;
    const bool crop = shot.window==0 || shot.window==12 || shot.window==23;
    shot.meta << "resource\t" << label << '\t' << reinterpret_cast<uintptr_t>(resource)
              << "\tview-format\t" << UINT(viewFormat) << "\tmip\t" << mip << "\tslice\t" << slice << '\n';
    if (SUCCEEDED(resource->QueryInterface(IID_PPV_ARGS(&tex))))
    {
        D3D11_TEXTURE2D_DESC desc {}; tex->GetDesc(&desc);
        shot.meta << "texture2d\t" << label << '\t' << desc.Width << '\t' << desc.Height << '\t'
                  << UINT(desc.Format) << '\t' << desc.MipLevels << '\t' << desc.ArraySize << '\t' << desc.SampleDesc.Count << '\n';
        UINT bpp=PixelBytes(desc.Format);
        if (!bpp || mip>=desc.MipLevels || slice>=desc.ArraySize || desc.SampleDesc.Count!=1 ||
            (desc.BindFlags&D3D11_BIND_DEPTH_STENCIL)) { shot.meta << "skip\t" << label << "\tunsupported-shape-or-depth\n"; return; }
        UINT w=std::max(1u,desc.Width>>mip), h=std::max(1u,desc.Height>>mip);
        bool isSmall=size_t(w)*h<=4096;
        if (!isSmall && !crop) { shot.meta << "skip\t" << label << "\tlarge-image-between-keyframes\n"; return; }
        UINT cw=isSmall?w:std::min(w,256u), ch=isSmall?h:std::min(h,144u), x=(w-cw)/2, y=(h-ch)/2;
        if (!Budget(shot,size_t(cw)*ch*bpp)) return;
        D3D11_TEXTURE2D_DESC stage=desc; stage.Width=cw; stage.Height=ch; stage.ArraySize=1; stage.MipLevels=1;
        stage.Usage=D3D11_USAGE_STAGING; stage.CPUAccessFlags=D3D11_CPU_ACCESS_READ; stage.BindFlags=0; stage.MiscFlags=0;
        ComPtr<ID3D11Texture2D> buffer; auto hr=d->CreateTexture2D(&stage,nullptr,&buffer);
        if (FAILED(hr)) { shot.meta << "error\t" << label << "\tcreate\t" << UINT(hr) << '\n'; return; }
        D3D11_BOX box {x,y,0,x+cw,y+ch,1};
        c->CopySubresourceRegion(buffer.Get(),0,0,0,0,resource,D3D11CalcSubresource(mip,slice,desc.MipLevels),&box);
        shot.meta << "region\t" << label << '\t' << x << '\t' << y << '\t' << cw << '\t' << ch << '\n';
        AddRead(shot,label,buffer.Get(),cw*bpp,ch);
    }
    else if (SUCCEEDED(resource->QueryInterface(IID_PPV_ARGS(&volume))))
    {
        D3D11_TEXTURE3D_DESC desc {}; volume->GetDesc(&desc);
        shot.meta << "texture3d\t" << label << '\t' << desc.Width << '\t' << desc.Height << '\t' << desc.Depth << '\t' << UINT(desc.Format) << '\n';
        if (mip>=desc.MipLevels) return;
        UINT w=std::max(1u,desc.Width>>mip), h=std::max(1u,desc.Height>>mip), z=std::max(1u,desc.Depth>>mip), bpp=PixelBytes(desc.Format);
        if (!bpp || size_t(w)*h*z>4096 || !Budget(shot,size_t(w)*h*z*bpp)) { shot.meta << "skip\t" << label << "\tvolume-size-or-format\n"; return; }
        auto stage=desc; stage.Width=w; stage.Height=h; stage.Depth=z; stage.MipLevels=1;
        stage.Usage=D3D11_USAGE_STAGING; stage.BindFlags=0; stage.MiscFlags=0; stage.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture3D> buffer; auto hr=d->CreateTexture3D(&stage,nullptr,&buffer);
        if (FAILED(hr)) { shot.meta << "error\t" << label << "\tcreate\t" << UINT(hr) << '\n'; return; }
        D3D11_BOX box {0,0,0,w,h,z}; c->CopySubresourceRegion(buffer.Get(),0,0,0,0,resource,mip,&box);
        AddRead(shot,label,buffer.Get(),w*bpp,h,z);
    }
    else shot.meta << "skip\t" << label << "\tresource-type\n";
}
inline void CopyConstants(ID3D11DeviceContext* c, Shot& shot)
{
    ComPtr<ID3D11DeviceContext1> c1; c->QueryInterface(IID_PPV_ARGS(&c1));
    ComPtr<ID3D11Device> d; c->GetDevice(&d);
    // Preserve each complete visible CB range, not a guessed first float.
    for (UINT slot=0; slot<D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT; ++slot)
    {
        ComPtr<ID3D11Buffer> source; UINT first=0,count=4096;
        if (c1) c1->PSGetConstantBuffers1(slot,1,&source,&first,&count);
        else c->PSGetConstantBuffers(slot,1,&source);
        if (!source) continue;
        D3D11_BUFFER_DESC desc {}; source->GetDesc(&desc);
        auto label="cb"+std::to_string(slot);
        shot.meta << "constant\t" << label << '\t' << reinterpret_cast<uintptr_t>(source.Get()) << '\t'
                  << desc.ByteWidth << '\t' << first << '\t' << count << '\n';
        size_t offset=size_t(first)*16, available=size_t(count)*16;
        if (offset>=desc.ByteWidth || !available) continue;
        UINT bytes=static_cast<UINT>(std::min({size_t(desc.ByteWidth)-offset,available,size_t(65536)}));
        if (!Budget(shot,bytes)) continue;
        D3D11_BUFFER_DESC stage {}; stage.ByteWidth=bytes; stage.Usage=D3D11_USAGE_STAGING; stage.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Buffer> buffer; auto hr=d->CreateBuffer(&stage,nullptr,&buffer);
        if (FAILED(hr)) { shot.meta << "error\t" << label << "\tcreate\t" << UINT(hr) << '\n'; continue; }
        D3D11_BOX box {UINT(offset),0,0,UINT(offset)+bytes,1,1};
        c->CopySubresourceRegion(buffer.Get(),0,0,0,0,source.Get(),0,&box);
        AddRead(shot,label.c_str(),buffer.Get(),bytes);
    }
}
inline void CopyInputs(ID3D11DeviceContext* c, Shot& shot)
{
    // All nine verified shaders declare only t0/t1. Unused/stale bindings are
    // recorded too; the shader disassembly determines which are actually read.
    for (UINT slot=0; slot<2; ++slot)
    {
        ComPtr<ID3D11ShaderResourceView> view; c->PSGetShaderResources(slot,1,&view);
        if (!view) { shot.meta << "unbound\tsrv" << slot << '\n'; continue; }
        D3D11_SHADER_RESOURCE_VIEW_DESC vd {}; view->GetDesc(&vd);
        UINT mip=0,slice=0;
        switch(vd.ViewDimension)
        {
        case D3D11_SRV_DIMENSION_TEXTURE2D: mip=vd.Texture2D.MostDetailedMip; break;
        case D3D11_SRV_DIMENSION_TEXTURE2DARRAY: mip=vd.Texture2DArray.MostDetailedMip; slice=vd.Texture2DArray.FirstArraySlice; break;
        case D3D11_SRV_DIMENSION_TEXTURE3D: mip=vd.Texture3D.MostDetailedMip; break;
        default: shot.meta << "skip\tsrv" << slot << "\tview-dimension\t" << UINT(vd.ViewDimension) << '\n'; continue;
        }
        ComPtr<ID3D11Resource> resource; view->GetResource(&resource);
        auto label="srv"+std::to_string(slot);
        CopyTexture(c,shot,resource.Get(),mip,slice,vd.Format,label.c_str());
    }
}
inline std::unique_ptr<Shot> Before(ID3D11DeviceContext* c, const char* drawKind)
{
    auto& s=State();
    if (s.pending.size()>=maxPending || s.bytes>=maxBytes || s.serial>=512) return {};
    ComPtr<ID3D11PixelShader> ps; c->PSGetShader(&ps,nullptr,nullptr);
    UINT id=0,size=sizeof(id);
    if (!ps || FAILED(ps->GetPrivateData(tag,&size,&id)) || !id || id>shaders.size()) return {};
    UINT index=id-1, maxDraws=index==3?4u:1u;
    if (s.thisFrame[index]>=maxDraws) return {};
    ++s.thisFrame[index];
    ComPtr<ID3D11Predicate> predicate; BOOL predicateValue=FALSE; c->GetPredication(&predicate,&predicateValue);
    if (predicate) { ++s.dropped; return {}; } // Copies must not depend on predication.
    auto shot=std::make_unique<Shot>(); shot->shader=index; shot->id=++s.serial;
    shot->window=s.window-1; shot->queued=GetTickCount64();
    ComPtr<ID3D11Device> d; c->GetDevice(&d);
    D3D11_QUERY_DESC q {D3D11_QUERY_EVENT,0};
    if (FAILED(d->CreateQuery(&q,&shot->fence))) { ++s.dropped; return {}; }
    shot->meta << "schema\t1\nshader\t" << shaders[index].name << '\t' << shaders[index].crc
               << "\nwindow\t" << shot->window << "\nms\t" << shot->queued-s.start << "\ndraw\t" << drawKind << '\n';
    D3D11_VIEWPORT vp {}; UINT vpCount=1; c->RSGetViewports(&vpCount,&vp);
    shot->meta << "viewport\t" << vp.TopLeftX << '\t' << vp.TopLeftY << '\t' << vp.Width << '\t' << vp.Height << '\n';
    void* stack[12] {}; USHORT n=CaptureStackBackTrace(1,12,stack,nullptr);
    auto base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    shot->meta << "exe-base\t" << base << "\nstack-addresses";
    for (USHORT i=0; i<n; ++i) shot->meta << '\t' << reinterpret_cast<uintptr_t>(stack[i]);
    shot->meta << '\n';
    CopyConstants(c,*shot); CopyInputs(c,*shot);
    ComPtr<ID3D11RenderTargetView> target; c->OMGetRenderTargets(1,&target,nullptr);
    if (target)
    {
        D3D11_RENDER_TARGET_VIEW_DESC vd {}; target->GetDesc(&vd);
        if (vd.ViewDimension==D3D11_RTV_DIMENSION_TEXTURE2D || vd.ViewDimension==D3D11_RTV_DIMENSION_TEXTURE2DARRAY)
        {
            target->GetResource(&shot->target); shot->targetFormat=vd.Format;
            shot->mip=vd.ViewDimension==D3D11_RTV_DIMENSION_TEXTURE2D?vd.Texture2D.MipSlice:vd.Texture2DArray.MipSlice;
            shot->slice=vd.ViewDimension==D3D11_RTV_DIMENSION_TEXTURE2D?0:vd.Texture2DArray.FirstArraySlice;
            CopyTexture(c,*shot,shot->target.Get(),shot->mip,shot->slice,vd.Format,"output-before");
        }
        else shot->meta << "skip\toutput\tview-dimension\t" << UINT(vd.ViewDimension) << '\n';
    }
    return shot;
}
inline void After(ID3D11DeviceContext* c, std::unique_ptr<Shot> shot)
{
    if (!shot) return;
    if (shot->target) CopyTexture(c,*shot,shot->target.Get(),shot->mip,shot->slice,shot->targetFormat,"output-after");
    c->End(shot->fence.Get()); shot->target.Reset();
    auto& s=State(); ++s.captured[shot->shader]; s.bytes+=shot->bytes;
    s.pending.push_back(std::move(shot));
}
inline void WriteFile(const std::filesystem::path& name, const void* bytes, size_t count)
{
    std::ofstream out; out.exceptions(std::ios::badbit|std::ios::failbit);
    out.open(name,std::ios::binary|std::ios::trunc); out.write(static_cast<const char*>(bytes),count);
}
// Observe the unmodified D3D11 NGX input before the bridge prepares copies or
// substitutes D3D12 resources. This establishes whether NR sees tone-mapped data.
inline void UpscaleInput(ID3D11DeviceContext* c, ID3D11Resource* color, ID3D11Resource* output,
                         ID3D11Resource* exposure, UINT flags, bool haveFlags, UINT width, UINT height,
                         float preExposure, float exposureScale)
{
    if (!sampling.load(std::memory_order_relaxed) || c!=context || inside || !color) return;
    auto& s=State(); std::lock_guard lock(s.mutex);
    if (s.upscaleWindow==s.window || s.pending.size()>=maxPending || s.bytes>=maxBytes || s.serial>=512) return;
    s.upscaleWindow=s.window;
    try
    {
        ComPtr<ID3D11Predicate> predicate; BOOL value=FALSE; c->GetPredication(&predicate,&value);
        if (predicate) { ++s.dropped; return; }
        auto shot=std::make_unique<Shot>(); shot->id=++s.serial; shot->window=s.window-1; shot->queued=GetTickCount64();
        ComPtr<ID3D11Device> d; c->GetDevice(&d); D3D11_QUERY_DESC q {D3D11_QUERY_EVENT,0};
        if (FAILED(d->CreateQuery(&q,&shot->fence))) { ++s.dropped; return; }
        shot->meta << "schema\t2\nshader\tDLSSBoundary\t0\nwindow\t" << shot->window
            << "\nms\t" << shot->queued-s.start << "\ndraw\tNGX-input-before-bridge"
            << "\ncreate-flags\t" << flags << "\tavailable\t" << haveFlags
            << "\nrender-size\t" << width << '\t' << height
            << "\npre-exposure\t" << preExposure << "\nexposure-scale\t" << exposureScale
            << "\nngx-exposure-resource\t" << reinterpret_cast<uintptr_t>(exposure)
            << "\nngx-output-resource\t" << reinterpret_cast<uintptr_t>(output) << '\n';
        ComPtr<ID3D11Texture2D> outputTexture;
        if (output && SUCCEEDED(output->QueryInterface(IID_PPV_ARGS(&outputTexture))))
        {
            D3D11_TEXTURE2D_DESC desc {}; outputTexture->GetDesc(&desc);
            shot->meta << "ngx-output-desc\t" << desc.Width << '\t' << desc.Height << '\t' << UINT(desc.Format) << '\n';
        }
        CopyTexture(c,*shot,color,0,0,DXGI_FORMAT_UNKNOWN,"ngx-color");
        c->End(shot->fence.Get()); s.bytes+=shot->bytes; ++s.upscaleSamples;
        s.pending.push_back(std::move(shot));
    }
    catch (...) { ++s.dropped; }
}
inline void Poll(ID3D11DeviceContext* c)
{
    auto& s=State();
    for (UINT pass=0; pass<4 && !s.pending.empty(); ++pass)
    {
        auto& shot=*s.pending.front(); BOOL done=FALSE;
        auto hr=c->GetData(shot.fence.Get(),&done,sizeof(done),D3D11_ASYNC_GETDATA_DONOTFLUSH);
        bool expired=GetTickCount64()-shot.queued>3000;
        if ((hr==S_FALSE || (hr==S_OK && !done)) && !expired) break;
        if (hr!=S_OK || !done)
        {
            shot.meta << "incomplete\tquery\t" << UINT(hr) << "\ttimeout\t" << expired << '\n'; ++s.dropped;
        }
        else
        {
            while (shot.saved<shot.reads.size())
            {
                auto& read=shot.reads[shot.saved]; D3D11_MAPPED_SUBRESOURCE mapped {};
                hr=c->Map(read.staging.Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&mapped);
                if (hr==DXGI_ERROR_WAS_STILL_DRAWING && !expired) return;
                if (FAILED(hr)) { shot.meta << "incomplete\tmap\t" << read.file << '\t' << UINT(hr) << '\n'; ++s.dropped; ++shot.saved; continue; }
                std::vector<unsigned char> data;
                // Allocate/copy in a scope that always unmaps, including allocation failure.
                try
                {
                    data.resize(size_t(read.rowBytes)*read.rows*read.depth);
                    for (UINT z=0; z<read.depth; ++z) for (UINT y=0; y<read.rows; ++y)
                        std::memcpy(data.data()+(size_t(z)*read.rows+y)*read.rowBytes,
                                    static_cast<const unsigned char*>(mapped.pData)+size_t(z)*mapped.DepthPitch+size_t(y)*mapped.RowPitch,read.rowBytes);
                }
                catch (...) { c->Unmap(read.staging.Get(),0); throw; }
                c->Unmap(read.staging.Get(),0);
                WriteFile(s.directory/read.file,data.data(),data.size()); ++shot.saved;
            }
            shot.meta << "gpu-complete\t1\n"; ++s.finished;
        }
        auto metadata=shot.meta.str(); WriteFile(s.directory/(std::to_string(shot.id)+".tsv"),metadata.data(),metadata.size());
        s.pending.pop_front();
    }
}
inline void Tick(ID3D11DeviceContext* c)
{
    if (!service.load() || c!=context) return;
    auto& s=State(); std::lock_guard lock(s.mutex); sampling.store(false);
    try
    {
        Poll(c);
        const auto now=GetTickCount64(); UINT kind=request.exchange(0);
        if (kind && !s.active && s.pending.empty())
        {
            SYSTEMTIME t {}; GetLocalTime(&t);
            const char* label=kind==1?"bright":kind==2?"dark":"transition";
            char name[128]; sprintf_s(name,"%04u%02u%02u-%02u%02u%02u-%03u-%lu-%u-%s",t.wYear,t.wMonth,t.wDay,
                t.wHour,t.wMinute,t.wSecond,t.wMilliseconds,GetCurrentProcessId(),++s.sessions,label);
            s.directory=Root()/name; std::filesystem::create_directories(s.directory);
            s.start=now; s.next=now; s.window=0; s.serial=s.finished=s.dropped=s.commandLists=0; s.bytes=0;
            s.upscaleWindow=s.upscaleSamples=0;
            s.captured.fill(0); s.active=true;
            std::ostringstream info; info << "Native DX11 lighting diagnostic, schema 1\nlabel=" << label
                << "\nduration_ms=" << durationMs << "\ninterval_ms=" << intervalMs << "\nOnly exact shader signatures are captured. No exposure is applied.\n";
            auto text=info.str(); WriteFile(s.directory/"README.txt",text.data(),text.size());
            LOG_INFO("FFXIV lighting: started {} capture at {}",label,s.directory.string());
        }
        if (s.active)
        {
            if (now-s.start>=durationMs || s.bytes>=maxBytes || s.serial>=512) s.active=false;
            else if (now>=s.next)
            {
                s.next=now+intervalMs; ++s.window; s.thisFrame.fill(0); sampling.store(true);
            }
            s.status="Capturing: "+std::to_string(std::min<ULONGLONG>(12,(now-s.start)/1000))+" / 12 s; "+std::to_string(s.finished)+" draws saved.";
        }
        if (!s.active && s.pending.empty())
        {
            std::ostringstream summary; summary << "saved=" << s.finished << "\ndropped=" << s.dropped
                << "\nbytes_queued=" << s.bytes << "\ncommand_lists_during_sample_frames=" << s.commandLists
                << "\ndlss_input_samples=" << s.upscaleSamples << '\n';
            for (UINT i=0; i<shaders.size(); ++i) summary << shaders[i].name << "\tcreated=" << created[i].load() << "\tcaptured=" << s.captured[i] << '\n';
            auto text=summary.str(); WriteFile(s.directory/"summary.txt",text.data(),text.size());
            s.status="Finished: "+std::to_string(s.finished)+" draws saved; "+std::to_string(s.dropped)+" incomplete/skipped. Folder: OptiScaler_LightingCaptures";
            LOG_INFO("FFXIV lighting: {}",s.status); service.store(false);
        }
        else if (!s.active) s.status="Finishing GPU readbacks...";
    }
    catch (const std::exception& e)
    {
        sampling.store(false); service.store(false); s.active=false; s.pending.clear();
        s.status="Capture stopped: "+std::string(e.what()); LOG_ERROR("FFXIV lighting: {}",s.status);
    }
}

using Create = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*,const void*,SIZE_T,ID3D11ClassLinkage*,ID3D11PixelShader**);
using Draw = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,UINT,UINT);
using Indexed = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,UINT,UINT,INT);
using Instanced = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,UINT,UINT,UINT,UINT);
using IndexedInstanced = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,UINT,UINT,UINT,INT,UINT);
using Indirect = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11Buffer*,UINT);
using Execute = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11CommandList*,BOOL);
inline Create create=nullptr; inline Draw draw=nullptr; inline Indexed indexed=nullptr;
inline Instanced instanced=nullptr; inline IndexedInstanced indexedInstanced=nullptr;
inline Indirect indirect=nullptr,indexedIndirect=nullptr; inline Execute execute=nullptr;
inline HRESULT STDMETHODCALLTYPE CreateShader(ID3D11Device* d,const void* code,SIZE_T size,ID3D11ClassLinkage* link,ID3D11PixelShader** out)
{
    auto hr=create(d,code,size,link,out);
    if (SUCCEEDED(hr) && out && *out)
        if (auto id=Identify(code,size))
        {
            if (SUCCEEDED((*out)->SetPrivateData(tag,sizeof(id),&id))) ++created[id-1];
        }
    return hr;
}
template<class F> inline void Around(ID3D11DeviceContext* c,const char* kind,F&& original)
{
    const bool diagnostic = sampling.load(std::memory_order_relaxed);
    const bool live = FfxivLightingScan::armed.load(std::memory_order_relaxed);
    if ((!diagnostic && !live) || c!=context || inside) { original(); return; }
    struct Guard { Guard(){inside=true;} ~Guard(){inside=false;} } guard;
    UINT id = 0;
    if (live)
    {
        ComPtr<ID3D11PixelShader> ps; c->PSGetShader(&ps, nullptr, nullptr); UINT bytes = sizeof(id);
        if (ps) ps->GetPrivateData(tag, &bytes, &id);
        FfxivLightingScan::Before(c, id);
    }
    auto& s=State(); std::unique_lock lock(s.mutex, std::defer_lock); std::unique_ptr<Shot> shot;
    if (diagnostic)
    {
        lock.lock();
        try { shot=Before(c,kind); } catch (...) { ++s.dropped; }
    }
    original(); // Exactly once, even if diagnostic allocation/copy preparation failed.
    if (live) FfxivLightingScan::After(c, id);
    if (diagnostic) { try { After(c,std::move(shot)); } catch (...) { ++s.dropped; } }
}
inline void STDMETHODCALLTYPE OnDraw(ID3D11DeviceContext* c,UINT n,UINT start) { Around(c,"Draw",[&]{draw(c,n,start);}); }
inline void STDMETHODCALLTYPE OnIndexed(ID3D11DeviceContext* c,UINT n,UINT start,INT b) { Around(c,"DrawIndexed",[&]{indexed(c,n,start,b);}); }
inline void STDMETHODCALLTYPE OnInstanced(ID3D11DeviceContext* c,UINT n,UINT i,UINT start,UINT f) { Around(c,"DrawInstanced",[&]{instanced(c,n,i,start,f);}); }
inline void STDMETHODCALLTYPE OnIndexedInstanced(ID3D11DeviceContext* c,UINT n,UINT i,UINT start,INT b,UINT f) { Around(c,"DrawIndexedInstanced",[&]{indexedInstanced(c,n,i,start,b,f);}); }
inline void STDMETHODCALLTYPE OnIndirect(ID3D11DeviceContext* c,ID3D11Buffer* b,UINT o) { Around(c,"DrawInstancedIndirect",[&]{indirect(c,b,o);}); }
inline void STDMETHODCALLTYPE OnIndexedIndirect(ID3D11DeviceContext* c,ID3D11Buffer* b,UINT o) { Around(c,"DrawIndexedInstancedIndirect",[&]{indexedIndirect(c,b,o);}); }
inline void STDMETHODCALLTYPE OnExecute(ID3D11DeviceContext* c,ID3D11CommandList* list,BOOL restore)
{
    // Playback does not re-enter Draw hooks. Count explicitly so a zero capture
    // cannot be mistaken for proof that a shader never ran.
    if (sampling.load() && c==context && !inside) { std::lock_guard lock(State().mutex); ++State().commandLists; }
    execute(c,list,restore);
}
inline void Install(ID3D11Device* d)
{
    if (create || !d) return;
    wchar_t name[MAX_PATH] {}; GetModuleFileNameW(nullptr,name,MAX_PATH);
    if (_wcsicmp(std::filesystem::path(name).filename().c_str(),L"ffxiv_dx11.exe")) return;
    ComPtr<ID3D11DeviceContext> c; d->GetImmediateContext(&c); if (!c) return;
    auto dv=*reinterpret_cast<void***>(d), cv=*reinterpret_cast<void***>(c.Get());
    create=reinterpret_cast<Create>(dv[15]); draw=reinterpret_cast<Draw>(cv[13]); indexed=reinterpret_cast<Indexed>(cv[12]);
    instanced=reinterpret_cast<Instanced>(cv[21]); indexedInstanced=reinterpret_cast<IndexedInstanced>(cv[20]);
    indirect=reinterpret_cast<Indirect>(cv[40]); indexedIndirect=reinterpret_cast<Indirect>(cv[39]); execute=reinterpret_cast<Execute>(cv[58]);
    if (DetourTransactionBegin()!=NO_ERROR) { create=nullptr; return; }
    bool ok=DetourUpdateThread(GetCurrentThread())==NO_ERROR;
#define LIGHT_ATTACH(o,h) ok &= DetourAttach(reinterpret_cast<PVOID*>(&o),h)==NO_ERROR
    LIGHT_ATTACH(create,CreateShader); LIGHT_ATTACH(draw,OnDraw); LIGHT_ATTACH(indexed,OnIndexed);
    LIGHT_ATTACH(instanced,OnInstanced); LIGHT_ATTACH(indexedInstanced,OnIndexedInstanced);
    LIGHT_ATTACH(indirect,OnIndirect); LIGHT_ATTACH(indexedIndirect,OnIndexedIndirect); LIGHT_ATTACH(execute,OnExecute);
#undef LIGHT_ATTACH
    LONG result=ERROR_INVALID_FUNCTION;
    if (ok) result=DetourTransactionCommit(); else DetourTransactionAbort();
    if (result==NO_ERROR) { context=c.Get(); installed.store(true); LOG_INFO("FFXIV lighting: diagnostic hooks ready (idle until requested)"); }
    else { create=nullptr; LOG_ERROR("FFXIV lighting: hook installation failed {}",result); }
}
inline void Detach()
{
    FfxivLightingScan::Stop();
    sampling.store(false); service.store(false); request.store(0); installed.store(false);
    if (!create || DetourTransactionBegin()!=NO_ERROR) return;
    bool ok=DetourUpdateThread(GetCurrentThread())==NO_ERROR;
#define LIGHT_DETACH(o,h) ok &= DetourDetach(reinterpret_cast<PVOID*>(&o),h)==NO_ERROR
    LIGHT_DETACH(create,CreateShader); LIGHT_DETACH(draw,OnDraw); LIGHT_DETACH(indexed,OnIndexed);
    LIGHT_DETACH(instanced,OnInstanced); LIGHT_DETACH(indexedInstanced,OnIndexedInstanced);
    LIGHT_DETACH(indirect,OnIndirect); LIGHT_DETACH(indexedIndirect,OnIndexedIndirect); LIGHT_DETACH(execute,OnExecute);
#undef LIGHT_DETACH
    if (ok) { if (DetourTransactionCommit()==NO_ERROR) { create=nullptr; context=nullptr; } }
    else DetourTransactionAbort();
}
}
