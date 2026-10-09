#pragma once
#include "Hdr10.h"
#include "DlssSceneInput_Shader.h"
#include "NrSceneInput.h"
#include <mutex>
namespace DlssSceneInput {
using Microsoft::WRL::ComPtr;
// The bridge establishes the states before NR can replace the colour input.
inline thread_local bool bridgeInitialising=false;
struct BridgeInitScope { bool previous=bridgeInitialising;BridgeInitScope(){bridgeInitialising=true;}~BridgeInitScope(){bridgeInitialising=previous;} };
inline thread_local ID3D12Resource* bridgeColour=nullptr;
inline thread_local ID3D12Resource* bridgeOutput=nullptr;
inline thread_local D3D12_RESOURCE_STATES bridgeColourState=D3D12_RESOURCE_STATE_COMMON;
struct Packet {
 ComPtr<ID3D12Device> device;ComPtr<ID3D12Resource> input,output,reference,encoded,nativeColour,nativeOutput;
 ComPtr<ID3D12DescriptorHeap> heap;ComPtr<ID3D12RootSignature> root;ComPtr<ID3D12PipelineState> pso;
 Hdr10::SceneInput scene;std::function<bool()> done;UINT w=0,h=0,ow=0,oh=0;DXGI_FORMAT referenceFormat=DXGI_FORMAT_UNKNOWN;bool recorded=false;
 ULONGLONG lastUse=0;bool reconciled=false;
};
inline std::mutex& Mutex(){static auto* m=new std::mutex;return *m;}
inline auto& Pool(){static auto* p=new std::vector<std::shared_ptr<Packet>>;return *p;}
inline void Barrier(ID3D12GraphicsCommandList*c,ID3D12Resource*r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b){NrSceneInput::Transition(c,r,a,b);}
inline DXGI_FORMAT Typed(DXGI_FORMAT f){switch(f){case DXGI_FORMAT_R16G16B16A16_TYPELESS:return DXGI_FORMAT_R16G16B16A16_FLOAT;case DXGI_FORMAT_R8G8B8A8_TYPELESS:return DXGI_FORMAT_R8G8B8A8_UNORM;case DXGI_FORMAT_R10G10B10A2_TYPELESS:return DXGI_FORMAT_R10G10B10A2_UNORM;default:return f;}}
inline bool Init(Packet& p,ID3D12Device*d,UINT w,UINT h,UINT ow,UINT oh,DXGI_FORMAT referenceFormat){
 p.device=d;p.w=w;p.h=h;p.ow=ow;p.oh=oh;p.referenceFormat=referenceFormat;
 D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,20,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
 if(FAILED(d->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&p.heap))))return false;
 D3D12_DESCRIPTOR_RANGE ranges[2]={{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,4,0,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,4}};
 D3D12_ROOT_PARAMETER params[2]{};params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;params[0].DescriptorTable={2,ranges};
 params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;params[1].Constants={0,0,8};
 D3D12_ROOT_SIGNATURE_DESC rd{2,params,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_NONE};ComPtr<ID3DBlob> blob,error;
 if(FAILED(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error)))return false;
 if(FAILED(d->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&p.root))))return false;
 D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=p.root.Get();pd.CS={DlssSceneInput_cso,sizeof(DlssSceneInput_cso)};
 if(FAILED(d->CreateComputePipelineState(&pd,IID_PPV_ARGS(&p.pso))))return false;
 auto allocate=[&](ComPtr<ID3D12Resource>& r,UINT x,UINT y,DXGI_FORMAT format=DXGI_FORMAT_R16G16B16A16_FLOAT){D3D12_RESOURCE_DESC td{};td.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;td.Width=x;td.Height=y;td.DepthOrArraySize=td.MipLevels=1;td.Format=format;td.SampleDesc.Count=1;td.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;return SUCCEEDED(d->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&td,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&r)));};
 if(!allocate(p.input,w,h)||!allocate(p.output,ow,oh)||!allocate(p.reference,ow,oh,referenceFormat))return false;
 // Encoding is pointwise: reuse the DLSS result after constructing its SDR reference.
 p.encoded=p.output;return true;
}
inline void Bind(Packet& p,ID3D12GraphicsCommandList*c,UINT mode){
 struct Constants{UINT w,h,mode,scene;float rect[4];} k{mode?p.ow:p.w,mode?p.oh:p.h,mode,UINT(p.scene.hdr&&p.scene.reference)};
 std::copy(p.scene.rect.begin(),p.scene.rect.end(),k.rect);
 ID3D12DescriptorHeap* heaps[]={p.heap.Get()};c->SetDescriptorHeaps(1,heaps);c->SetComputeRootSignature(p.root.Get());c->SetPipelineState(p.pso.Get());
 auto gpu=p.heap->GetGPUDescriptorHandleForHeapStart();gpu.ptr+=mode*5*p.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
 c->SetComputeRootDescriptorTable(0,gpu);c->SetComputeRoot32BitConstants(1,8,&k,0);c->Dispatch((k.w+7)/8,(k.h+7)/8,1);
}
inline std::shared_ptr<Packet> Prepare(ID3D12Device*d,ID3D12GraphicsCommandList*c,ID3D12Resource*colour,ID3D12Resource*output,UINT w,UINT h,Hdr10::SceneInput scene,const char** failure=nullptr){
 auto fail=[&](const char* reason)->std::shared_ptr<Packet>{if(failure)*failure=reason;return {};};
 if(!d||!c||!colour||!output||!w||!h)return fail("missing resource or render extent");
 auto a=colour->GetDesc(),b=output->GetDesc();
 auto layout=[](const D3D12_RESOURCE_DESC& x){return x.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D&&x.SampleDesc.Count==1&&x.DepthOrArraySize==1;};
 if(!layout(a)||!layout(b)||b.MipLevels!=1)return fail("unsupported texture layout");
 if(Typed(a.Format)!=DXGI_FORMAT_R16G16B16A16_FLOAT || (a.Flags&D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE))return fail("unsupported colour SRV format or flags");
 const auto outputFormat=Typed(b.Format);
 if(outputFormat!=DXGI_FORMAT_R16G16B16A16_FLOAT&&outputFormat!=DXGI_FORMAT_R8G8B8A8_UNORM&&outputFormat!=DXGI_FORMAT_R10G10B10A2_UNORM&&outputFormat!=DXGI_FORMAT_R11G11B10_FLOAT)return fail("unsupported SDR output format");
 if(w>a.Width||h>a.Height)return fail("render extent exceeds colour allocation");
 std::lock_guard lock(Mutex());auto& pool=Pool();const auto now=GetTickCount64();
 for(auto& p:pool)if(p.use_count()==1&&Hdr10::ReusableRecordedWork(p->recorded,p->done)){p->scene={};p->nativeColour.Reset();p->nativeOutput.Reset();}
 std::erase_if(pool,[&](auto& p){return p.use_count()==1&&Hdr10::ReusableRecordedWork(p->recorded,p->done)&&now-p->lastUse>2000;});
 std::shared_ptr<Packet> chosen;
 for(auto& p:pool)if(p.use_count()==1&&Hdr10::ReusableRecordedWork(p->recorded,p->done)){chosen=p;break;}
 if(!chosen){if(pool.size()>=6)return fail("HDR work buffers still in flight");chosen=std::make_shared<Packet>();pool.push_back(chosen);}
 auto& p=*chosen;if(p.device.Get()!=d||p.w!=w||p.h!=h||p.ow!=b.Width||p.oh!=b.Height||p.referenceFormat!=outputFormat||!p.encoded){p=Packet{};if(!Init(p,d,w,h,UINT(b.Width),b.Height,outputFormat))return fail("HDR buffer or pipeline allocation failed");}
 p.lastUse=now;p.reconciled=false;p.nativeColour=colour;p.nativeOutput=output;p.scene=std::move(scene);
 auto cpu=p.heap->GetCPUDescriptorHandleForHeapStart();auto stride=d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
 for(UINT mode=0;mode<3;++mode){for(auto* res:{mode==2?(ID3D12Resource*)nullptr:mode?p.output.Get():colour,p.scene.hdr.Get(),p.scene.reference.Get(),(ID3D12Resource*)nullptr}){D3D12_SHADER_RESOURCE_VIEW_DESC s{};s.Format=res?Typed(res->GetDesc().Format):DXGI_FORMAT_R16G16B16A16_FLOAT;s.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;s.Texture2D.MipLevels=1;s.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;d->CreateShaderResourceView(res,&s,cpu);cpu.ptr+=stride;}D3D12_UNORDERED_ACCESS_VIEW_DESC u{};u.Format=mode==1?p.referenceFormat:DXGI_FORMAT_R16G16B16A16_FLOAT;u.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;d->CreateUnorderedAccessView(mode==0?p.input.Get():mode==1?p.reference.Get():p.encoded.Get(),nullptr,&u,cpu);cpu.ptr+=stride;}
 p.done=Hdr10::TrackCommands(c);p.recorded=true;if(p.scene.retire)p.scene.retire(p.done);
 auto arrival=colour==bridgeColour?bridgeColourState:D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
 Barrier(c,colour,arrival,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
 if(p.scene.hdr){Barrier(c,p.scene.hdr.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);Barrier(c,p.scene.reference.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);}
 Barrier(c,p.input.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);Bind(p,c,0);Barrier(c,p.input.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
 Barrier(c,colour,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,arrival);
 if(p.scene.hdr){Barrier(c,p.scene.hdr.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);Barrier(c,p.scene.reference.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);}
 Barrier(c,p.output.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
 return chosen;
}
inline void Finish(const std::shared_ptr<Packet>& packet,ID3D12GraphicsCommandList*c,bool success){
 auto& p=*packet;
 Barrier(c,p.input.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
 Barrier(c,p.output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
 if(success){
  if(p.scene.hdr){Barrier(c,p.scene.hdr.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);Barrier(c,p.scene.reference.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);}
  Barrier(c,p.reference.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);Bind(p,c,1);Barrier(c,p.reference.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COMMON);
  Barrier(c,p.reference.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);
  auto arrival=p.nativeOutput.Get()==bridgeOutput?D3D12_RESOURCE_STATE_COMMON:D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  Barrier(c,p.nativeOutput.Get(),arrival,D3D12_RESOURCE_STATE_COPY_DEST);c->CopyResource(p.nativeOutput.Get(),p.reference.Get());Barrier(c,p.nativeOutput.Get(),D3D12_RESOURCE_STATE_COPY_DEST,arrival);Barrier(c,p.reference.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);
  Barrier(c,p.output.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);Bind(p,c,2);Barrier(c,p.output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COMMON);
 }
 if(success&&p.scene.hdr){Barrier(c,p.scene.hdr.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);Barrier(c,p.scene.reference.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);}
 if(!success)Barrier(c,p.output.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
}
inline Hdr10::SceneInput Image(const std::shared_ptr<Packet>& p){Hdr10::SceneInput image;image.hdr=p->encoded;image.reference=p->reference;image.owner=p;image.referenceSrgb=true;image.referenceHdrShoulder=bool(p->scene.hdr);return image;}
// Only reconstructed DLSS packets can be reconciled. Native DX11 captures
// belong to a different stage and must never be modified as a presentation.
inline bool Reconcile(const Hdr10::SceneInput& image,ID3D12GraphicsCommandList*c,ID3D12Resource*edited,D3D12_RESOURCE_STATES arrival){
 if(!c||!edited||!image.hdr||!image.reference)return false;
 std::lock_guard lock(Mutex());std::shared_ptr<Packet> packet;
 for(auto& candidate:Pool())if(candidate->encoded==image.hdr&&candidate->reference==image.reference){packet=candidate;break;}
 if(!packet||packet->reconciled)return false;
 auto& p=*packet;auto desc=edited->GetDesc();
 if(desc.Width!=p.ow||desc.Height!=p.oh||desc.Format!=p.reference->GetDesc().Format||desc.MipLevels!=1||desc.DepthOrArraySize!=1||desc.SampleDesc.Count!=1||(desc.Flags&D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE))return false;
 auto cpu=p.heap->GetCPUDescriptorHandleForHeapStart();auto stride=p.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);cpu.ptr+=15*stride;
 for(auto* res:{edited,(ID3D12Resource*)nullptr,p.reference.Get(),(ID3D12Resource*)nullptr}){
  D3D12_SHADER_RESOURCE_VIEW_DESC s{};s.Format=res?Typed(res->GetDesc().Format):DXGI_FORMAT_R16G16B16A16_FLOAT;s.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;s.Texture2D.MipLevels=1;s.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;p.device->CreateShaderResourceView(res,&s,cpu);cpu.ptr+=stride;
 }
 D3D12_UNORDERED_ACCESS_VIEW_DESC u{};u.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;u.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;p.device->CreateUnorderedAccessView(p.encoded.Get(),nullptr,&u,cpu);
 p.done=Hdr10::TrackCommands(c);p.recorded=true;p.reconciled=true;
 Barrier(c,edited,arrival,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
 Barrier(c,p.reference.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
 Barrier(c,p.encoded.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);Bind(p,c,3);
 Barrier(c,p.encoded.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COMMON);
 Barrier(c,p.reference.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
 Barrier(c,edited,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_SOURCE);c->CopyResource(p.reference.Get(),edited);
 Barrier(c,edited,D3D12_RESOURCE_STATE_COPY_SOURCE,arrival);Barrier(c,p.reference.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);
 return true;
}
}
