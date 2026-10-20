#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdio>
#include <stdexcept>
#include "../OptiScaler/hooks/DxgiSwapchainSizing.h"
using Microsoft::WRL::ComPtr;
void expect(bool ok,const char* reason){if(!ok)throw std::runtime_error(reason);}
void check(HRESULT hr){if(FAILED(hr)){printf("HRESULT %08x\n",unsigned(hr));throw std::runtime_error("DXGI/D3D12 call failed");}}
struct Window{
 HWND hwnd=CreateWindowExW(0,L"STATIC",L"OptiScaler resize flag test",WS_POPUP,0,0,96,96,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
 Window(){expect(hwnd!=nullptr,"hidden window creation failed");}
 ~Window(){DestroyWindow(hwnd);}
};
int main()try{
 constexpr UINT mask=DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING|DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
 for(UINT existing: {0u,0x40u,0x800u,0x840u,0x842u})for(UINT requested:{0u,2u,0x40u,0x800u,0x840u,0x842u,0x100u}){
  UINT flags=PreserveSwapchainResizeFlags(requested,existing);
  expect((flags&mask)==(existing&mask),"creation-time capability changed");
  expect((flags&~mask)==(requested&~mask),"mutable resize request lost");
  expect(PreserveSwapchainResizeFlags(flags,existing)==flags,"resize flags were not idempotent");
 }
 ComPtr<IDXGIFactory5> factory;check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
 BOOL tearing=FALSE;check(factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,&tearing,sizeof(tearing)));
 expect(tearing,"native tearing contract unavailable on this system");
 ComPtr<IDXGIAdapter> adapter;check(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));
 ComPtr<ID3D12Device> device;check(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)));
 D3D12_COMMAND_QUEUE_DESC qd{};ComPtr<ID3D12CommandQueue> queue;check(device->CreateCommandQueue(&qd,IID_PPV_ARGS(&queue)));
 for(UINT existing:{0u,0x40u,0x800u,0x840u}){
  printf("Creating native swapchain flags %x\n",existing|2);
  Window window;DXGI_SWAP_CHAIN_DESC1 desc{};desc.Width=64;desc.Height=64;desc.Format=DXGI_FORMAT_R10G10B10A2_UNORM;
  desc.SampleDesc.Count=1;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.BufferCount=6;desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;desc.Flags=existing|2;
  ComPtr<IDXGISwapChain1> chain;check(factory->CreateSwapChainForHwnd(queue.Get(),window.hwnd,&desc,nullptr,nullptr,&chain));
  if(existing==0x840){
   // Exact native flags in the user's log: 0x842 -> 0x42 during startup resize.
   const HRESULT old=chain->ResizeBuffers(6,96,96,desc.Format,0x42);
   printf("Native DXGI negative control 0x842 -> 0x42: %08x\n",unsigned(old));
   expect(FAILED(old),"old invalid flag transition unexpectedly succeeded");
  }
  // The DX11 game supplies 0x2. The proxy may additionally supply 0x40.
  // Both resize APIs must preserve the actual native chain's immutable flags.
  for(UINT requested:{2u,0x42u,0x802u,0x842u}){
   DXGI_SWAP_CHAIN_DESC actual{};check(chain->GetDesc(&actual));
   const UINT flags=PreserveSwapchainResizeFlags(requested,actual.Flags);
   check(chain->ResizeBuffers(6,96,96,desc.Format,flags));
   ComPtr<ID3D12Resource> backbuffer;check(chain->GetBuffer(0,IID_PPV_ARGS(&backbuffer)));expect(backbuffer!=nullptr,"resize left a null backbuffer");backbuffer.Reset();
   ComPtr<IDXGISwapChain3> chain3;check(chain.As(&chain3));
   UINT nodeMasks[6]{};IUnknown* queues[6];for(auto& presentQueue:queues)presentQueue=queue.Get();
   check(chain3->ResizeBuffers1(0,64,64,DXGI_FORMAT_UNKNOWN,flags,nodeMasks,queues));
   check(chain3->GetBuffer(0,IID_PPV_ARGS(&backbuffer)));expect(backbuffer!=nullptr,"ResizeBuffers1 left a null backbuffer");backbuffer.Reset();
  }
 }
 puts("PASS: native WARP DXGI reproduces invalid flag removal; both resize APIs preserve 35 flag combinations, mutable requests, zero-count/unknown-format semantics and valid backbuffers");return 0;
}catch(const std::exception& e){printf("FAIL: %s\n",e.what());return 1;}
