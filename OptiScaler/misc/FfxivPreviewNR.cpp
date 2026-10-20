#include <pch.h>
#include <misc/FramePacingTrace.h>
#include "FfxivPreviewNR.h"
#include "FfxivPreviewInterop.h"
#include <Config.h>
#include <dlssnr/RuntimeSelection.h>
#include <State.h>
#include <Util.h>
#include <proxies/NVNGX_Proxy.h>
#include <dlssnr/PassProfiles.h>
#include <dlssnr/DlssNrNative.h>
#include <dlssnr/NrFocusPolicy.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <vector>
namespace FfxivPreviewNR {
using Microsoft::WRL::ComPtr;
static auto& registryMutex=*new std::recursive_mutex;
static auto& registry=*new std::vector<Instance*>;
static std::atomic<bool> suspended{false};
using Create=void*(__cdecl*)(const wchar_t*,const wchar_t*,ID3D12Device*,ID3D12GraphicsCommandList*,void*,unsigned,unsigned,int,float,int,float,float,float,int,int);
using Evaluate=int(__cdecl*)(ID3D12GraphicsCommandList*,void*,void*,ID3D12Resource*,ID3D12Resource*,ID3D12Resource*,ID3D12Resource*,unsigned,unsigned,unsigned,unsigned,unsigned,unsigned,unsigned,unsigned,unsigned,unsigned,int,int,float,int,float,float,float,int,float,float);
using Release=void(__cdecl*)(void*);
using Surface=FfxivPreviewInterop::Surface;
struct Instance::Impl {
    ComPtr<ID3D12Device> device;ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> commands;
    struct Recording{ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> commands;UINT64 submitted=0;};
    std::array<Recording,4> recordings;unsigned recording=0;UINT64 creation=0;
    ComPtr<ID3D12Fence> fence;ComPtr<ID3D11Fence> dx11Fence;ComPtr<ID3D11DeviceContext4> context;
    Surface input,depth,motion,output;HMODULE library=nullptr;Create create=nullptr;Evaluate evaluate=nullptr;Release release=nullptr;
    ComPtr<ID3D11ShaderResourceView> inputView;
    NVSDK_NGX_Parameter* params=nullptr;void* feature=nullptr;UINT64 value=0,complete=0,lastEval=0,lastFrame=0;
    unsigned width=0,height=0,gw=0,gh=0,preset=0,style=0;
    DlssNr::Profiles::NrPassTuning tuning;std::filesystem::path model;
    bool failed=false,counted=false,stopped=false;
    unsigned previousReuseEvery=1;
    bool previousReuseOverride=false;
    ~Impl(){
        if(feature && release)release(feature);
        if(params && NVNGXProxy::D3D12_DestroyParameters())NVNGXProxy::D3D12_DestroyParameters()(params);
        if(library)FreeLibrary(library);if(counted)--instances;
    }
    bool Ready() const{return !fence || fence->GetCompletedValue()>=complete;}
    void Barrier(ID3D12Resource* resource,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after){
        D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after};commands->ResourceBarrier(1,&b);
    }
    bool Shared(ID3D11Device* d,Surface& surface,UINT w,UINT h,DXGI_FORMAT format){
        return FfxivPreviewInterop::CreateSurface(d,device.Get(),surface,w,h,format);
    }
    bool Submit(){
        if(FAILED(commands->Close()))return false;
        ID3D12CommandList* lists[]{commands.Get()};queue->ExecuteCommandLists(1,lists);
        complete=++value;return SUCCEEDED(queue->Signal(fence.Get(),complete));
    }
    bool Initialize(ID3D11DeviceContext* c,ID3D11Texture2D* colour,ID3D11Texture2D* guide){
        device=State::Instance().currentD3D12Device;
        if(!device || !NVNGXProxy::InitDx12(device.Get()))return false;
        ComPtr<ID3D11Device> d;c->GetDevice(&d);ComPtr<IDXGIDevice> dxgi;ComPtr<IDXGIAdapter> adapter;DXGI_ADAPTER_DESC adapterDesc{};
        if(FAILED(d.As(&dxgi)) || FAILED(dxgi->GetAdapter(&adapter)) || FAILED(adapter->GetDesc(&adapterDesc)))return false;
        auto luid=device->GetAdapterLuid();if(luid.LowPart!=adapterDesc.AdapterLuid.LowPart || luid.HighPart!=adapterDesc.AdapterLuid.HighPart)return false;
        if(FAILED(c->QueryInterface(IID_PPV_ARGS(&context))))return false;
        ComPtr<ID3D11Device5> d5;if(FAILED(d.As(&d5)))return false;
        D3D12_COMMAND_QUEUE_DESC q{};q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
        if(FAILED(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue))) || FAILED(device->CreateCommandAllocator(q.Type,IID_PPV_ARGS(&allocator))) ||
           FAILED(device->CreateCommandList(0,q.Type,allocator.Get(),nullptr,IID_PPV_ARGS(&commands))) || !FfxivPreviewInterop::CreateFence(d5.Get(),device.Get(),dx11Fence,fence))return false;
        D3D11_TEXTURE2D_DESC cd{},gd{};colour->GetDesc(&cd);guide->GetDesc(&gd);width=cd.Width;height=cd.Height;gw=gd.Width;gh=gd.Height;
        if(cd.Format!=DXGI_FORMAT_R16G16B16A16_FLOAT || gd.Format!=DXGI_FORMAT_R32_FLOAT)return false;
        if(!Shared(d.Get(),input,width,height,cd.Format) || !Shared(d.Get(),output,width,height,cd.Format) || !Shared(d.Get(),depth,gw,gh,gd.Format) || !Shared(d.Get(),motion,gw,gh,DXGI_FORMAT_R16G16_FLOAT))return false;
        if(FAILED(d->CreateShaderResourceView(input.dx11.Get(),nullptr,&inputView)))return false;
        auto find=[](const char* name){auto path=Util::FindFilePath(Util::DllPath().remove_filename(),name);if(!path)path=Util::FindFilePath(Util::ExePath().remove_filename(),name);return path;};
        auto forwarder=find("nvngx.dll_dlssnr.dll"),snippet=DlssNr::RuntimeSelection::Path();if(!forwarder || !snippet)return false;model=*snippet;
        library=LoadLibraryW(forwarder->wstring().c_str());if(!library)return false;
        create=reinterpret_cast<Create>(GetProcAddress(library,"dlssnr_call_create"));evaluate=reinterpret_cast<Evaluate>(GetProcAddress(library,"dlssnr_call_evaluate_v2"));release=reinterpret_cast<Release>(GetProcAddress(library,"dlssnr_call_release"));
        if(!create || !evaluate || !release || !NVNGXProxy::D3D12_GetCapabilityParameters() || NVNGXProxy::D3D12_GetCapabilityParameters()(&params)!=NVSDK_NGX_Result_Success || !params)return false;
        auto probe=reinterpret_cast<void(__cdecl*)(void*,const char*,float,int)>(GetProcAddress(library,"dlssnr_call_probe_float"));
        auto setSlot=reinterpret_cast<void(__cdecl*)(int)>(GetProcAddress(library,"dlssnr_call_set_float_slot"));
        if(probe && setSlot){for(int slot:{1,2,5,6,7,4,3,0}){float read=0;probe(params,"DLSSNR.PreviewFloatProbe",.375f,slot);if(params->Get("DLSSNR.PreviewFloatProbe",&read)==NVSDK_NGX_Result_Success && read==.375f){setSlot(slot);break;}}}
        auto cfg=Config::Instance();preset=DlssNr::Profiles::PassPreset(*cfg,0);style=DlssNr::Profiles::PassStyle(*cfg,0);tuning=DlssNr::Profiles::PassTuning(*cfg,0);
        feature=create(model.wstring().c_str(),State::Instance().NVNGX_ApplicationDataPath.c_str(),device.Get(),commands.Get(),params,width,height,int(preset),tuning.intensity,int(style),tuning.structure,tuning.tone,tuning.skin,tuning.autoMask,0);
        if(!Submit())return false;
        creation=complete;recordings[0]={allocator,commands,complete};
        if(!feature)return false;
        ++instances;counted=true;LOG_INFO("CharaView NR: independent one-pass feature {}x{}, GPU initialization submitted",width,height);return true;
    }
};
Instance::Instance():p(new Impl){std::lock_guard lock(registryMutex);registry.push_back(this);}
Instance::~Instance(){std::lock_guard lock(registryMutex);std::erase(registry,this);delete p;}
bool Instance::Ready() const{return p->Ready();}
ID3D11ShaderResourceView* Instance::InputView() const{return p->inputView.Get();}
void Instance::Stop(UINT64 deadline){
    p->stopped=true;
    if(!p->Ready() && p->fence){
        if(p->context)p->context->Flush();
        HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        if(event){if(SUCCEEDED(p->fence->SetEventOnCompletion(p->complete,event))){const auto now=GetTickCount64();WaitForSingleObject(event,DWORD(now<deadline?deadline-now:0));}CloseHandle(event);}
    }
    if(p->Ready()){
        if(p->feature && p->release)p->release(p->feature);p->feature=nullptr;
        if(p->params && NVNGXProxy::D3D12_DestroyParameters())NVNGXProxy::D3D12_DestroyParameters()(p->params);p->params=nullptr;
        if(p->counted){--instances;p->counted=false;}
    }else LOG_WARN("CharaView NR: shutdown fence unresolved; GPU ownership retained");
}
bool Instance::Process(ID3D11DeviceContext* c,ID3D11Texture2D* colour,ID3D11Texture2D* depth,ID3D11Texture2D* motion,bool inverted,bool reset,unsigned long long frameId){
    FramePacingTrace::Scope cpuTiming(FramePacingTrace::Phase::PreviewNR);
    std::lock_guard lock(registryMutex);
    if(suspended.load() || State::Instance().isShuttingDown){++skipped;return false;}
    if(DlssNr::FocusPolicy::Paused(Config::Instance()->DlssNrPauseOnFocusLoss.value_or_default())){
        p->lastEval=0; ++skipped;return false;
    }
    reset|=!p->lastEval || frameId!=p->lastFrame+1;
    if(p->stopped){if(!p->Ready()){++skipped;return false;}delete p;p=new Impl;}
    if(p->failed){++skipped;return false;}
    if(!p->device){
        if(!State::Instance().currentD3D12Device){++skipped;return false;}
        if(!p->Initialize(c,colour,depth)){p->failed=true;LOG_WARN("CharaView NR: initialization failed; clean preview DLSS retained");}
        ++skipped;return false;
    }
    if(p->device.Get()!=State::Instance().currentD3D12Device || p->fence->GetCompletedValue()<p->creation){++skipped;return false;}
    D3D11_TEXTURE2D_DESC cd{},dd{},md{};colour->GetDesc(&cd);depth->GetDesc(&dd);motion->GetDesc(&md);
    if(cd.Width!=p->width || cd.Height!=p->height || dd.Width!=p->gw || dd.Height!=p->gh || md.Width!=p->gw || md.Height!=p->gh){++skipped;return false;}
    auto available=std::find_if(p->recordings.begin(),p->recordings.end(),[&](const auto& r){return !r.allocator || p->fence->GetCompletedValue()>=r.submitted;});
    if(available==p->recordings.end()){++skipped;return false;}
    if(!available->allocator){
        if(FAILED(p->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&available->allocator))) || FAILED(p->device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,available->allocator.Get(),nullptr,IID_PPV_ARGS(&available->commands)))){p->failed=true;return false;}
    }else if(FAILED(available->allocator->Reset()) || FAILED(available->commands->Reset(available->allocator.Get(),nullptr))){p->failed=true;return false;}
    p->allocator=available->allocator;p->commands=available->commands;
    c->CopyResource(p->input.dx11.Get(),colour);c->CopyResource(p->depth.dx11.Get(),depth);c->CopyResource(p->motion.dx11.Get(),motion);
    const UINT64 inputReady=++p->value;
    if(FAILED(p->context->Signal(p->dx11Fence.Get(),inputReady))){p->failed=true;return false;}
    // Even an unsuccessful evaluation owns the shared input until this copy completes.
    p->complete=inputReady;
    if(FAILED(p->queue->Wait(p->fence.Get(),inputReady))){p->failed=true;return false;}
    for(auto* resource:{p->input.dx12.Get(),p->depth.dx12.Get(),p->motion.dx12.Get()})p->Barrier(resource,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    p->Barrier(p->output.dx12.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const auto now=GetTickCount64();auto& t=p->tuning;
    const auto& cfg=*Config::Instance();
    const unsigned reuseEvery=cfg.FfxivPreviewNRReuse.value_or_default()?2u:1u;
    // Match the shared module-verification policy. Alternating policies here
    // would clear the main scene's caches on every preview evaluation.
    const bool reuseOverride=cfg.DlssNrVitAllowUnverified.value_or_default();
    const bool evaluationReset=reset || !p->lastEval || now-p->lastEval>100 ||
        reuseEvery!=p->previousReuseEvery || reuseOverride!=p->previousReuseOverride;
    DlssNrNative::BeginEvaluate(p->feature,evaluationReset,reuseEvery,reuseOverride);
    const int result=p->evaluate(p->commands.Get(),p->feature,p->params,p->input.dx12.Get(),p->depth.dx12.Get(),p->motion.dx12.Get(),p->output.dx12.Get(),p->width,p->height,p->gw,p->gh,p->gw,p->gh,0,0,0,0,inverted,evaluationReset,t.intensity,int(p->style),t.structure,t.tone,t.skin,t.autoMask,float(p->width),float(p->height));
    DlssNrNative::EndEvaluate(result==1);
    const auto reuseCounts=DlssNrNative::LastReuseEvaluation();
    D3D12_RESOURCE_BARRIER uav{};uav.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;uav.UAV.pResource=p->output.dx12.Get();p->commands->ResourceBarrier(1,&uav);
    p->Barrier(p->output.dx12.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COMMON);
    for(auto* resource:{p->input.dx12.Get(),p->depth.dx12.Get(),p->motion.dx12.Get()})p->Barrier(resource,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
    if(!p->Submit()){p->failed=true;return false;}
    available->submitted=p->complete;
    if(result!=1){p->failed=true;LOG_WARN("CharaView NR evaluate {}: clean preview DLSS retained",result);return false;}
    bottleneckComputed.fetch_add(reuseCounts.computed);bottleneckReused.fetch_add(reuseCounts.reused);
    p->previousReuseEvery=reuseEvery;p->previousReuseOverride=reuseOverride;
    if(FAILED(p->context->Wait(p->dx11Fence.Get(),p->complete))){p->failed=true;return false;}
    c->CopyResource(colour,p->output.dx11.Get());
    // Destruction/reuse also waits for the DX11 consumer's copy, not just
    // completion of the model's DX12 dispatch.
    p->complete=++p->value;if(FAILED(p->context->Signal(p->dx11Fence.Get(),p->complete))){p->failed=true;return false;}
    p->lastEval=now;p->lastFrame=frameId;
    if(++evaluated==1 || evaluated.load()%300==0)LOG_INFO("CharaView NR: {} independent preview evaluations; bottleneck computed {}, reused {}",evaluated.load(),bottleneckComputed.load(),bottleneckReused.load());return true;
}
static auto& retired=*new std::vector<std::unique_ptr<Instance>>;
void Retire(std::unique_ptr<Instance> instance){std::lock_guard lock(registryMutex);if(instance)retired.push_back(std::move(instance));}
void Collect(){std::lock_guard lock(registryMutex);std::erase_if(retired,[](const auto& p){return p->Ready();});}
void Suspend(){suspended.store(true);std::lock_guard lock(registryMutex);const auto deadline=GetTickCount64()+1000;for(auto* instance:registry)instance->Stop(deadline);}
void Resume(){suspended.store(false);}
}
