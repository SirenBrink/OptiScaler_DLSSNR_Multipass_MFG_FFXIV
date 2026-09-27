#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdio>
#include <stdexcept>
#include <cstring>
#include "../OptiScaler/shaders/hdr/Hdr10.h"
using Microsoft::WRL::ComPtr;
void check(HRESULT h){if(FAILED(h))throw std::runtime_error("D3D12 failure");}
int main(int argc,char**)try{
 const bool untracked = argc > 1;
 ComPtr<IDXGIFactory4>f;check(CreateDXGIFactory1(IID_PPV_ARGS(&f)));ComPtr<IDXGIAdapter>a;check(f->EnumWarpAdapter(IID_PPV_ARGS(&a)));ComPtr<ID3D12Device>d;check(D3D12CreateDevice(a.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&d)));
 D3D12_COMMAND_QUEUE_DESC qd{};ComPtr<ID3D12CommandQueue>q;check(d->CreateCommandQueue(&qd,IID_PPV_ARGS(&q)));ComPtr<ID3D12CommandAllocator>alloc;check(d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&alloc)));ComPtr<ID3D12GraphicsCommandList>c;check(d->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,alloc.Get(),nullptr,IID_PPV_ARGS(&c)));
 D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;D3D12_RESOURCE_DESC td{};td.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;td.Width=4;td.Height=td.DepthOrArraySize=td.MipLevels=td.SampleDesc.Count=1;td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;ComPtr<ID3D12Resource>src;check(d->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&td,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&src)));
 D3D12_RESOURCE_DESC bd{};bd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;bd.Width=256;bd.Height=bd.DepthOrArraySize=bd.MipLevels=bd.SampleDesc.Count=1;bd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;hp.Type=D3D12_HEAP_TYPE_UPLOAD;ComPtr<ID3D12Resource>upload,read;check(d->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&upload)));void*ptr;check(upload->Map(0,nullptr,&ptr));memset(ptr,255,256);memset(ptr,0,4);upload->Unmap(0,nullptr);hp.Type=D3D12_HEAP_TYPE_READBACK;check(d->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&read)));
 D3D12_TEXTURE_COPY_LOCATION dst{},from{};dst.pResource=src.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;from.pResource=upload.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;from.PlacedFootprint.Footprint={DXGI_FORMAT_R8G8B8A8_UNORM,4,1,1,256};c->CopyTextureRegion(&dst,0,0,0,&from,nullptr);
 ComPtr<ID3D12Fence>fence;check(d->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));HANDLE event=CreateEvent(nullptr,FALSE,FALSE,nullptr);
 for(unsigned frame=1;frame<=256;frame++){
  if(frame>1){check(alloc->Reset());check(untracked ? c->Reset(alloc.Get(),nullptr) : Hdr10::ResetCommands(c.Get(),alloc.Get()));}
  auto*out=Hdr10::Convert(d.Get(),c.Get(),src.Get(),D3D12_RESOURCE_STATE_COPY_DEST);if(!out){if(untracked && frame==33){puts("PASS: negative control reproduces original 32-packet exhaustion without notifications");CloseHandle(event);return 0;}throw std::runtime_error("conversion/pool failed");}
  D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition={out,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_SOURCE};c->ResourceBarrier(1,&b);
  from={};from.pResource=out;from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;dst={};dst.pResource=read.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint.Footprint={DXGI_FORMAT_R10G10B10A2_UNORM,4,1,1,256};c->CopyTextureRegion(&dst,0,0,0,&from,nullptr);b.Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_SOURCE;b.Transition.StateAfter=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;c->ResourceBarrier(1,&b);check(c->Close());ID3D12CommandList*l[]={c.Get()};if(untracked)q->ExecuteCommandLists(1,l);else Hdr10::ExecuteCommands(q.Get(),1,l);check(q->Signal(fence.Get(),frame));check(fence->SetEventOnCompletion(frame,event));WaitForSingleObject(event,10000);
  check(read->Map(0,nullptr,&ptr));auto*p=(unsigned*)ptr;if((p[0]&0x3fffffff)!=0 || (p[1]&1023)<590 || (p[1]&1023)>600)throw std::runtime_error("HDR10 pixel incorrect");read->Unmap(0,nullptr);
 }
 if(untracked)throw std::runtime_error("negative control failed to reproduce exhaustion");
 CloseHandle(event);puts("PASS: production DX12 HDR converter, 10-bit output, black/203-nit white, 256 bridge-owned submission/reset cycles without global tracking hooks");return 0;
}catch(const std::exception&e){puts(e.what());return 1;}
