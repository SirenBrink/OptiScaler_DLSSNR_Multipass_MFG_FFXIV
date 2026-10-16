#pragma once
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <mutex>
#include <atomic>
#include "FfxivCharaViewTrace.h"

namespace FfxivPreviewEnhance {
using Microsoft::WRL::ComPtr;
inline thread_local bool processing=false;
inline std::atomic<UINT64> immediateCandidates{0},deferredCandidates{0},matchedDraws{0},processedDraws{0};
inline constexpr char shaderSource[]=R"(
Texture2D<float4> Source:register(t0);SamplerState LinearClamp:register(s0);
struct V {float4 position:SV_Position;float2 uv:TEXCOORD;};
V VS(uint id:SV_VertexID){V o;float2 uv=float2((id<<1)&2,id&2);o.uv=uv;o.position=float4(uv*float2(2,-2)+float2(-1,1),0,1);return o;}
float luma(float3 c){return dot(c,float3(.299,.587,.114));}
float4 PS(V i):SV_Target {
 uint w,h;Source.GetDimensions(w,h);float2 px=1.0/float2(w,h);
 float4 c=Source.SampleLevel(LinearClamp,i.uv,0);
 float4 nw=Source.SampleLevel(LinearClamp,i.uv+float2(-1,-1)*px,0);
 float4 ne=Source.SampleLevel(LinearClamp,i.uv+float2(1,-1)*px,0);
 float4 sw=Source.SampleLevel(LinearClamp,i.uv+float2(-1,1)*px,0);
 float4 se=Source.SampleLevel(LinearClamp,i.uv+float2(1,1)*px,0);
 // Preserve the native alpha and avoid pulling background RGB across cutouts.
 if(min(c.a,min(min(nw.a,ne.a),min(sw.a,se.a)))<.98)return c;
 float a=luma(nw.rgb),b=luma(ne.rgb),d=luma(sw.rgb),e=luma(se.rgb),m=luma(c.rgb);
 float lo=min(m,min(min(a,b),min(d,e))),hi=max(m,max(max(a,b),max(d,e)));
 if(hi-lo<max(.0312,hi*.125))return c;
 float2 dir=float2(-((a+b)-(d+e)),(a+d)-(b+e));
 float reduce=max((a+b+d+e)*.03125,.0078125);
 dir=clamp(dir/(min(abs(dir.x),abs(dir.y))+reduce),-8,8)*px;
 float3 A=.5*(Source.SampleLevel(LinearClamp,i.uv-dir/6,0).rgb+Source.SampleLevel(LinearClamp,i.uv+dir/6,0).rgb);
 float3 B=.5*A+.25*(Source.SampleLevel(LinearClamp,i.uv-dir*.5,0).rgb+Source.SampleLevel(LinearClamp,i.uv+dir*.5,0).rgb);
 float lum=luma(B);return float4(lum<lo||lum>hi?A:B,c.a);
})";
struct State {
    std::mutex mutex;ComPtr<ID3D11Device> device;
    ComPtr<ID3DDeviceContextState> privateState;
    ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11Texture2D> texture;ComPtr<ID3D11ShaderResourceView> view;
    ComPtr<ID3D11RenderTargetView> target;
    UINT width=0,height=0;bool failed=false;UINT64 draws=0;
};
inline State& Data(){static auto* value=new State;return *value;}
inline bool Initialize(State& s,ID3D11Device* device) {
    if(s.failed)return false;
    if(s.ps)return true;
    ComPtr<ID3D11Device1> d;if(FAILED(device->QueryInterface(IID_PPV_ARGS(&d))))return false;
    D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_0},chosen{};
    HRESULT hr=d->CreateDeviceContextState(0,levels,1,D3D11_SDK_VERSION,__uuidof(ID3D11Device),&chosen,&s.privateState);
    ComPtr<ID3DBlob> code,error;
    if(SUCCEEDED(hr))hr=D3DCompile(shaderSource,sizeof(shaderSource)-1,nullptr,nullptr,nullptr,"VS","vs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&error);
    if(SUCCEEDED(hr))hr=device->CreateVertexShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&s.vs);
    code.Reset();error.Reset();
    if(SUCCEEDED(hr))hr=D3DCompile(shaderSource,sizeof(shaderSource)-1,nullptr,nullptr,nullptr,"PS","ps_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&error);
    if(SUCCEEDED(hr))hr=device->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&s.ps);
    D3D11_SAMPLER_DESC desc{};desc.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    desc.AddressU=desc.AddressV=desc.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;desc.MaxLOD=D3D11_FLOAT32_MAX;
    if(SUCCEEDED(hr))hr=device->CreateSamplerState(&desc,&s.sampler);
    if(FAILED(hr)){s.failed=true;LOG_INFO("CharaView enhancement unavailable: initialization {:X}",UINT(hr));return false;}
    return true;
}
inline ComPtr<ID3D11ShaderResourceView> Process(ID3D11DeviceContext* c,ID3D11ShaderResourceView* source,UINT width,UINT height,bool doubleSize) {
    ComPtr<ID3D11DeviceContext1> context;if(FAILED(c->QueryInterface(IID_PPV_ARGS(&context))))return {};
    ComPtr<ID3D11Device> device;c->GetDevice(&device);
    auto& s=Data();std::lock_guard lock(s.mutex);
    if(s.device && s.device!=device)return {}; // Never carry resources between devices.
    s.device=device;if(!Initialize(s,device.Get()))return {};
    const UINT ow=width*(doubleSize?2:1),oh=height*(doubleSize?2:1);
    if(ow>4096 || oh>4096)return {};
    if(!s.texture || s.width!=ow || s.height!=oh) {
        ComPtr<ID3D11Texture2D> nextTexture;ComPtr<ID3D11ShaderResourceView> nextView;ComPtr<ID3D11RenderTargetView> nextTarget;
        D3D11_TEXTURE2D_DESC desc{};desc.Width=ow;desc.Height=oh;desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;
        desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
        if(FAILED(device->CreateTexture2D(&desc,nullptr,&nextTexture)) || FAILED(device->CreateShaderResourceView(nextTexture.Get(),nullptr,&nextView)) || FAILED(device->CreateRenderTargetView(nextTexture.Get(),nullptr,&nextTarget)))return {};
        s.texture=nextTexture;s.view=nextView;s.target=nextTarget;
        s.width=ow;s.height=oh;
    }
    ComPtr<ID3DDeviceContextState> native;context->SwapDeviceContextState(s.privateState.Get(),&native);
    c->OMSetRenderTargets(1,s.target.GetAddressOf(),nullptr);c->OMSetBlendState(nullptr,nullptr,~0u);
    c->OMSetDepthStencilState(nullptr,0);c->RSSetState(nullptr);
    D3D11_VIEWPORT vp{0,0,float(ow),float(oh),0,1};c->RSSetViewports(1,&vp);
    c->IASetInputLayout(nullptr);c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    c->VSSetShader(s.vs.Get(),nullptr,0);c->PSSetShader(s.ps.Get(),nullptr,0);
    c->GSSetShader(nullptr,nullptr,0);c->HSSetShader(nullptr,nullptr,0);c->DSSetShader(nullptr,nullptr,0);
    c->PSSetShaderResources(0,1,&source);c->PSSetSamplers(0,1,s.sampler.GetAddressOf());
    processing=true;c->Draw(3,0);processing=false;
    ID3D11ShaderResourceView* empty=nullptr;c->PSSetShaderResources(0,1,&empty);c->OMSetRenderTargets(0,nullptr,nullptr);
    context->SwapDeviceContextState(native.Get(),nullptr);
    ++processedDraws;
    if(++s.draws==1 || s.draws%600==0)LOG_INFO("CharaView spatial AA: {}x{} -> {}x{}, processed draws {}",width,height,ow,oh,s.draws);
    return s.view;
}
template<class F> inline void Around(ID3D11DeviceContext* c,bool doubleSize,F&& draw) {
    if(processing){draw();return;}
    ComPtr<ID3D11PixelShader> shader;c->PSGetShader(&shader,nullptr,nullptr);
    UINT crc=0,bytes=sizeof(crc);
    constexpr GUID shaderTag{0xd2b564c4,0x7708,0x48f0,{0xa7,0xb5,0x55,0x22,0x19,0x42,0x31,0xf0}};
    if(!shader || FAILED(shader->GetPrivateData(shaderTag,&bytes,&crc)) || crc!=2067850010u){draw();return;}
    const bool deferred=c->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE;
    auto& candidates=deferred?deferredCandidates:immediateCandidates;
    if(++candidates==1)LOG_INFO("CharaView enhancement: preview shader seen on {} context",deferred?"deferred":"immediate");
    ComPtr<ID3D11ShaderResourceView> source;c->PSGetShaderResources(0,1,&source);
    ComPtr<ID3D11Resource> resource;ComPtr<ID3D11Texture2D> texture;
    if(!source){draw();return;}source->GetResource(&resource);
    if(FAILED(resource.As(&texture))){draw();return;}
    D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);
    if(desc.SampleDesc.Count!=1 || FfxivCharaViewTrace::Match(FfxivCharaViewTrace::Current(),texture.Get(),desc.Width,desc.Height)!=0){draw();return;}
    if(++matchedDraws==1)LOG_INFO("CharaView enhancement: native equipment output matched, {}x{}, deferred {}",desc.Width,desc.Height,deferred);
    // A deferred draw cannot safely consume an immediate-context filter result:
    // its source may still be waiting for execution. Preserve native rendering.
    if(deferred){draw();return;}
    auto replacement=Process(c,source.Get(),desc.Width,desc.Height,doubleSize);
    if(!replacement){draw();return;}
    c->PSSetShaderResources(0,1,replacement.GetAddressOf());draw();
    c->PSSetShaderResources(0,1,source.GetAddressOf());
}
inline void Shutdown() {
    auto& s=Data();std::lock_guard lock(s.mutex);
    s.texture.Reset();s.target.Reset();s.view.Reset();s.privateState.Reset();s.vs.Reset();s.ps.Reset();s.sampler.Reset();s.device.Reset();s.failed=false;s.draws=0;
}
}
