#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cassert>
#include <cstdio>
#include <vector>
#include <cstring>
#include "../OptiScaler/misc/ExternalHudless.h"
#include "../OptiScaler/misc/ExternalHudlessNormalize.h"
using Microsoft::WRL::ComPtr;
void check(HRESULT hr){assert(SUCCEEDED(hr));}
int wmain(int argc,wchar_t** argv){
    assert(argc==3);
    static_assert(sizeof(ExternalHudless::StatusV1)==216,"Addon ABI changed");
    for(int bits=0;bits<16;++bits)
        assert(ExternalHudless::Eligible(bits&1,bits&2,bits&4,bits&8)==(bits==3));
    ComPtr<ID3D11Device> d;ComPtr<ID3D11DeviceContext> c;
    check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&d,nullptr,&c));
    assert(!ExternalHudlessNeedsNormalization(DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R8G8B8A8_UNORM));
    assert(!ExternalHudlessNeedsNormalization(DXGI_FORMAT_B8G8R8A8_UNORM,DXGI_FORMAT_B8G8R8A8_UNORM));
    for(auto format : {DXGI_FORMAT_B8G8R8A8_UNORM,DXGI_FORMAT_B8G8R8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_TYPELESS}) {
        const unsigned char bgra[8]={17,51,221,123,245,19,7,255};
        D3D11_TEXTURE2D_DESC td{};td.Width=2;td.Height=1;td.MipLevels=td.ArraySize=1;
        td.Format=format;td.SampleDesc.Count=1;td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data{bgra,8,0};ComPtr<ID3D11Texture2D> input,output,read;
        check(d->CreateTexture2D(&td,&data,&input));
        td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;td.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
        check(d->CreateTexture2D(&td,nullptr,&output));
        const bool isBGRA=format==DXGI_FORMAT_B8G8R8A8_UNORM || format==DXGI_FORMAT_B8G8R8A8_TYPELESS;
        const bool normalize=ExternalHudlessNeedsNormalization(isBGRA?DXGI_FORMAT_B8G8R8A8_UNORM:DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R8G8B8A8_UNORM);
        if(normalize) {ExternalHudlessNormalize normalizer;assert(normalizer.Copy(d.Get(),c.Get(),input.Get(),output.Get()));}
        else c->CopyResource(output.Get(),input.Get());
        ComPtr<ID3D11ComputeShader> restored;ComPtr<ID3D11ShaderResourceView> restoredSrv;
        ComPtr<ID3D11UnorderedAccessView> restoredUav;
        c->CSGetShader(&restored,nullptr,nullptr);c->CSGetShaderResources(0,1,&restoredSrv);
        c->CSGetUnorderedAccessViews(0,1,&restoredUav);assert(!restored && !restoredSrv && !restoredUav);
        td.BindFlags=0;td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        check(d->CreateTexture2D(&td,nullptr,&read));c->CopyResource(read.Get(),output.Get());
        D3D11_MAPPED_SUBRESOURCE map{};check(c->Map(read.Get(),0,D3D11_MAP_READ,0,&map));
        const unsigned char rgba[8]={221,51,17,123,7,19,245,255};assert(!memcmp(map.pData,normalize?rgba:bgra,8));
        c->Unmap(read.Get(),0);
    }
    puts("PASS: production typed/typeless BGRA to RGBA normalization and matching-format direct copies preserve colour, alpha and DX11 state");
    struct Tex{ComPtr<ID3D11Texture2D> t;ComPtr<ID3D11ShaderResourceView> s;ComPtr<ID3D11UnorderedAccessView> u;};
    auto make=[&](const std::vector<float>& pixels){
        Tex t;D3D11_TEXTURE2D_DESC td{};td.Width=5;td.Height=1;td.MipLevels=td.ArraySize=1;
        td.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;td.SampleDesc.Count=1;
        td.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
        D3D11_SUBRESOURCE_DATA data{pixels.data(),80,0};check(d->CreateTexture2D(&td,&data,&t.t));
        check(d->CreateShaderResourceView(t.t.Get(),nullptr,&t.s));check(d->CreateUnorderedAccessView(t.t.Get(),nullptr,&t.u));return t;
    };
    std::vector<float> scene(20,.2f),final=scene,previous(20,0),zeros(20,0);
    for(int i=0;i<5;++i)scene[i*4+3]=final[i*4+3]=1;
    final[8]=.8f;previous[3]=1;
    auto hdrPixels=final; for (auto& v : hdrPixels) v *= .5f;
    auto a=make(final),b=make(scene),p=make(previous),out=make(zeros),hdrImage=make(hdrPixels);
    struct K{float threshold;unsigned radius,width,height,hasPrevious,cleanup,useHDR,pad;};
    ComPtr<ID3D11Buffer> cb;D3D11_BUFFER_DESC bd{};bd.ByteWidth=32;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    check(d->CreateBuffer(&bd,nullptr,&cb));
    for(int shader=1;shader<=2;++shader){
        ComPtr<ID3DBlob> code,error;check(D3DCompileFromFile(argv[shader],nullptr,D3D_COMPILE_STANDARD_FILE_INCLUDE,"CSMain","cs_5_0",0,0,&code,&error));
        ComPtr<ID3D11ComputeShader> cs;check(d->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&cs));
        for(unsigned radius=0;radius<=1;++radius)for(unsigned cleanup=0;cleanup<=1;++cleanup)for(unsigned hdr=0;hdr<=(shader==2?1u:0u);++hdr){
            K k{.01f,radius,5,1,1,cleanup,hdr,0};c->UpdateSubresource(cb.Get(),0,nullptr,&k,0,0);
            ID3D11ShaderResourceView* srv[]={a.s.Get(),b.s.Get(),p.s.Get(),hdrImage.s.Get()};auto* uav=out.u.Get();auto* buffer=cb.Get();
            c->CSSetShader(cs.Get(),nullptr,0);c->CSSetConstantBuffers(0,1,&buffer);c->CSSetShaderResources(0,4,srv);c->CSSetUnorderedAccessViews(0,1,&uav,nullptr);c->Dispatch(1,1,1);
            ID3D11ShaderResourceView* none[4]={};ID3D11UnorderedAccessView* no=nullptr;
            c->CSSetShaderResources(0,4,none);c->CSSetUnorderedAccessViews(0,1,&no,nullptr);
            D3D11_TEXTURE2D_DESC td{};out.t->GetDesc(&td);td.BindFlags=0;td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Texture2D> read;check(d->CreateTexture2D(&td,nullptr,&read));c->CopyResource(read.Get(),out.t.Get());
            D3D11_MAPPED_SUBRESOURCE map{};check(c->Map(read.Get(),0,D3D11_MAP_READ,0,&map));auto* values=(float*)map.pData;
            for(unsigned i=0;i<5;++i){
                const bool ui=i==2 || (radius==1 && (i==1 || i==3));
                const float alpha=ui?1.f:(shader==2 && cleanup && i==0?.5f:0.f);
                assert(values[i*4+3]==alpha);
                assert(values[i*4]==(alpha?(hdr?hdrPixels[i*4]:final[i*4]):0.f));
            }
            c->Unmap(read.Get(),0);
        }
    }
    puts("PASS: addon eligibility/ABI and production UI shaders on WARP: threshold, dilation, cleanup, SDR mask/HDR colour separation and untouched scene");
}
