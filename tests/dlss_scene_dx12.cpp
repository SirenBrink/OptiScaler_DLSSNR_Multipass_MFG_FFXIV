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
 ID3D12Resource* typed=nullptr;void* untyped=nullptr;float scalar=2.5f;bool haveScalar=true;
 NVSDK_NGX_Result Get(const char*,float* out){if(!haveScalar)return NVSDK_NGX_Result_FAIL_InvalidParameter;*out=scalar;return NVSDK_NGX_Result_Success;}
 void Set(const char*,float value){scalar=value;}
 NVSDK_NGX_Result Get(const char*,ID3D12Resource** out){*out=typed;return NVSDK_NGX_Result_Success;}
 NVSDK_NGX_Result Get(const char*,void** out){*out=untyped;return NVSDK_NGX_Result_Success;}
 void Set(const char*,ID3D12Resource* value){typed=value;}
 void Set(const char*,void* value){untyped=value;}
};
int main()try{
 expect(!DlssSceneInput::bridgeSceneHdr,"HDR evaluation domain leaked");
 {DlssSceneInput::BridgeEvaluationScope active(true);DlssSceneInput::bridgeHdrShoulder=true;expect(DlssSceneInput::bridgeSceneHdr,"effective HDR feature not visible to NR");
  {DlssSceneInput::BridgeEvaluationScope nested(false);expect(!DlssSceneInput::bridgeSceneHdr&&!DlssSceneInput::bridgeHdrShoulder,"nested SDR feature inherited HDR");}
  expect(DlssSceneInput::bridgeSceneHdr&&DlssSceneInput::bridgeHdrShoulder,"nested SDR scope lost main HDR domain");}
 expect(!DlssSceneInput::bridgeSceneHdr,"HDR domain survived evaluation scope");
 expect(!DlssSceneInput::bridgeInitialising,"bridge init flag leaked");{DlssSceneInput::BridgeInitScope scope;expect(DlssSceneInput::bridgeInitialising,"bridge scope inactive");{DlssSceneInput::BridgeInitScope nested;expect(DlssSceneInput::bridgeInitialising,"nested bridge scope inactive");}expect(DlssSceneInput::bridgeInitialising,"nested bridge scope reset parent");}expect(!DlssSceneInput::bridgeInitialising,"bridge init scope not restored");
 {SeparateSlots p;NgxResourceBinding::FloatBinding pre(&p,"pre");pre.Set(1);expect(p.scalar==1,"scene HDR was exposed twice");pre.Restore();expect(p.scalar==2.5f,"native exposure was not restored for NR");
  p.haveScalar=false;NgxResourceBinding::FloatBinding absent(&p,"absent");absent.Set(1);absent.Restore();expect(p.scalar==1,"missing exposure did not retain SDK unit default");}
 ComPtr<ID3D12Debug> debug;if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))debug->EnableDebugLayer();
 ComPtr<IDXGIFactory4> f;check(CreateDXGIFactory1(IID_PPV_ARGS(&f)));ComPtr<IDXGIAdapter>a;check(f->EnumWarpAdapter(IID_PPV_ARGS(&a)));ComPtr<ID3D12Device>d;check(D3D12CreateDevice(a.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&d)));
 ComPtr<ID3D12InfoQueue> info;d.As(&info);
 D3D12_COMMAND_QUEUE_DESC qd{};ComPtr<ID3D12CommandQueue>q;check(d->CreateCommandQueue(&qd,IID_PPV_ARGS(&q)));ComPtr<ID3D12CommandAllocator>alloc;check(d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&alloc)));ComPtr<ID3D12GraphicsCommandList>c;check(d->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,alloc.Get(),nullptr,IID_PPV_ARGS(&c)));
 auto texture=[&](DXGI_FORMAT format=DXGI_FORMAT_R16G16B16A16_FLOAT,D3D12_RESOURCE_FLAGS flags=D3D12_RESOURCE_FLAG_NONE){D3D12_RESOURCE_DESC t{};t.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;t.Width=4;t.Height=t.DepthOrArraySize=t.MipLevels=t.SampleDesc.Count=1;t.Format=format;t.Flags=flags;D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;ComPtr<ID3D12Resource>r;check(d->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&t,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&r)));return r;};
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

 // Matching colour/reference values must reconstruct the captured HDR exactly,
 // independent of which dark texel a jittered raster samples this frame.
 auto linearValue=[](float v){return v<=.04045f?v/12.92f:std::pow((v+.055f)/1.055f,2.4f);};
 auto toHalf=[](float v){unsigned bits=0;memcpy(&bits,&v,4);unsigned exponent=(bits>>23)&255;
  if(v==0)return (unsigned short)0;
  expect(exponent>=113&&exponent<143,"test upload outside normal half range");
  unsigned rounded=(bits+0x1000u)>>13;return (unsigned short)((rounded&0x3ffu)|(((rounded>>10)-112u)<<10));};
 for(unsigned phase=0;phase<9;++phase){
  check(alloc->Reset());check(Hdr10::ResetCommands(c.Get(),alloc.Get()));
  check(upload->Map(0,nullptr,&ptr));auto* data=(unsigned char*)ptr;
  const float levels[]={.01f,.03f,.1f,.5f};float expected[4]{};
  for(unsigned x=0;x<4;++x){
   float encodedNative=half(toHalf(levels[(x+phase)%4]));
   float encodedReference=encodedNative;
   float encodedHdr=half(toHalf(std::pow(linearValue(encodedNative)*8,1.f/2.2f)));
   if(phase==8&&x==0){encodedNative=encodedReference=0;encodedHdr=2;}
   if(phase==8&&x==1){encodedReference=0;encodedHdr=2;}
   expected[x]=encodedNative==0?0:encodedReference==0?linearValue(encodedNative):std::pow(encodedHdr,2.2f);
   for(unsigned ch=0;ch<4;++ch){
    ((unsigned short*)(data+0))[x*4+ch]=ch==3?0x3800:encodedNative==0?0:toHalf(encodedNative);
    ((unsigned short*)(data+256))[x*4+ch]=ch==3?0x3800:encodedReference==0?0:toHalf(encodedReference);
    ((unsigned short*)(data+512))[x*4+ch]=ch==3?0x3800:toHalf(encodedHdr);
   }
  }
  upload->Unmap(0,nullptr);uploadTexture(native.Get(),0);uploadTexture(ref.Get(),256);uploadTexture(hdr.Get(),512);
  Hdr10::SceneInput scene;scene.hdr=hdr;scene.reference=ref;
  auto packet=DlssSceneInput::Prepare(d.Get(),c.Get(),native.Get(),dest.Get(),4,1,scene);expect(bool(packet),"dark detail prepare failed");
  DlssSceneInput::Barrier(c.Get(),packet->input.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION to{},from{};to.pResource=read.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint.Footprint={DXGI_FORMAT_R16G16B16A16_FLOAT,4,1,1,256};from.pResource=packet->input.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;c->CopyTextureRegion(&to,0,0,0,&from,nullptr);
  DlssSceneInput::Barrier(c.Get(),packet->input.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);
  DlssSceneInput::Barrier(c.Get(),packet->output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COMMON);
  submit(200+phase);Hdr10::Reset(c.Get());check(read->Map(0,nullptr,&ptr));auto* output=(unsigned short*)ptr;
  for(unsigned x=0;x<4;++x){float actual=half(output[x*4]);
   if(std::abs(actual-expected[x])>std::max(1e-6f,expected[x]*.003f)){
    printf("dark reconstruction phase %u texel %u: got %.8f expected %.8f\n",phase,x,actual,expected[x]);
    throw std::runtime_error("HDR gain depends on reference brightness / amplifies reference-black noise");
   }
  }
  read->Unmap(0,nullptr);
 }
 puts("PASS: 8 shifted dark-detail phases recover matched HDR; reference-black noise and true black remain stable");

 // Feed noisy, stationary HDR reconstructions through the production resolve.
 // Verify motion/disocclusion/reset rejection and bounded packet ownership,
 // rather than accepting a lower temporal metric from unconditional blending.
 float baselineToggle[2]{};
 for(bool adaptive:{false,true}) {
  auto depthGuide=texture(DXGI_FORMAT_R32_FLOAT),motionGuide=texture(),nrScratch=texture(DXGI_FORMAT_R16G16B16A16_FLOAT,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  ComPtr<ID3D12CommandQueue> otherQueue;check(d->CreateCommandQueue(&qd,IID_PPV_ARGS(&otherQueue)));
  std::shared_ptr<DlssSceneInput::Packet> history;
  for(unsigned n=0;n<96;++n){
   check(alloc->Reset());check(Hdr10::ResetCommands(c.Get(),alloc.Get()));
   auto data=buffer(D3D12_HEAP_TYPE_UPLOAD,1536);check(data->Map(0,nullptr,&ptr));memset(ptr,0,1536);
   auto* bits=(unsigned short*)ptr;
   const bool moving=n<80&&n%8==3,disoccluded=n<80&&n%8==4,reset=n<80&&n%8==5,flash=n<80&&n%8==6;
   for(unsigned x=0;x<4;++x){
    for(unsigned channel=0;channel<3;++channel)bits[x*4+channel]=n>=80?(x==1?0x4000:0x3800):!n?0x3800:flash?0x3b33:((n>=16&&n<50?(x+n+1)%2:x%2)?0x3829:0x37ae); // thin 2.0 highlight, .5, .900, .520/.480
    bits[x*4+3]=0x3c00;
    ((float*)((char*)ptr+512))[x]=disoccluded?.25f:.5f;
    bits[512+x*4]=moving?0x3c00:0; // normalized motion -> four output pixels
   }
   data->Unmap(0,nullptr);
   auto copy=[&](ID3D12Resource*t,DXGI_FORMAT format,UINT64 offset,D3D12_RESOURCE_STATES before){
    DlssSceneInput::Barrier(c.Get(),t,before,D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=data.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;from.PlacedFootprint.Offset=offset;from.PlacedFootprint.Footprint={format,4,1,1,256};to.pResource=t;to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;c->CopyTextureRegion(&to,0,0,0,&from,nullptr);
    DlssSceneInput::Barrier(c.Get(),t,D3D12_RESOURCE_STATE_COPY_DEST,before);
   };
   copy(depthGuide.Get(),DXGI_FORMAT_R32_FLOAT,512,D3D12_RESOURCE_STATE_COMMON);
   copy(motionGuide.Get(),DXGI_FORMAT_R16G16B16A16_FLOAT,1024,D3D12_RESOURCE_STATE_COMMON);
   auto packet=DlssSceneInput::Prepare(d.Get(),c.Get(),native.Get(),dest.Get(),4,1,{});expect(bool(packet),"temporal prepare failed");
   copy(packet->output.Get(),DXGI_FORMAT_R16G16B16A16_FLOAT,0,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
   const bool used=DlssSceneInput::Stabilize(packet,c.Get(),reset?nullptr:history,depthGuide.Get(),motionGuide.Get(),4,1,0,0,true,adaptive);
   expect(used==bool(n&&!reset),"temporal history reset/prime failed");
   DlssSceneInput::Finish(packet,c.Get(),true);
   // Real PreSR uses its just-consumed composed texture as scratch. Reconcile
   // makes a deliberately large edit, which must never enter clean SR history.
   const bool nrActive=n>=16&&n<48;
   if(n==0)DlssSceneInput::Barrier(c.Get(),nrScratch.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
   if(nrActive){
    auto image=DlssSceneInput::Image(packet);
    expect(DlssSceneInput::PreserveHistory(image,c.Get(),nrScratch.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS),"PreSR clean history snapshot failed");
    copy(edit.Get(),DXGI_FORMAT_R16G16B16A16_FLOAT,0,D3D12_RESOURCE_STATE_COMMON);
    expect(DlssSceneInput::Reconcile(image,c.Get(),edit.Get(),D3D12_RESOURCE_STATE_COMMON),"NR temporal reconciliation failed");
   }
   auto* readableHistory=nrActive?packet->cleanHistory.Get():packet->encoded.Get();
   const auto historyState=nrActive?D3D12_RESOURCE_STATE_UNORDERED_ACCESS:D3D12_RESOURCE_STATE_COMMON;
   DlssSceneInput::Barrier(c.Get(),readableHistory,historyState,D3D12_RESOURCE_STATE_COPY_SOURCE);
   D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=readableHistory;from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;to.pResource=read.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint.Footprint={DXGI_FORMAT_R16G16B16A16_FLOAT,4,1,1,256};c->CopyTextureRegion(&to,0,0,0,&from,nullptr);
   DlssSceneInput::Barrier(c.Get(),readableHistory,D3D12_RESOURCE_STATE_COPY_SOURCE,historyState);
   auto submitted=Hdr10::HistorySubmission(c.Get(),q.Get()),wrongQueue=Hdr10::HistorySubmission(c.Get(),otherQueue.Get());
   expect(!submitted(),"unsubmitted HDR history was marked readable");
   submit(500+n+unsigned(adaptive)*1000);expect(submitted()&&!wrongQueue(),"HDR history submission did not respect its producer queue");Hdr10::Reset(c.Get());check(read->Map(0,nullptr,&ptr));bits=(unsigned short*)ptr;
   const float result=std::pow(half(bits[4]),2.2f),current=n>=80?2.f:!n?.5f:flash?half(0x3b33):half(n>=16&&n<50&&n%2==0?0x37ae:0x3829);
   expect(std::abs(half(bits[7])-(disoccluded?.25f:.5f))<.0001f,"private history lost depth");
   if((moving||disoccluded||reset||flash||!n)&&std::abs(result-current)>=.0015f){printf("mode %d frame %u result %.6f current %.6f\n",adaptive,n,result,current);throw std::runtime_error("stale HDR history survived motion/depth/reset/lighting rejection");}
   if(n==1)expect(result<current-.005f&&result>.495f,"stationary HDR noise was not stabilized");
   if(n==17||n==49){const unsigned index=n==17?0:1;if(!adaptive)baselineToggle[index]=result;else expect(std::abs(result-.5f)<std::abs(baselineToggle[index]-.5f)-.0001f,"adaptive history did not reduce the controlled stationary residual");printf("NR toggle history frame %u: current %.6f resolved %.6f\n",n,current,result);expect(result<current-.003f,"NR edits contaminated clean history across enable/disable");}
   if(n>=80&&std::abs(result-2.f)>=.004f){printf("thin highlight frame %u: %.9f (encoded %04x)\n",n,result,bits[4]);throw std::runtime_error("temporal clipping dimmed a stable thin HDR highlight");}
   read->Unmap(0,nullptr);history=packet;
  }
  check(alloc->Reset());check(Hdr10::ResetCommands(c.Get(),alloc.Get()));
  auto switched=DlssSceneInput::Prepare(d.Get(),c.Get(),native.Get(),dest.Get(),4,1,{});
  expect(!DlssSceneInput::Stabilize(switched,c.Get(),history,depthGuide.Get(),motionGuide.Get(),4,1,0,0,true,!adaptive),"live stabilizer toggle reused old-mode history");
  DlssSceneInput::Finish(switched,c.Get(),false);check(c->Close());Hdr10::Reset(c.Get());switched.reset();
  history.reset();check(alloc->Reset());check(Hdr10::ResetCommands(c.Get(),alloc.Get()));
  auto discarded=DlssSceneInput::Prepare(d.Get(),c.Get(),native.Get(),dest.Get(),4,1,{});
  DlssSceneInput::Stabilize(discarded,c.Get(),nullptr,depthGuide.Get(),motionGuide.Get(),4,1,0,0);
  DlssSceneInput::Finish(discarded,c.Get(),true);check(c->Close());check(alloc->Reset());check(Hdr10::ResetCommands(c.Get(),alloc.Get()));
  expect(!discarded->historySubmitted(),"discarded HDR recording became readable history");
  auto next=DlssSceneInput::Prepare(d.Get(),c.Get(),native.Get(),dest.Get(),4,1,{});
  expect(!DlssSceneInput::Stabilize(next,c.Get(),discarded,depthGuide.Get(),motionGuide.Get(),4,1,0,0),"resolve used discarded HDR history");
  DlssSceneInput::Finish(next,c.Get(),false);check(c->Close());Hdr10::Reset(c.Get());
  next.reset();discarded.reset();check(alloc->Reset());check(Hdr10::ResetCommands(c.Get(),alloc.Get()));
  auto scaled=DlssSceneInput::Prepare(d.Get(),c.Get(),native.Get(),dest.Get(),2,1,{});
  expect(!DlssSceneInput::Stabilize(scaled,c.Get(),nullptr,depthGuide.Get(),motionGuide.Get(),4,1,0,0,false)&&!scaled->hasDepth,"incompatible motion layout did not fail closed");
  DlssSceneInput::Stabilize(scaled,c.Get(),nullptr,depthGuide.Get(),motionGuide.Get(),4,1,0,0);
  expect(scaled->motionScale[0]==8&&scaled->motionScale[1]==1,"render-space motion was not scaled to output-space history");
  DlssSceneInput::Finish(scaled,c.Get(),false);check(c->Close());Hdr10::Reset(c.Get());
  expect(DlssSceneInput::Pool().size()<=3,"temporal history retained an unbounded packet chain");
 }
 puts("PASS: production HDR temporal resolve reduces stationary noise, preserves thin highlights, rejects motion/depth/reset/lighting and discarded history, validates producer queue, preserves depth in private alpha and bounds history ownership over 96 frames per mode; adaptive toggle discards old-mode history");


 // Run the two production adapters together, independently enabled, including
 // missing captures and NR initialization/failure. NGX itself is an identity
 // reconstruction here; native model behaviour still requires gameplay testing.
 {
 auto nrNative=texture(),nrOutput=texture(),nrHdr=texture(),nrRef=texture();
 DlssNr::GpuLifetime nrLifetime;UINT sequence=5000;
 for(bool dlssHdr:{false,true})for(bool nrHdrInput:{false,true})for(bool captured:{false,true}){
  unsigned short baseline=0;
  for(unsigned attempt=0;attempt<3;++attempt){
   check(alloc->Reset());check(Hdr10::ResetCommands(c.Get(),alloc.Get()));nrLifetime.ResetRecording(c.Get());
   check(upload->Map(0,nullptr,&ptr));auto* bits=(unsigned short*)ptr;
   for(unsigned x=0;x<4;++x)for(unsigned channel=0;channel<4;++channel){
    bits[x*4+channel]=0x3800; // native and original reference: 0.5, including alpha
    bits[128+x*4+channel]=channel==3?0x3c00:0x4000; // encoded HDR = 2
    bits[256+x*4+channel]=channel==3?0x3800:0; // corrupt/partial NR result
   }
   upload->Unmap(0,nullptr);uploadTexture(nrNative.Get(),0);uploadTexture(nrRef.Get(),0);uploadTexture(nrHdr.Get(),256);
   Hdr10::SceneInput scene;if(captured){scene.hdr=nrHdr;scene.reference=nrRef;}
   if(nrHdrInput&&captured){
    auto* packet=NrSceneInput::Prepare(d.Get(),c.Get(),nrNative.Get(),D3D12_RESOURCE_STATE_COMMON,4,1,scene,nrLifetime);
    expect(packet,"combined NR HDR preparation failed");
    // attempt 0 = identity, 1 = model failure after touching private work,
    // 2 = successful model darkening. Only the last may alter the native input.
    if(attempt){NrSceneInput::Transition(c.Get(),packet->work.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
     uploadTexture(packet->work.Get(),512);NrSceneInput::Transition(c.Get(),packet->work.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);}
    NrSceneInput::Finish(*packet,c.Get(),D3D12_RESOURCE_STATE_COMMON,attempt!=1);
   }
   if(dlssHdr){
    DlssSceneInput::bridgeColour=nrNative.Get();DlssSceneInput::bridgeOutput=nrOutput.Get();
    auto packet=DlssSceneInput::Prepare(d.Get(),c.Get(),nrNative.Get(),nrOutput.Get(),4,1,scene);
    expect(bool(packet),"combined DLSS HDR preparation failed");
    DlssSceneInput::Barrier(c.Get(),packet->input.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_SOURCE);
    DlssSceneInput::Barrier(c.Get(),packet->output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_DEST);
    c->CopyResource(packet->output.Get(),packet->input.Get());
    DlssSceneInput::Barrier(c.Get(),packet->input.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    DlssSceneInput::Barrier(c.Get(),packet->output.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    DlssSceneInput::Finish(packet,c.Get(),true);
   }else{
    DlssSceneInput::Barrier(c.Get(),nrNative.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);
    DlssSceneInput::Barrier(c.Get(),nrOutput.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);c->CopyResource(nrOutput.Get(),nrNative.Get());
    DlssSceneInput::Barrier(c.Get(),nrNative.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);
    DlssSceneInput::Barrier(c.Get(),nrOutput.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);
   }
   DlssSceneInput::Barrier(c.Get(),nrOutput.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);
   D3D12_TEXTURE_COPY_LOCATION to{},from{};to.pResource=read.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint.Footprint={DXGI_FORMAT_R16G16B16A16_FLOAT,4,1,1,256};from.pResource=nrOutput.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
   c->CopyTextureRegion(&to,0,0,0,&from,nullptr);DlssSceneInput::Barrier(c.Get(),nrOutput.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);
   submit(sequence++);ID3D12CommandList* lists[]={c.Get()};nrLifetime.Submitted(q.Get(),1,lists);
   check(read->Map(0,nullptr,&ptr));auto* result=(unsigned short*)ptr;
   if(!attempt)baseline=result[0];
   if(attempt<=1||!nrHdrInput||!captured)expect(result[0]==baseline,"disabled, missing or failed NR changed combined HDR output");
   else {if(!(half(result[0])<half(baseline)))printf("combined case DLSS %u NR %u capture %u: before %04x after %04x\n",dlssHdr,nrHdrInput,captured,baseline,result[0]);expect(half(result[0])<half(baseline),"successful NR edit lost at DLSS HDR boundary");}
   expect(std::isfinite(half(result[0]))&&result[3]==(dlssHdr?0x3c00:0x3800),"combined pipeline lost finite colour or alpha contract");
   read->Unmap(0,nullptr);
  }
 }
 check(alloc->Reset());check(Hdr10::ResetCommands(c.Get(),alloc.Get()));nrLifetime.ResetRecording(c.Get());NrSceneInput::Collect(false);check(c->Close());Hdr10::Reset(c.Get());
 expect(NrSceneInput::Packets().empty(),"combined HDR adapter retained completed work");
 puts("PASS: DLSS HDR on/off x NR HDR on/off x scene available/missing, with identity, failed and successful NR edits; native alpha and packet retirement retained");
 }

 if(info){for(UINT64 i=0;i<info->GetNumStoredMessages();++i){SIZE_T size=0;info->GetMessage(i,nullptr,&size);std::vector<char>b(size);auto*m=(D3D12_MESSAGE*)b.data();check(info->GetMessage(i,m,&size));if(m->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){puts(m->pDescription);throw std::runtime_error("D3D12 debug validation error");}}}
 expect(DlssSceneInput::Pool().size()<=3,"work buffers not reused");CloseHandle(event);puts("PASS: production HDR-DLSS conversion, typed/typeless FP16 inputs, FP16/RGBA8/RGB10A2/R11G11B10 outputs, SDR tone curve, reconstructed HDR, missing-scene fallback, 128 reset/submission cycles and resource lifetime, opaque output alpha, identity/non-identity PreSR HDR reconciliation and descriptor immutability");return 0;
}catch(const std::exception&e){puts(e.what());return 1;}
