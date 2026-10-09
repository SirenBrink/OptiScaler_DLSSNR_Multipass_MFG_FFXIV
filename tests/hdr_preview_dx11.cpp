#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <array>
#include <algorithm>
#include <cmath>
#include <memory>
#include <functional>
#include <stdexcept>
#include <cstdio>
#define LOG_INFO(...) ((void)0)
#include "../OptiScaler/misc/FfxivHdrPreview.h"
#pragma comment(lib,"d3d11.lib")
#pragma comment(lib,"d3dcompiler.lib")
using Microsoft::WRL::ComPtr;
void check(HRESULT hr){if(FAILED(hr))throw std::runtime_error("D3D11 failure");}
void expect(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
int main() try {
    ComPtr<ID3D11Device> d;ComPtr<ID3D11DeviceContext> c;
    check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,D3D11_CREATE_DEVICE_DEBUG,nullptr,0,D3D11_SDK_VERSION,&d,nullptr,&c));
    const char* hlsl="Texture2D<float4> Source:register(t0);float4 VS(uint id:SV_VertexID):SV_Position{return float4(id==2?3:-1,id==1?3:-1,0,1);}float4 PS():SV_Target{return Source.Load(int3(0,0,0));}";
    ComPtr<ID3DBlob> code,error;check(D3DCompile(hlsl,strlen(hlsl),nullptr,nullptr,nullptr,"VS","vs_5_0",0,0,&code,&error));
    ComPtr<ID3D11VertexShader> vs;check(d->CreateVertexShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&vs));
    check(D3DCompile(hlsl,strlen(hlsl),nullptr,nullptr,nullptr,"PS","ps_5_0",0,0,&code,&error));
    ComPtr<ID3D11PixelShader> ps;check(d->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&ps));
    FfxivHdrPreview::Tag(ps.Get(),3865947726u);
    D3D11_TEXTURE2D_DESC td{};td.Width=64;td.Height=64;td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;
    td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;td.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> source,target,read;check(d->CreateTexture2D(&td,nullptr,&source));
    ComPtr<ID3D11RenderTargetView> inputRtv;check(d->CreateRenderTargetView(source.Get(),nullptr,&inputRtv));
    ComPtr<ID3D11ShaderResourceView> input;check(d->CreateShaderResourceView(source.Get(),nullptr,&input));
    td.Width=640;td.Height=360;check(d->CreateTexture2D(&td,nullptr,&target));
    ComPtr<ID3D11RenderTargetView> targetRtv;check(d->CreateRenderTargetView(target.Get(),nullptr,&targetRtv));
    td.BindFlags=0;td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;check(d->CreateTexture2D(&td,nullptr,&read));
    D3D11_BLEND_DESC bd{};auto& r=bd.RenderTarget[0];r.BlendEnable=true;r.SrcBlend=D3D11_BLEND_SRC_ALPHA;r.DestBlend=D3D11_BLEND_INV_SRC_ALPHA;r.BlendOp=D3D11_BLEND_OP_ADD;
    r.SrcBlendAlpha=D3D11_BLEND_ONE;r.DestBlendAlpha=D3D11_BLEND_ZERO;r.BlendOpAlpha=D3D11_BLEND_OP_ADD;r.RenderTargetWriteMask=15;
    bd.IndependentBlendEnable=false;for(UINT i=1;i<8;++i)bd.RenderTarget[i]=r;
    ComPtr<ID3D11BlendState> blend;check(d->CreateBlendState(&bd,&blend));
    c->OMSetRenderTargets(1,targetRtv.GetAddressOf(),nullptr);c->OMSetBlendState(blend.Get(),nullptr,~0u);
    c->VSSetShader(vs.Get(),nullptr,0);c->PSSetShader(ps.Get(),nullptr,0);c->PSSetShaderResources(0,1,input.GetAddressOf());c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D11_VIEWPORT vp{0,0,320,360,0,1};c->RSSetViewports(1,&vp);
    auto draw=[&]{c->Draw(3,0);};
    for(float alpha:{1.f,.5f}) {
        FfxivHdrPreview::EndFrame();const float colour[4]{1,1,1,alpha};c->ClearRenderTargetView(inputRtv.Get(),colour);
        FfxivHdrPreview::BeforeDraw(c.Get(),draw);draw();
        auto& state=FfxivHdrPreview::State();expect(bool(state.current),"Preview draw not captured");
        c->CopyResource(read.Get(),state.current->texture.Get());D3D11_MAPPED_SUBRESOURCE mapped{};check(c->Map(read.Get(),0,D3D11_MAP_READ,0,&mapped));
        auto* bytes=static_cast<unsigned char*>(mapped.pData);
        expect(std::abs(int(bytes[4*100+3])-int((1-alpha)*255))<=1,"Wrong native preview transmittance");
        expect(bytes[4*100]==bytes[4*100+3],"RGB and alpha coverage differ");
        expect(bytes[4*500]==255,"RGB coverage escaped native preview viewport");
        expect(bytes[4*500+3]==255,"Coverage escaped native preview viewport");c->Unmap(read.Get(),0);
        ComPtr<ID3D11RenderTargetView> restored;c->OMGetRenderTargets(1,&restored,nullptr);expect(restored==targetRtv,"Native target not restored");
        ComPtr<ID3D11BlendState> restoredBlend;c->OMGetBlendState(&restoredBlend,nullptr,nullptr);expect(restoredBlend==blend,"Native blend not restored");
    }
    // Depth/stencil-writing UI must be sampled before its one native draw.
    // The replay tests the original buffer but must never change it.
    D3D11_TEXTURE2D_DESC depthDesc=td;depthDesc.Usage=D3D11_USAGE_DEFAULT;depthDesc.CPUAccessFlags=0;
    depthDesc.Format=DXGI_FORMAT_D24_UNORM_S8_UINT;depthDesc.BindFlags=D3D11_BIND_DEPTH_STENCIL;
    ComPtr<ID3D11Texture2D> depth;check(d->CreateTexture2D(&depthDesc,nullptr,&depth));
    ComPtr<ID3D11DepthStencilView> depthView;check(d->CreateDepthStencilView(depth.Get(),nullptr,&depthView));
    const FLOAT white[4]{1,1,1,1},black[4]{};c->ClearRenderTargetView(inputRtv.Get(),white);
    for(bool stencil:{false,true}) {
        D3D11_DEPTH_STENCIL_DESC ds{};ds.DepthEnable=!stencil;ds.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;ds.DepthFunc=D3D11_COMPARISON_LESS;
        ds.StencilEnable=stencil;ds.StencilReadMask=ds.StencilWriteMask=255;
        ds.FrontFace=ds.BackFace={D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_INCR_SAT,D3D11_COMPARISON_EQUAL};
        ComPtr<ID3D11DepthStencilState> nativeDs;check(d->CreateDepthStencilState(&ds,&nativeDs));
        c->OMSetRenderTargets(1,targetRtv.GetAddressOf(),depthView.Get());c->OMSetDepthStencilState(nativeDs.Get(),7);
        c->ClearDepthStencilView(depthView.Get(),D3D11_CLEAR_DEPTH|D3D11_CLEAR_STENCIL,1,7);c->ClearRenderTargetView(targetRtv.Get(),black);
        FfxivHdrPreview::EndFrame();FfxivHdrPreview::BeforeDraw(c.Get(),draw);draw();
        c->CopyResource(read.Get(),target.Get());D3D11_MAPPED_SUBRESOURCE mapped{};check(c->Map(read.Get(),0,D3D11_MAP_READ,0,&mapped));
        expect(static_cast<unsigned char*>(mapped.pData)[4*100]==255,"Mask replay changed depth/stencil before the native draw");c->Unmap(read.Get(),0);
        ComPtr<ID3D11DepthStencilState> restored;UINT reference=0;c->OMGetDepthStencilState(&restored,&reference);
        expect(restored==nativeDs && reference==7,"Native clipping state not restored");
        FfxivHdrPreview::EndFrame();FfxivHdrPreview::BeforeDraw(c.Get(),draw);
        c->CopyResource(read.Get(),FfxivHdrPreview::State().current->texture.Get());check(c->Map(read.Get(),0,D3D11_MAP_READ,0,&mapped));
        expect(static_cast<unsigned char*>(mapped.pData)[4*100+3]==255,"Replay ignored the native depth/stencil test");c->Unmap(read.Get(),0);
    }
    c->OMSetRenderTargets(1,targetRtv.GetAddressOf(),nullptr);c->OMSetDepthStencilState(nullptr,0);
    // A rendered preview can exceed the output extent and need not be at t0.
    // Opaque blending ignores shader alpha, so its mask must do the same.
    FfxivHdrPreview::EndFrame();D3D11_TEXTURE2D_DESC large{};large.Width=large.Height=1024;large.MipLevels=large.ArraySize=large.SampleDesc.Count=1;
    large.Format=DXGI_FORMAT_R8G8B8A8_UNORM;large.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> largeSource;check(d->CreateTexture2D(&large,nullptr,&largeSource));
    ComPtr<ID3D11ShaderResourceView> largeView;check(d->CreateShaderResourceView(largeSource.Get(),nullptr,&largeView));
    c->PSSetShaderResources(3,1,largeView.GetAddressOf());c->OMSetBlendState(nullptr,nullptr,~0u);
    const FLOAT zeroAlpha[4]{1,1,1,0};c->ClearRenderTargetView(inputRtv.Get(),zeroAlpha);
    FfxivHdrPreview::BeforeDraw(c.Get(),draw);expect(bool(FfxivHdrPreview::State().current),"Large/non-t0 preview rejected");
    c->CopyResource(read.Get(),FfxivHdrPreview::State().current->texture.Get());D3D11_MAPPED_SUBRESOURCE opaqueMapped{};
    check(c->Map(read.Get(),0,D3D11_MAP_READ,0,&opaqueMapped));expect(static_cast<unsigned char*>(opaqueMapped.pData)[4*100+3]==0,"Opaque draw inherited zero shader alpha");c->Unmap(read.Get(),0);
    ID3D11ShaderResourceView* nullView=nullptr;c->PSSetShaderResources(3,1,&nullView);
    c->OMSetBlendState(blend.Get(),nullptr,~0u);
    FfxivHdrPreview::EndFrame();source.Reset(); // Atlas panels also contribute real opacity.
    td.Width=64;td.Height=64;td.Usage=D3D11_USAGE_DEFAULT;td.CPUAccessFlags=0;td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    std::vector<unsigned> atlas(64*64,0x80ffffffu);D3D11_SUBRESOURCE_DATA atlasData{atlas.data(),64*4,0};
    check(d->CreateTexture2D(&td,&atlasData,&source));check(d->CreateShaderResourceView(source.Get(),nullptr,&input));c->PSSetShaderResources(0,1,input.GetAddressOf());
    FfxivHdrPreview::BeforeDraw(c.Get(),draw);expect(bool(FfxivHdrPreview::State().current),"Atlas-backed panel opacity omitted");
    c->CopyResource(read.Get(),FfxivHdrPreview::State().current->texture.Get());D3D11_MAPPED_SUBRESOURCE atlasMapped{};
    check(c->Map(read.Get(),0,D3D11_MAP_READ,0,&atlasMapped));
    expect(std::abs(int(static_cast<unsigned char*>(atlasMapped.pData)[4*100])-127)<=1,"Transparent atlas panel became opaque");c->Unmap(read.Get(),0);
    FfxivHdrPreview::BeforeDraw(c.Get(),draw);
    c->CopyResource(read.Get(),FfxivHdrPreview::State().current->texture.Get());
    check(c->Map(read.Get(),0,D3D11_MAP_READ,0,&atlasMapped));
    expect(std::abs(int(static_cast<unsigned char*>(atlasMapped.pData)[4*100])-63)<=1,"Layered UI transmittance did not multiply");c->Unmap(read.Get(),0);
    // Freeze coverage at the bridge boundary; later native UI draws belong to
    // the next frame and must not mutate the mask consumed by DX12.
    FfxivHdrPreview::SealFrame();
    auto& state=FfxivHdrPreview::State();const auto before=state.draws;
    FfxivHdrPreview::BeforeDraw(c.Get(),draw);
    expect(state.draws==before,"Preview mask changed after the bridge frame boundary");
    c->CopyResource(read.Get(),state.current->texture.Get());
    check(c->Map(read.Get(),0,D3D11_MAP_READ,0,&atlasMapped));
    expect(std::abs(int(static_cast<unsigned char*>(atlasMapped.pData)[4*100])-63)<=1,"Sealed layered coverage changed");
    c->Unmap(read.Get(),0);
    FfxivHdrPreview::EndFrame();FfxivHdrPreview::BeforeDraw(c.Get(),draw);
    expect(state.draws==before+1,"Preview coverage did not resume next frame");
    puts("PASS: preview alpha/viewport, opaque/transparent coverage, depth/stencil safety, large/non-t0 images, binding restoration and transparent atlas coverage (D3D11 WARP)");return 0;
}catch(const std::exception& e){printf("FAIL: %s\n",e.what());return 1;}
