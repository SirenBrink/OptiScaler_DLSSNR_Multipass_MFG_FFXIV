// Execute the real shared shader on WARP. Synthetic moving edits exercise a
// known trail input, not the proprietary DLSS history or FFXIV image quality.
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <vector>
#include <algorithm>
#include <utility>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include "../OptiScaler/shaders/dlssnr/DlssNr_Common.h"
using Microsoft::WRL::ComPtr;
struct Pixel { float r,g,b,a; };
void check(HRESULT hr) { if(FAILED(hr)) throw std::runtime_error("D3D11 failure"); }
void expect(bool ok,const char* why) { if(!ok) throw std::runtime_error(why); }
bool same(Pixel a,Pixel b) { return std::abs(a.r-b.r)<1e-5f && std::abs(a.g-b.g)<1e-5f && std::abs(a.b-b.b)<1e-5f && a.a==b.a; }
int wmain(int argc,wchar_t** argv) try {
    expect(argc==2,"Expected HLSL path");
    ComPtr<ID3DBlob> code,errors;
    auto result=D3DCompileFromFile(argv[1],nullptr,nullptr,"CSMain","cs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&errors);
    if(errors) std::fprintf(stderr,"%s",(char*)errors->GetBufferPointer()); check(result);
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> ctx;
    check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,D3D11_CREATE_DEVICE_DEBUG,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&ctx));
    ComPtr<ID3D11ComputeShader> shader; check(device->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&shader));
    for(const auto dims : {std::pair{16u,8u},std::pair{13u,7u}}) {
        const unsigned iw=8,ih=4,ow=dims.first,oh=dims.second;
        auto texture=[&](unsigned w,unsigned h,UINT flags,D3D11_USAGE usage=D3D11_USAGE_DEFAULT) {
            D3D11_TEXTURE2D_DESC d{}; d.Width=w;d.Height=h;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;
            d.Format=DXGI_FORMAT_R32G32B32A32_FLOAT; d.BindFlags=flags;d.Usage=usage;
            if(usage==D3D11_USAGE_STAGING) d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Texture2D> r;check(device->CreateTexture2D(&d,nullptr,&r));return r;
        };
        auto current=texture(iw,ih,D3D11_BIND_SHADER_RESOURCE), candidate=texture(ow,oh,D3D11_BIND_SHADER_RESOURCE);
        auto output=texture(ow,oh,D3D11_BIND_UNORDERED_ACCESS),keep=texture(ow,oh,D3D11_BIND_UNORDERED_ACCESS);
        auto staging=texture(ow,oh,0,D3D11_USAGE_STAGING);
        ComPtr<ID3D11ShaderResourceView> currentSrv,candidateSrv;
        check(device->CreateShaderResourceView(current.Get(),nullptr,&currentSrv));
        check(device->CreateShaderResourceView(candidate.Get(),nullptr,&candidateSrv));
        ComPtr<ID3D11UnorderedAccessView> outputUav,keepUav;
        check(device->CreateUnorderedAccessView(output.Get(),nullptr,&outputUav));check(device->CreateUnorderedAccessView(keep.Get(),nullptr,&keepUav));
        D3D11_BUFFER_DESC bd{};bd.ByteWidth=sizeof(DlssNrConstants);bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        ComPtr<ID3D11Buffer> cb;check(device->CreateBuffer(&bd,nullptr,&cb));
        D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sd.MaxLOD=D3D11_FLOAT32_MAX;
        ComPtr<ID3D11SamplerState> sampler;check(device->CreateSamplerState(&sd,&sampler));
        ID3D11ShaderResourceView* srvs[]{candidateSrv.Get(),currentSrv.Get(),currentSrv.Get(),currentSrv.Get(),currentSrv.Get()};
        ID3D11UnorderedAccessView* uavs[]{outputUav.Get(),keepUav.Get()};
        ctx->CSSetShader(shader.Get(),nullptr,0);ctx->CSSetShaderResources(0,5,srvs);ctx->CSSetUnorderedAccessViews(0,2,uavs,nullptr);
        ctx->CSSetConstantBuffers(0,1,cb.GetAddressOf());ctx->CSSetSamplers(0,1,sampler.GetAddressOf());
        DlssNrConstants settings{};settings.Mode=DlssNrMode_BoundResidual;settings.Width=ow;settings.Height=oh;settings.GuideWidth=iw;settings.GuideHeight=ih;
        ctx->UpdateSubresource(cb.Get(),0,nullptr,&settings,0,0);
        std::vector<Pixel> input(iw*ih),raw(ow*oh);
        auto run=[&]() {
            ctx->UpdateSubresource(current.Get(),0,nullptr,input.data(),iw*sizeof(Pixel),0);
            ctx->UpdateSubresource(candidate.Get(),0,nullptr,raw.data(),ow*sizeof(Pixel),0);
            ctx->Dispatch((ow+7)/8,(oh+7)/8,1);ctx->CopyResource(staging.Get(),output.Get());
            D3D11_MAPPED_SUBRESOURCE map{};check(ctx->Map(staging.Get(),0,D3D11_MAP_READ,0,&map));
            std::vector<Pixel> pixels(ow*oh);
            for(unsigned y=0;y<oh;++y) memcpy(pixels.data()+y*ow,(char*)map.pData+y*map.RowPitch,ow*sizeof(Pixel));
            ctx->Unmap(staging.Get(),0);return pixels;
        };
        const Pixel neutral{.5f,.5f,.5f,1}, edit{.75f,.35f,.65f,1};
        std::fill(input.begin(),input.end(),neutral);std::fill(raw.begin(),raw.end(),edit);
        for(auto p:run()) expect(same(p,neutral),"A departed edit still trails on a neutral frame");
        std::fill(input.begin(),input.end(),edit);
        for(auto p:run()) expect(same(p,edit),"Stable supported edit was changed");
        // The current stripe moves right; old residual on the left is unsupported.
        for(unsigned frame=2;frame<7;++frame) {
            std::fill(input.begin(),input.end(),neutral);std::fill(raw.begin(),raw.end(),edit);
            for(unsigned y=0;y<ih;++y) input[y*iw+frame]=edit;
            auto pixels=run();
            for(unsigned y=0;y<oh;++y) for(unsigned x=0;x<ow;++x) {
                unsigned centre=unsigned((x+.5f)*iw/ow);
                if(centre+1<frame || centre>frame+1) expect(same(pixels[y*ow+x],neutral),"Moving edit left a long trail");
            }
        }
        for(unsigned y=0;y<ih;++y) for(unsigned x=0;x<iw;++x) {float v=x%2?.6f:.4f;input[y*iw+x]={v,v,v,1};}
        const Pixel valid{.58f,.44f,.51f,1};std::fill(raw.begin(),raw.end(),valid);
        for(auto p:run()) expect(same(p,valid),"Within-neighbourhood reconstructed detail was changed");
        std::fill(raw.begin(),raw.end(),Pixel{.95f,.75f,.35f,1});
        auto clipped=run();
        if(ow==16) expect(same(clipped[3*ow+3],Pixel{.6f,.575f,.525f,1}),"RGB did not use one common clipping fraction");
        for(auto p:clipped) expect(p.r>=.39999f && p.r<=.60001f && p.g>=.39999f && p.g<=.60001f && p.b>=.39999f && p.b<=.60001f,"Result escaped current bounds");
        std::fill(input.begin(),input.end(),Pixel{NAN,INFINITY,NAN,1});std::fill(raw.begin(),raw.end(),Pixel{INFINITY,NAN,INFINITY,1});
        for(auto p:run()) expect(same(p,neutral),"Nonfinite edits were not neutralized");
        ctx->ClearState();
    }
    std::puts("PASS: actual PreSR bounds shader: moving/removed edits, supported detail, common RGB fraction, borders, noninteger scaling, nonfinite inputs (WARP)");
    return 0;
} catch(const std::exception& e) {std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
