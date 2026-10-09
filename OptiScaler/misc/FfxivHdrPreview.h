#pragma once
#include <shaders/hdr/SceneInput.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <mutex>
#include <vector>
#include <array>

// Accumulate native UI transmittance, including atlas-backed panels behind
// rendered previews. RGB remains in the native SDR chain; this mask only tells
// HDR composition how much of the original world can pass through the UI.
namespace FfxivHdrPreview {
using Microsoft::WRL::ComPtr;
inline constexpr GUID tag{0x69ac2fe2,0x65cf,0x4290,{0x87,0x26,0x31,0x0b,0x0c,0x54,0x12,0x92}};
inline bool Tag(ID3D11PixelShader* shader,UINT crc) {
    switch(crc) {
    case 3865947726u:case 2966694105u:case 3759127293u:case 1898885941u:
    case 4076256744u:case 3915247300u:case 1823062889u: {
        UINT value=1;return SUCCEEDED(shader->SetPrivateData(tag,sizeof(value),&value));
    }
    }
    return false;
}
struct Slot {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D12Resource> opened;
    ComPtr<ID3D12Device> openedDevice;
    std::function<bool()> done;
    UINT width=0,height=0;bool recorded=false;
};
struct Data {
    std::mutex mutex;ComPtr<ID3D11Device> device;ComPtr<ID3D11BlendState> blend,opaqueBlend;
    struct DepthState {ComPtr<ID3D11DepthStencilState> native,readOnly;};
    std::vector<DepthState> depthStates;
    std::vector<std::shared_ptr<Slot>> slots;std::shared_ptr<Slot> current;
    uint64_t draws=0;bool sealed=false;

};
inline Data& State(){static auto* value=new Data;return *value;}
template<class F> inline void BeforeDraw(ID3D11DeviceContext* c,F&& draw) {
    {auto& s=State();std::lock_guard lock(s.mutex);if(s.sealed)return;}
    ComPtr<ID3D11PixelShader> shader;c->PSGetShader(&shader,nullptr,nullptr);
    UINT marked=0,size=sizeof(marked);
    if(!shader || FAILED(shader->GetPrivateData(tag,&size,&marked)) || !marked)return;
    std::array<ComPtr<ID3D11ShaderResourceView>,8> inputs;ID3D11ShaderResourceView* rawInputs[8]{};
    c->PSGetShaderResources(0,8,rawInputs);for(UINT i=0;i<8;++i)inputs[i].Attach(rawInputs[i]);
    ComPtr<ID3D11Resource> resource;ComPtr<ID3D11Texture2D> source;D3D11_TEXTURE2D_DESC sd{};
    for(UINT i=0;i<8;++i)if(inputs[i]) {
        inputs[i]->GetResource(&resource);ComPtr<ID3D11Texture2D> t;if(FAILED(resource.As(&t)))continue;
        D3D11_TEXTURE2D_DESC desc{};t->GetDesc(&desc);
        if(desc.SampleDesc.Count!=1)continue;
        if(!source || UINT64(desc.Width)*desc.Height>UINT64(sd.Width)*sd.Height){source=t;sd=desc;}
    }
    if(!source)return;
    ComPtr<ID3D11RenderTargetView> target;ComPtr<ID3D11DepthStencilView> depth;
    c->OMGetRenderTargets(1,&target,&depth);if(!target)return;
    target->GetResource(&resource);ComPtr<ID3D11Texture2D> native;if(FAILED(resource.As(&native)))return;
    D3D11_TEXTURE2D_DESC td{};native->GetDesc(&td);
    if(td.Width<640 || td.Height<360 || td.SampleDesc.Count!=1 || source==native)return;
    // Replay BEFORE the native draw, with identical clipping/tests but no
    // depth/stencil writes. The actual native draw then runs exactly once.
    ComPtr<ID3D11DepthStencilState> nativeDepth;UINT stencilRef=0;
    D3D11_DEPTH_STENCIL_DESC dd{};
    if(depth) {
        c->OMGetDepthStencilState(&nativeDepth,&stencilRef);
        if(nativeDepth)nativeDepth->GetDesc(&dd);
        else {dd.DepthEnable=true;dd.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;dd.DepthFunc=D3D11_COMPARISON_LESS;dd.StencilReadMask=dd.StencilWriteMask=255;dd.FrontFace=dd.BackFace={D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_KEEP,D3D11_COMPARISON_ALWAYS};}
    }
    ComPtr<ID3D11BlendState> original;FLOAT factor[4]{};UINT sampleMask=0;
    c->OMGetBlendState(&original,factor,&sampleMask);
    D3D11_BLEND_DESC bd{};if(original)original->GetDesc(&bd);
    else bd.RenderTarget[0].RenderTargetWriteMask=15;
    const auto& nativeBlend=bd.RenderTarget[0];
    const bool opaque=!nativeBlend.BlendEnable || (nativeBlend.BlendOp==D3D11_BLEND_OP_ADD && nativeBlend.DestBlend==D3D11_BLEND_ZERO);
    auto& s=State();
    if(original && (bd.AlphaToCoverageEnable || (nativeBlend.RenderTargetWriteMask&7)!=7 ||
       (nativeBlend.BlendEnable && !opaque && (nativeBlend.BlendOp!=D3D11_BLEND_OP_ADD ||
       (nativeBlend.SrcBlend!=D3D11_BLEND_SRC_ALPHA && nativeBlend.SrcBlend!=D3D11_BLEND_ONE) ||
       nativeBlend.DestBlend!=D3D11_BLEND_INV_SRC_ALPHA))))return;
    ComPtr<ID3D11Predicate> predicate;BOOL predicateValue=FALSE;c->GetPredication(&predicate,&predicateValue);if(predicate)return;
    std::array<ComPtr<ID3D11RenderTargetView>,8> targets;ID3D11RenderTargetView* raw[8]{};
    c->OMGetRenderTargets(8,raw,nullptr);for(UINT i=0;i<8;++i)targets[i].Attach(raw[i]);
    for(UINT i=1;i<8;++i)if(raw[i])return;
    ComPtr<ID3D11Device> device;c->GetDevice(&device);std::lock_guard lock(s.mutex);
    if(s.device!=device){s.device=device;s.blend.Reset();s.opaqueBlend.Reset();s.depthStates.clear();s.current.reset();s.slots.clear();}
    auto& maskBlend=opaque?s.opaqueBlend:s.blend;
    if(!maskBlend) {
        D3D11_BLEND_DESC b{};auto& r=b.RenderTarget[0];r.BlendEnable=true;
        r.SrcBlend=D3D11_BLEND_ZERO;r.DestBlend=opaque?D3D11_BLEND_ZERO:D3D11_BLEND_INV_SRC_ALPHA;r.BlendOp=D3D11_BLEND_OP_ADD;
        r.SrcBlendAlpha=D3D11_BLEND_ZERO;r.DestBlendAlpha=opaque?D3D11_BLEND_ZERO:D3D11_BLEND_INV_SRC_ALPHA;
        r.BlendOpAlpha=D3D11_BLEND_OP_ADD;r.RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;
        if(FAILED(device->CreateBlendState(&b,&maskBlend)))return;
    }
    ComPtr<ID3D11DepthStencilState> readOnly;
    if(depth) {
        for(auto& entry:s.depthStates)if(entry.native==nativeDepth){readOnly=entry.readOnly;break;}
        if(!readOnly) {
            if(s.depthStates.size()>=32)return;
            dd.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ZERO;dd.StencilWriteMask=0;
            if(FAILED(device->CreateDepthStencilState(&dd,&readOnly)))return;
            s.depthStates.push_back({nativeDepth,readOnly});
        }
    }
    if(s.current && (s.current->width!=td.Width || s.current->height!=td.Height)) {
        // Offscreen UI can render before the full-screen composition. It must
        // not occupy the frame's mask and block later screen-size previews.
        if(UINT64(td.Width)*td.Height<=UINT64(s.current->width)*s.current->height)return;
        s.current.reset();
    }
    if(!s.current) {
        for(auto& p:s.slots)if(p.use_count()==1 && Hdr10::ReusableRecordedWork(p->recorded,p->done)){s.current=p;break;}
        if(!s.current){if(s.slots.size()>=6)return;s.current=std::make_shared<Slot>();s.slots.push_back(s.current);}
        auto& p=*s.current;
        p.done={};p.recorded=false;
        if(!p.texture || p.width!=td.Width || p.height!=td.Height) {
            p=Slot{};D3D11_TEXTURE2D_DESC d{};d.Width=td.Width;d.Height=td.Height;
            d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;d.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
            d.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
            d.MiscFlags=D3D11_RESOURCE_MISC_SHARED|D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
            if(FAILED(device->CreateTexture2D(&d,nullptr,&p.texture)) ||
               FAILED(device->CreateRenderTargetView(p.texture.Get(),nullptr,&p.rtv))){s.current.reset();return;}
            p.width=d.Width;p.height=d.Height;
        }
        const FLOAT clear[4]{1,1,1,1};c->ClearRenderTargetView(p.rtv.Get(),clear);
    }
    auto& p=*s.current;if(p.width!=td.Width || p.height!=td.Height)return;
    c->OMSetRenderTargets(1,p.rtv.GetAddressOf(),depth.Get());c->OMSetBlendState(maskBlend.Get(),factor,sampleMask);
    if(depth)c->OMSetDepthStencilState(readOnly.Get(),stencilRef);
    draw();
    c->OMSetRenderTargets(8,raw,depth.Get());c->OMSetBlendState(original.Get(),factor,sampleMask);
    if(depth)c->OMSetDepthStencilState(nativeDepth.Get(),stencilRef);
    ++s.draws;
}
// Freeze the mask at the same frame boundary as the SDR backbuffer, before
// the bridge signals its DX11 fence. Importing never records native GPU work.
inline void SealFrame() {
    auto& s=State();std::lock_guard lock(s.mutex);s.sealed=true;
}
inline Hdr10::SceneInput Attach(Hdr10::SceneInput image,ID3D12Device* device,UINT w,UINT h) {
    auto& s=State();std::lock_guard lock(s.mutex);auto p=s.current;
    if(!image.hdr || !p || p->width!=w || p->height!=h)return image;
    if(p->openedDevice.Get()!=device) {
        ComPtr<IDXGIResource1> resource;if(FAILED(p->texture.As(&resource)))return image;
        HANDLE handle=nullptr;if(FAILED(resource->CreateSharedHandle(nullptr,DXGI_SHARED_RESOURCE_READ|DXGI_SHARED_RESOURCE_WRITE,nullptr,&handle)))return image;
        ComPtr<ID3D12Resource> opened;auto hr=device->OpenSharedHandle(handle,IID_PPV_ARGS(&opened));CloseHandle(handle);
        if(FAILED(hr))return image;p->opened=opened;p->openedDevice=device;
    }
    struct Owners {std::shared_ptr<void> scene;std::shared_ptr<Slot> preview;};
    image.owner=std::make_shared<Owners>(Owners{image.owner,p});image.previewMask=p->opened;
    auto previous=image.retire;image.retire=[previous,p](std::function<bool()> done) {
        if(previous)previous(done);auto& state=State();std::lock_guard lock(state.mutex);
        // Multiple conversions can read this frame; all must finish before reuse.
        auto older=p->done;p->done=[older,done]{return (!older || older()) && done && done();};p->recorded=true;
    };
    return image;
}
inline void EndFrame(){auto& s=State();std::lock_guard lock(s.mutex);s.current.reset();s.sealed=false;}
}
