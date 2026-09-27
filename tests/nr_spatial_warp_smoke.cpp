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
using namespace DlssNr::Spatial;
void check(HRESULT r) { if (FAILED(r)) throw std::runtime_error("D3D failure"); }
void expect(bool v) { if (!v) throw std::runtime_error("Spatial shader reference mismatch"); }
struct Texture { ComPtr<ID3D11Texture2D> tex; ComPtr<ID3D11ShaderResourceView> srv; ComPtr<ID3D11UnorderedAccessView> uav; unsigned w,h,channels; };
struct Runner {
    ComPtr<ID3D11Device> d; ComPtr<ID3D11DeviceContext> c; ComPtr<ID3D11ComputeShader> color,guides;
    ComPtr<ID3D11Buffer> cb; ComPtr<ID3D11SamplerState> sampler;
    Runner(const wchar_t* path) {
        check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&d,nullptr,&c));
        for (unsigned i=0;i<2;++i) {
            ComPtr<ID3DBlob> code,error; D3D_SHADER_MACRO defs[]={{"SPATIAL_GUIDES","1"},{nullptr,nullptr}};
            auto hr=D3DCompileFromFile(path,i?defs:nullptr,D3D_COMPILE_STANDARD_FILE_INCLUDE,"CSMain","cs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&error);
            if (error) std::printf("%s",(const char*)error->GetBufferPointer()); check(hr);
            check(d->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,i?guides.GetAddressOf():color.GetAddressOf()));
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
    void run(const Constants& k,Texture& a,Texture& b,Texture& m,Texture& out,Texture& second) {
        ID3D11ShaderResourceView* sv[]={a.srv.Get(),b.srv.Get(),m.srv.Get()};ID3D11UnorderedAccessView* uv[]={out.uav.Get(),second.uav.Get()};
        c->UpdateSubresource(cb.Get(),0,nullptr,&k,0,0);c->CSSetShader(k.mode==101?guides.Get():color.Get(),nullptr,0);
        c->CSSetShaderResources(0,3,sv);c->CSSetUnorderedAccessViews(0,2,uv,nullptr);c->CSSetConstantBuffers(0,1,cb.GetAddressOf());c->CSSetSamplers(0,1,sampler.GetAddressOf());
        c->Dispatch((k.width+7)/8,(k.height+7)/8,1);
        ID3D11ShaderResourceView* ns[3]={};ID3D11UnorderedAccessView* nu[2]={};c->CSSetShaderResources(0,3,ns);c->CSSetUnorderedAccessViews(0,2,nu,nullptr);
    }
    std::vector<float> read(Texture& t) {
        D3D11_TEXTURE2D_DESC desc{};t.tex->GetDesc(&desc);desc.BindFlags=0;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;check(d->CreateTexture2D(&desc,nullptr,&staging));c->CopyResource(staging.Get(),t.tex.Get());
        D3D11_MAPPED_SUBRESOURCE map{};check(c->Map(staging.Get(),0,D3D11_MAP_READ,0,&map));std::vector<float> result(t.w*t.h*t.channels);
        for(unsigned y=0;y<t.h;++y)memcpy(result.data()+y*t.w*t.channels,(char*)map.pData+y*map.RowPitch,t.w*t.channels*4);
        c->Unmap(staging.Get(),0);return result;
    }
};
int wmain(int argc,wchar_t**argv) try {
    expect(argc==2);Runner r(argv[1]);Settings s{};s.enabled=true;auto layout=Build(s,320,160,1.0f);expect(layout.active);
    DlssNr::GuideRegions regions{{4,3,320,160},{4,3,320,160}};
    std::vector<float> col(320*160*4),dep(328*168),mv(328*168*2);
    for(unsigned y=0;y<160;++y)for(unsigned x=0;x<320;++x){auto i=(y*320+x)*4;col[i]=(x+.5f)/320;col[i+1]=(y+.5f)/160;col[i+2]=.25;col[i+3]=.75;}
    for(unsigned y=0;y<168;++y)for(unsigned x=0;x<328;++x){dep[y*328+x]=float(x+y*2)/1000;mv[(y*328+x)*2]=2;mv[(y*328+x)*2+1]=-1;}
    auto color=r.make(320,160,4,col),depth=r.make(328,168,1,dep),motion=r.make(328,168,2,mv);
    auto packed=r.make(layout.modelW,layout.modelH,4),dummy=r.make(layout.modelW,layout.modelH,4);
    auto pd=r.make(layout.modelW,layout.modelH,1),pm=r.make(layout.modelW,layout.modelH,2);
    r.run(MakeConstants(layout,100,regions,1,1,320,160),color,color,color,packed,dummy);
    r.run(MakeConstants(layout,101,regions,1,1,320,160),color,depth,motion,pd,pm);
    const auto pc=r.read(packed),dd=r.read(pd),mm=r.read(pm);
    for(unsigned y=0;y<layout.modelH;++y)for(unsigned x=0;x<layout.modelW;++x){
        auto i=y*layout.modelW+x;float nx=pw::Unpack(x+.5f,layout.warp.x),ny=pw::Unpack(y+.5f,layout.warp.y);
        expect(std::abs(mm[i*2]-(pw::Pack(nx+2,layout.warp.x)-pw::Pack(nx,layout.warp.x)))<.003f);
        expect(std::abs(mm[i*2+1]-(pw::Pack(ny-1,layout.warp.y)-pw::Pack(ny,layout.warp.y)))<.003f);
        auto dx=std::clamp(int(4+nx),4,323),dy=std::clamp(int(3+ny),3,162);
        expect(std::abs(dd[i]-dep[dy*328+dx])<.00001f);expect(std::abs(pc[i*4+3]-.75f)<.00001f);
    }
    auto a=r.make(320,160,4),b=r.make(320,160,4);
    r.run(MakeConstants(layout,102,regions,1,1,320,160),packed,packed,packed,a,b);
    auto aa=r.read(a),bb=r.read(b);expect(aa==bb);
    for(unsigned y=8;y<152;++y)for(unsigned x=8;x<312;++x){auto i=(y*320+x)*4;
        expect(std::abs(aa[i]-col[i])<.004f && std::abs(aa[i+1]-col[i+1])<.007f);
    }
    std::puts("PASS: WARP spatial pack/unpack geometry, padded depth, endpoint-transformed motion, alpha and zero-edit identity");
    return 0;
} catch(const std::exception&e){std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
