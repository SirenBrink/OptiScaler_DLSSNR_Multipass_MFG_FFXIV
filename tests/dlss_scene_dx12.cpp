#include "hdr_stubs/pch.h"
#include "../OptiScaler/shaders/hdr/DlssSceneInput.h"
#include "../OptiScaler/misc/NgxResourceBinding.h"
#include <cstdio>
#include <stdexcept>
using Microsoft::WRL::ComPtr;
void check(HRESULT h){if(FAILED(h))throw std::runtime_error("DX12 failure");}
void expect(bool b,const char* s){if(!b)throw std::runtime_error(s);}
float half(unsigned short x){unsigned bits=((x&0x8000u)<<16)|(((x>>10)&31u)+112u)<<23|((x&1023u)<<13);if((x&0x7fff)==0)bits=0;float f;memcpy(&f,&bits,4);return f;}
// Match native NGX's separate slots, including successful typed reads of NULL.
// The OptiScaler parameter map merges these slots and would hide this regression.
struct SeparateSlots {
 ID3D12Resource* typed=nullptr;void* untyped=nullptr;
 NVSDK_NGX_Result Get(const char*,ID3D12Resource** out){*out=typed;return NVSDK_NGX_Result_Success;}
 NVSDK_NGX_Result Get(const char*,void** out){*out=untyped;return NVSDK_NGX_Result_Success;}
 void Set(const char*,ID3D12Resource* value){typed=value;}
 void Set(const char*,void* value){untyped=value;}
};
int main()try{
 expect(!DlssSceneInput::bridgeInitialising,"bridge init flag leaked");{DlssSceneInput::BridgeInitScope scope;expect(DlssSceneInput::bridgeInitialising,"bridge scope inactive");{DlssSceneInput::BridgeInitScope nested;expect(DlssSceneInput::bridgeInitialising,"nested bridge scope inactive");}expect(DlssSceneInput::bridgeInitialising,"nested bridge scope reset parent");}expect(!DlssSceneInput::bridgeInitialising,"bridge init scope not restored");
 ComPtr<ID3D12Debug> debug;if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))debug->EnableDebugLayer();
 ComPtr<IDXGIFactory4> f;check(CreateDXGIFactory1(IID_PPV_ARGS(&f)));ComPtr<IDXGIAdapter>a;check(f->EnumWarpAdapter(IID_PPV_ARGS(&a)));ComPtr<ID3D12Device>d;check(D3D12CreateDevice(a.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&d)));
 ComPtr<ID3D12InfoQueue> info;d.As(&info);
 D3D12_COMMAND_QUEUE_DESC qd{};ComPtr<ID3D12CommandQueue>q;check(d->CreateCommandQueue(&qd,IID_PPV_ARGS(&q)));ComPtr<ID3D12CommandAllocator>alloc;check(d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&alloc)));ComPtr<ID3D12GraphicsCommandList>c;check(d->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,alloc.Get(),nullptr,IID_PPV_ARGS(&c)));
 auto texture=[&](DXGI_FORMAT format=DXGI_FORMAT_R16G16B16A16_FLOAT){D3D12_RESOURCE_DESC t{};t.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;t.Width=4;t.Height=t.DepthOrArraySize=t.MipLevels=t.SampleDesc.Count=1;t.Format=format;D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;ComPtr<ID3D12Resource>r;check(d->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&t,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&r)));return r;};
 auto buffer=[&](D3D12_HEAP_TYPE type,UINT64 size){D3D12_RESOURCE_DESC t{};t.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;t.Width=size;t.Height=t.DepthOrArraySize=t.MipLevels=t.SampleDesc.Count=1;t.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;D3D12_HEAP_PROPERTIES hp{};hp.Type=type;ComPtr<ID3D12Resource>r;check(d->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&t,type==D3D12_HEAP_TYPE_UPLOAD?D3D12_RESOURCE_STATE_GENERIC_READ:D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&r)));return r;};
 auto native=texture(),typelessNative=texture(DXGI_FORMAT_R16G16B16A16_TYPELESS),dest=texture(),dest8=texture(DXGI_FORMAT_R8G8B8A8_UNORM),dest10=texture(DXGI_FORMAT_R10G10B10A2_UNORM),dest11=texture(DXGI_FORMAT_R11G11B10_FLOAT),hdr=texture(),ref=texture(),edit=texture();auto upload=buffer(D3D12_HEAP_TYPE_UPLOAD,768),read=buffer(D3D12_HEAP_TYPE_READBACK,768);
 for(bool bridge:{false,true}){
  SeparateSlots params; if(bridge)params.Set("Color",(void*)native.Get());else params.Set("Color",native.Get());
  NgxResourceBinding::Binding binding(&params,"Color");expect(binding.original==native.Get(),"bridge input was lost on typed NULL read");
  binding.Set(hdr.Get());expect(bridge?params.untyped==hdr.Get()&&params.typed==nullptr:params.typed==hdr.Get()&&params.untyped==nullptr,"HDR substitution used the wrong parameter slot");
  binding.Restore();expect(bridge?params.untyped==native.Get()&&params.typed==nullptr:params.typed==native.Get()&&params.untyped==nullptr,"NR input not restored in original slot");
  SeparateSlots exposure;NgxResourceBinding::Binding absent(&exposure,"Exposure");absent.Set(nullptr);absent.Restore();expect(!exposure.typed&&!exposure.untyped,"absent exposure changed");
 }
 void* ptr;check(upload->Map(0,nullptr,&ptr));auto* p=(unsigned short*)ptr;for(int i=0;i<16;i++)p[i]=(i%4==3)?0:0x3800;for(int i=128;i<144;i++)p[i]=0x4000;for(int i=256;i<272;i++)p[i]=(i%4==3)?0x3c00:0x3a00;upload->Unmap(0,nullptr);
 auto uploadTexture=[&](ID3D12Resource*t,UINT64 offset){DlssSceneInput::Barrier(c.Get(),t,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);D3D12_TEXTURE_COPY_LOCATION to{},from{};to.pResource=t;to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;from.pResource=upload.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;from.PlacedFootprint.Offset=offset;from.PlacedFootprint.Footprint={DXGI_FORMAT_R16G16B16A16_FLOAT,4,1,1,256};c->CopyTextureRegion(&to,0,0,0,&from,nullptr);DlssSceneInput::Barrier(c.Get(),t,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);};
 uploadTexture(native.Get(),0);uploadTexture(typelessNative.Get(),0);uploadTexture(ref.Get(),0);uploadTexture(hdr.Get(),256);uploadTexture(edit.Get(),512);
 ComPtr<ID3D12Fence> fence;check(d->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));HANDLE event=CreateEvent(nullptr,FALSE,FALSE,nullptr);
 auto submit=[&](UINT value){check(c->Close());ID3D12CommandList*l[]={c.Get()};Hdr10::ExecuteCommands(q.Get(),1,l);check(q->Signal(fence.Get(),value));check(fence->SetEventOnCompletion(value,event));expect(WaitForSingleObject(event,10000)==WAIT_OBJECT_0,"GPU timeout");};
 submit(1);
 {auto unsupported=texture(DXGI_FORMAT_R32G32B32A32_FLOAT);const char* failure=nullptr;auto rejected=DlssSceneInput::Prepare(d.Get(),c.Get(),unsupported.Get(),dest.Get(),4,1,{},&failure);expect(!rejected&&failure&&strstr(failure,"colour SRV"),"unsupported colour must return an explicit failure before recording");}
 for(UINT frame=2;frame<130;++frame){check(alloc->Reset());check(Hdr10::ResetCommands(c.Get(),alloc.Get()));
  Hdr10::SceneInput scene;bool sceneUsed=frame%5<3;std::function<bool()> retire;if(sceneUsed){scene.hdr=hdr;scene.reference=ref;scene.retire=[&](auto probe){retire=probe;};}
  auto* nativeThisFrame=frame%4<2?native.Get():typelessNative.Get();
  auto* destThisFrame=frame%4==0?dest.Get():frame%4==1?dest8.Get():frame%4==2?dest10.Get():dest11.Get();
  DlssSceneInput::bridgeColour=nativeThisFrame;DlssSceneInput::bridgeOutput=destThisFrame;
  auto packet=DlssSceneInput::Prepare(d.Get(),c.Get(),nativeThisFrame,destThisFrame,4,1,scene);expect(bool(packet),"prepare failed");
  // Simulate identity reconstruction. Real NGX correctness requires the in-game test.
  DlssSceneInput::Barrier(c.Get(),packet->input.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_SOURCE);
  DlssSceneInput::Barrier(c.Get(),packet->output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_DEST);c->CopyResource(packet->output.Get(),packet->input.Get());
  DlssSceneInput::Barrier(c.Get(),packet->input.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  DlssSceneInput::Barrier(c.Get(),packet->output.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  DlssSceneInput::Finish(packet,c.Get(),true);
  auto image=DlssSceneInput::Image(packet);expect(image.owner!=nullptr,"missing ownership");
  bool edited=destThisFrame==dest.Get()&&frame%8==0;
  if(destThisFrame==dest.Get()){
   expect(DlssSceneInput::Reconcile(image,c.Get(),edited?edit.Get():destThisFrame,D3D12_RESOURCE_STATE_COMMON),"reconciliation failed");
   expect(!DlssSceneInput::Reconcile(image,c.Get(),destThisFrame,D3D12_RESOURCE_STATE_COMMON),"recorded descriptors must not be overwritten");
  }
  UINT64 offset=0;for(auto* texture:{packet->input.Get(),image.reference.Get(),image.hdr.Get()}){DlssSceneInput::Barrier(c.Get(),texture,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);D3D12_TEXTURE_COPY_LOCATION to{},from{};to.pResource=read.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint.Offset=offset;to.PlacedFootprint.Footprint={texture->GetDesc().Format,4,1,1,256};from.pResource=texture;from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;c->CopyTextureRegion(&to,0,0,0,&from,nullptr);DlssSceneInput::Barrier(c.Get(),texture,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);offset+=256;}
  expect(!packet->done(),"retired before submission");submit(frame);expect(!packet->done(),"recording retired before reset");Hdr10::Reset(c.Get());expect(packet->done(),"recording retirement failed");if(sceneUsed)expect(retire&&retire(),"scene retirement failed");
  check(read->Map(0,nullptr,&ptr));auto* pixels=(unsigned short*)ptr;float input=half(pixels[0]),encoded=half(pixels[256]);
  const unsigned packed=*(unsigned*)((unsigned char*)ptr+256);float smallFloat=0;unsigned bits=(((packed&2047u)>>6)+112u)<<23|((packed&63u)<<17);memcpy(&smallFloat,&bits,4);
  float sdr=destThisFrame==dest.Get()?half(pixels[128]):destThisFrame==dest8.Get()?((unsigned char*)ptr)[256]/255.f:destThisFrame==dest10.Get()?(packed&1023u)/1023.f:smallFloat;
  auto encode=[](float v){return v<=.0031308f?12.92f*v:1.055f*std::pow(v,1.f/2.4f)-.055f;};
  auto linear=[](float v){return v<=.04045f?v/12.92f:std::pow((v+.055f)/1.055f,2.4f);};
  const float mapped=sceneUsed?.75f+.25f*(input-.75f)/(input-.5f):input;
  const float expectedSdr=edited?.75f:encode(mapped);
  const float expectedHdr=edited?std::pow(linear(.75f),1.f/2.2f):std::pow(input,1.f/2.2f);
  expect(std::abs(sdr-expectedSdr)<(destThisFrame==dest11.Get()?.01f:.004f),"pointwise SDR shoulder incorrect");
  expect(sceneUsed?input>4.f:std::abs(input-.214f)<.002f,"HDR input / SDR fallback incorrect");
  expect(std::abs(encoded-expectedHdr)<.015f,"HDR reconstruction / edit transport incorrect");
  float alpha=destThisFrame==dest.Get()?half(pixels[131]):destThisFrame==dest8.Get()?((unsigned char*)ptr)[259]/255.f:destThisFrame==dest10.Get()?float(packed>>30)/3:1;
  expect(alpha==1.f,"opaque main scene alpha not restored");read->Unmap(0,nullptr);
 }
 // A temporally reconstructed pixel must not change when the old unfiltered
 // SDR guide changes. The spatial inverse failed this test (.5 vs .75 guide).
 float compatibility=0;
 for(unsigned variant=0;variant<2;++variant){
  check(alloc->Reset());check(Hdr10::ResetCommands(c.Get(),alloc.Get()));
  Hdr10::SceneInput scene;scene.hdr=hdr;scene.reference=variant?edit:ref;
  DlssSceneInput::bridgeColour=native.Get();DlssSceneInput::bridgeOutput=dest.Get();
  auto packet=DlssSceneInput::Prepare(d.Get(),c.Get(),native.Get(),dest.Get(),4,1,scene);expect(bool(packet),"fixed reconstruction prepare failed");
  DlssSceneInput::Barrier(c.Get(),hdr.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);
  DlssSceneInput::Barrier(c.Get(),packet->output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_DEST);c->CopyResource(packet->output.Get(),hdr.Get());
  DlssSceneInput::Barrier(c.Get(),hdr.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);
  DlssSceneInput::Barrier(c.Get(),packet->output.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  DlssSceneInput::Finish(packet,c.Get(),true);
  DlssSceneInput::Barrier(c.Get(),packet->reference.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION to{},from{};to.pResource=read.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint.Footprint={DXGI_FORMAT_R16G16B16A16_FLOAT,4,1,1,256};from.pResource=packet->reference.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;c->CopyTextureRegion(&to,0,0,0,&from,nullptr);
  DlssSceneInput::Barrier(c.Get(),packet->reference.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);
  submit(130+variant);Hdr10::Reset(c.Get());check(read->Map(0,nullptr,&ptr));float value=half(*(unsigned short*)ptr);read->Unmap(0,nullptr);
  if(!variant)compatibility=value;else expect(value==compatibility,"unfiltered guide reintroduced edges into reconstructed SDR");
 }
 puts("PASS: fixed HDR reconstruction produces identical SDR despite changing unfiltered guides; no second DLSS evaluation");
 if(info){for(UINT64 i=0;i<info->GetNumStoredMessages();++i){SIZE_T size=0;info->GetMessage(i,nullptr,&size);std::vector<char>b(size);auto*m=(D3D12_MESSAGE*)b.data();check(info->GetMessage(i,m,&size));if(m->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){puts(m->pDescription);throw std::runtime_error("D3D12 debug validation error");}}}
 expect(DlssSceneInput::Pool().size()<=2,"work buffers not reused");CloseHandle(event);puts("PASS: production HDR-DLSS conversion, typed/typeless FP16 inputs, FP16/RGBA8/RGB10A2/R11G11B10 outputs, SDR tone curve, reconstructed HDR, missing-scene fallback, 128 reset/submission cycles and resource lifetime, opaque output alpha, identity/non-identity PreSR HDR reconciliation and descriptor immutability");return 0;
}catch(const std::exception&e){puts(e.what());return 1;}
