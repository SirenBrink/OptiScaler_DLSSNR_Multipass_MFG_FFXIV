#pragma once
#include <d3d11_1.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <array>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <functional>
#include <Config.h>
#include <shaders/hdr/SceneInput.h>
#include <shaders/hdr/Hdr10.h>
#include <shaders/hdr/renodx/SceneShaders.h>

// RenoDX FFXIV shaders run in an independent FP16 branch. This deliberately
// leaves native SDR, NGX/NR inputs and ReShade's SDR effects unchanged.
namespace FfxivSceneHdr {
using Microsoft::WRL::ComPtr;
inline constexpr GUID shaderTag{0xe3e16e11,0x412b,0x458b,{0x9d,0x2c,0x3a,0x81,0xb5,0x29,0xaa,0x01}};
inline bool Requested() { return Hdr10::Active() && Config::Instance()->FfxivHDR.value_or_default() && Config::Instance()->FfxivHDRMode.value_or_default()==1; }
inline UINT Stage(UINT crc) {
    switch(crc) {case 0x85e777ef:return 1;case 0xf8f57f0a:return 2;
    case 0x27ebc404:return 3;case 0x1f264d17:return 4;case 0xf6e81a1b:return 5;default:return 0;}
}
struct Slot {
    ComPtr<ID3D11Texture2D> hdr, reference;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D12Resource> openedHdr, openedReference;
    ComPtr<ID3D12Device> openedDevice;
    std::function<bool()> done;
    bool reserved=false;
    UINT width=0,height=0,stage=0;
    ULONGLONG lastUse=0;bool dx12Recorded=false;
    D3D11_VIEWPORT viewport{};
    std::array<float,4> crop{};
    DXGI_FORMAT referenceFormat=DXGI_FORMAT_UNKNOWN;
};
struct Data {
    std::mutex mutex;
    ComPtr<ID3D11Device> device;
    std::array<ComPtr<ID3D11PixelShader>,5> shaders;
    std::vector<std::shared_ptr<Slot>> slots;
    struct Alias { ComPtr<ID3D11Resource> native;std::shared_ptr<Slot> image; };
    std::unordered_map<ID3D11Resource*,Alias> aliases;
    std::shared_ptr<Slot> latest;
    uint64_t captures=0;
    uint64_t requests=0,available=0,missing=0,aspectMismatch=0,importFailed=0;
    std::array<bool,5> observed{};
    std::array<std::array<bool,24>,5> rejected{};
};
inline Data& State(){static auto* data=new Data;return *data;} // No COM destruction under loader lock.
inline void Tag(ID3D11PixelShader* shader,UINT crc) {
    if(auto stage=Stage(crc)) shader->SetPrivateData(shaderTag,sizeof(stage),&stage);
}
inline void Reject(Data& s,UINT stage,UINT reason,const char* description) {
    if(stage && stage<=5 && reason<s.rejected[0].size() && !s.rejected[stage-1][reason]) {
        s.rejected[stage-1][reason]=true;
        LOG_INFO("RenoDX scene HDR: stage {} rejected: {}",stage,description);
    }
}
inline DXGI_FORMAT Typed(DXGI_FORMAT f) {
    if(f==DXGI_FORMAT_R16G16B16A16_TYPELESS)return DXGI_FORMAT_R16G16B16A16_FLOAT;
    if(f==DXGI_FORMAT_R8G8B8A8_TYPELESS || f==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)return DXGI_FORMAT_R8G8B8A8_UNORM;
    if(f==DXGI_FORMAT_B8G8R8A8_TYPELESS || f==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)return DXGI_FORMAT_B8G8R8A8_UNORM;
    return f;
}
inline bool Allocate(Slot& s,ID3D11Device* d,const D3D11_TEXTURE2D_DESC& input) {
    D3D11_TEXTURE2D_DESC td{};td.Width=input.Width;td.Height=input.Height;
    td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;td.Usage=D3D11_USAGE_DEFAULT;
    td.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags=D3D11_RESOURCE_MISC_SHARED|D3D11_RESOURCE_MISC_SHARED_NTHANDLE;td.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;
    if(FAILED(d->CreateTexture2D(&td,nullptr,&s.hdr)) || FAILED(d->CreateRenderTargetView(s.hdr.Get(),nullptr,&s.rtv)) ||
       FAILED(d->CreateShaderResourceView(s.hdr.Get(),nullptr,&s.srv)))return false;
    td.Format=Typed(input.Format);td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    if(FAILED(d->CreateTexture2D(&td,nullptr,&s.reference)))return false;
    s.width=td.Width;s.height=td.Height;s.referenceFormat=td.Format;return true;
}
template<class F> inline void AfterDraw(ID3D11DeviceContext* c,F&& draw) {
    if(!Requested())return;
    auto& s=State();std::lock_guard lock(s.mutex);
    ComPtr<ID3D11RenderTargetView> target;ComPtr<ID3D11DepthStencilView> depth;
    c->OMGetRenderTargets(1,&target,&depth);if(!target)return;
    ComPtr<ID3D11Resource> native;target->GetResource(&native);
    ComPtr<ID3D11PixelShader> original;UINT instances=0;c->PSGetShader(&original,nullptr,&instances);
    UINT stage=0,bytes=sizeof(stage);if(original)original->GetPrivateData(shaderTag,&bytes,&stage);
    // Any unrecognised write breaks forwarding, but keeps the immutable scene
    // snapshot for the final SDR grade bridge (HUD is intentionally not replayed).
    if(!stage || stage>5){s.aliases.erase(native.Get());return;}
    s.aliases.erase(native.Get()); // A rejected overwrite must not forward an earlier snapshot.
    auto reject=[&](UINT reason,const char* description){Reject(s,stage,reason,description);};
    if(instances){reject(0,"dynamic shader linkage");return;}
    if(!s.observed[stage-1]) {
        s.observed[stage-1]=true;
        LOG_INFO("RenoDX scene HDR: recognised native stage {}",stage);
    }
    if(depth){ComPtr<ID3D11DepthStencilState> ds;UINT stencil=0;c->OMGetDepthStencilState(&ds,&stencil);
        if(!ds){reject(1,"default active depth state");return;}D3D11_DEPTH_STENCIL_DESC dd{};ds->GetDesc(&dd);if(dd.DepthEnable || dd.StencilEnable){reject(2,"active depth/stencil test");return;}}
    ComPtr<ID3D11Predicate> predicate;BOOL predicateValue=FALSE;c->GetPredication(&predicate,&predicateValue);if(predicate){reject(3,"predicated draw");return;}
    ComPtr<ID3D11Texture2D> texture;if(FAILED(native.As(&texture))){reject(4,"target is not a 2D texture");return;}
    D3D11_RENDER_TARGET_VIEW_DESC targetView{};target->GetDesc(&targetView);
    if(targetView.ViewDimension!=D3D11_RTV_DIMENSION_TEXTURE2D || targetView.Texture2D.MipSlice || targetView.Format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || targetView.Format==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB){reject(5,"unsupported target view or sRGB target");return;}
    D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);
    if(desc.Width<640 || desc.Height<360 || desc.MipLevels!=1 || desc.ArraySize!=1 || desc.SampleDesc.Count!=1 ||
       (Typed(desc.Format)!=DXGI_FORMAT_R16G16B16A16_FLOAT && Typed(desc.Format)!=DXGI_FORMAT_R8G8B8A8_UNORM && Typed(desc.Format)!=DXGI_FORMAT_B8G8R8A8_UNORM)){reject(6,"unsupported target dimensions, format or samples");return;}
    D3D11_VIEWPORT viewport{};UINT count=1;c->RSGetViewports(&count,&viewport);
    // FFXIV uses a 0.5-pixel viewport origin. Keep rasterisation unchanged,
    // but describe the covered texel centres rather than treating that origin
    // as a fractional texture-sampling offset. Right/bottom edges are exclusive.
    auto halfAligned=[](float v){return std::abs(v*2-std::round(v*2))<.001f;};
    const bool finiteViewport=std::isfinite(viewport.TopLeftX) && std::isfinite(viewport.TopLeftY) &&
        std::isfinite(viewport.Width) && std::isfinite(viewport.Height);
    const float left=finiteViewport?std::ceil(viewport.TopLeftX-.5f):0;
    const float top=finiteViewport?std::ceil(viewport.TopLeftY-.5f):0;
    const float right=finiteViewport?std::ceil(viewport.TopLeftX+viewport.Width-.5f):0;
    const float bottom=finiteViewport?std::ceil(viewport.TopLeftY+viewport.Height-.5f):0;
    const bool validViewport=count==1 && finiteViewport && viewport.TopLeftX>=0 && viewport.TopLeftY>=0 &&
        halfAligned(viewport.TopLeftX) && halfAligned(viewport.TopLeftY) &&
        std::abs(viewport.Width-std::round(viewport.Width))<.001f && std::abs(viewport.Height-std::round(viewport.Height))<.001f &&
        right-left>=640 && bottom-top>=360 && left>=0 && top>=0 && right<=desc.Width && bottom<=desc.Height;
    if(!validViewport){
        if(!s.rejected[stage-1][7])LOG_INFO("RenoDX scene HDR: stage {} viewport count {}, origin {}/{}, size {}x{}, target {}x{}",stage,count,viewport.TopLeftX,viewport.TopLeftY,viewport.Width,viewport.Height,desc.Width,desc.Height);
        reject(7,"viewport cannot be safely cropped");return;
    }
    std::array<ComPtr<ID3D11RenderTargetView>,8> targets;ID3D11RenderTargetView* rawTargets[8]{};
    c->OMGetRenderTargets(8,rawTargets,nullptr);for(UINT i=0;i<8;i++)targets[i].Attach(rawTargets[i]);
    for(UINT i=1;i<8;i++)if(targets[i]){reject(8,"multiple render targets");return;}
    ComPtr<ID3D11BlendState> blend;FLOAT factor[4]{};UINT mask=0;c->OMGetBlendState(&blend,factor,&mask);
    if(blend){D3D11_BLEND_DESC b{};blend->GetDesc(&b);if(b.RenderTarget[0].BlendEnable || (b.RenderTarget[0].RenderTargetWriteMask & 7)!=7){reject(9,"blending or incomplete RGB writes");return;}}
    ComPtr<ID3D11ShaderResourceView> input;c->PSGetShaderResources(0,1,&input);if(!input){reject(10,"missing colour input");return;}
    D3D11_SHADER_RESOURCE_VIEW_DESC inputDesc{};input->GetDesc(&inputDesc);
    if(inputDesc.ViewDimension!=D3D11_SRV_DIMENSION_TEXTURE2D || inputDesc.Texture2D.MostDetailedMip){reject(11,"unsupported colour input view");return;}
    ComPtr<ID3D11Resource> inputResource;input->GetResource(&inputResource);if(inputResource.Get()==native.Get()){reject(12,"input aliases target");return;}
    std::shared_ptr<Slot> upstream;
    if(auto it=s.aliases.find(inputResource.Get());it!=s.aliases.end())upstream=it->second.image;
    if(stage!=1 && !upstream){reject(13,"no captured upstream scene");return;}
    if(stage==1){ComPtr<ID3D11Texture2D> t;if(FAILED(inputResource.As(&t))){reject(14,"root input is not a 2D texture");return;}D3D11_TEXTURE2D_DESC d{};t->GetDesc(&d);
        if(Typed(d.Format)!=DXGI_FORMAT_R16G16B16A16_FLOAT || inputDesc.Format!=DXGI_FORMAT_R16G16B16A16_FLOAT || d.SampleDesc.Count!=1){reject(15,"root input is not FP16 colour");return;}
    }
    ComPtr<ID3D11Device> d;c->GetDevice(&d);
    if(s.device.Get()!=d.Get()){s.device=d;s.shaders={};s.aliases.clear();s.latest.reset();s.slots.clear();upstream.reset();if(stage!=1)return;}
    auto& ps=s.shaders[stage-1];if(!ps){
        const void* code=nullptr;size_t size=0;
#define SCENE_SHADER(i,n) case i:code=n;size=sizeof(n);break
        switch(stage){SCENE_SHADER(1,Tonemap_0x85E777EF);SCENE_SHADER(2,PostTonemapPreLUT_0xF8F57F0A);
        SCENE_SHADER(3,LUT_0x27EBC404);SCENE_SHADER(4,LUT_0x1F264D17);SCENE_SHADER(5,FullscreenGammaCorrection_0xF6E81A1B);}
#undef SCENE_SHADER
        if(FAILED(d->CreatePixelShader(code,size,nullptr,&ps))){reject(16,"replacement shader creation failed");return;}
    }
    std::shared_ptr<Slot> slot;
    for(auto& p:s.slots)if(p.use_count()==1 && !p->reserved && Hdr10::ReusableRecordedWork(p->dx12Recorded,p->done)){slot=p;break;}
    if(!slot){if(s.slots.size()>=16){reject(17,"all scene snapshots are in flight");return;}slot=std::make_shared<Slot>();s.slots.push_back(slot);}
    if(!slot->hdr || slot->width!=desc.Width || slot->height!=desc.Height || slot->referenceFormat!=Typed(desc.Format)){
        *slot=Slot{};if(!Allocate(*slot,d.Get(),desc)){reject(18,"shared scene texture allocation failed");return;}
    }
    // Snapshot the real SDR result, then replay only the verified fullscreen
    // pass into FP16. Every modified binding is restored before the next draw.
    c->CopyResource(slot->reference.Get(),texture.Get());
    c->OMSetRenderTargets(1,slot->rtv.GetAddressOf(),nullptr);
    c->PSSetShader(ps.Get(),nullptr,0);
    if(upstream)c->PSSetShaderResources(0,1,upstream->srv.GetAddressOf());
    const float clear[4]{};c->ClearRenderTargetView(slot->rtv.Get(),clear);draw();
    c->OMSetRenderTargets(8,rawTargets,depth.Get());
    c->PSSetShaderResources(0,1,input.GetAddressOf());c->PSSetShader(original.Get(),nullptr,0);
    slot->lastUse=GetTickCount64();slot->stage=stage;slot->viewport=viewport;slot->crop={left,top,right-left,bottom-top};s.aliases[native.Get()]={native,slot};
    if(!s.latest || viewport.Width*double(viewport.Height)>=s.latest->viewport.Width*double(s.latest->viewport.Height))s.latest=slot;
    if(++s.captures==1 || s.captures==120)LOG_INFO("RenoDX scene HDR: captured {}x{} viewport at {}/{} in {}x{} texture, stage {}, {} draws; SDR chain untouched",viewport.Width,viewport.Height,viewport.TopLeftX,viewport.TopLeftY,desc.Width,desc.Height,stage,s.captures);
}
inline void Copy(ID3D11Resource* dest,ID3D11Resource* source,bool full) {
    if(!Requested())return;auto& s=State();std::lock_guard lock(s.mutex);
    auto it=s.aliases.find(source);if(full && it!=s.aliases.end())s.aliases[dest]={dest,it->second.image};else s.aliases.erase(dest);
}
inline Hdr10::SceneInput Open(ID3D12Device* d,UINT width,UINT height,bool presentation=true) {
    Hdr10::SceneInput result;if(!Requested() || !d || !width || !height)return result;
    auto& s=State();std::lock_guard lock(s.mutex);auto p=s.latest;
    auto report=[&](UINT outcome){
        if(!presentation)return;
        ++s.requests;
        if(outcome==0)++s.available;else if(outcome==1)++s.missing;else if(outcome==2)++s.aspectMismatch;else ++s.importFailed;
        if(s.requests==1 || s.requests%600==0)
            LOG_INFO("RenoDX scene HDR: {} presentation requests, {} scene imports, {} no-scene fallbacks, {} aspect mismatches, {} import failures; latest stage {}, crop {}x{}, output {}x{}",
                s.requests,s.available,s.missing,s.aspectMismatch,s.importFailed,p?p->stage:0,p?p->crop[2]:0,p?p->crop[3]:0,width,height);
    };
    if(!p){report(1);return result;}
    // Never stretch a different scene/camera or use a previous frame's image.
    if(std::abs(double(p->viewport.Width)/p->viewport.Height-double(width)/height)>.01){report(2);return result;}
    if(p->openedDevice.Get()!=d){
        p->openedHdr.Reset();p->openedReference.Reset();p->openedDevice.Reset();
        auto open=[&](ID3D11Texture2D* t,ComPtr<ID3D12Resource>& r){ComPtr<IDXGIResource1> shared;if(FAILED(t->QueryInterface(IID_PPV_ARGS(&shared))))return false;
            HANDLE handle=nullptr;if(FAILED(shared->CreateSharedHandle(nullptr,DXGI_SHARED_RESOURCE_READ|DXGI_SHARED_RESOURCE_WRITE,nullptr,&handle)))return false;
            auto hr=d->OpenSharedHandle(handle,IID_PPV_ARGS(&r));CloseHandle(handle);return SUCCEEDED(hr);};
        if(!open(p->hdr.Get(),p->openedHdr) || !open(p->reference.Get(),p->openedReference)){report(3);return result;}p->openedDevice=d;
    }
    p->lastUse=GetTickCount64();result.hdr=p->openedHdr;result.reference=p->openedReference;result.owner=p;
    result.rect={p->crop[0]/p->width,p->crop[1]/p->height,p->crop[2]/p->width,p->crop[3]/p->height};
    result.retire=[p](std::function<bool()> done){auto& s=State();std::lock_guard lock(s.mutex);p->dx12Recorded=true;p->done=std::move(done);p->reserved=false;};
    report(0);return result;
}
// DX11 retains resources referenced by its queued commands. DX12 consumers additionally
// own the slot and install a completion-plus-recording-retirement probe BEFORE recording.
inline void CollectLocked(Data& s,bool keepWarm,ULONGLONG now) {
    std::erase_if(s.slots,[&](const auto& p){
        if(p.use_count()!=1 || p->reserved)return false;
        if(!Hdr10::ReusableRecordedWork(p->dx12Recorded,p->done))return false;
        return !keepWarm || now-p->lastUse>=2000;
    });
}
inline void EndFrame(){auto& s=State();std::lock_guard lock(s.mutex);s.aliases.clear();s.latest.reset();CollectLocked(s,Requested(),GetTickCount64());}
}
