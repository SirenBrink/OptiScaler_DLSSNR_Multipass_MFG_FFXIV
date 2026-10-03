#include "hdr_stubs/pch.h"
#include <d3d11.h>
#include <cstdio>
#include <stdexcept>
#include "../OptiScaler/misc/FfxivSceneHdr.h"
#pragma comment(lib,"d3d11.lib")
#pragma comment(lib,"d3d12.lib")
#pragma comment(lib,"dxgi.lib")
#pragma comment(lib,"d3dcompiler.lib")
namespace Hdr10 { bool Active(){return true;} }
using Microsoft::WRL::ComPtr;
void Check(HRESULT hr){if(FAILED(hr))throw std::runtime_error("D3D failure: "+std::to_string(unsigned(hr)));}
void Expect(bool value,const char* why){if(!value)throw std::runtime_error(why);}
int main() try {
    Config::Instance()->FfxivHDRMode.v=1;
    ComPtr<ID3D11Device> d;ComPtr<ID3D11DeviceContext> c;
    Check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&d,nullptr,&c));
    const char* vsCode="void main(uint i:SV_VertexID,out float4 p:SV_Position,out float2 uv:TEXCOORD0){uv=float2((i<<1)&2,i&2);p=float4(uv*float2(2,-2)+float2(-1,1),0,1);}";
    ComPtr<ID3DBlob> compiled;Check(D3DCompile(vsCode,strlen(vsCode),nullptr,nullptr,nullptr,"main","vs_5_0",0,0,&compiled,nullptr));
    ComPtr<ID3D11VertexShader> vs;Check(d->CreateVertexShader(compiled->GetBufferPointer(),compiled->GetBufferSize(),nullptr,&vs));
    ComPtr<ID3D11PixelShader> ps;Check(d->CreatePixelShader(Tonemap_0x85E777EF,sizeof(Tonemap_0x85E777EF),nullptr,&ps));
    FfxivSceneHdr::Tag(ps.Get(),0x85e777ef);
    std::vector<unsigned short> pixels(640*360*4,0x4000); // extended scene: gamma-encoded 2
    for(UINT y=0;y<360;y++)for(UINT x=320;x<640;x++)for(UINT k=0;k<3;k++)pixels[(y*640+x)*4+k]=0x4200; // 3
    D3D11_TEXTURE2D_DESC td{};td.Width=640;td.Height=360;td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;
    td.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA inputData{pixels.data(),640*8,0};ComPtr<ID3D11Texture2D> input;Check(d->CreateTexture2D(&td,&inputData,&input));
    ComPtr<ID3D11ShaderResourceView> inputView;Check(d->CreateShaderResourceView(input.Get(),nullptr,&inputView));
    unsigned short one[4]{0x3c00,0x3c00,0x3c00,0x3c00};td.Width=td.Height=1;inputData={one,8,0};
    ComPtr<ID3D11Texture2D> lut;Check(d->CreateTexture2D(&td,&inputData,&lut));
    ComPtr<ID3D11ShaderResourceView> lutView;Check(d->CreateShaderResourceView(lut.Get(),nullptr,&lutView));
    td.Width=640;td.Height=360;td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;td.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> target;Check(d->CreateTexture2D(&td,nullptr,&target));ComPtr<ID3D11RenderTargetView> rtv;Check(d->CreateRenderTargetView(target.Get(),nullptr,&rtv));
    float common[4]{0,1,0,0},tone[8]{0,0,1,0,0,1,0,0};D3D11_BUFFER_DESC bd{};bd.ByteWidth=16;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA cbData{common,0,0};ComPtr<ID3D11Buffer> cb0,cb1;Check(d->CreateBuffer(&bd,&cbData,&cb0));bd.ByteWidth=32;cbData.pSysMem=tone;Check(d->CreateBuffer(&bd,&cbData,&cb1));
    D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_MIN_MAG_MIP_POINT;sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sd.MaxLOD=D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> sampler;Check(d->CreateSamplerState(&sd,&sampler));
    ID3D11Buffer* buffers[]{cb0.Get(),cb1.Get()};ID3D11ShaderResourceView* inputs[]{inputView.Get(),lutView.Get()};ID3D11SamplerState* samplers[]{sampler.Get(),sampler.Get()};
    c->PSSetShaderResources(0,2,inputs);c->PSSetConstantBuffers(0,2,buffers);c->PSSetSamplers(0,2,samplers);
    c->VSSetShader(vs.Get(),nullptr,0);c->PSSetShader(ps.Get(),nullptr,0);c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D11_VIEWPORT vp{0,0,640,360,0,1};c->RSSetViewports(1,&vp);c->OMSetRenderTargets(1,rtv.GetAddressOf(),nullptr);
    auto draw=[&]{c->Draw(3,0);};draw();FfxivSceneHdr::AfterDraw(c.Get(),draw);
    auto& state=FfxivSceneHdr::State();Expect(state.latest!=nullptr,"scene capture missing");

    ComPtr<ID3D11PixelShader> restored;c->PSGetShader(&restored,nullptr,nullptr);Expect(restored.Get()==ps.Get(),"pixel shader was not restored");
    ComPtr<ID3D11RenderTargetView> restoredRtv;c->OMGetRenderTargets(1,&restoredRtv,nullptr);Expect(restoredRtv.Get()==rtv.Get(),"native RTV changed");
    ComPtr<ID3D11ShaderResourceView> restoredInput;c->PSGetShaderResources(0,1,&restoredInput);Expect(restoredInput.Get()==inputView.Get(),"native input changed");
    auto checkPixels=[&](ID3D11Texture2D* texture,bool floating){D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);desc.BindFlags=desc.MiscFlags=0;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> read;Check(d->CreateTexture2D(&desc,nullptr,&read));c->CopyResource(read.Get(),texture);D3D11_MAPPED_SUBRESOURCE m{};Check(c->Map(read.Get(),0,D3D11_MAP_READ,0,&m));
        if(floating){auto p=static_cast<unsigned short*>(m.pData);Expect(p[0]==0x4000 && p[320*4]==0x4200,"FP16 highlights lost");}
        else {auto p=static_cast<unsigned char*>(m.pData);Expect(p[0]==255 && p[320*4]==255,"native SDR changed");}c->Unmap(read.Get(),0);};
    checkPixels(target.Get(),false);checkPixels(state.latest->hdr.Get(),true);checkPixels(state.latest->reference.Get(),false);
    ComPtr<IDXGIFactory4> factory;Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));ComPtr<IDXGIAdapter> adapter;Check(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));ComPtr<ID3D12Device> dx12;Check(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&dx12)));
    auto shared=FfxivSceneHdr::Open(dx12.Get(),3840,2160);Expect(shared.hdr && shared.reference,"DX11/DX12 sharing failed");
    Expect(!FfxivSceneHdr::Open(dx12.Get(),640,640).hdr,"wrong aspect accepted");
    FfxivSceneHdr::EndFrame();Expect(!FfxivSceneHdr::Open(dx12.Get(),640,360).hdr,"previous-frame scene accepted");
    bool finished=false;shared.retire([&]{return finished;});shared={};
    auto previous=state.slots.front().get();draw();FfxivSceneHdr::AfterDraw(c.Get(),draw);Expect(state.latest.get()!=previous,"in-flight FP16 scene reused");finished=true;
    // Real games can create typeless colour resources and bind typed FP16 views.
    // RGB-only writes are also valid: the HDR branch never consumes alpha.
    FfxivSceneHdr::EndFrame();
    td.Width=640;td.Height=360;td.Format=DXGI_FORMAT_R16G16B16A16_TYPELESS;
    td.BindFlags=D3D11_BIND_SHADER_RESOURCE;inputData={pixels.data(),640*8,0};
    ComPtr<ID3D11Texture2D> typelessInput;Check(d->CreateTexture2D(&td,&inputData,&typelessInput));
    D3D11_SHADER_RESOURCE_VIEW_DESC sv{};sv.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;
    sv.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;sv.Texture2D.MipLevels=1;
    ComPtr<ID3D11ShaderResourceView> typedInput;Check(d->CreateShaderResourceView(typelessInput.Get(),&sv,&typedInput));
    td.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> typelessTarget;Check(d->CreateTexture2D(&td,nullptr,&typelessTarget));
    D3D11_RENDER_TARGET_VIEW_DESC rv{};rv.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;rv.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11RenderTargetView> typedTarget;Check(d->CreateRenderTargetView(typelessTarget.Get(),&rv,&typedTarget));
    D3D11_BLEND_DESC rgbOnly{};rgbOnly.RenderTarget[0].RenderTargetWriteMask=7;
    ComPtr<ID3D11BlendState> rgbBlend;Check(d->CreateBlendState(&rgbOnly,&rgbBlend));
    c->OMSetBlendState(rgbBlend.Get(),nullptr,~0u);c->OMSetRenderTargets(1,typedTarget.GetAddressOf(),nullptr);
    c->PSSetShaderResources(0,1,typedInput.GetAddressOf());draw();FfxivSceneHdr::AfterDraw(c.Get(),draw);
    Expect(state.latest!=nullptr,"typed FP16 views or RGB-only writes rejected");checkPixels(state.latest->hdr.Get(),true);
    ComPtr<ID3D11BlendState> restoredBlend;FLOAT factor[4]{};UINT mask=0;c->OMGetBlendState(&restoredBlend,factor,&mask);
    Expect(restoredBlend.Get()==rgbBlend.Get(),"native RGB write mask changed");
    auto typelessShared=FfxivSceneHdr::Open(dx12.Get(),640,360);Expect(typelessShared.hdr && typelessShared.reference,"typed FP16 snapshot sharing failed");
    // Rejecting a later partial write must invalidate forwarding from that target.
    D3D11_BLEND_DESC partial=rgbOnly;partial.RenderTarget[0].RenderTargetWriteMask=1;
    ComPtr<ID3D11BlendState> partialBlend;Check(d->CreateBlendState(&partial,&partialBlend));
    c->OMSetBlendState(partialBlend.Get(),nullptr,~0u);draw();FfxivSceneHdr::AfterDraw(c.Get(),draw);
    Expect(state.aliases.find(typelessTarget.Get())==state.aliases.end(),"rejected overwrite left a stale scene alias");
    // A full scene viewport need not fill the game's allocated target texture.
    FfxivSceneHdr::EndFrame();td.Width=1024;td.Height=512;
    ComPtr<ID3D11Texture2D> paddedTarget;Check(d->CreateTexture2D(&td,nullptr,&paddedTarget));
    ComPtr<ID3D11RenderTargetView> paddedView;Check(d->CreateRenderTargetView(paddedTarget.Get(),&rv,&paddedView));
    D3D11_VIEWPORT region{64,32,640,360,0,1};c->RSSetViewports(1,&region);
    c->OMSetBlendState(rgbBlend.Get(),nullptr,~0u);c->OMSetRenderTargets(1,paddedView.GetAddressOf(),nullptr);
    draw();FfxivSceneHdr::AfterDraw(c.Get(),draw);
    Expect(state.latest && state.latest->width==1024 && state.latest->height==512,"padded scene capture failed");
    auto cropped=FfxivSceneHdr::Open(dx12.Get(),3840,2160);
    Expect(cropped.hdr && cropped.reference,"logical viewport aspect was rejected");
    Expect(cropped.rect==std::array<float,4>{64.f/1024,32.f/512,640.f/1024,360.f/512},"viewport crop metadata incorrect");
    D3D11_VIEWPORT restoredViewport{};UINT count=1;c->RSGetViewports(&count,&restoredViewport);
    Expect(count==1 && restoredViewport.TopLeftX==64 && restoredViewport.Width==640,"native viewport changed");
    region.Width=1024;c->RSSetViewports(1,&region);draw();FfxivSceneHdr::AfterDraw(c.Get(),draw);
    Expect(state.aliases.find(paddedTarget.Get())==state.aliases.end(),"out-of-bounds viewport accepted");
    // FFXIV's exact full-target half-pixel origin must not shift sampling.
    FfxivSceneHdr::EndFrame();c->OMSetRenderTargets(1,typedTarget.GetAddressOf(),nullptr);
    region={.5f,.5f,640,360,0,1};c->RSSetViewports(1,&region);
    draw();FfxivSceneHdr::AfterDraw(c.Get(),draw);
    auto halfPixel=FfxivSceneHdr::Open(dx12.Get(),640,360);
    Expect(halfPixel.hdr && halfPixel.rect==std::array<float,4>{0,0,1,1},"full-target half-pixel viewport rejected or sampling shifted");
    c->RSGetViewports(&count,&restoredViewport);
    Expect(restoredViewport.TopLeftX==.5f && restoredViewport.TopLeftY==.5f,"native half-pixel origin changed");
    FfxivSceneHdr::EndFrame();c->OMSetRenderTargets(1,paddedView.GetAddressOf(),nullptr);
    region={64.5f,32.5f,640,360,0,1};c->RSSetViewports(1,&region);draw();FfxivSceneHdr::AfterDraw(c.Get(),draw);
    auto paddedHalfPixel=FfxivSceneHdr::Open(dx12.Get(),3840,2160);
    Expect(paddedHalfPixel.hdr && paddedHalfPixel.rect==std::array<float,4>{64.f/1024,32.f/512,640.f/1024,360.f/512},"padded half-pixel crop incorrect");
    FfxivSceneHdr::EndFrame();
    {std::lock_guard lock(state.mutex);FfxivSceneHdr::CollectLocked(state,false,GetTickCount64());}
    Expect(!state.slots.empty(),"snapshot cleanup ignored consumer ownership");
    bool unresolved=true;
    for(auto& slot:state.slots){slot->dx12Recorded=true;slot->done=[&]{return !unresolved;};}
    shared={};typelessShared={};cropped={};halfPixel={};paddedHalfPixel={};
    {std::lock_guard lock(state.mutex);FfxivSceneHdr::CollectLocked(state,false,GetTickCount64());}
    Expect(!state.slots.empty(),"snapshot cleanup ignored unresolved GPU probe");
    unresolved=false;
    {std::lock_guard lock(state.mutex);FfxivSceneHdr::CollectLocked(state,false,GetTickCount64());}
    Expect(state.slots.empty(),"completed unowned snapshots not reclaimed");
    puts("PASS: real RenoDX DX11 replay retains distinct FP16 highlights, preserves native SDR/bindings, shares into DX12, rejects stale/aspect-mismatched scenes and retains in-flight ownership");
    return 0;
}catch(const std::exception& e){puts(e.what());return 1;}
