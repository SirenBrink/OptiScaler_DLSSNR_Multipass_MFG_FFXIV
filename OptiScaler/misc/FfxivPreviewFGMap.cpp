#include <pch.h>
#include "FfxivPreviewFGMap.h"
#include "ExternalHudless.h"
#include "FfxivPreviewMotion.h"
#include <Config.h>
#include <State.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <array>
#include <algorithm>
#include <mutex>
#include <vector>

namespace FfxivPreviewFGMap {
using Microsoft::WRL::ComPtr;
namespace {
// TEXCOORD2 was measured on the game's UI draw: it spans the preview image.
// Its derivatives convert the private NGX guide to screen-space pixels without
// assuming an equipment or portrait window size.
constexpr char shader[] = R"(
struct Input { float4 pos : SV_Position; float2 uv0 : TEXCOORD0; float2 uv1 : TEXCOORD1;
               float2 uv2 : TEXCOORD2; float2 uv3 : TEXCOORD3; };
Texture2D<float2> PreviewMotion : register(t15);
cbuffer Params : register(b13) { uint History; float3 Padding; };
float4 Main(Input i) : SV_Target {
    uint w,h; PreviewMotion.GetDimensions(w,h);
    int2 p=int2(clamp(i.uv2,float2(0,0),float2(.999999,.999999))*float2(w,h));
    float2 mv=PreviewMotion.Load(int3(p,0));
    float2 delta=float2(ddx(i.uv2.x),ddy(i.uv2.y));
    bool mapped=abs(delta.x)>1e-7 && abs(delta.y)>1e-7;
    float2 pixels=mapped ? mv/delta : 0;
    bool reliable=mapped && all(isfinite(pixels)) && all(abs(pixels)<=48.0);
    pixels=reliable ? pixels : 0;
    // Centre on the exactly representable UNORM byte 128. Encoding zero as
    // 0.5 rounded it up, which decoded as a spurious quarter-pixel movement.
    return float4((128.0+pixels*(255.0/128.0))/255.0,
                  (History+1)/16.0, reliable ? 1.0 : 0.0);
}
)";
struct Map {
    ComPtr<ID3D11Texture2D> dx11;
    ComPtr<ID3D11RenderTargetView> target;
    ComPtr<ID3D12Resource> dx12;
    ComPtr<ID3D12Device> openedDevice;
    UINT width=0,height=0,draws=0;
    ULONGLONG completedAt=0;
};
struct Store {
    std::mutex mutex;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11PixelShader> shader;
    ComPtr<ID3D11Buffer> constants;
    std::vector<std::shared_ptr<Map>> pool;
    std::shared_ptr<Map> current,latest;
    UINT64 captures=0,skips=0;
    bool publishedEarly=false;
    bool failed=false;
};
Store& Data(){static auto* value=new Store;return *value;}
bool Prepare(Store& s,ID3D11Device* device){
    if(s.device && s.device.Get()!=device)return false;
    s.device=device;
    if(s.shader && s.constants)return true;
    ComPtr<ID3DBlob> blob,error;
    if(FAILED(D3DCompile(shader,sizeof(shader)-1,nullptr,nullptr,nullptr,"Main","ps_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&blob,&error))){
        LOG_WARN("CharaView FG: motion-map shader compile failed: {}",error?static_cast<const char*>(error->GetBufferPointer()):"");return false;}
    if(FAILED(device->CreatePixelShader(blob->GetBufferPointer(),blob->GetBufferSize(),nullptr,&s.shader)))return false;
    D3D11_BUFFER_DESC desc{};desc.ByteWidth=16;desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    return SUCCEEDED(device->CreateBuffer(&desc,nullptr,&s.constants));
}
std::shared_ptr<Map> Reserve(Store& s,ID3D11Device* device,UINT w,UINT h){
    if(s.latest && (s.latest->width!=w || s.latest->height!=h))s.latest.reset();
    std::erase_if(s.pool,[&](const auto& map){return map.use_count()==1 && (map->width!=w || map->height!=h);});
    for(auto& map:s.pool)if(map.use_count()==1 && map->width==w && map->height==h){map->draws=0;return map;}
    // The paste path can retain three completed preview poses while a newer
    // map is being drawn. Leave one additional map available for that draw.
    if(s.pool.size()>=8)return {};
    auto map=std::make_shared<Map>();
    D3D11_TEXTURE2D_DESC desc{};desc.Width=w;desc.Height=h;desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;
    desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags=D3D11_RESOURCE_MISC_SHARED|D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    if(FAILED(device->CreateTexture2D(&desc,nullptr,&map->dx11)) ||
       FAILED(device->CreateRenderTargetView(map->dx11.Get(),nullptr,&map->target)))return {};
    map->width=w;map->height=h;s.pool.push_back(map);return map;
}
}
void Capture(ID3D11DeviceContext* c,unsigned history,ID3D11ShaderResourceView* motion,const std::function<void()>& draw){
    if(!c || !motion || history>=16 || !Config::Instance()->FfxivPreviewFG.value_or_default() ||
       !ExternalHudless::Active() || FfxivPreviewMotion::activeQueries.load())return;
    auto& state=State::Instance();
    if(state.activeFgOutput!=FGOutput::DLSSG || !state.currentFG || !state.currentFG->IsActive())return;
    auto& s=Data();std::lock_guard lock(s.mutex);
    if(s.failed || (s.current && s.current->draws>=16))return;
    ID3D11UnorderedAccessView* uavs[8]{};c->OMGetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,0,8,uavs);
    ID3D11Buffer* streams[4]{};c->SOGetTargets(4,streams);
    bool sideEffects=false;
    for(auto* u:uavs)if(u){sideEffects=true;u->Release();}
    for(auto* b:streams)if(b){sideEffects=true;b->Release();}
    if(sideEffects)return;
    ID3D11RenderTargetView* raw[8]{};ComPtr<ID3D11DepthStencilView> depth;c->OMGetRenderTargets(8,raw,&depth);
    std::array<ComPtr<ID3D11RenderTargetView>,8> original;for(unsigned i=0;i<8;++i)original[i].Attach(raw[i]);
    if(!original[0])return;
    for(unsigned i=1;i<8;++i)if(original[i])return;
    ComPtr<ID3D11Resource> resource;ComPtr<ID3D11Texture2D> texture;original[0]->GetResource(&resource);
    if(FAILED(resource.As(&texture)))return;
    D3D11_TEXTURE2D_DESC td{};texture->GetDesc(&td);
    if(td.Width<1024 || td.Height<720 || td.Width>8192 || td.Height>8192 || td.SampleDesc.Count!=1)return;
    ComPtr<ID3D11Predicate> predicate;BOOL predicateValue=FALSE;c->GetPredication(&predicate,&predicateValue);if(predicate)return;
    ComPtr<ID3D11Device> device;c->GetDevice(&device);
    if(!Prepare(s,device.Get())){s.failed=true;return;}
    if(s.current && (s.current->width!=td.Width || s.current->height!=td.Height))s.current.reset();
    if(!s.current){s.current=Reserve(s,device.Get(),td.Width,td.Height);if(!s.current){++s.skips;return;}
        const FLOAT zero[4]{};c->ClearRenderTargetView(s.current->target.Get(),zero);}
    ComPtr<ID3D11PixelShader> ps;c->PSGetShader(&ps,nullptr,nullptr);
    ComPtr<ID3D11ShaderResourceView> oldMotion;c->PSGetShaderResources(15,1,&oldMotion);
    ComPtr<ID3D11Buffer> oldConstants;c->PSGetConstantBuffers(13,1,&oldConstants);
    ComPtr<ID3D11BlendState> blend;FLOAT factors[4]{};UINT mask=0;c->OMGetBlendState(&blend,factors,&mask);
    ComPtr<ID3D11DepthStencilState> oldDepth;UINT reference=0;c->OMGetDepthStencilState(&oldDepth,&reference);
    std::array<UINT,4> values{history,0,0,0};c->UpdateSubresource(s.constants.Get(),0,nullptr,values.data(),0,0);
    ID3D11RenderTargetView* target=s.current->target.Get();c->OMSetRenderTargets(1,&target,nullptr);
    c->OMSetBlendState(nullptr,nullptr,~0u);c->OMSetDepthStencilState(nullptr,0);
    c->PSSetShader(s.shader.Get(),nullptr,0);c->PSSetShaderResources(15,1,&motion);
    ID3D11Buffer* constants=s.constants.Get();c->PSSetConstantBuffers(13,1,&constants);
    draw();
    c->OMSetRenderTargets(8,raw,depth.Get());c->OMSetBlendState(blend.Get(),factors,mask);
    c->OMSetDepthStencilState(oldDepth.Get(),reference);c->PSSetShader(ps.Get(),nullptr,0);
    ID3D11ShaderResourceView* oldView=oldMotion.Get();c->PSSetShaderResources(15,1,&oldView);
    ID3D11Buffer* oldBuffer=oldConstants.Get();c->PSSetConstantBuffers(13,1,&oldBuffer);
    ++s.current->draws;
    if(++s.captures==1 || s.captures%600==0)
        LOG_INFO("CharaView FG: {} mapped preview UI draws at {}x{}, {} maps busy",s.captures,td.Width,td.Height,s.skips);
}
void PublishForHudPaste(){
    auto& s=Data();std::lock_guard lock(s.mutex);
    s.latest=std::move(s.current);
    if(s.latest)s.latest->completedAt=GetTickCount64();
    s.publishedEarly=true;
}
void EndFrame(){
    auto& s=Data();std::lock_guard lock(s.mutex);
    if(!s.publishedEarly){
        s.latest=std::move(s.current);
        if(s.latest)s.latest->completedAt=GetTickCount64();
    }
    s.publishedEarly=false;
}
Image Open(ID3D12Device* device,unsigned width,unsigned height){
    Image image{};
    if(!device || !Config::Instance()->FfxivPreviewFG.value_or_default())return image;
    auto& s=Data();std::lock_guard lock(s.mutex);auto p=s.latest;
    if(!p || p->width!=width || p->height!=height || !p->draws || GetTickCount64()-p->completedAt>100)return image;
    if(p->openedDevice.Get()!=device){
        p->dx12.Reset();p->openedDevice.Reset();
        ComPtr<IDXGIResource1> shared;if(FAILED(p->dx11.As(&shared)))return image;
        HANDLE handle=nullptr;if(FAILED(shared->CreateSharedHandle(nullptr,DXGI_SHARED_RESOURCE_READ|DXGI_SHARED_RESOURCE_WRITE,nullptr,&handle)))return image;
        const auto hr=device->OpenSharedHandle(handle,IID_PPV_ARGS(&p->dx12));CloseHandle(handle);
        if(FAILED(hr))return image;p->openedDevice=device;
    }
    image.resource=p->dx12.Get();image.owner=p;return image;
}
}
