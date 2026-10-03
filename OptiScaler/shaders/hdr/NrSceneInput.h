#pragma once
#include "SceneInput.h"
#include "NrSceneInput_Shader.h"
#include <dlssnr/DlssNr_GpuLifetime.h>
#include <vector>
#include <algorithm>
namespace NrSceneInput {
using Microsoft::WRL::ComPtr;
inline void Transition(ID3D12GraphicsCommandList* c,ID3D12Resource* r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b) {
 if(a==b)return;D3D12_RESOURCE_BARRIER x{};x.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;x.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,a,b};c->ResourceBarrier(1,&x);
}
struct Packet {
 ComPtr<ID3D12Device> device;ComPtr<ID3D12Resource> work,answer,native;
 ComPtr<ID3D12DescriptorHeap> heap;ComPtr<ID3D12RootSignature> root;ComPtr<ID3D12PipelineState> pso;
 Hdr10::SceneInput scene;std::function<bool()> done;UINT width=0,height=0;ULONGLONG lastUse=0;bool recorded=false;
};
// Serialized by the NR mutex. Survives DLL detach: queued GPU work must not lose its owners.
inline std::vector<std::unique_ptr<Packet>>& Packets(){static auto* p=new std::vector<std::unique_ptr<Packet>>;return *p;}
// Only completion AND command-recording retirement permit destruction. Called under NR's mutex.
inline void Collect(bool keepWarm=true,ULONGLONG now=GetTickCount64()) {
 for(auto& p:Packets())if(Hdr10::ReusableRecordedWork(p->recorded,p->done)){p->native.Reset();p->scene={};}
 std::erase_if(Packets(),[&](const auto& p){
     if(!Hdr10::ReusableRecordedWork(p->recorded,p->done))return false;
     return !keepWarm || now-p->lastUse>=2000;
 });
}
inline bool Init(Packet& p,ID3D12Device* d,UINT w,UINT h) {
 p.device=d;p.width=w;p.height=h;
 D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,10,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
 if(FAILED(d->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&p.heap))))return false;
 D3D12_DESCRIPTOR_RANGE ranges[2]={{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,4,0,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,4}};
 D3D12_ROOT_PARAMETER params[2]{};params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;params[0].DescriptorTable={2,ranges};
 params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;params[1].Constants={0,0,8};
 D3D12_ROOT_SIGNATURE_DESC rd{2,params,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_NONE};ComPtr<ID3DBlob> blob,error;
 if(FAILED(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error)))return false;
 if(FAILED(d->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&p.root))))return false;
 D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=p.root.Get();pd.CS={NrSceneInput_cso,sizeof(NrSceneInput_cso)};
 if(FAILED(d->CreateComputePipelineState(&pd,IID_PPV_ARGS(&p.pso))))return false;
 D3D12_RESOURCE_DESC td{};td.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;td.Width=w;td.Height=h;td.DepthOrArraySize=td.MipLevels=1;td.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;td.SampleDesc.Count=1;td.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
 D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
 return SUCCEEDED(d->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&td,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,nullptr,IID_PPV_ARGS(&p.work))) && SUCCEEDED(d->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&td,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,nullptr,IID_PPV_ARGS(&p.answer)));
}
inline void Bind(Packet& p,ID3D12GraphicsCommandList* c,UINT mode) {
 struct Constants{UINT w,h,mode,pad;float rect[4];} k{p.width,p.height,mode,0};std::copy(p.scene.rect.begin(),p.scene.rect.end(),k.rect);
 ID3D12DescriptorHeap* heaps[]={p.heap.Get()};c->SetDescriptorHeaps(1,heaps);c->SetComputeRootSignature(p.root.Get());c->SetPipelineState(p.pso.Get());
 auto gpu=p.heap->GetGPUDescriptorHandleForHeapStart();gpu.ptr+=mode*5*p.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
 c->SetComputeRootDescriptorTable(0,gpu);c->SetComputeRoot32BitConstants(1,8,&k,0);c->Dispatch((p.width+7)/8,(p.height+7)/8,1);
}
inline Packet* Prepare(ID3D12Device* d,ID3D12GraphicsCommandList* c,ID3D12Resource* native,
 D3D12_RESOURCE_STATES arrival,UINT w,UINT h,Hdr10::SceneInput scene,DlssNr::GpuLifetime& lifetime,Hdr10::NrSceneState* failure=nullptr) {
 auto fail=[&](Hdr10::NrSceneState reason)->Packet*{if(failure)*failure=reason;return nullptr;};
 if(!d||!c||!native||!scene.hdr||!scene.reference||!w||!h)return fail(Hdr10::NrSceneState::WaitingScene);
 const auto desc=native->GetDesc();
 // FFXIV's bridge colour is FP16. Unknown formats and layouts retain ordinary NR.
 if(desc.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||desc.Format!=DXGI_FORMAT_R16G16B16A16_FLOAT||desc.SampleDesc.Count!=1||desc.DepthOrArraySize!=1||w>desc.Width||h>desc.Height)return fail(Hdr10::NrSceneState::UnsupportedFormat);
 Collect();Packet* chosen=nullptr;auto& pool=Packets();
 for(auto& p:pool)if(Hdr10::ReusableRecordedWork(p->recorded,p->done)){chosen=p.get();break;}
 if(!chosen){if(pool.size()>=8)return fail(Hdr10::NrSceneState::WaitingGpu);pool.push_back(std::make_unique<Packet>());chosen=pool.back().get();}
 auto& p=*chosen;if(p.device.Get()!=d||p.width!=w||p.height!=h||!p.work||!p.answer){p=Packet{};if(!Init(p,d,w,h))return fail(Hdr10::NrSceneState::AllocationFailed);}
 p.lastUse=GetTickCount64();p.native=native;p.scene=std::move(scene);
 auto cpu=p.heap->GetCPUDescriptorHandleForHeapStart();const auto stride=d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
 for(UINT phase=0;phase<2;++phase){
  for(auto* res:{native,p.scene.hdr.Get(),p.scene.reference.Get(),phase?p.work.Get():nullptr}){
   D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=res?res->GetDesc().Format:DXGI_FORMAT_R16G16B16A16_FLOAT;srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;srv.Texture2D.MipLevels=1;srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;d->CreateShaderResourceView(res,&srv,cpu);cpu.ptr+=stride;
  }
  D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};uav.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;uav.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;d->CreateUnorderedAccessView(phase?p.answer.Get():p.work.Get(),nullptr,&uav,cpu);cpu.ptr+=stride;
 }
 // Reserve lifetime BEFORE recording, including NR feature-creation early returns.
 lifetime.Record(c);p.recorded=true;p.done=lifetime.ReuseProbe(c);if(p.scene.retire)p.scene.retire(p.done);
 Transition(c,native,arrival,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
 Transition(c,p.scene.hdr.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
 Transition(c,p.scene.reference.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
 Transition(c,p.work.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
 Bind(p,c,0);
 Transition(c,p.work.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
 Transition(c,native,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,arrival);
 return chosen;
}
inline void Finish(Packet& p,ID3D12GraphicsCommandList* c,D3D12_RESOURCE_STATES arrival) {
 Transition(c,p.native.Get(),arrival,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
 Transition(c,p.answer.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
 Bind(p,c,1);
 Transition(c,p.answer.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
 Transition(c,p.native.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
 D3D12_TEXTURE_COPY_LOCATION to{},from{};to.pResource=p.native.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;from.pResource=p.answer.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
 c->CopyTextureRegion(&to,0,0,0,&from,nullptr);
 Transition(c,p.native.Get(),D3D12_RESOURCE_STATE_COPY_DEST,arrival);
 Transition(c,p.answer.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
 Transition(c,p.scene.hdr.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
 Transition(c,p.scene.reference.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
}
}
