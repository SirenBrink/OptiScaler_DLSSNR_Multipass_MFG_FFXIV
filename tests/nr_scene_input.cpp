#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <cstring>
#include "../OptiScaler/shaders/hdr/NrSceneInput.h"
using Microsoft::WRL::ComPtr;
void check(HRESULT h){if(FAILED(h))throw std::runtime_error("D3D12 failure");}
void expect(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
float half(unsigned short bits){unsigned sign=bits>>15,exp=(bits>>10)&31,mant=bits&1023;return (sign?-1.f:1.f)*std::ldexp(exp?1.f+mant/1024.f:mant/1024.f,exp?int(exp)-15:-14);}
int main()try {
 ComPtr<IDXGIFactory4> factory;check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));ComPtr<IDXGIAdapter> adapter;check(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));ComPtr<ID3D12Device> device;check(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)));
 D3D12_COMMAND_QUEUE_DESC qd{};ComPtr<ID3D12CommandQueue> queue;check(device->CreateCommandQueue(&qd,IID_PPV_ARGS(&queue)));ComPtr<ID3D12CommandAllocator> alloc;check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&alloc)));ComPtr<ID3D12GraphicsCommandList> cmd;check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,alloc.Get(),nullptr,IID_PPV_ARGS(&cmd)));
 ComPtr<ID3D12Fence> fence;check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));HANDLE event=CreateEvent(nullptr,FALSE,FALSE,nullptr);
 auto texture=[&](unsigned w,unsigned h,D3D12_RESOURCE_STATES state){D3D12_RESOURCE_DESC td{};td.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;td.Width=w;td.Height=h;td.DepthOrArraySize=td.MipLevels=td.SampleDesc.Count=1;td.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;ComPtr<ID3D12Resource> res;check(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&td,state,nullptr,IID_PPV_ARGS(&res)));return res;};
 auto buffer=[&](D3D12_HEAP_TYPE type){D3D12_RESOURCE_DESC bd{};bd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;bd.Width=2048;bd.Height=bd.DepthOrArraySize=bd.MipLevels=bd.SampleDesc.Count=1;bd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;D3D12_HEAP_PROPERTIES hp{};hp.Type=type;ComPtr<ID3D12Resource> res;check(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&bd,type==D3D12_HEAP_TYPE_UPLOAD?D3D12_RESOURCE_STATE_GENERIC_READ:D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&res)));return res;};
 expect(!Hdr10::ReusableRecordedWork(true,{}),"missing probe accepted recorded work");
 expect(!Hdr10::ReusableRecordedWork(true,[]{return false;}),"false completion probe accepted");
 expect(Hdr10::ReusableRecordedWork(false,{}),"never-recorded allocation was retained");
 auto native=texture(8,2,D3D12_RESOURCE_STATE_COPY_DEST),hdr=texture(8,1,D3D12_RESOURCE_STATE_COPY_DEST),ref=texture(8,1,D3D12_RESOURCE_STATE_COPY_DEST),upload=buffer(D3D12_HEAP_TYPE_UPLOAD),read=buffer(D3D12_HEAP_TYPE_READBACK);
 void* ptr=nullptr;check(upload->Map(0,nullptr,&ptr));memset(ptr,0,2048);auto* data=(unsigned short*)ptr;
 // Native padding and alpha have nontrivial sentinels, which must survive every copy.
 for(unsigned y=0;y<2;y++)for(unsigned x=0;x<8;x++){auto* v=data+y*128+x*4;v[0]=v[1]=v[2]=0x3400;v[3]=0x3800;}
 for(unsigned x=0;x<4;x++)for(unsigned k=0;k<3;k++)data[x*4+k]=x==2?0:0x3c00;
 for(unsigned x=0;x<8;x++)for(unsigned k=0;k<4;k++){data[256+x*4+k]=(x>=2&&x<6)?0x4000:0x4c00;data[384+x*4+k]=0x3c00;}
 // Simulated model edit: 50% of the linear scene; pixel 2 retains black.
 for(unsigned x=0;x<4;x++){for(unsigned k=0;k<3;k++)data[512+x*4+k]=x==2?0:0x4098;data[512+x*4+3]=0x3800;}
 upload->Unmap(0,nullptr);
 auto uploadTexture=[&](ID3D12Resource* dst,unsigned w,unsigned h,unsigned offset){D3D12_TEXTURE_COPY_LOCATION a{},b{};a.pResource=dst;a.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;b.pResource=upload.Get();b.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;b.PlacedFootprint.Offset=offset;b.PlacedFootprint.Footprint={DXGI_FORMAT_R16G16B16A16_FLOAT,w,h,1,256};cmd->CopyTextureRegion(&a,0,0,0,&b,nullptr);};
 uploadTexture(hdr.Get(),8,1,512);uploadTexture(ref.Get(),8,1,768);NrSceneInput::Transition(cmd.Get(),hdr.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);NrSceneInput::Transition(cmd.Get(),ref.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);
 DlssNr::GpuLifetime lifetime;Hdr10::SceneInput scene;scene.hdr=hdr;scene.reference=ref;scene.rect={.25f,0,.5f,1};std::function<bool()> retired;scene.retire=[&](auto probe){retired=probe;};
 for(unsigned frame=1;frame<=96;frame++) {
  if(frame>1){check(alloc->Reset());check(cmd->Reset(alloc.Get(),nullptr));}
  uploadTexture(native.Get(),8,2,0);
  expect(!NrSceneInput::Prepare(device.Get(),cmd.Get(),native.Get(),D3D12_RESOURCE_STATE_COPY_DEST,9,1,scene,lifetime),"oversize region accepted");
  expect(!NrSceneInput::Prepare(device.Get(),cmd.Get(),native.Get(),D3D12_RESOURCE_STATE_COPY_DEST,4,1,{},lifetime),"missing scene accepted");
  auto* p=NrSceneInput::Prepare(device.Get(),cmd.Get(),native.Get(),D3D12_RESOURCE_STATE_COPY_DEST,4,1,scene,lifetime);expect(p,"adapter preparation failed");expect(retired&&!retired(),"premature retirement");
  NrSceneInput::Collect(false);expect(NrSceneInput::Packets().size()==1 && p->native && p->scene.owner==scene.owner && p->scene.hdr,"cleanup released unsubmitted resources");
  NrSceneInput::Transition(cmd.Get(),p->work.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION a{},b{};a.pResource=read.Get();a.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;a.PlacedFootprint.Footprint={DXGI_FORMAT_R16G16B16A16_FLOAT,4,1,1,256};b.pResource=p->work.Get();b.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;cmd->CopyTextureRegion(&a,0,0,0,&b,nullptr);
  const bool edit=frame%2==0;NrSceneInput::Transition(cmd.Get(),p->work.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,edit?D3D12_RESOURCE_STATE_COPY_DEST:D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  if(edit){uploadTexture(p->work.Get(),4,1,1024);NrSceneInput::Transition(cmd.Get(),p->work.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);}
  NrSceneInput::Finish(*p,cmd.Get(),D3D12_RESOURCE_STATE_COPY_DEST);
  NrSceneInput::Transition(cmd.Get(),native.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COPY_SOURCE);
  a.PlacedFootprint.Offset=512;a.PlacedFootprint.Footprint={DXGI_FORMAT_R16G16B16A16_FLOAT,8,2,1,256};b.pResource=native.Get();cmd->CopyTextureRegion(&a,0,0,0,&b,nullptr);NrSceneInput::Transition(cmd.Get(),native.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
  check(cmd->Close());ID3D12CommandList* lists[]={cmd.Get()};queue->ExecuteCommandLists(1,lists);lifetime.Submitted(queue.Get(),1,lists);check(queue->Signal(fence.Get(),frame));check(fence->SetEventOnCompletion(frame,event));expect(WaitForSingleObject(event,10000)==WAIT_OBJECT_0,"GPU timeout");
  expect(!retired(),"GPU completion alone allowed overwriting live recording");NrSceneInput::Collect(false);expect(NrSceneInput::Packets().size()==1 && p->native,"cleanup released completed but unreset recording");lifetime.ResetRecording(cmd.Get());expect(retired(),"completed reset recording was retained");
  check(read->Map(0,nullptr,&ptr));auto* out=(unsigned short*)ptr;expect(std::abs(half(out[0])-std::pow(2.f,2.2f))<.01,"HDR brightness lost or padding sampled");expect(out[8]==0,"black changed in HDR adapter");
  auto* result=out+256;expect(result[3]==0x3800&&result[11]==0x3800,"alpha lost");expect(result[8]==0,"black lost in SDR restore");
  for(unsigned y=0;y<2;y++)for(unsigned x=0;x<8;x++)if(y||x>=4){auto* pixel=result+y*128+x*4;expect(pixel[0]==0x3400&&pixel[3]==0x3800,"native padding changed");}
  if(edit)expect(half(result[0])>.73f&&half(result[0])<.74f,"HDR NR edit did not return to SDR");else {if(result[0]!=0x3c00)printf("identity got %04x %.8f; work %04x %.8f\n",result[0],half(result[0]),out[0],half(out[0]));expect(result[0]==0x3c00,"identity NR altered native SDR");}read->Unmap(0,nullptr);
 }
 expect(NrSceneInput::Packets().size()==1,"packet pool grew despite completed recordings");NrSceneInput::Collect(false);expect(NrSceneInput::Packets().empty(),"completed buffers not reclaimed");CloseHandle(event);puts("PASS: NR HDR adapter, 96 identity/edit frames, HDR highlights, cropped padding, alpha/black, safe resource retirement and reuse");return 0;
}catch(const std::exception& e){fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
