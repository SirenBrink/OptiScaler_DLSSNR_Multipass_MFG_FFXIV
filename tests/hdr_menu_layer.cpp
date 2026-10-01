#define NOMINMAX
#include "../OptiScaler/shaders/hdr/HdrMenuLayer.h"
#include "../OptiScaler/menu/menu_hdr_colors.h"
#include <dxgi1_6.h>
#include <d3d12sdklayers.h>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <vector>
#include <source_location>
#include <string>
#include <limits>
#pragma comment(lib,"d3d12.lib")
#pragma comment(lib,"dxgi.lib")
#pragma comment(lib,"d3dcompiler.lib")
using Microsoft::WRL::ComPtr;
void check(HRESULT hr) { if(FAILED(hr)) {printf("HRESULT %08lx\n",hr);throw std::runtime_error("DX12 failed");} }
void expect(bool ok, std::source_location at=std::source_location::current()) { if(!ok)throw std::runtime_error("pixel validation failed line " + std::to_string(at.line())); }
float encode(float n) {float p=powf(n/10000.f,2610.f/16384.f);return powf((3424.f/4096.f+2413.f/128.f*p)/(1+2392.f/128.f*p),2523.f/32.f);}
float decode(float v) {float p=powf(v,32.f/2523.f);return 10000*powf(fmaxf(p-3424.f/4096.f,0)/(2413.f/128.f-2392.f/128.f*p),16384.f/2610.f);}
float linear(float v) {return v<=.04045f?v/12.92f:powf((v+.055f)/1.055f,2.4f);}
int main() try {
    for (bool opti : {false, true}) for (bool output : {false, true}) for (bool native : {false, true}) {
        const bool legacy=MenuHdrColors::NeedsLegacyToneMap(opti,output,native);
        expect(legacy == (!opti && (output || native)));
        for (const ImVec4 colour : {ImVec4(1,1,1,.7f), ImVec4(.9f,.93f,.95f,1), ImVec4(.09f,.09f,.1f,1), ImVec4(0,.4f,.77f,.6f), ImVec4(0,0,0,0)}) {
            const auto actual=MenuHdrColors::Transform(colour,legacy);
            expect(actual.w==colour.w);
            if(!legacy) expect(actual.x==colour.x && actual.y==colour.y && actual.z==colour.z);
            else {
                const float peak=std::max(colour.x,std::max(colour.y,colour.z));
                expect(fabsf(actual.x-colour.x/(1+peak))<1e-6f && fabsf(actual.y-colour.y/(1+peak))<1e-6f && fabsf(actual.z-colour.z/(1+peak))<1e-6f);
            }
        }
    }
    expect(MenuHdrColors::Transform(ImVec4(1,1,1,1),true).x==.5f);
    puts("PASS: original theme colours reach OptiHDR unchanged; legacy HDR compression and SDR behaviour preserved");
    ImGui::CreateContext();
    {
        ImDrawList list(ImGui::GetDrawListSharedData());
        ImDrawData draw;draw.DisplayPos=ImVec2(20,10);draw.FramebufferScale=ImVec2(2,2);
        draw.CmdLists.push_back(&list);
        ImDrawVert v{};v.pos=ImVec2(120,90);list.VtxBuffer.push_back(v);
        v.pos=ImVec2(220,140);list.VtxBuffer.push_back(v);
        auto bounds=Hdr10::MenuBounds(draw,3840,2160);
        expect(bounds.left==199 && bounds.top==159 && bounds.right==401 && bounds.bottom==261);
        ImDrawCmd callback;callback.UserCallback=ImDrawCallback_ResetRenderState;list.CmdBuffer.push_back(callback);
        bounds=Hdr10::MenuBounds(draw,3840,2160);expect(bounds.left==0 && bounds.top==0 && bounds.right==3840 && bounds.bottom==2160);
        list.CmdBuffer.clear();list.VtxBuffer[0].pos.x=std::numeric_limits<float>::quiet_NaN();
        bounds=Hdr10::MenuBounds(draw,3840,2160);expect(bounds.right==3840 && bounds.bottom==2160);
    }
    ImGui::DestroyContext();
    puts("PASS: UI damage bounds account for display origin, DPI scaling, callbacks and invalid coordinates");
    ComPtr<ID3D12Debug> debug;
    const bool debugEnabled=SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));
    if(debugEnabled)debug->EnableDebugLayer();
    ComPtr<IDXGIFactory4> factory; check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter> adapter;check(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));
    ComPtr<ID3D12Device> device;check(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)));
    ComPtr<ID3D12InfoQueue> info;device.As(&info);
    ComPtr<ID3D12CommandQueue> queue;D3D12_COMMAND_QUEUE_DESC qd{};check(device->CreateCommandQueue(&qd,IID_PPV_ARGS(&queue)));
    ComPtr<ID3D12CommandAllocator> alloc;check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&alloc)));
    ComPtr<ID3D12GraphicsCommandList> cmd;check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,alloc.Get(),nullptr,IID_PPV_ARGS(&cmd)));check(cmd->Close());
    ComPtr<ID3D12Fence> fence;check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));
    HANDLE event=CreateEvent(nullptr,FALSE,FALSE,nullptr);expect(event!=nullptr);
    ComPtr<ID3D12DescriptorHeap> heap;D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;hd.NumDescriptors=1;check(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap)));
    Hdr10::MenuLayer menu;
    // ImGui's standard coverage blend into the same SDR intermediate format.
    const char* glyph=R"(float4 VS(uint i:SV_VertexID):SV_POSITION {float2 p=float2((i<<1)&2,i&2);return float4(p*float2(2,-2)+float2(-1,1),0,1);} float4 PS():SV_Target{return float4(1,1,1,.5);})";
    ComPtr<ID3DBlob> vs,ps,blob,error;
    check(D3DCompile(glyph,strlen(glyph),nullptr,nullptr,nullptr,"VS","vs_5_0",0,0,&vs,&error));
    check(D3DCompile(glyph,strlen(glyph),nullptr,nullptr,nullptr,"PS","ps_5_0",0,0,&ps,&error));
    D3D12_ROOT_SIGNATURE_DESC rd{};rd.Flags=D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    check(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error));
    ComPtr<ID3D12RootSignature> root;check(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)));
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};pd.pRootSignature=root.Get();pd.VS={vs->GetBufferPointer(),vs->GetBufferSize()};pd.PS={ps->GetBufferPointer(),ps->GetBufferSize()};
    auto& blend=pd.BlendState.RenderTarget[0];blend.BlendEnable=TRUE;blend.SrcBlend=D3D12_BLEND_SRC_ALPHA;blend.DestBlend=D3D12_BLEND_INV_SRC_ALPHA;blend.BlendOp=D3D12_BLEND_OP_ADD;
    blend.SrcBlendAlpha=D3D12_BLEND_ONE;blend.DestBlendAlpha=D3D12_BLEND_INV_SRC_ALPHA;blend.BlendOpAlpha=D3D12_BLEND_OP_ADD;blend.RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask=UINT_MAX;pd.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;pd.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;pd.RasterizerState.DepthClipEnable=TRUE;
    pd.NumRenderTargets=1;pd.RTVFormats[0]=DXGI_FORMAT_R8G8B8A8_UNORM;pd.SampleDesc.Count=1;pd.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    ComPtr<ID3D12PipelineState> glyphPipeline;check(device->CreateGraphicsPipelineState(&pd,IID_PPV_ARGS(&glyphPipeline)));
    for(UINT frame=1;frame<=32;++frame) {
        const UINT width=frame<=16?16:32;
        D3D12_RESOURCE_DESC td{};td.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;td.Width=width;td.Height=2;td.DepthOrArraySize=1;td.MipLevels=1;td.SampleDesc.Count=1;td.Format=DXGI_FORMAT_R10G10B10A2_UNORM;td.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
        ComPtr<ID3D12Resource> output;check(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&td,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&output)));
        auto target=heap->GetCPUDescriptorHandleForHeapStart();device->CreateRenderTargetView(output.Get(),nullptr,target);
        auto invalid=td;invalid.Format=DXGI_FORMAT_R8G8B8A8_UNORM;expect(FAILED(menu.Ensure(device.Get(),invalid)));
        check(menu.Ensure(device.Get(),td));
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT64 bytes;device->GetCopyableFootprints(&td,0,1,0,&footprint,nullptr,nullptr,&bytes);
        D3D12_RESOURCE_DESC bd{};bd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;bd.Width=bytes;bd.Height=1;bd.DepthOrArraySize=1;bd.MipLevels=1;bd.SampleDesc.Count=1;bd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;hp.Type=D3D12_HEAP_TYPE_READBACK;
        ComPtr<ID3D12Resource> read;check(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&read)));
        check(alloc->Reset());check(cmd->Reset(alloc.Get(),nullptr));
        float bg=encode(500);float clear[]={bg,bg,bg,1};cmd->ClearRenderTargetView(target,clear,0,nullptr);
        const D3D12_RECT region{0,0,8,1};
        menu.Begin(cmd.Get(),output.Get(), frame%2 ? &region : nullptr);
        const float colours[][4]={{.1f,.1f,.1f,1},{.5f,.5f,.5f,1},{1,1,1,1},{.25f,.25f,.25f,.5f},{1,0,0,1},{.1f,.1f,.1f,1},{0,0,0,1}};
        for(LONG i=0;i<7;++i) {
            D3D12_RECT rect{i+1,0,i+2,1};
            const auto colour=MenuHdrColors::Transform(ImVec4(colours[i][0],colours[i][1],colours[i][2],colours[i][3]),MenuHdrColors::NeedsLegacyToneMap(true,true,false));
            const float channels[]={colour.x,colour.y,colour.z,colour.w};
            cmd->ClearRenderTargetView(menu.Target(),channels,1,&rect);
        }
        D3D12_VIEWPORT vp{0,0,float(width),2,0,1};D3D12_RECT rect{6,0,7,1};cmd->RSSetViewports(1,&vp);cmd->RSSetScissorRects(1,&rect);
        cmd->SetGraphicsRootSignature(root.Get());cmd->SetPipelineState(glyphPipeline.Get());cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);cmd->DrawInstanced(3,1,0,0);
        menu.End(cmd.Get(),target);
        D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition={output.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE};cmd->ResourceBarrier(1,&b);
        D3D12_TEXTURE_COPY_LOCATION dst{},src{};dst.pResource=read.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=footprint;src.pResource=output.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        cmd->CopyTextureRegion(&dst,0,0,0,&src,nullptr);check(cmd->Close());ID3D12CommandList* lists[]={cmd.Get()};queue->ExecuteCommandLists(1,lists);check(queue->Signal(fence.Get(),frame));check(fence->SetEventOnCompletion(frame,event));expect(WaitForSingleObject(event,10000)==WAIT_OBJECT_0);
        void* mapped;check(read->Map(0,nullptr,&mapped));auto pixels=static_cast<UINT*>(mapped);
        const UINT bgCode=UINT(lroundf(bg*1023));
        expect((pixels[0]&1023)==bgCode && pixels[0]==pixels[footprint.Footprint.RowPitch/4]);
        for(UINT x=8;x<width;++x)expect(pixels[x]==pixels[0]);
        for(UINT x=0;x<width;++x)expect(pixels[footprint.Footprint.RowPitch/4+x]==pixels[0]);
        for(int x=1;x<=7;++x)for(int channel=0;channel<3;++channel) {
            float nits=0;
            if(x<=3) {float value=x==1?26.f/255:x==2?128.f/255:1;nits=203*linear(value);}
            if(x==4) {float alpha=128.f/255;nits=203*linear(.5f)*alpha+decode(bgCode/1023.f)*(1-alpha);}
            if(x==5) {const float matrix[]={.627404f,.069097f,.016391f};nits=203*matrix[channel];}
            if(x==6)nits=203*linear(141.f/255);
            const UINT actual=(pixels[x]>>(10*channel))&1023;
            if(abs(int(actual)-int(lroundf(encode(nits)*1023)))>2) {printf("frame %u x%d channel%d actual%u expected%.1f\n",frame,x,channel,actual,encode(nits)*1023);expect(false);}
        }
        read->Unmap(0,nullptr);
    }
    if(info)for(UINT64 i=0;i<info->GetNumStoredMessages();++i) {
        SIZE_T size=0;check(info->GetMessage(i,nullptr,&size));std::vector<char> data(size);auto message=reinterpret_cast<D3D12_MESSAGE*>(data.data());check(info->GetMessage(i,message,&size));
        if(message->Severity<=D3D12_MESSAGE_SEVERITY_ERROR) {puts(message->pDescription);throw std::runtime_error("debug layer error");}
    }
    CloseHandle(event);
    printf("PASS: production HDR menu layer; SDR glyph blend, black/grey/white, colour gamut, transparency, untouched scene; partial/full composition, 32 fenced reuse/resize cycles; debug layer %s\n",debugEnabled?"clean":"unavailable");
    return 0;
} catch(const std::exception& e) {puts(e.what());return 1;}
