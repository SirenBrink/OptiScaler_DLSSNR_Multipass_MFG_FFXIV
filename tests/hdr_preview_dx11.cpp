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
#include "../OptiScaler/misc/FfxivPreviewTrace.h"
#include "../OptiScaler/misc/FfxivPreviewEnhance.h"
#include "../OptiScaler/misc/FfxivPreviewGuides.h"
#include "../OptiScaler/misc/FfxivPreviewMotion.h"
#include "../OptiScaler/misc/FfxivPreviewDLSSShaders.h"
#include "../OptiScaler/misc/FfxivPreviewCamera.h"
#pragma comment(lib,"d3d11.lib")
#pragma comment(lib,"d3dcompiler.lib")
using Microsoft::WRL::ComPtr;
void check(HRESULT hr){if(FAILED(hr))throw std::runtime_error("D3D11 failure");}
void expect(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
int main() try {
    {
        using namespace FfxivCharaViewTrace;
        unsigned char code[64]{0x48,0x8b,0x05,0,1,0,0,0x8b,0xca,0x48,0x8b,0x84,0xc8,0xc8,0x02,0,0,0xc3};
        expect(FindAccessor(code,sizeof(code),0x1000)==0x1107,"CharaView accessor displacement incorrect");
        std::memcpy(code+32,code,18);
        expect(FindAccessor(code,sizeof(code),0x1000)==0,"Ambiguous CharaView accessor accepted");
        expect(FindAccessor(code,17,0x1000)==0,"Truncated CharaView accessor accepted");
        std::array<unsigned char,0x400> manager{};std::array<unsigned char,0x70> texture{};
        const UINT width=256,height=512;const UINT_PTR identity=0x123456;
        std::memcpy(texture.data()+0x40,&width,4);std::memcpy(texture.data()+0x44,&height,4);
        std::memcpy(texture.data()+0x68,&identity,sizeof(identity));
        const auto textureAddress=reinterpret_cast<UINT_PTR>(texture.data());
        std::memcpy(manager.data()+0x2c8,&textureAddress,sizeof(textureAddress));
        UINT_PTR managerAddress=reinterpret_cast<UINT_PTR>(manager.data());
        const auto snapshot=ReadSnapshot(reinterpret_cast<UINT_PTR>(&managerAddress));
        expect(Match(snapshot,reinterpret_cast<void*>(identity),width,height)==0,"Exact CharaView output not identified");
        expect(Match(snapshot,reinterpret_cast<void*>(identity),width+1,height)==-1,"Recycled CharaView identity with wrong extent accepted");
        expect(Match(ReadSnapshot(1),reinterpret_cast<void*>(identity),width,height)==-1,"Unreadable engine pointer accepted");
        puts("PASS: CharaView accessor uniqueness, truncated input rejection, read-only texture lookup and extent validation");
    }
    ComPtr<ID3D11Device> d;ComPtr<ID3D11DeviceContext> c;
    check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,D3D11_CREATE_DEVICE_DEBUG,nullptr,0,D3D11_SDK_VERSION,&d,nullptr,&c));
    {
        ComPtr<ID3D11DeviceContext1> c1;check(c.As(&c1));
        for(UINT bytes: {1024u,2048u}) {
            std::vector<unsigned char> data(bytes);for(UINT i=0;i<bytes;++i)data[i]=static_cast<unsigned char>(i*17);
            D3D11_BUFFER_DESC desc{};desc.ByteWidth=bytes;desc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
            D3D11_SUBRESOURCE_DATA initial{data.data(),0,0};ComPtr<ID3D11Buffer> native;check(d->CreateBuffer(&desc,&initial,&native));
            UINT first=bytes==2048?16:0,count=64;c1->PSSetConstantBuffers1(2,1,native.GetAddressOf(),&first,&count);
            FfxivPreviewCamera::Snapshot snapshot;expect(FfxivPreviewCamera::Capture(c.Get(),snapshot),"Padded or ranged native camera rejected");
            FfxivPreviewCamera::Bind(c.Get(),snapshot);ComPtr<ID3D11Buffer> bound;UINT gotFirst=0,gotCount=0;c1->PSGetConstantBuffers1(1,1,&bound,&gotFirst,&gotCount);
            expect(bound==snapshot.buffer && bound!=native && gotFirst==first && gotCount==count,"Private camera binding lost its native range");
            desc.BindFlags=0;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Buffer> read;check(d->CreateBuffer(&desc,nullptr,&read));c->CopyResource(read.Get(),snapshot.buffer.Get());D3D11_MAPPED_SUBRESOURCE mapped{};
            check(c->Map(read.Get(),0,D3D11_MAP_READ,0,&mapped));expect(!std::memcmp(mapped.pData,data.data(),bytes),"Private camera copy changed contents");c->Unmap(read.Get(),0);
            c1->PSGetConstantBuffers1(2,1,&bound,&gotFirst,&gotCount);expect(bound==native && gotFirst==first && gotCount==count,"Camera capture changed native binding");
        }
        D3D11_BUFFER_DESC undersized{};undersized.ByteWidth=256;undersized.BindFlags=D3D11_BIND_CONSTANT_BUFFER;ComPtr<ID3D11Buffer> invalid;check(d->CreateBuffer(&undersized,nullptr,&invalid));
        c->PSSetConstantBuffers(2,1,invalid.GetAddressOf());FfxivPreviewCamera::Snapshot snapshot;expect(!FfxivPreviewCamera::Capture(c.Get(),snapshot),"Undersized camera accepted");
        c->ClearState();puts("PASS: padded/ranged preview camera copies preserve all contents, native bindings and range; short buffers rejected");
    }
    {
        // Reproduce the native disappearance: a shifted colour pass cannot
        // use EQUAL against an unshifted depth prepass on a sloped surface.
        const char* shader="float4 VS(uint id:SV_VertexID):SV_Position{float x=id==2?3:-1;return float4(x,id==1?3:-1,.5+x*.1,1);}float4 PS():SV_Target{return float4(1,1,1,1);}";
        ComPtr<ID3DBlob> compiled,error;ComPtr<ID3D11VertexShader> vertex;ComPtr<ID3D11PixelShader> pixel;
        check(D3DCompile(shader,strlen(shader),nullptr,nullptr,nullptr,"VS","vs_5_0",0,0,&compiled,&error));check(d->CreateVertexShader(compiled->GetBufferPointer(),compiled->GetBufferSize(),nullptr,&vertex));compiled.Reset();
        check(D3DCompile(shader,strlen(shader),nullptr,nullptr,nullptr,"PS","ps_5_0",0,0,&compiled,&error));check(d->CreatePixelShader(compiled->GetBufferPointer(),compiled->GetBufferSize(),nullptr,&pixel));
        D3D11_TEXTURE2D_DESC td{};td.Width=td.Height=16;td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;td.BindFlags=D3D11_BIND_RENDER_TARGET;
        ComPtr<ID3D11Texture2D> colour,depth,read;check(d->CreateTexture2D(&td,nullptr,&colour));ComPtr<ID3D11RenderTargetView> target;check(d->CreateRenderTargetView(colour.Get(),nullptr,&target));
        td.Format=DXGI_FORMAT_D32_FLOAT;td.BindFlags=D3D11_BIND_DEPTH_STENCIL;check(d->CreateTexture2D(&td,nullptr,&depth));ComPtr<ID3D11DepthStencilView> dsv;check(d->CreateDepthStencilView(depth.Get(),nullptr,&dsv));
        td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;td.BindFlags=0;td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;check(d->CreateTexture2D(&td,nullptr,&read));
        D3D11_DEPTH_STENCIL_DESC dd{};dd.DepthEnable=true;ComPtr<ID3D11DepthStencilState> equal,preDepth;bool reversed=false;
        auto pixels=[&](float depthJitter,float colourJitter){
            c->ClearState();const FLOAT zero[4]{};c->ClearRenderTargetView(target.Get(),zero);c->ClearDepthStencilView(dsv.Get(),D3D11_CLEAR_DEPTH,reversed?0:1,0);
            c->VSSetShader(vertex.Get(),nullptr,0);c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            D3D11_VIEWPORT vp{depthJitter,0,16,16,0,1};c->RSSetViewports(1,&vp);c->OMSetRenderTargets(0,nullptr,dsv.Get());c->OMSetDepthStencilState(preDepth.Get(),0);c->Draw(3,0);
            vp.TopLeftX=colourJitter;c->RSSetViewports(1,&vp);c->OMSetRenderTargets(1,target.GetAddressOf(),dsv.Get());c->OMSetDepthStencilState(equal.Get(),0);c->PSSetShader(pixel.Get(),nullptr,0);c->Draw(3,0);
            c->CopyResource(read.Get(),colour.Get());D3D11_MAPPED_SUBRESOURCE map{};check(c->Map(read.Get(),0,D3D11_MAP_READ,0,&map));UINT lit=0;
            for(UINT y=0;y<16;++y)for(UINT x=0;x<16;++x)lit+=static_cast<const unsigned char*>(map.pData)[y*map.RowPitch+x*4]!=0;
            c->Unmap(read.Get(),0);return lit;
        };
        for(auto comparison:{D3D11_COMPARISON_EQUAL,D3D11_COMPARISON_GREATER_EQUAL}){
            reversed=comparison==D3D11_COMPARISON_GREATER_EQUAL;equal.Reset();preDepth.Reset();
            dd.DepthFunc=comparison;dd.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ZERO;check(d->CreateDepthStencilState(&dd,&equal));
            dd.DepthFunc=reversed?D3D11_COMPARISON_GREATER:D3D11_COMPARISON_LESS;dd.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;check(d->CreateDepthStencilState(&dd,&preDepth));
            const UINT native=pixels(0,0),broken=pixels(0,.375f),fixed=pixels(.375f,.375f);
            expect(native>200 && broken<native/4 && fixed>=native*9/10,"Depth/colour jitter synchronization did not preserve native geometry");
        }
        c->ClearState();puts("PASS: EQUAL and native reverse-Z GREATER_EQUAL reproduce colour-only jitter disappearance; synchronized jitter retains geometry");
    }
    const char* hlsl="Texture2D<float4> Source:register(t0);float4 VS(uint id:SV_VertexID):SV_Position{return float4(id==2?3:-1,id==1?3:-1,0,1);}float4 PS():SV_Target{return Source.Load(int3(0,0,0));}";
    ComPtr<ID3DBlob> code,error;check(D3DCompile(hlsl,strlen(hlsl),nullptr,nullptr,nullptr,"VS","vs_5_0",0,0,&code,&error));
    ComPtr<ID3D11VertexShader> vs;check(d->CreateVertexShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&vs));
    check(D3DCompile(hlsl,strlen(hlsl),nullptr,nullptr,nullptr,"PS","ps_5_0",0,0,&code,&error));
    ComPtr<ID3D11PixelShader> ps;check(d->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&ps));
    {
        const char* motion="struct O{float4 colour:SV_Target0;float4 motion:SV_Target3;};O PS(){O o;o.colour=float4(.25,.5,.75,1);o.motion=float4(-.125,.25,0,0);return o;}";
        expect(!FfxivPreviewMotion::BlocksReplay(D3D11_QUERY_TIMESTAMP_DISJOINT) &&
               !FfxivPreviewMotion::BlocksReplay(D3D11_QUERY_TIMESTAMP),"GPU timing queries incorrectly block motion capture");
        expect(FfxivPreviewMotion::BlocksReplay(D3D11_QUERY_OCCLUSION) &&
               FfxivPreviewMotion::BlocksReplay(D3D11_QUERY_PIPELINE_STATISTICS),"Visibility/statistics queries permit unsafe replay");
        ComPtr<ID3DBlob> mcode;check(D3DCompile(motion,strlen(motion),nullptr,nullptr,nullptr,"PS","ps_5_0",0,0,&mcode,&error));
        ComPtr<ID3D11PixelShader> mps;check(d->CreatePixelShader(mcode->GetBufferPointer(),mcode->GetBufferSize(),nullptr,&mps));
        FfxivPreviewMotion::Tag(mps.Get(),3425462897u);
        expect(!FfxivPreviewMotion::KnownMotion(2019648864u),"Transparent normal shader mistaken for motion");
        D3D11_TEXTURE2D_DESC td{};td.Width=td.Height=16;td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;
        td.Format=DXGI_FORMAT_B8G8R8A8_UNORM;td.BindFlags=D3D11_BIND_RENDER_TARGET;
        ComPtr<ID3D11Texture2D> native,clamped;check(d->CreateTexture2D(&td,nullptr,&native));check(d->CreateTexture2D(&td,nullptr,&clamped));
        ComPtr<ID3D11RenderTargetView> rtv,mrt3;check(d->CreateRenderTargetView(native.Get(),nullptr,&rtv));check(d->CreateRenderTargetView(clamped.Get(),nullptr,&mrt3));
        td.Format=DXGI_FORMAT_D32_FLOAT;td.BindFlags=D3D11_BIND_DEPTH_STENCIL;
        ComPtr<ID3D11Texture2D> depthTex;ComPtr<ID3D11DepthStencilView> dsv;check(d->CreateTexture2D(&td,nullptr,&depthTex));check(d->CreateDepthStencilView(depthTex.Get(),nullptr,&dsv));
        ID3D11RenderTargetView* rt[4]{rtv.Get(),nullptr,nullptr,mrt3.Get()};c->OMSetRenderTargets(4,rt,dsv.Get());
        c->OMSetDepthStencilState(nullptr,0);c->OMSetBlendState(nullptr,nullptr,~0u);
        c->ClearDepthStencilView(dsv.Get(),D3D11_CLEAR_DEPTH,1,0);
        c->VSSetShader(vs.Get(),nullptr,0);c->PSSetShader(mps.Get(),nullptr,0);
        c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        D3D11_VIEWPORT vp{0,0,16,16,0,1};c->RSSetViewports(1,&vp);
        FfxivCharaViewTrace::Snapshot snapshot{};snapshot.entries[8]={reinterpret_cast<UINT_PTR>(native.Get()),16,16};
        FfxivPreviewMotion::Clear(c.Get(),rtv.Get(),snapshot);
        UINT calls=0;FfxivPreviewMotion::Around(c.Get(),[&]{++calls;c->Draw(3,0);},snapshot);
        expect(calls==2 && FfxivPreviewMotion::draws.load()==1,"Motion replay did not draw exactly once alongside native draw");
        ComPtr<ID3D11RenderTargetView> restored;c->OMGetRenderTargets(1,&restored,nullptr);expect(restored==rtv,"Motion replay changed native render target");
        ComPtr<ID3D11DepthStencilState> restoredDepth;UINT ref;c->OMGetDepthStencilState(&restoredDepth,&ref);expect(!restoredDepth,"Motion replay changed native depth state");
        ComPtr<ID3D11BlendState> restoredBlend;FLOAT factors[4];UINT mask;c->OMGetBlendState(&restoredBlend,factors,&mask);expect(!restoredBlend && mask==~0u,"Motion replay changed native blend state");
        auto read=[&](ID3D11Texture2D* t,auto verify){D3D11_TEXTURE2D_DESC rd{};t->GetDesc(&rd);rd.BindFlags=0;rd.Usage=D3D11_USAGE_STAGING;rd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Texture2D> staging;check(d->CreateTexture2D(&rd,nullptr,&staging));c->CopyResource(staging.Get(),t);D3D11_MAPPED_SUBRESOURCE map{};check(c->Map(staging.Get(),0,D3D11_MAP_READ,0,&map));verify(map.pData);c->Unmap(staging.Get(),0);};
        read(FfxivPreviewMotion::Data().texture.Get(),[&](void* p){auto v=static_cast<unsigned short*>(p);expect(FfxivPreviewMotion::Half(v[0])==-.125f && FfxivPreviewMotion::Half(v[1])==.25f,"Signed motion was clamped or encoded incorrectly");});
        read(native.Get(),[&](void* p){auto v=static_cast<unsigned char*>(p);expect(v[0]==191 && v[1]==128 && v[2]==64 && v[3]==255,"Motion replay modified native colour");});
        read(depthTex.Get(),[&](void* p){expect(*static_cast<float*>(p)==0,"Native depth draw was dropped");});
        FfxivPreviewMotion::activeQueries.store(1);calls=0;FfxivPreviewMotion::Around(c.Get(),[&]{++calls;c->Draw(3,0);},snapshot);expect(calls==1,"Active query received an extra replay draw");FfxivPreviewMotion::activeQueries.store(0);
        snapshot.entries[8]={};calls=0;FfxivPreviewMotion::Around(c.Get(),[&]{++calls;},snapshot);expect(calls==1,"Unknown target received a replay");
        c->OMSetRenderTargets(0,nullptr,nullptr);FfxivPreviewMotion::Shutdown();FfxivPreviewMotion::draws.store(0);FfxivPreviewMotion::clears.store(0);
        puts("PASS: signed preview motion, native colour/depth preservation, state restoration and query/unknown-target fallback");
    }
    {
        // Exercise the production guide conversion and alpha composition on WARP.
        ComPtr<ID3DBlob> blob;ComPtr<ID3D11VertexShader> pv;ComPtr<ID3D11PixelShader> gp,ap,cp,np,ni;
        auto shader=FfxivPreviewDLSSShaders::code;
        check(D3DCompile(shader,strlen(shader),nullptr,nullptr,nullptr,"VS","vs_5_0",0,0,&blob,&error));check(d->CreateVertexShader(blob->GetBufferPointer(),blob->GetBufferSize(),nullptr,&pv));
        for(auto [entry,dest]:std::array<std::pair<const char*,ComPtr<ID3D11PixelShader>*>,5>{{{"Copy",&cp},{"Guides",&gp},{"Alpha",&ap},{"AlphaNR",&np},{"NRInputs",&ni}}}){
            blob.Reset();check(D3DCompile(shader,strlen(shader),nullptr,nullptr,nullptr,entry,"ps_5_0",0,0,&blob,&error));check(d->CreatePixelShader(blob->GetBufferPointer(),blob->GetBufferSize(),nullptr,dest->GetAddressOf()));}
        D3D11_TEXTURE2D_DESC td{};td.Width=td.Height=2;td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;td.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;td.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;
        std::array<float,16> pixels{};for(UINT i=0;i<4;++i){pixels[i*4]=-.125f;pixels[i*4+1]=.25f;pixels[i*4+2]=.75f;pixels[i*4+3]=.5f;}
        D3D11_SUBRESOURCE_DATA data{pixels.data(),32,0};ComPtr<ID3D11Texture2D> input,md,dd;check(d->CreateTexture2D(&td,&data,&input));
        ComPtr<ID3D11ShaderResourceView> iv;check(d->CreateShaderResourceView(input.Get(),nullptr,&iv));
        td.Format=DXGI_FORMAT_R32_FLOAT;check(d->CreateTexture2D(&td,nullptr,&dd));td.Format=DXGI_FORMAT_R32G32_FLOAT;check(d->CreateTexture2D(&td,nullptr,&md));
        ComPtr<ID3D11RenderTargetView> dr,mr;check(d->CreateRenderTargetView(dd.Get(),nullptr,&dr));check(d->CreateRenderTargetView(md.Get(),nullptr,&mr));
        ComPtr<ID3D11ShaderResourceView> depth;D3D11_SHADER_RESOURCE_VIEW_DESC sv{};sv.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;sv.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;sv.Texture2D.MipLevels=1;check(d->CreateShaderResourceView(input.Get(),&sv,&depth));
        std::array<float,236> camera{};camera[234]=.0625f;camera[235]=-.125f;
        D3D11_BUFFER_DESC cb{};cb.ByteWidth=944;cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER;D3D11_SUBRESOURCE_DATA cd{camera.data(),0,0};ComPtr<ID3D11Buffer> cam;check(d->CreateBuffer(&cb,&cd,&cam));
        c->VSSetShader(pv.Get(),nullptr,0);c->PSSetShader(gp.Get(),nullptr,0);c->PSSetConstantBuffers(1,1,cam.GetAddressOf());
        ID3D11ShaderResourceView* views[3]{nullptr,iv.Get(),depth.Get()};c->PSSetShaderResources(0,3,views);
        ID3D11RenderTargetView* rt[2]{dr.Get(),mr.Get()};c->OMSetRenderTargets(2,rt,nullptr);c->OMSetDepthStencilState(nullptr,0);c->OMSetBlendState(nullptr,nullptr,~0u);
        D3D11_VIEWPORT vp{0,0,2,2,0,1};c->RSSetViewports(1,&vp);c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);c->Draw(3,0);
        td.BindFlags=0;td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;ComPtr<ID3D11Texture2D> read;check(d->CreateTexture2D(&td,nullptr,&read));c->CopyResource(read.Get(),md.Get());D3D11_MAPPED_SUBRESOURCE map{};check(c->Map(read.Get(),0,D3D11_MAP_READ,0,&map));auto m=static_cast<float*>(map.pData);
        expect(m[0]==-.1875f && m[1]==.375f,"Guide conversion did not remove native jitter compensation");c->Unmap(read.Get(),0);
        // Preserve native half-transparent alpha independently of NGX output alpha.
        td.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;td.Usage=D3D11_USAGE_DEFAULT;td.BindFlags=D3D11_BIND_RENDER_TARGET;td.CPUAccessFlags=0;ComPtr<ID3D11Texture2D> out;ComPtr<ID3D11RenderTargetView> ov;check(d->CreateTexture2D(&td,nullptr,&out));check(d->CreateRenderTargetView(out.Get(),nullptr,&ov));
        float settings[4]{0,0,.5f,.5f};cb.ByteWidth=16;cd.pSysMem=settings;ComPtr<ID3D11Buffer> params;check(d->CreateBuffer(&cb,&cd,&params));c->PSSetConstantBuffers(0,1,params.GetAddressOf());
        D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sd.MaxLOD=D3D11_FLOAT32_MAX;ComPtr<ID3D11SamplerState> sampler;check(d->CreateSamplerState(&sd,&sampler));c->PSSetSamplers(0,1,sampler.GetAddressOf());
        c->PSSetShader(ap.Get(),nullptr,0);c->PSSetShaderResources(0,1,iv.GetAddressOf());c->PSSetShaderResources(3,1,iv.GetAddressOf());c->OMSetRenderTargets(1,ov.GetAddressOf(),nullptr);c->Draw(3,0);
        td.BindFlags=0;td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;read.Reset();check(d->CreateTexture2D(&td,nullptr,&read));c->CopyResource(read.Get(),out.Get());check(c->Map(read.Get(),0,D3D11_MAP_READ,0,&map));m=static_cast<float*>(map.pData);expect(m[0]==-.125f && m[1]==.25f && m[3]==.5f,"Preview composition changed native alpha or DLSS colour");c->Unmap(read.Get(),0);
        // A native-resolution NR residual must preserve the extra detail in
        // the 2X DLSS result, retain signed changes, and leave native alpha.
        c->ClearState();
        td.Width=td.Height=4;td.BindFlags=D3D11_BIND_SHADER_RESOURCE;td.Usage=D3D11_USAGE_DEFAULT;td.CPUAccessFlags=0;
        std::array<float,64> detailed{};for(unsigned i=0;i<16;++i){const float value=(i+(i/4))%2?.7f:.2f;
            detailed[i*4]=detailed[i*4+1]=detailed[i*4+2]=value;detailed[i*4+3]=1;}
        data={detailed.data(),64,0};ComPtr<ID3D11Texture2D> high;ComPtr<ID3D11ShaderResourceView> highView;
        check(d->CreateTexture2D(&td,&data,&high));check(d->CreateShaderResourceView(high.Get(),nullptr,&highView));
        td.BindFlags=D3D11_BIND_RENDER_TARGET;ComPtr<ID3D11Texture2D> residualOut;ComPtr<ID3D11RenderTargetView> residualTarget;
        check(d->CreateTexture2D(&td,nullptr,&residualOut));check(d->CreateRenderTargetView(residualOut.Get(),nullptr,&residualTarget));
        td.BindFlags=0;td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;read.Reset();check(d->CreateTexture2D(&td,nullptr,&read));
        td.Width=td.Height=2;td.BindFlags=D3D11_BIND_SHADER_RESOURCE;td.Usage=D3D11_USAGE_DEFAULT;td.CPUAccessFlags=0;
        auto correction=pixels;for(unsigned i=0;i<4;++i){correction[i*4]+=.1f;correction[i*4+1]-=.05f;correction[i*4+2]+=.15f;correction[i*4+3]=0;}
        data={correction.data(),32,0};ComPtr<ID3D11Texture2D> nr;ComPtr<ID3D11ShaderResourceView> nrView;
        check(d->CreateTexture2D(&td,&data,&nr));check(d->CreateShaderResourceView(nr.Get(),nullptr,&nrView));
        c->VSSetShader(pv.Get(),nullptr,0);c->PSSetShader(np.Get(),nullptr,0);c->PSSetConstantBuffers(0,1,params.GetAddressOf());
        c->PSSetSamplers(0,1,sampler.GetAddressOf());c->PSSetShaderResources(0,1,highView.GetAddressOf());c->PSSetShaderResources(3,1,iv.GetAddressOf());
        c->PSSetShaderResources(5,1,iv.GetAddressOf());
        c->OMSetRenderTargets(1,residualTarget.GetAddressOf(),nullptr);vp.Width=vp.Height=4;c->RSSetViewports(1,&vp);c->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        for(bool corrected:{false,true}){
            c->PSSetShaderResources(4,1,corrected?nrView.GetAddressOf():iv.GetAddressOf());c->Draw(3,0);c->CopyResource(read.Get(),residualOut.Get());
            check(c->Map(read.Get(),0,D3D11_MAP_READ,0,&map));
            for(unsigned y=0;y<4;++y)for(unsigned x=0;x<4;++x){const auto* p=reinterpret_cast<const float*>(static_cast<const char*>(map.pData)+y*map.RowPitch)+x*4;
                const float base=detailed[(y*4+x)*4];
                expect(std::abs(p[0]-base-(corrected?.1f:0))<1e-5f && std::abs(p[1]-base+(corrected?.05f:0))<1e-5f &&
                       std::abs(p[2]-base-(corrected?.15f:0))<1e-5f && p[3]==.5f,"Low-resolution NR residual changed DLSS detail, sign or alpha");}
            c->Unmap(read.Get(),0);
        }
        // Remove colour jitter and move both guides onto the same NR grid.
        c->ClearState();for(unsigned i=0;i<4;++i)pixels[i*4]=(i%2)*.4f;
        c->UpdateSubresource(input.Get(),0,nullptr,pixels.data(),32,0);
        settings[0]=.5f;c->UpdateSubresource(params.Get(),0,nullptr,settings,0,0);
        c->VSSetShader(pv.Get(),nullptr,0);c->PSSetShader(ni.Get(),nullptr,0);c->PSSetConstantBuffers(0,1,params.GetAddressOf());
        c->PSSetSamplers(0,1,sampler.GetAddressOf());ID3D11ShaderResourceView* nrSources[]{iv.Get(),iv.Get(),iv.Get()};c->PSSetShaderResources(0,3,nrSources);
        ID3D11RenderTargetView* nrTargets[]{ov.Get(),dr.Get(),mr.Get()};c->OMSetRenderTargets(3,nrTargets,nullptr);
        vp.Width=vp.Height=2;c->RSSetViewports(1,&vp);c->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);c->Draw(3,0);
        td.Width=td.Height=2;td.BindFlags=0;td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> nrRead;check(d->CreateTexture2D(&td,nullptr,&nrRead));c->CopyResource(nrRead.Get(),out.Get());
        check(c->Map(nrRead.Get(),0,D3D11_MAP_READ,0,&map));m=static_cast<float*>(map.pData);
        expect(std::abs(m[0]-.2f)<1e-6f && m[3]==.5f,"NR colour grid did not compensate native half-pixel jitter");c->Unmap(nrRead.Get(),0);
        // Motion is already compensated; NR alignment must not subtract it twice.
        td.Format=DXGI_FORMAT_R32G32_FLOAT;nrRead.Reset();check(d->CreateTexture2D(&td,nullptr,&nrRead));c->CopyResource(nrRead.Get(),md.Get());
        check(c->Map(nrRead.Get(),0,D3D11_MAP_READ,0,&map));m=static_cast<float*>(map.pData);
        expect(std::abs(m[0]-.2f)<1e-6f && m[1]==.25f,"NR motion was misaligned or compensated twice");c->Unmap(nrRead.Get(),0);
        td.Format=DXGI_FORMAT_R32_FLOAT;nrRead.Reset();check(d->CreateTexture2D(&td,nullptr,&nrRead));c->CopyResource(nrRead.Get(),dd.Get());
        check(c->Map(nrRead.Get(),0,D3D11_MAP_READ,0,&map));m=static_cast<float*>(map.pData);
        expect(std::abs(m[0]-.4f)<1e-6f,"NR depth did not use the matching native sample");c->Unmap(nrRead.Get(),0);
        c->ClearState();c->VSSetShader(pv.Get(),nullptr,0);c->PSSetShader(np.Get(),nullptr,0);c->PSSetConstantBuffers(0,1,params.GetAddressOf());
        c->PSSetSamplers(0,1,sampler.GetAddressOf());c->PSSetShaderResources(0,1,highView.GetAddressOf());
        c->PSSetShaderResources(3,1,iv.GetAddressOf());c->PSSetShaderResources(4,1,iv.GetAddressOf());c->PSSetShaderResources(5,1,iv.GetAddressOf());
        c->OMSetRenderTargets(1,residualTarget.GetAddressOf(),nullptr);vp.Width=vp.Height=4;c->RSSetViewports(1,&vp);c->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);c->Draw(3,0);
        c->CopyResource(read.Get(),residualOut.Get());check(c->Map(read.Get(),0,D3D11_MAP_READ,0,&map));
        for(unsigned y=0;y<4;++y)for(unsigned x=0;x<4;++x){const auto* p=reinterpret_cast<const float*>(static_cast<const char*>(map.pData)+y*map.RowPitch)+x*4;
            expect(std::abs(p[0]-detailed[(y*4+x)*4])<1e-6f,"Nonzero jitter introduced a false NR correction");}
        c->Unmap(read.Get(),0);
        c->ClearState();puts("PASS: preview guides, alpha and signed NR residuals; aligned NR colour/depth/motion and jitter-independent identity correction");
    }
    FfxivPreviewGuides::Tag(ps.Get(),code->GetBufferPointer(),code->GetBufferSize());
    UINT savedBytes=0;check(ps->GetPrivateData(FfxivPreviewGuides::codeTag,&savedBytes,nullptr));
    expect(savedBytes==code->GetBufferSize(),"Preview shader bytecode not retained exactly");
    std::vector<unsigned char> savedCode(savedBytes);check(ps->GetPrivateData(FfxivPreviewGuides::codeTag,&savedBytes,savedCode.data()));
    expect(!std::memcmp(savedCode.data(),code->GetBufferPointer(),savedBytes),"Preview shader bytecode changed");
    c->PSSetShader(ps.Get(),nullptr,0);
    FfxivPreviewGuides::Observe(c.Get());
    ComPtr<ID3D11PixelShader> afterProbe;c->PSGetShader(&afterProbe,nullptr,nullptr);
    expect(afterProbe==ps && FfxivPreviewGuides::captured.load()==0,"Guide discovery changed a native shader or captured a non-preview draw");
    FfxivHdrPreview::Tag(ps.Get(),3865947726u);
    {
        D3D11_TEXTURE2D_DESC desc{};desc.Width=desc.Height=16;desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;
        desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        std::array<UINT,256> pixels{};pixels.fill(0x80804020);
        D3D11_SUBRESOURCE_DATA data{pixels.data(),64,0};
        ComPtr<ID3D11Texture2D> input;check(d->CreateTexture2D(&desc,&data,&input));
        ComPtr<ID3D11ShaderResourceView> srv;check(d->CreateShaderResourceView(input.Get(),nullptr,&srv));
        c->PSSetShader(ps.Get(),nullptr,0);c->PSSetShaderResources(0,1,srv.GetAddressOf());
        D3D11_VIEWPORT vp{3,4,17,19,0,1};c->RSSetViewports(1,&vp);
        for(bool upscale:{false,true}) {
            auto filtered=FfxivPreviewEnhance::Process(c.Get(),srv.Get(),16,16,upscale);
            expect(bool(filtered),"Preview AA failed to initialize");
            ComPtr<ID3D11PixelShader> restored;c->PSGetShader(&restored,nullptr,nullptr);
            expect(restored==ps,"Preview AA changed native pixel shader");
            ComPtr<ID3D11ShaderResourceView> restoredSrv;c->PSGetShaderResources(0,1,&restoredSrv);
            expect(restoredSrv==srv,"Preview AA changed native source binding");
            D3D11_VIEWPORT actual{};UINT count=1;c->RSGetViewports(&count,&actual);
            expect(actual.TopLeftX==3 && actual.Width==17,"Preview AA changed native viewport");
            ComPtr<ID3D11Resource> r;filtered->GetResource(&r);ComPtr<ID3D11Texture2D> t;check(r.As(&t));
            D3D11_TEXTURE2D_DESC output{};t->GetDesc(&output);
            expect(output.Width==(upscale?32:16),"Preview upscale output extent incorrect");
            output.BindFlags=0;output.Usage=D3D11_USAGE_STAGING;output.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Texture2D> read;check(d->CreateTexture2D(&output,nullptr,&read));c->CopyResource(read.Get(),t.Get());
            D3D11_MAPPED_SUBRESOURCE m{};check(c->Map(read.Get(),0,D3D11_MAP_READ,0,&m));
            auto* p=static_cast<unsigned char*>(m.pData);
            expect(p[0]==128 && p[1]==64 && p[2]==32 && p[3]==128,"Preview AA changed flat colour or transparent alpha");
            c->Unmap(read.Get(),0);
        }
        for(UINT y=0;y<16;++y)for(UINT x=0;x<16;++x)pixels[y*16+x]=x>=y?0xffffffffu:0xff000000u;
        c->UpdateSubresource(input.Get(),0,nullptr,pixels.data(),64,0);
        auto edge=FfxivPreviewEnhance::Process(c.Get(),srv.Get(),16,16,false);
        ComPtr<ID3D11Resource> edgeResource;edge->GetResource(&edgeResource);
        ComPtr<ID3D11Texture2D> edgeTexture;check(edgeResource.As(&edgeTexture));
        D3D11_TEXTURE2D_DESC readDesc{};edgeTexture->GetDesc(&readDesc);
        readDesc.BindFlags=0;readDesc.Usage=D3D11_USAGE_STAGING;readDesc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> edgeRead;check(d->CreateTexture2D(&readDesc,nullptr,&edgeRead));c->CopyResource(edgeRead.Get(),edgeTexture.Get());
        D3D11_MAPPED_SUBRESOURCE edgeMap{};check(c->Map(edgeRead.Get(),0,D3D11_MAP_READ,0,&edgeMap));
        UINT softened=0;
        for(UINT y=0;y<16;++y)for(UINT x=0;x<16;++x) {
            const auto* p=static_cast<unsigned char*>(edgeMap.pData)+y*edgeMap.RowPitch+x*4;
            softened+=p[0]>0 && p[0]<255;expect(p[3]==255,"Preview AA changed opaque alpha");
        }
        c->Unmap(edgeRead.Get(),0);expect(softened>0,"Preview AA did not soften a diagonal edge");
        FfxivPreviewEnhance::Shutdown();
        puts("PASS: preview spatial AA, 2x output, BGRA colour/alpha and native shader/SRV/viewport restoration");
    }
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
    // Metadata-only preview discovery must preserve native bindings and pixels.
    {
        using namespace FfxivPreviewTrace;
        D3D11_TEXTURE2D_DESC desc{};desc.Width=desc.Height=32;
        desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;
        desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> colour,alias,depthTexture,readback;
        check(d->CreateTexture2D(&desc,nullptr,&colour));check(d->CreateTexture2D(&desc,nullptr,&alias));
        ComPtr<ID3D11RenderTargetView> rtv;check(d->CreateRenderTargetView(colour.Get(),nullptr,&rtv));
        desc.Format=DXGI_FORMAT_D24_UNORM_S8_UINT;desc.BindFlags=D3D11_BIND_DEPTH_STENCIL;
        check(d->CreateTexture2D(&desc,nullptr,&depthTexture));
        ComPtr<ID3D11DepthStencilView> depth;check(d->CreateDepthStencilView(depthTexture.Get(),nullptr,&depth));
        c->OMSetRenderTargets(1,rtv.GetAddressOf(),depth.Get());
        D3D11_VIEWPORT vp{2,3,24,25,0,1};c->RSSetViewports(1,&vp);
        const float clear[]{.25f,.5f,.75f,1};c->ClearRenderTargetView(rtv.Get(),clear);
        Observe(c.Get());Observe(c.Get());
        Producer p{};UINT bytes=sizeof(p);check(colour->GetPrivateData(producerTag,&bytes,&p));
        expect(p.draws==2 && p.depthWidth==32 && p.depthHeight==32 && p.depthFormat==DXGI_FORMAT_D24_UNORM_S8_UINT && p.viewport.Width==24,"Preview producer metadata incorrect");
        ComPtr<ID3D11RenderTargetView> actual;ComPtr<ID3D11DepthStencilView> actualDepth;
        c->OMGetRenderTargets(1,&actual,&actualDepth);
        expect(actual==rtv && actualDepth==depth,"Preview trace changed native bindings");
        Copy(alias.Get(),colour.Get(),true);bytes=sizeof(p);check(alias->GetPrivateData(producerTag,&bytes,&p));
        expect(p.draws==2,"Preview copy lineage lost");
        ComPtr<ID3D11RenderTargetView> aliasRtv;check(d->CreateRenderTargetView(alias.Get(),nullptr,&aliasRtv));
        ComPtr<ID3D11ShaderResourceView> colourSrv;check(d->CreateShaderResourceView(colour.Get(),nullptr,&colourSrv));
        c->OMSetRenderTargets(1,aliasRtv.GetAddressOf(),nullptr);c->PSSetShaderResources(0,1,colourSrv.GetAddressOf());
        Observe(c.Get());bytes=sizeof(p);check(alias->GetPrivateData(producerTag,&bytes,&p));
        expect(p.inheritedDepth==1 && p.depthWidth==32 && p.draws==3,"Depth-free finishing lineage lost");
        expect(p.inputs[0].width==32 && p.inputs[0].format==DXGI_FORMAT_R8G8B8A8_UNORM && p.inputs[0].depthWidth==32,"Finishing input metadata lost");
        ID3D11ShaderResourceView* empty=nullptr;c->PSSetShaderResources(0,1,&empty);
        Observe(c.Get());bytes=sizeof(p);check(alias->GetPrivateData(producerTag,&bytes,&p));
        expect(p.inheritedDepth==0 && p.depthWidth==0,"Unrelated depth-free pass retained stale depth");
        expect(p.inputs[0].width==0,"Unbound finishing input retained stale metadata");
        expect(p.stages[1].depthWidth==32,"Finishing pass erased earlier native depth evidence");
        Shader(ps.Get(),2503081625u);
        c->PSSetShaderResources(31,1,colourSrv.GetAddressOf());
        Observe(c.Get());bytes=sizeof(p);check(alias->GetPrivateData(producerTag,&bytes,&p));
        expect(p.inputs[31].width==32 && p.inputs[31].depthWidth==32,"Targeted producer trace missed high-slot guide");
        expect(p.stages[0].shader==2503081625u,"Targeted producer shader identity lost");
        c->PSSetShaderResources(31,1,&empty);
        ComPtr<ID3D11DeviceContext> deferred;check(d->CreateDeferredContext(0,&deferred));
        deferred->OMSetRenderTargets(1,aliasRtv.GetAddressOf(),depth.Get());deferred->RSSetViewports(1,&vp);
        Observe(deferred.Get());bytes=sizeof(p);check(alias->GetPrivateData(producerTag,&bytes,&p));
        expect(p.deferred==1 && p.depthWidth==32,"Deferred producer was not recorded");
        Copy(alias.Get(),depthTexture.Get(),true);bytes=sizeof(p);
        expect(FAILED(alias->GetPrivateData(producerTag,&bytes,&p)),"Unrelated full copy retained stale producer");
        colour->GetDesc(&desc);desc.BindFlags=desc.MiscFlags=0;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        check(d->CreateTexture2D(&desc,nullptr,&readback));c->CopyResource(readback.Get(),colour.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};check(c->Map(readback.Get(),0,D3D11_MAP_READ,0,&mapped));
        auto* pixel=static_cast<unsigned char*>(mapped.pData);
        expect(pixel[0]==64 && pixel[1]==128 && pixel[2]==191 && pixel[3]==255,"Preview trace changed native pixels");
        c->Unmap(readback.Get(),0);
        puts("PASS: preview producer/depth/viewport metadata, depth-free finishing lineage, deferred contexts, native bindings and pixels unchanged");
    }
    puts("PASS: preview alpha/viewport, opaque/transparent coverage, depth/stencil safety, large/non-t0 images, binding restoration and transparent atlas coverage (D3D11 WARP)");return 0;
}catch(const std::exception& e){printf("FAIL: %s\n",e.what());return 1;}
