#define NOMINMAX
#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <vector>
#include <array>
#include <cstdio>
#include <stdexcept>
#include "../OptiScaler/misc/FfxivPreviewInterop.h"
#pragma comment(lib,"d3d11.lib")
#pragma comment(lib,"d3d12.lib")
#pragma comment(lib,"dxgi.lib")
using Microsoft::WRL::ComPtr;
static void check(HRESULT result){if(FAILED(result)){printf("HRESULT %08X\n",unsigned(result));throw std::runtime_error("Interop failed");}}
int main(){
    ComPtr<ID3D11Device> d11;ComPtr<ID3D11DeviceContext> c;check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&d11,nullptr,&c));
    ComPtr<IDXGIDevice> dxgi;check(d11.As(&dxgi));ComPtr<IDXGIAdapter> adapter;check(dxgi->GetAdapter(&adapter));
    ComPtr<ID3D12Device> d12;check(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&d12)));
    ComPtr<ID3D11Device5> d5;ComPtr<ID3D11DeviceContext4> c4;check(d11.As(&d5));check(c.As(&c4));
    ComPtr<ID3D11Fence> f11;ComPtr<ID3D12Fence> f12;
    if(!FfxivPreviewInterop::CreateFence(d5.Get(),d12.Get(),f11,f12))throw std::runtime_error("Shared fence failed");
    ComPtr<ID3D12CommandQueue> queue;D3D12_COMMAND_QUEUE_DESC q{};q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;check(d12->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)));
    UINT64 value=0;
    for(auto format:{DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_R32_FLOAT,DXGI_FORMAT_R16G16_FLOAT,
                     DXGI_FORMAT_R8G8B8A8_UNORM}){
        FfxivPreviewInterop::Surface source,output;
        if(!FfxivPreviewInterop::CreateSurface(d11.Get(),d12.Get(),source,17,9,format) || !FfxivPreviewInterop::CreateSurface(d11.Get(),d12.Get(),output,17,9,format))throw std::runtime_error("Shared surface failed");
        const UINT bpp=format==DXGI_FORMAT_R16G16B16A16_FLOAT?8:4;
        std::vector<unsigned char> pixels(17*9*bpp);for(UINT i=0;i<pixels.size();++i)pixels[i]=unsigned char(i*13);
        c->UpdateSubresource(source.dx11.Get(),0,nullptr,pixels.data(),17*bpp,0);
        check(c4->Signal(f11.Get(),++value));check(queue->Wait(f12.Get(),value));
        ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> commands;
        check(d12->CreateCommandAllocator(q.Type,IID_PPV_ARGS(&allocator)));check(d12->CreateCommandList(0,q.Type,allocator.Get(),nullptr,IID_PPV_ARGS(&commands)));
        auto barrier=[&](ID3D12Resource* r,D3D12_RESOURCE_STATES from,D3D12_RESOURCE_STATES to){D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,from,to};commands->ResourceBarrier(1,&b);};
        barrier(source.dx12.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);barrier(output.dx12.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);
        commands->CopyResource(output.dx12.Get(),source.dx12.Get());
        barrier(source.dx12.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);barrier(output.dx12.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);
        check(commands->Close());ID3D12CommandList* list[]{commands.Get()};queue->ExecuteCommandLists(1,list);check(queue->Signal(f12.Get(),++value));check(c4->Wait(f11.Get(),value));
        D3D11_TEXTURE2D_DESC desc{};output.dx11->GetDesc(&desc);desc.BindFlags=desc.MiscFlags=0;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> read;check(d11->CreateTexture2D(&desc,nullptr,&read));c->CopyResource(read.Get(),output.dx11.Get());check(c4->Signal(f11.Get(),++value));c->Flush();
        HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);check(f12->SetEventOnCompletion(value,event));DWORD waited=WaitForSingleObject(event,5000);CloseHandle(event);if(waited!=WAIT_OBJECT_0)throw std::runtime_error("Shared fence timed out");
        D3D11_MAPPED_SUBRESOURCE mapped{};check(c->Map(read.Get(),0,D3D11_MAP_READ,0,&mapped));
        for(UINT row=0;row<9;++row)if(memcmp(static_cast<unsigned char*>(mapped.pData)+row*mapped.RowPitch,pixels.data()+row*17*bpp,17*bpp))throw std::runtime_error("Interop pixels changed");
        c->Unmap(read.Get(),0);printf("PASS: production shared surface format %u, DX11 -> DX12 -> DX11 GPU fences and exact pixels\n",unsigned(format));
    }
}
