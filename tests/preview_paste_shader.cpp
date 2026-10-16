#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <fstream>
#include <string>
#include <vector>
#include <array>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#pragma comment(lib,"d3d11.lib")
#pragma comment(lib,"d3dcompiler.lib")
using Microsoft::WRL::ComPtr;
struct Pixel { float r,g,b,a; };
void check(HRESULT hr) { if(FAILED(hr))throw std::runtime_error("D3D call failed"); }
void expect(float got,float wanted,const char* why) {
    if(std::abs(got-wanted)>.002f){printf("got %.4f, expected %.4f: ",got,wanted);throw std::runtime_error(why);}
}
int main(int argc,char** argv) try {
    if(argc!=2)throw std::runtime_error("Expected UiPaste.cpp path");
    std::ifstream file(argv[1]);std::string source((std::istreambuf_iterator<char>(file)),{});
    const std::string marker="const char* PasteShaderCode = R\"(";
    auto begin=source.find(marker);if(begin==std::string::npos)throw std::runtime_error("Missing production paste shader");
    begin+=marker.size();auto end=source.find(")\";",begin);source=source.substr(begin,end-begin);
    ComPtr<ID3D11Device> d;ComPtr<ID3D11DeviceContext> c;
    check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&d,nullptr,&c));
    auto compile=[&](const char* entry,const char* target){
        ComPtr<ID3DBlob> code,error;auto hr=D3DCompile(source.data(),source.size(),nullptr,nullptr,nullptr,entry,target,
            D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&error);
        if(FAILED(hr)&&error)printf("%s\n",static_cast<const char*>(error->GetBufferPointer()));check(hr);return code;};
    auto vs=compile("VSMain","vs_5_0"),ps=compile("PSMain","ps_5_0");
    ComPtr<ID3D11VertexShader> vertex;ComPtr<ID3D11PixelShader> pixel;
    check(d->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,&vertex));
    check(d->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,&pixel));
    constexpr unsigned W=64,H=32;
    D3D11_TEXTURE2D_DESC desc{};desc.Width=W;desc.Height=H;desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;
    desc.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;desc.BindFlags=D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> output,read;ComPtr<ID3D11RenderTargetView> target;
    check(d->CreateTexture2D(&desc,nullptr,&output));check(d->CreateRenderTargetView(output.Get(),nullptr,&target));
    desc.BindFlags=0;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    check(d->CreateTexture2D(&desc,nullptr,&read));
    D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sd.MaxLOD=D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> sampler;check(d->CreateSamplerState(&sd,&sampler));
    std::array<std::vector<Pixel>,5> images;
    auto fill=[&](float old,float now){
        images[0].assign(W*H,{0,.25f,0,1});images[1].assign(W*H,{128.f/255,128.f/255,1.f/16,1});
        images[2].assign(W*H,{now,now,now,1});images[3].assign(W*H,{old,old,old,1});
        images[4].assign(W/4*H/4,{24,0,1,1});};
    auto run=[&](float phase,bool flow){
        c->ClearState();std::array<ComPtr<ID3D11ShaderResourceView>,5> views;ID3D11ShaderResourceView* raw[5]{};
        for(unsigned i=0;i<5;++i){
            desc.Width=i==4?W/4:W;desc.Height=i==4?H/4:H;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
            desc.Usage=D3D11_USAGE_IMMUTABLE;desc.CPUAccessFlags=0;
            D3D11_SUBRESOURCE_DATA data{images[i].data(),desc.Width*sizeof(Pixel),0};ComPtr<ID3D11Texture2D> texture;
            check(d->CreateTexture2D(&desc,&data,&texture));check(d->CreateShaderResourceView(texture.Get(),nullptr,&views[i]));raw[i]=views[i].Get();}
        struct Params{unsigned tint,use;float phase;unsigned paired,flow,pad[3];}params{0,1,phase,1,flow?1u:0u,{}};
        D3D11_BUFFER_DESC bd{};bd.ByteWidth=sizeof(params);bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA data{&params,0,0};ComPtr<ID3D11Buffer> constants;check(d->CreateBuffer(&bd,&data,&constants));
        c->VSSetShader(vertex.Get(),nullptr,0);c->PSSetShader(pixel.Get(),nullptr,0);c->PSSetConstantBuffers(0,1,constants.GetAddressOf());
        c->PSSetShaderResources(0,5,raw);c->PSSetSamplers(0,1,sampler.GetAddressOf());
        D3D11_VIEWPORT vp{0,0,float(W),float(H),0,1};c->RSSetViewports(1,&vp);c->OMSetRenderTargets(1,target.GetAddressOf(),nullptr);
        c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);c->Draw(3,0);c->CopyResource(read.Get(),output.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};check(c->Map(read.Get(),0,D3D11_MAP_READ,0,&mapped));std::vector<Pixel> result(W*H);
        for(unsigned y=0;y<H;++y)memcpy(result.data()+y*W,static_cast<const char*>(mapped.pData)+y*mapped.RowPitch,W*sizeof(Pixel));
        c->Unmap(read.Get(),0);return result;};
    auto at=[](const std::vector<Pixel>& p,unsigned x){return p[16*W+x].r;};
    fill(.2f,.8f);expect(at(run(0,false),20),.2f,"Previous endpoint changed");expect(at(run(1,false),20),.8f,"Current endpoint changed");
    expect(at(run(.25f,false),20),.2f,"Disocclusion dissolved two incompatible surfaces");
    expect(at(run(.75f,false),20),.8f,"Disocclusion retained the obsolete surface");
    fill(.4f,.41f);expect(at(run(.25f,false),20),.4025f,"Aligned detail no longer blends smoothly");
    fill(.2f,.8f);
    for(unsigned y=0;y<H;++y)for(unsigned x=0;x<W;++x){images[1][y*W+x].r=(128.f+16.f*255.f/128.f)/255.f;
        if(x>=32)images[1][y*W+x].b=2.f/16;}
    expect(at(run(.25f,false),30),.8f,"Warp crossed into another portrait's history");
    images[1][16*W+2].a=0;expect(run(.25f,false)[16*W+2].g,.25f,"Non-preview HUD changed");
    fill(.8f,.4f);for(unsigned y=0;y<H;++y)for(unsigned x=16;x<=24;++x)images[3][y*W+x]={.4f,.4f,.4f,1};
    expect(at(run(.25f,true),20),.4f,"Filtered background flow overrode correct full-resolution motion");
    fill(.8f,.4f);for(unsigned y=0;y<H;++y)for(unsigned x=25;x<W;++x)images[3][y*W+x]={.4f,.4f,.4f,1};
    expect(at(run(.25f,true),20),.4f,"A verified optical-flow correction was rejected");
    puts("PASS: production preview shader endpoints, occlusions, aligned detail, portrait boundaries, unchanged HUD and full-resolution flow validation");
    return 0;
} catch(const std::exception& e){printf("FAIL: %s\n",e.what());return 1;}
