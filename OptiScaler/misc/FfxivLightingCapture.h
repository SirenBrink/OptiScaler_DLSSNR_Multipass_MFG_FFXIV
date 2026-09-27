#pragma once
#include <d3d11_1.h>
#include <wrl/client.h>
#include <detours/detours.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <filesystem>
#include "FfxivLightingScan.h"

// Native exposure scan hooks. Signatures identify the game's
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
inline std::atomic<bool> installed {false};
inline ID3D11DeviceContext* context = nullptr; // Identity only; no idle COM ownership.
inline thread_local bool inside = false;
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
using Create = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*,const void*,SIZE_T,ID3D11ClassLinkage*,ID3D11PixelShader**);
using Draw = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,UINT,UINT);
using Indexed = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,UINT,UINT,INT);
using Instanced = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,UINT,UINT,UINT,UINT);
using IndexedInstanced = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,UINT,UINT,UINT,INT,UINT);
using Indirect = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11Buffer*,UINT);
inline Create create=nullptr; inline Draw draw=nullptr; inline Indexed indexed=nullptr;
inline Instanced instanced=nullptr; inline IndexedInstanced indexedInstanced=nullptr;
inline Indirect indirect=nullptr,indexedIndirect=nullptr;
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
    if (!FfxivLightingScan::armed.load(std::memory_order_relaxed) || c!=context || inside)
    { original(); return; }
    struct Guard { Guard(){inside=true;} ~Guard(){inside=false;} } guard;
    UINT id = 0;
    ComPtr<ID3D11PixelShader> ps; c->PSGetShader(&ps, nullptr, nullptr); UINT bytes = sizeof(id);
    if (ps) ps->GetPrivateData(tag, &bytes, &id);
    FfxivLightingScan::Before(c, id);
    original();
    FfxivLightingScan::After(c, id);
}
inline void STDMETHODCALLTYPE OnDraw(ID3D11DeviceContext* c,UINT n,UINT start) { Around(c,"Draw",[&]{draw(c,n,start);}); }
inline void STDMETHODCALLTYPE OnIndexed(ID3D11DeviceContext* c,UINT n,UINT start,INT b) { Around(c,"DrawIndexed",[&]{indexed(c,n,start,b);}); }
inline void STDMETHODCALLTYPE OnInstanced(ID3D11DeviceContext* c,UINT n,UINT i,UINT start,UINT f) { Around(c,"DrawInstanced",[&]{instanced(c,n,i,start,f);}); }
inline void STDMETHODCALLTYPE OnIndexedInstanced(ID3D11DeviceContext* c,UINT n,UINT i,UINT start,INT b,UINT f) { Around(c,"DrawIndexedInstanced",[&]{indexedInstanced(c,n,i,start,b,f);}); }
inline void STDMETHODCALLTYPE OnIndirect(ID3D11DeviceContext* c,ID3D11Buffer* b,UINT o) { Around(c,"DrawInstancedIndirect",[&]{indirect(c,b,o);}); }
inline void STDMETHODCALLTYPE OnIndexedIndirect(ID3D11DeviceContext* c,ID3D11Buffer* b,UINT o) { Around(c,"DrawIndexedInstancedIndirect",[&]{indexedIndirect(c,b,o);}); }
inline void Install(ID3D11Device* d)
{
    if (create || !d) return;
    wchar_t name[MAX_PATH] {}; GetModuleFileNameW(nullptr,name,MAX_PATH);
    if (_wcsicmp(std::filesystem::path(name).filename().c_str(),L"ffxiv_dx11.exe")) return;
    ComPtr<ID3D11DeviceContext> c; d->GetImmediateContext(&c); if (!c) return;
    auto dv=*reinterpret_cast<void***>(d), cv=*reinterpret_cast<void***>(c.Get());
    create=reinterpret_cast<Create>(dv[15]); draw=reinterpret_cast<Draw>(cv[13]); indexed=reinterpret_cast<Indexed>(cv[12]);
    instanced=reinterpret_cast<Instanced>(cv[21]); indexedInstanced=reinterpret_cast<IndexedInstanced>(cv[20]);
    indirect=reinterpret_cast<Indirect>(cv[40]); indexedIndirect=reinterpret_cast<Indirect>(cv[39]);
    if (DetourTransactionBegin()!=NO_ERROR) { create=nullptr; return; }
    bool ok=DetourUpdateThread(GetCurrentThread())==NO_ERROR;
#define LIGHT_ATTACH(o,h) ok &= DetourAttach(reinterpret_cast<PVOID*>(&o),h)==NO_ERROR
    LIGHT_ATTACH(create,CreateShader); LIGHT_ATTACH(draw,OnDraw); LIGHT_ATTACH(indexed,OnIndexed);
    LIGHT_ATTACH(instanced,OnInstanced); LIGHT_ATTACH(indexedInstanced,OnIndexedInstanced);
    LIGHT_ATTACH(indirect,OnIndirect); LIGHT_ATTACH(indexedIndirect,OnIndexedIndirect);
#undef LIGHT_ATTACH
    LONG result=ERROR_INVALID_FUNCTION;
    if (ok) result=DetourTransactionCommit(); else DetourTransactionAbort();
    if (result==NO_ERROR) { context=c.Get(); installed.store(true); LOG_INFO("FFXIV lighting: native exposure hooks ready"); }
    else { create=nullptr; LOG_ERROR("FFXIV lighting: hook installation failed {}",result); }
}
inline void Detach()
{
    FfxivLightingScan::Stop();
    installed.store(false);
    if (!create || DetourTransactionBegin()!=NO_ERROR) return;
    bool ok=DetourUpdateThread(GetCurrentThread())==NO_ERROR;
#define LIGHT_DETACH(o,h) ok &= DetourDetach(reinterpret_cast<PVOID*>(&o),h)==NO_ERROR
    LIGHT_DETACH(create,CreateShader); LIGHT_DETACH(draw,OnDraw); LIGHT_DETACH(indexed,OnIndexed);
    LIGHT_DETACH(instanced,OnInstanced); LIGHT_DETACH(indexedInstanced,OnIndexedInstanced);
    LIGHT_DETACH(indirect,OnIndirect); LIGHT_DETACH(indexedIndirect,OnIndexedIndirect);
#undef LIGHT_DETACH
    if (ok) { if (DetourTransactionCommit()==NO_ERROR) { create=nullptr; context=nullptr; } }
    else DetourTransactionAbort();
}
}
