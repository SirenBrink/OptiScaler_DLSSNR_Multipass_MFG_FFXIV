#pragma once
#include <d3d11.h>
#include <d3d11shader.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <array>
#include <mutex>
#include <vector>
#include <atomic>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include "FfxivCharaViewTrace.h"

// Bounded, read-only preview guide discovery. No resource copies, GPU readback,
// camera writes, synthetic motion vectors or NGX evaluations are performed.
namespace FfxivPreviewGuides {
using Microsoft::WRL::ComPtr;
inline constexpr GUID codeTag{0x8b97e1d2,0xa3d7,0x4b99,{0x8a,0x56,0xa1,0x03,0x24,0xb8,0x52,0x19}};
inline constexpr UINT MaxCode=256*1024;
inline std::atomic<UINT> captured{0};
inline std::atomic<UINT> retainedBytes{0};
inline void Tag(ID3D11DeviceChild* shader,const void* code,SIZE_T size) {
    if(!shader || !code || size<32 || size>MaxCode || std::memcmp(code,"DXBC",4))return;
    UINT used=retainedBytes.load();
    do {if(used>64*1024*1024-UINT(size))return;}
    while(!retainedBytes.compare_exchange_weak(used,used+UINT(size)));
    if(FAILED(shader->SetPrivateData(codeTag,UINT(size),code)))retainedBytes.fetch_sub(UINT(size));
}
inline std::filesystem::path Directory() {
    wchar_t exe[32768]{};
    const auto count=GetModuleFileNameW(nullptr,exe,UINT(std::size(exe)));
    if(!count || count>=std::size(exe))return {};
    return std::filesystem::path(exe).parent_path()/L"OptiScaler"/L"PreviewGuides";
}
inline void Describe(ID3D11DeviceChild* shader,const char* stage,std::ofstream& manifest) {
    if(!shader)return;
    UINT size=0;if(FAILED(shader->GetPrivateData(codeTag,&size,nullptr)) || !size || size>MaxCode) {
        manifest<<stage<<" bytecode unavailable (shader predates capture)\n";return;
    }
    std::vector<unsigned char> code(size);
    if(FAILED(shader->GetPrivateData(codeTag,&size,code.data())))return;
    // DXBC's embedded checksum names exact shader variants, with no guesses
    // about which render target contains normals, depth or motion.
    char name[64]{};char* p=name;p+=sprintf_s(p,sizeof(name),"%s-",stage);
    for(UINT i=4;i<20;++i)p+=sprintf_s(p,sizeof(name)-(p-name),"%02x",code[i]);
    const auto path=Directory()/name;
    std::error_code fileError;
    if(!std::filesystem::exists(path.string()+".dxbc",fileError) && !fileError) {
        std::ofstream binary(path.string()+".dxbc",std::ios::binary);
        binary.write(reinterpret_cast<const char*>(code.data()),size);
        ComPtr<ID3DBlob> assembly;
        if(SUCCEEDED(D3DDisassemble(code.data(),size,0,nullptr,&assembly))) {
            std::ofstream text(path.string()+".txt",std::ios::binary);
            text.write(static_cast<const char*>(assembly->GetBufferPointer()),assembly->GetBufferSize());
        }
    }
    manifest<<stage<<" "<<name<<"\n";
    ComPtr<ID3D11ShaderReflection> reflection;
    if(FAILED(D3DReflect(code.data(),size,__uuidof(ID3D11ShaderReflection),&reflection)))return;
    D3D11_SHADER_DESC desc{};if(FAILED(reflection->GetDesc(&desc)))return;
    for(UINT i=0;i<desc.OutputParameters;++i) {
        D3D11_SIGNATURE_PARAMETER_DESC out{};
        if(SUCCEEDED(reflection->GetOutputParameterDesc(i,&out)))
            manifest<<" output "<<out.SemanticName<<out.SemanticIndex<<" register "<<out.Register<<" mask "<<UINT(out.Mask)<<"\n";
    }
    for(UINT i=0;i<desc.BoundResources;++i) {
        D3D11_SHADER_INPUT_BIND_DESC resource{};
        if(SUCCEEDED(reflection->GetResourceBindingDesc(i,&resource)))
            manifest<<" resource "<<resource.Name<<" type "<<resource.Type<<" slot "<<resource.BindPoint<<" count "<<resource.BindCount<<"\n";
    }
    for(UINT i=0;i<desc.ConstantBuffers;++i) {
        auto cb=reflection->GetConstantBufferByIndex(i);D3D11_SHADER_BUFFER_DESC buffer{};
        if(FAILED(cb->GetDesc(&buffer)))continue;
        manifest<<" constants "<<buffer.Name<<" bytes "<<buffer.Size<<"\n";
        for(UINT j=0;j<buffer.Variables;++j) {
            D3D11_SHADER_VARIABLE_DESC variable{};
            if(SUCCEEDED(cb->GetVariableByIndex(j)->GetDesc(&variable)))
                manifest<<"  "<<variable.Name<<" offset "<<variable.StartOffset<<" bytes "<<variable.Size<<"\n";
        }
    }
}
inline void Observe(ID3D11DeviceContext* c) {
    if(c->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE || captured.load()>=24)return;
    ID3D11RenderTargetView* raw[8]{};ComPtr<ID3D11DepthStencilView> depth;
    c->OMGetRenderTargets(8,raw,&depth);
    std::array<ComPtr<ID3D11RenderTargetView>,8> targets;
    for(UINT i=0;i<8;++i)targets[i].Attach(raw[i]);
    if(!targets[0] || !depth)return;
    ComPtr<ID3D11Resource> resource;ComPtr<ID3D11Texture2D> texture;
    targets[0]->GetResource(&resource);if(FAILED(resource.As(&texture)))return;
    D3D11_TEXTURE2D_DESC td{};texture->GetDesc(&td);
    const auto& snapshot=FfxivCharaViewTrace::Current();
    const auto entry=FfxivCharaViewTrace::Match(snapshot,texture.Get(),td.Width,td.Height);
    // Geometry only; exclude the finishing/fullscreen/UI consumers and world.
    if(entry!=8 && entry!=13)return;
    ComPtr<ID3D11PixelShader> ps;ComPtr<ID3D11VertexShader> vs;
    c->PSGetShader(&ps,nullptr,nullptr);c->VSGetShader(&vs,nullptr,nullptr);
    if(!ps || !vs)return;
    struct Key {void* vs;void* ps;int entry;bool operator==(const Key&)const=default;};
    static std::mutex mutex;static std::array<Key,24> seen{};static UINT count=0;
    std::lock_guard lock(mutex);const Key key{vs.Get(),ps.Get(),entry};
    if(count==seen.size() || std::find(seen.begin(),seen.begin()+count,key)!=seen.begin()+count)return;
    // Retain shader identity via bytecode checksum on disk, not idle COM refs.
    const auto directory=Directory();if(directory.empty())return;
    std::error_code error;std::filesystem::create_directories(directory,error);if(error)return;
    std::ofstream manifest(directory/("draw-"+std::to_string(count)+".txt"));if(!manifest)return;
    manifest<<"Native preview entry "<<entry<<" extent "<<td.Width<<"x"<<td.Height<<"\n";
    D3D11_VIEWPORT viewport{};UINT vpCount=1;c->RSGetViewports(&vpCount,&viewport);
    manifest<<"viewport "<<viewport.TopLeftX<<","<<viewport.TopLeftY<<" "<<viewport.Width<<"x"<<viewport.Height<<"\n";
    for(UINT i=0;i<targets.size();++i)if(targets[i]) {
        targets[i]->GetResource(&resource);texture.Reset();if(FAILED(resource.As(&texture)))continue;
        texture->GetDesc(&td);D3D11_RENDER_TARGET_VIEW_DESC view{};targets[i]->GetDesc(&view);
        manifest<<"MRT "<<i<<" "<<td.Width<<"x"<<td.Height<<" format "<<view.Format<<" table entry "<<FfxivCharaViewTrace::Match(snapshot,texture.Get(),td.Width,td.Height)<<"\n";
    }
    depth->GetResource(&resource);texture.Reset();
    if(SUCCEEDED(resource.As(&texture))) {
        texture->GetDesc(&td);D3D11_DEPTH_STENCIL_VIEW_DESC view{};depth->GetDesc(&view);
        manifest<<"depth "<<td.Width<<"x"<<td.Height<<" texture format "<<td.Format<<" view format "<<view.Format<<" binds "<<td.BindFlags<<"\n";
    }
    Describe(vs.Get(),"VS",manifest);Describe(ps.Get(),"PS",manifest);
    seen[count++]=key;captured.store(count);
    LOG_INFO("CharaView DLSS guide capture: native geometry sample {} saved, entry {}",count,entry);
}
}
