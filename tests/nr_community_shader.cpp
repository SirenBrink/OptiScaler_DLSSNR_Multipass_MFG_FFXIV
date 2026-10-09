#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <vector>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include "../OptiScaler/shaders/dlssnr/DlssNr_Spatial.h"
using Microsoft::WRL::ComPtr;
#include "../OptiScaler/shaders/dlssnr/DlssNr_Common.h"
using namespace DlssNr::Spatial;
void check(HRESULT r) { if (FAILED(r)) throw std::runtime_error("D3D failure"); }
void expect(bool v) { if (!v) throw std::runtime_error("Spatial shader reference mismatch"); }
struct Texture { ComPtr<ID3D11Texture2D> tex; ComPtr<ID3D11ShaderResourceView> srv; ComPtr<ID3D11UnorderedAccessView> uav; unsigned w,h,channels; };
struct Runner {
    ComPtr<ID3D11Device> d; ComPtr<ID3D11DeviceContext> c; ComPtr<ID3D11ComputeShader> shaders[3];
    ComPtr<ID3D11Buffer> cb; ComPtr<ID3D11SamplerState> sampler;
    Runner(wchar_t** paths) {
        check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&d,nullptr,&c));
        for (unsigned i=0;i<3;++i) {
            ComPtr<ID3DBlob> code,error;
            auto hr=D3DCompileFromFile(paths[i+1],nullptr,D3D_COMPILE_STANDARD_FILE_INCLUDE,"CSMain","cs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&error);
            if(error) std::printf("%s",(const char*)error->GetBufferPointer());check(hr);
            check(d->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&shaders[i]));
        }
        D3D11_BUFFER_DESC bd{}; bd.ByteWidth=sizeof(Constants);bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;check(d->CreateBuffer(&bd,nullptr,&cb));
        D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sd.MaxLOD=D3D11_FLOAT32_MAX;check(d->CreateSamplerState(&sd,&sampler));
    }
    Texture make(unsigned w,unsigned h,unsigned channels,const std::vector<float>& data={}) {
        Texture t{};t.w=w;t.h=h;t.channels=channels;
        D3D11_TEXTURE2D_DESC td{};td.Width=w;td.Height=h;td.MipLevels=td.ArraySize=1;td.SampleDesc.Count=1;
        td.Format=channels==1?DXGI_FORMAT_R32_FLOAT:channels==2?DXGI_FORMAT_R32G32_FLOAT:DXGI_FORMAT_R32G32B32A32_FLOAT;
        td.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
        D3D11_SUBRESOURCE_DATA init{data.data(),w*channels*4,0};check(d->CreateTexture2D(&td,data.empty()?nullptr:&init,&t.tex));
        check(d->CreateShaderResourceView(t.tex.Get(),nullptr,&t.srv));check(d->CreateUnorderedAccessView(t.tex.Get(),nullptr,&t.uav));return t;
    }
    void run(const DlssNrConstants& k,Texture& a,Texture& b,Texture& m,Texture& out,Texture& second,unsigned pipeline=0) {
        ID3D11ShaderResourceView* sv[]={a.srv.Get(),b.srv.Get(),m.srv.Get(),m.srv.Get(),m.srv.Get()};ID3D11UnorderedAccessView* uv[]={out.uav.Get(),second.uav.Get()};
        c->UpdateSubresource(cb.Get(),0,nullptr,&k,0,0);c->CSSetShader(shaders[pipeline].Get(),nullptr,0);
        c->CSSetShaderResources(0,5,sv);c->CSSetUnorderedAccessViews(0,2,uv,nullptr);c->CSSetConstantBuffers(0,1,cb.GetAddressOf());c->CSSetSamplers(0,1,sampler.GetAddressOf());
        c->Dispatch((k.Width+7)/8,(k.Height+7)/8,1);
        ID3D11ShaderResourceView* ns[5]={};ID3D11UnorderedAccessView* nu[2]={};c->CSSetShaderResources(0,5,ns);c->CSSetUnorderedAccessViews(0,2,nu,nullptr);
    }
    std::vector<float> read(Texture& t) {
        D3D11_TEXTURE2D_DESC desc{};t.tex->GetDesc(&desc);desc.BindFlags=0;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;check(d->CreateTexture2D(&desc,nullptr,&staging));c->CopyResource(staging.Get(),t.tex.Get());
        D3D11_MAPPED_SUBRESOURCE map{};check(c->Map(staging.Get(),0,D3D11_MAP_READ,0,&map));std::vector<float> result(t.w*t.h*t.channels);
        for(unsigned y=0;y<t.h;++y)memcpy(result.data()+y*t.w*t.channels,(char*)map.pData+y*map.RowPitch,t.w*t.channels*4);
        c->Unmap(staging.Get(),0);return result;
    }
};

int wmain(int argc,wchar_t** argv) try {
    expect(argc==4);Runner r(argv);
    static_assert(sizeof(DlssNrConstants)==256);
    static_assert(offsetof(DlssNrConstants,SpatialWarp)==144);
    // Actual typed scalar/RG UAVs and independent offset/padded source regions.
    std::vector<float> dep(16*10), mv(20*12*2);
    for(unsigned y=0;y<10;++y)for(unsigned x=0;x<16;++x)dep[y*16+x]=float(x+100*y);
    for(unsigned y=0;y<12;++y)for(unsigned x=0;x<20;++x){mv[(y*20+x)*2]=float(x);mv[(y*20+x)*2+1]=-float(y);}
    auto d=r.make(16,10,1,dep),m=r.make(20,12,2,mv),od=r.make(5,3,1),om=r.make(5,3,2);
    DlssNrConstants k{};k.Mode=DlssNrMode_ResizePrivateGuides;k.Width=5;k.Height=3;
    k.GuideWidth=10;k.GuideHeight=6;k.DebugView=3;k.CompareMode=2;
    k.TransferStrength=12;k.ColourStrength=8;k.CompareSwap=4;k.Transfer=1;
    r.run(k,d,m,d,od,om,2);
    auto dd=r.read(od),mm=r.read(om);
    for(unsigned y=0;y<3;++y)for(unsigned x=0;x<5;++x){
        unsigned dx=3+unsigned((x+.5f)/5*10),dy=2+unsigned((y+.5f)/3*6);
        unsigned mx=4+unsigned((x+.5f)/5*12),my=1+unsigned((y+.5f)/3*8);
        expect(dd[y*5+x]==dep[dy*16+dx]);expect(mm[(y*5+x)*2]==float(mx));expect(mm[(y*5+x)*2+1]==-float(my));
    }
    std::puts("PASS: typed NR guide resize, padded independent regions, point depth and unchanged vector units");
    for(float scale : {1.0f,.75f}) { // shader maths validated independently of the production native-only gate
        Settings settings{};settings.enabled=true;settings.offsetX=7;settings.offsetY=-4;
        auto layout=Build(settings,320,160,scale);expect(layout.active);
        std::vector<float> proxy(layout.modelW*layout.modelH*4),answer(proxy.size()),base(320*160*4);
        for(unsigned y=0;y<layout.modelH;++y)for(unsigned x=0;x<layout.modelW;++x){auto i=(y*layout.modelW+x)*4;
            proxy[i]=.1f+.6f*x/layout.modelW;proxy[i+1]=.15f+.5f*y/layout.modelH;proxy[i+2]=.3f;proxy[i+3]=1;
            answer[i]=proxy[i]+.03f*std::sin(x*.13f);answer[i+1]=proxy[i+1]+.02f*std::cos(y*.19f);answer[i+2]=.32f;answer[i+3]=1;
        }
        for(unsigned y=0;y<160;++y)for(unsigned x=0;x<320;++x){auto i=(y*320+x)*4;base[i]=.1f+2.f*x/320;base[i+1]=.2f+1.5f*y/160;base[i+2]=.3f;base[i+3]=.8f;}
        auto p=r.make(layout.modelW,layout.modelH,4,proxy),a=r.make(layout.modelW,layout.modelH,4,answer),o=r.make(320,160,4,base);
        auto up=r.make(320,160,4),ua=r.make(320,160,4),out=r.make(320,160,4),keep=r.make(320,160,4);
        // Explicit legacy unpack onto the native uniform grid, then resolve.
        layout.ordinaryW=320;layout.ordinaryH=160;
        auto sc=MakeConstants(layout,102,{{0,0,320,160},{0,0,320,160}},1,1,320,160);
        DlssNrConstants bytes{};std::memcpy(&bytes,&sc,sizeof(sc));r.run(bytes,p,a,o,up,ua,1);
        for(unsigned hdr : {0u,1u})for(unsigned mode : {0u,1u,2u,3u,4u})for(float strength : {0.f,1.f,2.f}) {
            k={};k.Mode=DlssNrMode_Resolve;k.Width=320;k.Height=160;k.WhitePoint=1;
            k.Passthrough=hdr?0:1;k.ApplyModel=1;k.Transfer=1;k.SpatialResidual=1;k.ReversibleMode=mode;
            k.TransferStrength=strength;k.ColourStrength=1;k.MaxRatio=3;
            k.SkinProtection=1;k.SkinDetail=.6f;k.SkinColour=.7f;k.EnvironmentDetail=1;k.EnvironmentColour=1;
            r.run(k,up,ua,o,out,keep);auto legacy=r.read(out);
            k.SpatialResidual=2;const auto warp=pw::MakeShaderConstants(layout.warp);
            k.SpatialWorkSize[0]=warp.workWidth;k.SpatialWorkSize[1]=warp.workHeight;
            const float* fields[]={&warp.bandCenterX,&warp.halfSpanNegX,&warp.halfSpanPosX,&warp.sideWorkNegX,&warp.sideWorkPosX,&warp.sideEdgeSlopeNegX,&warp.workScaleX};
            for(unsigned i=0;i<7;++i)std::memcpy(k.SpatialWarp+i*4,fields[i],16);
            r.run(k,p,a,o,out,keep);auto fused=r.read(out);
            for(size_t i=0;i<fused.size();++i)expect(std::isfinite(fused[i]) && std::abs(fused[i]-legacy[i])<.0002f);
        }
    }
    std::puts("PASS: fused native spatial resolve matches legacy unpack/resolve (SDR/HDR, all reversible curves, strengths, skin protection, shifted periphery)");
    return 0;
} catch(const std::exception& e){std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
