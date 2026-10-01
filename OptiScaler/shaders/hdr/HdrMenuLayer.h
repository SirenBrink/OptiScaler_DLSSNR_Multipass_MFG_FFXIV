#pragma once
#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstring>
#include <utility>
#include <climits>
#include <algorithm>
#include <cmath>
#include "../../include/imgui/imgui.h"

namespace Hdr10 {
inline D3D12_RECT MenuBounds(const ImDrawData& data, UINT width, UINT height)
{
    const D3D12_RECT full{0,0,LONG(width),LONG(height)};
    float left=float(width),top=float(height),right=0,bottom=0;
    for (const auto* list : data.CmdLists)
    {
        // Callbacks can draw arbitrary geometry not described by VtxBuffer.
        for (const auto& command : list->CmdBuffer)
            if (command.UserCallback) return full;
        for (const auto& vertex : list->VtxBuffer)
        {
            const float x=(vertex.pos.x-data.DisplayPos.x)*data.FramebufferScale.x;
            const float y=(vertex.pos.y-data.DisplayPos.y)*data.FramebufferScale.y;
            if (!std::isfinite(x) || !std::isfinite(y)) return full;
            left=std::min(left,x);top=std::min(top,y);
            right=std::max(right,x);bottom=std::max(bottom,y);
        }
    }
    // One pixel of padding covers rasterization/AA at geometry boundaries.
    return {LONG(std::clamp(std::floor(left)-1,0.f,float(width))),
            LONG(std::clamp(std::floor(top)-1,0.f,float(height))),
            LONG(std::clamp(std::ceil(right)+1,0.f,float(width))),
            LONG(std::clamp(std::ceil(bottom)+1,0.f,float(height)))};
}
// ImGui blends its theme and glyph coverage in an SDR intermediate. Only the
// completed layer is decoded, mapped to HDR, and composited with the HDR scene.
inline constexpr char MenuCompositeShader[] = R"(
Texture2D<float4> ui : register(t0);
Texture2D<float4> scene : register(t1);
float3 Linear(float3 x) {
    return float3(x.r <= .04045 ? x.r/12.92 : pow((x.r+.055)/1.055,2.4),
                  x.g <= .04045 ? x.g/12.92 : pow((x.g+.055)/1.055,2.4),
                  x.b <= .04045 ? x.b/12.92 : pow((x.b+.055)/1.055,2.4));
}
float3 Decode(float3 x) {
    float3 p = pow(saturate(x),32.0/2523.0);
    return 10000 * pow(max(p-3424.0/4096.0,0) / max(2413.0/128.0-2392.0/128.0*p,1e-6),16384.0/2610.0);
}
float3 Encode(float3 nits) {
    float3 p = pow(max(nits,0)/10000,2610.0/16384.0);
    return pow((3424.0/4096.0+2413.0/128.0*p)/(1+2392.0/128.0*p),2523.0/32.0);
}
float4 VS(uint id:SV_VertexID):SV_POSITION {
    float2 p=float2((id<<1)&2,id&2);
    return float4(p*float2(2,-2)+float2(-1,1),0,1);
}
float4 PS(float4 pos:SV_POSITION):SV_Target {
    int3 at=int3(pos.xy,0);
    float4 layer=ui.Load(at), background=scene.Load(at);
    if(layer.a <= 0) return background; // No colour roundtrip outside the UI.
    float3 rgb=Linear(saturate(layer.rgb / layer.a));
    float3 nits=203 * float3(dot(rgb,float3(.627404,.329282,.043314)),
        dot(rgb,float3(.069097,.919540,.011362)),dot(rgb,float3(.016391,.088013,.895595)));
    return float4(Encode(nits*layer.a + Decode(background.rgb)*(1-layer.a)), background.a);
})";

class MenuLayer {
    template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
    Ptr<ID3D12Device> device;
    Ptr<ID3D12Resource> layer, scene;
    Ptr<ID3D12DescriptorHeap> rtv, srv;
    Ptr<ID3D12RootSignature> root;
    Ptr<ID3D12PipelineState> pipeline;
    UINT width=0, height=0;
    D3D12_RECT bounds{};

    static void Barrier(ID3D12GraphicsCommandList* cmd, ID3D12Resource* image,
                        D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition={image,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after};
        cmd->ResourceBarrier(1,&b);
    }
    HRESULT Init(ID3D12Device* d, UINT w, UINT h) {
        device=d; width=w; height=h;
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors=1;
        HRESULT hr=d->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&rtv)); if(FAILED(hr))return hr;
        hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors=2;
        hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        hr=d->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&srv)); if(FAILED(hr))return hr;
        D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,2,0,0,0};
        D3D12_ROOT_PARAMETER param{};
        param.ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        param.DescriptorTable={1,&range}; param.ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC rd{1,&param,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
        Ptr<ID3DBlob> blob, error, vs, ps;
        hr=D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error); if(FAILED(hr))return hr;
        hr=d->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)); if(FAILED(hr))return hr;
        hr=D3DCompile(MenuCompositeShader,strlen(MenuCompositeShader),nullptr,nullptr,nullptr,"VS","vs_5_0",0,0,&vs,&error); if(FAILED(hr))return hr;
        hr=D3DCompile(MenuCompositeShader,strlen(MenuCompositeShader),nullptr,nullptr,nullptr,"PS","ps_5_0",0,0,&ps,&error); if(FAILED(hr))return hr;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature=root.Get();
        pd.VS={vs->GetBufferPointer(),vs->GetBufferSize()}; pd.PS={ps->GetBufferPointer(),ps->GetBufferSize()};
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.SampleMask=UINT_MAX; pd.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode=D3D12_CULL_MODE_NONE; pd.RasterizerState.DepthClipEnable=TRUE;
        pd.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets=1; pd.RTVFormats[0]=DXGI_FORMAT_R10G10B10A2_UNORM; pd.SampleDesc.Count=1;
        hr=d->CreateGraphicsPipelineState(&pd,IID_PPV_ARGS(&pipeline)); if(FAILED(hr))return hr;
        D3D12_RESOURCE_DESC td{};
        td.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width=w; td.Height=h;
        td.DepthOrArraySize=1; td.MipLevels=1; td.SampleDesc.Count=1;
        td.Format=DXGI_FORMAT_R8G8B8A8_UNORM; td.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_HEAP_PROPERTIES hp{}; hp.Type=D3D12_HEAP_TYPE_DEFAULT;
        D3D12_CLEAR_VALUE clear{}; clear.Format=td.Format;
        hr=d->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&td,D3D12_RESOURCE_STATE_RENDER_TARGET,&clear,IID_PPV_ARGS(&layer)); if(FAILED(hr))return hr;
        d->CreateRenderTargetView(layer.Get(),nullptr,rtv->GetCPUDescriptorHandleForHeapStart());
        td.Format=DXGI_FORMAT_R10G10B10A2_UNORM; td.Flags=D3D12_RESOURCE_FLAG_NONE;
        hr=d->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&td,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&scene)); if(FAILED(hr))return hr;
        auto handle=srv->GetCPUDescriptorHandleForHeapStart();
        d->CreateShaderResourceView(layer.Get(),nullptr,handle);
        handle.ptr+=d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        d->CreateShaderResourceView(scene.Get(),nullptr,handle);
        return S_OK;
    }
public:
    // Caller must retire the previous menu submission before reuse, resize or
    // destruction. The DX12 overlay's existing completion fence owns this rule.
    HRESULT Ensure(ID3D12Device* d, const D3D12_RESOURCE_DESC& desc) {
        if(!d || desc.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
           desc.Format!=DXGI_FORMAT_R10G10B10A2_UNORM || desc.SampleDesc.Count!=1 ||
           desc.DepthOrArraySize!=1 || desc.MipLevels!=1 || !desc.Width || !desc.Height ||
           desc.Width>16384 || desc.Height>16384)return E_INVALIDARG;
        if(device.Get()==d && width==desc.Width && height==desc.Height && layer)return S_OK;
        MenuLayer next;
        auto hr=next.Init(d,UINT(desc.Width),desc.Height);
        if(SUCCEEDED(hr))*this=std::move(next);
        return hr;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE Target() const { return rtv->GetCPUDescriptorHandleForHeapStart(); }
    void Begin(ID3D12GraphicsCommandList* cmd, ID3D12Resource* output, const D3D12_RECT* region=nullptr) {
        bounds=region?*region:D3D12_RECT{0,0,LONG(width),LONG(height)};
        Barrier(cmd,output,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION from{},to{};
        from.pResource=output;from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        to.pResource=scene.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_BOX box{UINT(bounds.left),UINT(bounds.top),0,UINT(bounds.right),UINT(bounds.bottom),1};
        cmd->CopyTextureRegion(&to,box.left,box.top,0,&from,&box);
        Barrier(cmd,output,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);
        const float transparent[4]{};
        auto target=Target(); cmd->ClearRenderTargetView(target,transparent,1,&bounds);
        cmd->OMSetRenderTargets(1,&target,FALSE,nullptr);
    }
    void End(ID3D12GraphicsCommandList* cmd, D3D12_CPU_DESCRIPTOR_HANDLE output) {
        Barrier(cmd,layer.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Barrier(cmd,scene.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        cmd->OMSetRenderTargets(1,&output,FALSE,nullptr);
        auto heap=srv.Get(); cmd->SetDescriptorHeaps(1,&heap);
        cmd->SetGraphicsRootSignature(root.Get()); cmd->SetPipelineState(pipeline.Get());
        cmd->SetGraphicsRootDescriptorTable(0,srv->GetGPUDescriptorHandleForHeapStart());
        D3D12_VIEWPORT vp{0,0,float(width),float(height),0,1};
        cmd->RSSetViewports(1,&vp); cmd->RSSetScissorRects(1,&bounds);
        cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST); cmd->DrawInstanced(3,1,0,0);
        Barrier(cmd,layer.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);
        Barrier(cmd,scene.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
    }
};
}
