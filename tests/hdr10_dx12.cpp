#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdio>
#include <stdexcept>
#include <cstring>
#include "../OptiScaler/shaders/hdr/Hdr10.h"
#include "hdr_stubs/Config.h"
#include "../OptiScaler/shaders/hdr/HdrScreenshotReadback.h"
#include "../OptiScaler/framegen/xefg/XeFGHdr.h"
using Microsoft::WRL::ComPtr;
void check(HRESULT h){if(FAILED(h))throw std::runtime_error("D3D12 failure");}
int main(int argc,char** argv)try{
 const bool untracked = argc > 1 && !strcmp(argv[1],"--untracked");
 const bool xefg = argc > 1 && !strcmp(argv[1],"--xefg");
 ComPtr<IDXGIFactory4>f;check(CreateDXGIFactory1(IID_PPV_ARGS(&f)));ComPtr<IDXGIAdapter>a;check(f->EnumWarpAdapter(IID_PPV_ARGS(&a)));ComPtr<ID3D12Device>d;check(D3D12CreateDevice(a.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&d)));
 D3D12_COMMAND_QUEUE_DESC qd{};ComPtr<ID3D12CommandQueue>q;check(d->CreateCommandQueue(&qd,IID_PPV_ARGS(&q)));ComPtr<ID3D12CommandAllocator>alloc;check(d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&alloc)));ComPtr<ID3D12GraphicsCommandList>c;check(d->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,alloc.Get(),nullptr,IID_PPV_ARGS(&c)));
 D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;D3D12_RESOURCE_DESC td{};td.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;td.Width=4;td.Height=td.DepthOrArraySize=td.MipLevels=td.SampleDesc.Count=1;td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;ComPtr<ID3D12Resource>src;check(d->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&td,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&src)));
 D3D12_RESOURCE_DESC bd{};bd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;bd.Width=256;bd.Height=bd.DepthOrArraySize=bd.MipLevels=bd.SampleDesc.Count=1;bd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;hp.Type=D3D12_HEAP_TYPE_UPLOAD;ComPtr<ID3D12Resource>upload,read;check(d->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&upload)));void*ptr;check(upload->Map(0,nullptr,&ptr));memset(ptr,255,256);memset(ptr,0,4);upload->Unmap(0,nullptr);hp.Type=D3D12_HEAP_TYPE_READBACK;check(d->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&read)));
 D3D12_TEXTURE_COPY_LOCATION dst{},from{};dst.pResource=src.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;from.pResource=upload.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;from.PlacedFootprint.Footprint={DXGI_FORMAT_R8G8B8A8_UNORM,4,1,1,256};c->CopyTextureRegion(&dst,0,0,0,&from,nullptr);
 ComPtr<ID3D12Fence>fence;check(d->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));HANDLE event=CreateEvent(nullptr,FALSE,FALSE,nullptr);
 const bool sceneTest=argc>1 && !strcmp(argv[1],"--scene");
 Hdr10::SceneInput scene;std::function<bool()> sceneDone;unsigned retirementCalls=0;
 if(sceneTest){
   Config::Instance()->FfxivHDRMode.v=1;Config::Instance()->FfxivHDRExpansion.v=1;
   D3D12_HEAP_PROPERTIES gpu{};gpu.Type=D3D12_HEAP_TYPE_DEFAULT;D3D12_HEAP_PROPERTIES up{};up.Type=D3D12_HEAP_TYPE_UPLOAD;
   auto td=src->GetDesc();td.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;
   check(d->CreateCommittedResource(&gpu,D3D12_HEAP_FLAG_NONE,&td,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&scene.hdr)));
   check(d->CreateCommittedResource(&gpu,D3D12_HEAP_FLAG_NONE,&td,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&scene.reference)));
   ComPtr<ID3D12Resource>sceneUpload;auto uploadDesc=upload->GetDesc();uploadDesc.Width=1024;
   check(d->CreateCommittedResource(&up,D3D12_HEAP_FLAG_NONE,&uploadDesc,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&sceneUpload)));
   void* values=nullptr;check(sceneUpload->Map(0,nullptr,&values));memset(values,0,1024);auto* halves=(unsigned short*)values;
   for(unsigned i=0;i<16;i++){halves[i]=0x4000;halves[256+i]=0x3c00;}sceneUpload->Unmap(0,nullptr);
   for(unsigned image=0;image<2;image++){
      D3D12_TEXTURE_COPY_LOCATION a{},b{};a.pResource=image?scene.reference.Get():scene.hdr.Get();a.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      b.pResource=sceneUpload.Get();b.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;b.PlacedFootprint.Offset=image*512;b.PlacedFootprint.Footprint={DXGI_FORMAT_R16G16B16A16_FLOAT,4,1,1,256};c->CopyTextureRegion(&a,0,0,0,&b,nullptr);
      D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;barrier.Transition={a.pResource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON};c->ResourceBarrier(1,&barrier);
   }
   // Keep the upload alive through all submissions too.
   scene.owner=std::make_shared<ComPtr<ID3D12Resource>>(sceneUpload);
   scene.retire=[&](std::function<bool()> done){sceneDone=std::move(done);++retirementCalls;};
 }
 bool useScene=false;
 auto whiteCorrect=[&](unsigned value){return useScene?(value>680 && value<760):(value>=590 && value<=600);};
 for(unsigned frame=1;frame<=256;frame++){
  if(frame>1){check(alloc->Reset());check(untracked ? c->Reset(alloc.Get(),nullptr) : Hdr10::ResetCommands(c.Get(),alloc.Get()));}
  useScene=sceneTest && frame%2==1;
  ID3D12Resource* out=nullptr;
  if(xefg){
   xefg_swapchain_d3d12_resource_data_t tag{};tag.type=XEFG_SWAPCHAIN_RES_HUDLESS_COLOR;tag.pResource=src.Get();tag.incomingState=D3D12_RESOURCE_STATE_COPY_DEST;tag.validity=XEFG_SWAPCHAIN_RV_UNTIL_NEXT_PRESENT;tag.resourceBase={0,0};tag.resourceSize={4,1};
   auto invalid=tag;invalid.type=XEFG_SWAPCHAIN_RES_UI;
   if(XeFGHdr::PrepareHudless(d.Get(),c.Get(),invalid) || XeFGHdr::PrepareHudless(d.Get(),nullptr,tag))throw std::runtime_error("invalid HDR tag accepted");
   if(!XeFGHdr::PrepareHudless(d.Get(),c.Get(),tag))throw std::runtime_error("XeFG HDR preparation failed");
   if(tag.validity!=XEFG_SWAPCHAIN_RV_ONLY_NOW || tag.incomingState!=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE || tag.pResource==src.Get() || tag.pResource->GetDesc().Format!=DXGI_FORMAT_R10G10B10A2_UNORM || src->GetDesc().Format!=DXGI_FORMAT_R8G8B8A8_UNORM || tag.resourceSize.x!=4 || tag.resourceSize.y!=1)throw std::runtime_error("XeFG HDR tag contract failed");
   out=tag.pResource;
  }else out=Hdr10::Convert(d.Get(),c.Get(),src.Get(),D3D12_RESOURCE_STATE_COPY_DEST,useScene?scene:Hdr10::SceneInput{});if(!out){if(untracked && frame==33){puts("PASS: negative control reproduces original 32-packet exhaustion without notifications");CloseHandle(event);return 0;}throw std::runtime_error("conversion/pool failed");}
  if(useScene && (!sceneDone || sceneDone()))throw std::runtime_error("scene lifetime retired before submission");
  D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition={out,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_SOURCE};c->ResourceBarrier(1,&b);
  Hdr10::Screenshot::Readback screenshot;
  if(SUCCEEDED(screenshot.Allocate(d.Get(),src.Get())))throw std::runtime_error("SDR screenshot source accepted");
  check(screenshot.Allocate(d.Get(),out));screenshot.Record(c.Get());
  Hdr10::Screenshot::Readback sdrScreenshot;
  if(SUCCEEDED(sdrScreenshot.Allocate(d.Get(),out,false)))throw std::runtime_error("HDR source accepted as original SDR");
  check(sdrScreenshot.Allocate(d.Get(),src.Get(),false));sdrScreenshot.RecordPreservingState(c.Get(),D3D12_RESOURCE_STATE_COPY_DEST);
  // The preview diagnostic reads a shared RGBA8 mask in COMMON and restores
  // its state. Use the known SDR alpha pattern to verify the same copy path.
  D3D12_RESOURCE_BARRIER maskState{};maskState.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  maskState.Transition={src.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON};
  c->ResourceBarrier(1,&maskState);
  Hdr10::Screenshot::Readback maskScreenshot;
  check(maskScreenshot.Allocate(d.Get(),src.Get(),false));
  maskScreenshot.RecordPreservingState(c.Get(),D3D12_RESOURCE_STATE_COMMON);
  maskState.Transition.StateBefore=D3D12_RESOURCE_STATE_COMMON;maskState.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_DEST;
  c->ResourceBarrier(1,&maskState);
  from={};from.pResource=out;from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;dst={};dst.pResource=read.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint.Footprint={DXGI_FORMAT_R10G10B10A2_UNORM,4,1,1,256};c->CopyTextureRegion(&dst,0,0,0,&from,nullptr);b.Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_SOURCE;b.Transition.StateAfter=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;c->ResourceBarrier(1,&b);check(c->Close());ID3D12CommandList*l[]={c.Get()};if(untracked)q->ExecuteCommandLists(1,l);else Hdr10::ExecuteCommands(q.Get(),1,l);check(q->Signal(fence.Get(),frame));check(fence->SetEventOnCompletion(frame,event));WaitForSingleObject(event,10000);
  if(useScene)Hdr10::Reset(c.Get()); // A reusable recording must also be closed, not just GPU-complete.
  if(useScene && !sceneDone())throw std::runtime_error("scene lifetime not retired after GPU completion");
  check(read->Map(0,nullptr,&ptr));auto*p=(unsigned*)ptr;if((p[0]&0x3fffffff)!=0 || !whiteCorrect(p[1]&1023))throw std::runtime_error("HDR10 pixel incorrect");read->Unmap(0,nullptr);
  check(q->Signal(screenshot.fence.Get(),1));check(screenshot.fence->SetEventOnCompletion(1,event));
  if(WaitForSingleObject(event,10000)!=WAIT_OBJECT_0)throw std::runtime_error("screenshot fence timeout");
  check(screenshot.readback->Map(0,nullptr,&ptr));p=(unsigned*)((BYTE*)ptr+screenshot.footprint.Offset);
  if((p[0]&0x3fffffff)!=0 || !whiteCorrect(p[1]&1023))throw std::runtime_error("screenshot pixel incorrect");
  screenshot.readback->Unmap(0,nullptr);
  check(sdrScreenshot.readback->Map(0,nullptr,&ptr));p=(unsigned*)((BYTE*)ptr+sdrScreenshot.footprint.Offset);
  if(p[0]!=0 || p[1]!=0xffffffff)throw std::runtime_error("pre-HDR screenshot source changed");
  sdrScreenshot.readback->Unmap(0,nullptr);
  check(maskScreenshot.readback->Map(0,nullptr,&ptr));
  const auto* maskBytes=static_cast<const BYTE*>(ptr)+maskScreenshot.footprint.Offset;
  if(maskBytes[3]!=0 || maskBytes[7]!=255)throw std::runtime_error("preview alpha diagnostic readback incorrect");
  maskScreenshot.readback->Unmap(0,nullptr);
 }
 if(untracked)throw std::runtime_error("negative control failed to reproduce exhaustion");
 CloseHandle(event);
 if(sceneTest){if(retirementCalls!=128)throw std::runtime_error("scene retirement count incorrect");puts("PASS: production DX12 scene bridge, 256 HDR/missing-scene cycles, distinct highlights, explicit SDR fallback, and completion-gated guide lifetime");}
 else puts("PASS: production DX12 HDR converter, 10-bit output, black/203-nit white, 256 bridge-owned submission/reset cycles without global tracking hooks");return 0;
}catch(const std::exception&e){puts(e.what());return 1;}
