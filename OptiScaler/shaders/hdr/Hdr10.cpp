#include "pch.h"
#include "Hdr10.h"
#include "Hdr10_Shader.h"
#include <Config.h>
#include <State.h>
#include <dlssnr/DlssNr_GpuLifetime.h>
#include <wrl/client.h>
#include <mutex>
#include <atomic>
using Microsoft::WRL::ComPtr;
namespace Hdr10 {
namespace {
std::atomic<bool> active{false};
std::mutex statusMutex;
std::string status="Off (restart required to enable)";
void Message(std::string text){std::lock_guard lock(statusMutex);status=std::move(text);}
struct Packet {
 ComPtr<ID3D12Device> device;
 ComPtr<ID3D12Resource> image, source;
 ComPtr<ID3D12DescriptorHeap> heap;
 ComPtr<ID3D12RootSignature> root;
 ComPtr<ID3D12PipelineState> pso;
 std::function<bool()> done;
 UINT width=0,height=0;
};
struct Pool {
 std::mutex mutex; DlssNr::GpuLifetime lifetime; std::vector<std::unique_ptr<Packet>> packets;
 float loggedPaper=-1, loggedPeak=-1, loggedExpansion=-1, loggedContrast=-1, loggedSaturation=-1, loggedVibrance=-2;
};
Pool& P(){static auto* p=new Pool;return *p;} // Never destroy in-flight GPU ownership under loader lock.
void Barrier(ID3D12GraphicsCommandList*c,ID3D12Resource*r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b){
 if(a==b)return;D3D12_RESOURCE_BARRIER x{};x.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;x.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,a,b};c->ResourceBarrier(1,&x);
}
bool Init(Packet& p,ID3D12Device*d,UINT w,UINT h){
 p.device=d;p.width=w;p.height=h;
 D3D12_FEATURE_DATA_FORMAT_SUPPORT support{DXGI_FORMAT_R10G10B10A2_UNORM};
 if(FAILED(d->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,&support,sizeof(support))) || !(support.Support2&D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE)) return false;
 D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;hd.NumDescriptors=2;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
 if(FAILED(d->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&p.heap))))return false;
 D3D12_DESCRIPTOR_RANGE ranges[2]{};
 ranges[0]={D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,0,0}; ranges[1]={D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,1};
 D3D12_ROOT_PARAMETER params[2]{};params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;params[0].DescriptorTable={2,ranges};
 params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;params[1].Constants={0,0,8};
 D3D12_ROOT_SIGNATURE_DESC rd{2,params,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_NONE};ComPtr<ID3DBlob> blob,error;
 if(FAILED(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error)))return false;
 if(FAILED(d->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&p.root))))return false;
 D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=p.root.Get();pd.CS={Hdr10_cso,sizeof(Hdr10_cso)};
 if(FAILED(d->CreateComputePipelineState(&pd,IID_PPV_ARGS(&p.pso))))return false;
 D3D12_RESOURCE_DESC td{};td.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;td.Width=w;td.Height=h;td.DepthOrArraySize=td.MipLevels=1;td.Format=DXGI_FORMAT_R10G10B10A2_UNORM;td.SampleDesc.Count=1;td.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
 D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
 return SUCCEEDED(d->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&td,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,nullptr,IID_PPV_ARGS(&p.image)));
}
}
bool Request(HWND hwnd){
 if(!Config::Instance()->FfxivHDR.value_or_default() || State::Instance().gameExe!="ffxiv_dx11.exe")return false;
 const auto fg=Config::Instance()->FGOutput.value_or_default();
 if(fg!=FGOutput::NoFG && fg!=FGOutput::DLSSG && fg!=FGOutput::XeFG){Message("OptiHDR requires DLSS-G, XeFG, or FG off");return false;}
 ComPtr<IDXGIFactory1> factory; if(FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))return false;
 auto monitor=MonitorFromWindow(hwnd,MONITOR_DEFAULTTONEAREST);
 for(UINT i=0;;++i){ComPtr<IDXGIAdapter1>a;if(factory->EnumAdapters1(i,&a)!=S_OK)break;
  for(UINT j=0;;++j){ComPtr<IDXGIOutput>o;if(a->EnumOutputs(j,&o)!=S_OK)break;ComPtr<IDXGIOutput6>o6;
   if(SUCCEEDED(o.As(&o6))){DXGI_OUTPUT_DESC1 desc{};if(SUCCEEDED(o6->GetDesc1(&desc))&&desc.Monitor==monitor&&desc.ColorSpace==DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020)return true;}
  }
 }
 Message("SDR fallback: enable Windows HDR on the game display before launch");return false;
}
bool Active(){return active.load();}
void Deactivate(){active=false;}
std::string Status(){std::lock_guard lock(statusMutex);return status;}
bool Configure(IDXGISwapChain4*c){
 DXGI_SWAP_CHAIN_DESC1 d{}; if(!c||FAILED(c->GetDesc1(&d))||d.Format!=DXGI_FORMAT_R10G10B10A2_UNORM)return false;
 UINT flags=0;const auto cs=DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
 if(FAILED(c->CheckColorSpaceSupport(cs,&flags))||!(flags&DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT)||FAILED(c->SetColorSpace1(cs))){Message("HDR10 output negotiation failed");return false;}
 active=true;State::Instance().isHdrActive=true;Message("HDR10 active: SDR expansion; game HUD shares the same curve");
 LOG_INFO("FFXIV HDR10 output active (PQ / BT.2020); SDR upscaler and NR inputs preserved");return true;
}
ID3D12Resource* Convert(ID3D12Device*d,ID3D12GraphicsCommandList*c,ID3D12Resource*src,D3D12_RESOURCE_STATES state){
 if(!src||!c||!d)return nullptr;auto sd=src->GetDesc();
 DXGI_FORMAT format=sd.Format;
 switch(format){case DXGI_FORMAT_R8G8B8A8_TYPELESS:case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:format=DXGI_FORMAT_R8G8B8A8_UNORM;break;
 case DXGI_FORMAT_B8G8R8A8_TYPELESS:case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:format=DXGI_FORMAT_B8G8R8A8_UNORM;break;default:break;}
 if(sd.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||sd.SampleDesc.Count!=1||sd.DepthOrArraySize!=1||(format!=DXGI_FORMAT_R8G8B8A8_UNORM&&format!=DXGI_FORMAT_B8G8R8A8_UNORM&&format!=DXGI_FORMAT_R16G16B16A16_FLOAT&&format!=DXGI_FORMAT_R10G10B10A2_UNORM))return nullptr;
 auto& pool=P();std::lock_guard lock(pool.mutex);pool.lifetime.Collect();Packet* packet=nullptr;
 for(auto& p:pool.packets)if(!p->done||p->done()){packet=p.get();break;}
 if(!packet){if(pool.packets.size()>=32){LOG_ERROR("HDR10 conversion pool busy; no in-flight packet was reused");return nullptr;}pool.packets.push_back(std::make_unique<Packet>());packet=pool.packets.back().get();}
 auto&p=*packet;
 if(p.device.Get()!=d||p.width!=sd.Width||p.height!=sd.Height||!p.image){p=Packet{};if(!Init(p,d,(UINT)sd.Width,sd.Height)){Message("HDR10 conversion allocation failed");return nullptr;}}
 p.source=src;
 auto cpu=p.heap->GetCPUDescriptorHandleForHeapStart();D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=format;srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;srv.Texture2D.MipLevels=1;srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;d->CreateShaderResourceView(src,&srv,cpu);
 cpu.ptr+=d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};uav.Format=DXGI_FORMAT_R10G10B10A2_UNORM;uav.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;d->CreateUnorderedAccessView(p.image.Get(),nullptr,&uav,cpu);
 auto&cfg=*Config::Instance();struct Constants{UINT w,h;float paper,peak,expansion,contrast,saturation,vibrance;} k{p.width,p.height};
 auto finite=[](float v,float fallback){return std::isfinite(v)?v:fallback;};
 k.peak=std::clamp(finite(cfg.FfxivHDRPeak.value_or_default(),1100.f),400.f,4000.f);k.paper=std::clamp(finite(cfg.FfxivHDRPaper.value_or_default(),203.f),80.f,std::min(400.f,k.peak));k.expansion=std::clamp(finite(cfg.FfxivHDRExpansion.value_or_default(),.5f),0.f,1.f);
 k.contrast=std::clamp(finite(cfg.FfxivHDRContrast.value_or_default(),1.f),.5f,1.5f);
 k.saturation=std::clamp(finite(cfg.FfxivHDRSaturation.value_or_default(),1.f),0.f,2.f);
 k.vibrance=std::clamp(finite(cfg.FfxivHDRVibrance.value_or_default(),0.f),-1.f,1.f);
 if(pool.loggedPaper!=k.paper || pool.loggedPeak!=k.peak || pool.loggedExpansion!=k.expansion || pool.loggedContrast!=k.contrast || pool.loggedSaturation!=k.saturation || pool.loggedVibrance!=k.vibrance){
  LOG_INFO("HDR10 conversion: paper white {} nits, peak {} nits, expansion {}, contrast {}, saturation {}, vibrance {}, SDR white maps to {} nits",
           k.paper,k.peak,k.expansion,k.contrast,k.saturation,k.vibrance,k.paper+(k.peak-k.paper)*k.expansion);
  pool.loggedPaper=k.paper;pool.loggedPeak=k.peak;pool.loggedExpansion=k.expansion;pool.loggedContrast=k.contrast;pool.loggedSaturation=k.saturation;pool.loggedVibrance=k.vibrance;
 }
 Barrier(c,src,state,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);Barrier(c,p.image.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
 ID3D12DescriptorHeap* heaps[]={p.heap.Get()};c->SetDescriptorHeaps(1,heaps);c->SetComputeRootSignature(p.root.Get());c->SetPipelineState(p.pso.Get());c->SetComputeRootDescriptorTable(0,p.heap->GetGPUDescriptorHandleForHeapStart());c->SetComputeRoot32BitConstants(1,8,&k,0);c->Dispatch((p.width+7)/8,(p.height+7)/8,1);
 Barrier(c,p.image.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);Barrier(c,src,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,state);
 pool.lifetime.Record(c);p.done=pool.lifetime.ReuseProbe(c);return p.image.Get();
}
HRESULT ResetCommands(ID3D12GraphicsCommandList*c,ID3D12CommandAllocator*a){
 auto hr=c->Reset(a,nullptr);if(SUCCEEDED(hr))Reset(c);return hr;
}
void ExecuteCommands(ID3D12CommandQueue*q,UINT n,ID3D12CommandList*const*l){
 q->ExecuteCommandLists(n,l);Submitted(q,n,l);
}
void Submitted(ID3D12CommandQueue*q,UINT n,ID3D12CommandList*const*l){P().lifetime.Submitted(q,n,l);}
void Reset(ID3D12CommandList*c){P().lifetime.ResetRecording(c);}
}
