#include <pch.h>
#include <misc/FramePacingTrace.h>
#include "FfxivPreviewDLSS.h"
#include "FfxivPreviewDLSSShaders.h"
#include "FfxivPreviewCamera.h"
#include "FfxivPreviewNR.h"
#include "FfxivPreviewFGMap.h"
#include "FfxivPreviewMotion.h"
#include "FfxivPreviewEnhance.h"
#include "FfxivHdrPreview.h"
#include <Config.h>
#include <State.h>
#include <proxies/NVNGX_Proxy.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <array>
#include <mutex>
#include <algorithm>
#include <filesystem>
#include <fstream>
namespace FfxivPreviewDLSS {
using Microsoft::WRL::ComPtr;
namespace {
constexpr GUID producerTag{0xb62d551f,0x43a3,0x4f60,{0x92,0x1a,0x32,0xa6,0x1c,0x42,0x99,0x67}};
thread_local bool privateDraw=false,managing=false;
struct ManagerGuard{ManagerGuard(){managing=true;}~ManagerGuard(){managing=false;}};
struct Texture {ComPtr<ID3D11Texture2D> texture;ComPtr<ID3D11ShaderResourceView> view;ComPtr<ID3D11RenderTargetView> target;};
struct Slot {
    ComPtr<ID3D11Texture2D> native;
    Texture input,depth,motion,output,composed,nrColour,nrDepth,nrMotion;
    ComPtr<ID3D11Query> done;
    NVSDK_NGX_Parameter* params=nullptr;NVSDK_NGX_Handle* handle=nullptr;
    std::unique_ptr<FfxivPreviewNR::Instance> nr;
    UINT width=0,height=0;UINT64 lastUsed=0,lastEval=0,serial=0;
    bool valid=false,failed=false,inverted=false,nrAllocationFailed=false;
};
struct Data {
    std::mutex mutex;ComPtr<ID3D11Device> device;ComPtr<ID3DDeviceContextState> privateState;
    ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> copy,guide,alpha,alphaNR,nrInputs;
    ComPtr<ID3D11SamplerState> sampler;ComPtr<ID3D11Buffer> constants;FfxivPreviewCamera::Snapshot camera;
    ComPtr<ID3D11Texture2D> nativeDepth;ComPtr<ID3D11ShaderResourceView> depthView;
    std::array<Slot,16> slots{};
    struct Candidate{ComPtr<ID3D11Texture2D> native;UINT64 seen=0,serial=0;};
    std::array<Candidate,16> candidates{};
    UINT width=0,height=0;UINT64 serial=0,lastSweep=0,frame=0,depthClears=0;float jitterX=0,jitterY=0;
    std::array<bool,7> reported{};
    struct DepthCycle{ComPtr<ID3D11Texture2D> texture;UINT64 cleared=0;};
    std::array<DepthCycle,2> depthCycles{};
    struct Diagnostic {
        std::array<ComPtr<ID3D11Texture2D>,6> textures;
        ComPtr<ID3D11Query> done;UINT mask=0,stage=0;UINT64 started=0;
        float jitterX=0,jitterY=0;
        bool pending=false;
    } diagnostic;
    bool building=false,prepassSeen=false,motion=false,cameraCaptured=false,requested=false,inverted=false,failed=false,shutdown=false;
};
Data& Get(){static auto* value=new Data;return *value;}
const char* code=FfxivPreviewDLSSShaders::code;
struct Guard{bool savedPrivate=privateDraw,savedProcessing=FfxivPreviewEnhance::processing;Guard(){privateDraw=true;FfxivPreviewEnhance::processing=true;}~Guard(){privateDraw=savedPrivate;FfxivPreviewEnhance::processing=savedProcessing;}};
void CaptureDiagnostic(Data& s,ID3D11DeviceContext* c,Slot& slot){
    if(!Config::Instance()->FfxivPreviewGuideCapture.value_or_default())return;
    auto& capture=s.diagnostic;
    const UINT stage=evaluated.load()<120?1:2;
    if(capture.pending || (capture.mask&(1u<<stage)) || UINT64(slot.width)*slot.height>2097152)return;
    ID3D11Texture2D* inputs[]{slot.native.Get(),slot.input.texture.Get(),slot.output.texture.Get(),slot.composed.texture.Get(),slot.depth.texture.Get(),slot.motion.texture.Get()};
    for(UINT i=0;i<6;++i){
        D3D11_TEXTURE2D_DESC desc{};inputs[i]->GetDesc(&desc);
        desc.BindFlags=desc.MiscFlags=0;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        capture.textures[i].Reset();if(FAILED(s.device->CreateTexture2D(&desc,nullptr,&capture.textures[i]))){capture.mask|=1u<<stage;return;}
        c->CopyResource(capture.textures[i].Get(),inputs[i]);
    }
    D3D11_QUERY_DESC desc{D3D11_QUERY_EVENT,0};capture.done.Reset();
    if(FAILED(s.device->CreateQuery(&desc,&capture.done))){capture.mask|=1u<<stage;return;}
    c->End(capture.done.Get());capture.stage=stage;capture.mask|=1u<<stage;capture.pending=true;capture.started=GetTickCount64();capture.jitterX=s.jitterX;capture.jitterY=s.jitterY;
}
void SaveDiagnostic(Data& s,ID3D11DeviceContext* c){
    auto& capture=s.diagnostic;if(!capture.pending)return;
    if(c->GetData(capture.done.Get(),nullptr,0,D3D11_ASYNC_GETDATA_DONOTFLUSH)!=S_OK){
        if(GetTickCount64()-capture.started>5000){capture.pending=false;capture.textures={};capture.done.Reset();}
        return;
    }
    wchar_t executable[MAX_PATH]{};GetModuleFileNameW(nullptr,executable,MAX_PATH);
    auto path=std::filesystem::path(executable).parent_path()/L"OptiScaler"/L"PreviewDLSSDiagnostics";
    std::error_code error;std::filesystem::create_directories(path,error);
    const char* names[]{"native","input","dlss","composed","depth","motion"};
    bool saved=!error;
    for(UINT i=0;!error && i<6;++i){
        D3D11_MAPPED_SUBRESOURCE mapped{};if(c->Map(capture.textures[i].Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&mapped)!=S_OK)return;
        D3D11_TEXTURE2D_DESC desc{};capture.textures[i]->GetDesc(&desc);
        std::ofstream file(path/(std::to_string(capture.stage)+"-"+names[i]+".bin"),std::ios::binary);
        UINT header[]{desc.Width,desc.Height,UINT(desc.Format)};file.write(reinterpret_cast<char*>(header),sizeof(header));
        const UINT bpp=desc.Format==DXGI_FORMAT_R16G16B16A16_FLOAT?8:4;
        for(UINT row=0;row<desc.Height;++row)file.write(static_cast<const char*>(mapped.pData)+row*mapped.RowPitch,desc.Width*bpp);
        file.flush();saved&=bool(file);
        c->Unmap(capture.textures[i].Get(),0);
    }
    LOG_INFO("CharaView DLSS diagnostic {}: six synchronized textures saved={}, jitter {}/{}",capture.stage,saved,capture.jitterX,capture.jitterY);
    capture.pending=false;capture.textures={};capture.done.Reset();
}
bool Make(Texture& t,ID3D11Device* d,UINT w,UINT h,DXGI_FORMAT format){
    D3D11_TEXTURE2D_DESC desc{};desc.Width=w;desc.Height=h;desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;desc.Format=format;
    desc.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
    return SUCCEEDED(d->CreateTexture2D(&desc,nullptr,&t.texture)) && SUCCEEDED(d->CreateShaderResourceView(t.texture.Get(),nullptr,&t.view)) && SUCCEEDED(d->CreateRenderTargetView(t.texture.Get(),nullptr,&t.target));
}
bool Ready(Slot& slot,ID3D11DeviceContext* c){return !slot.done || c->GetData(slot.done.Get(),nullptr,0,D3D11_ASYNC_GETDATA_DONOTFLUSH)==S_OK;}
void Release(Slot& slot){
    FfxivPreviewNR::Retire(std::move(slot.nr));
    if(slot.handle && NVNGXProxy::D3D11_ReleaseFeature())NVNGXProxy::D3D11_ReleaseFeature()(slot.handle);
    if(slot.params && NVNGXProxy::D3D11_DestroyParameters())NVNGXProxy::D3D11_DestroyParameters()(slot.params);
    slot=Slot{};
}
bool Initialize(Data& s,ID3D11Device* d){
    if(s.failed || s.shutdown)return false;
    if(s.device)return s.device.Get()==d;
    s.device=d;ComPtr<ID3D11Device1> d1;if(FAILED(d->QueryInterface(IID_PPV_ARGS(&d1))))return false;
    D3D_FEATURE_LEVEL level=D3D_FEATURE_LEVEL_11_0,chosen{};
    if(FAILED(d1->CreateDeviceContextState(0,&level,1,D3D11_SDK_VERSION,__uuidof(ID3D11Device),&chosen,&s.privateState)))return false;
    ComPtr<ID3DBlob> blob,error;
    if(FAILED(D3DCompile(code,strlen(code),nullptr,nullptr,nullptr,"VS","vs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&blob,&error)) || FAILED(d->CreateVertexShader(blob->GetBufferPointer(),blob->GetBufferSize(),nullptr,&s.vs)))return false;
    for(auto [entry,dest]:std::array<std::pair<const char*,ComPtr<ID3D11PixelShader>*>,5>{{{"Copy",&s.copy},{"Guides",&s.guide},{"Alpha",&s.alpha},{"AlphaNR",&s.alphaNR},{"NRInputs",&s.nrInputs}}}){
        blob.Reset();if(FAILED(D3DCompile(code,strlen(code),nullptr,nullptr,nullptr,entry,"ps_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&blob,&error)) || FAILED(d->CreatePixelShader(blob->GetBufferPointer(),blob->GetBufferSize(),nullptr,dest->GetAddressOf())))return false;
    }
    D3D11_SAMPLER_DESC samp{};samp.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;samp.AddressU=samp.AddressV=samp.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;samp.MaxLOD=D3D11_FLOAT32_MAX;
    if(FAILED(d->CreateSamplerState(&samp,&s.sampler)))return false;
    D3D11_BUFFER_DESC cb{};cb.ByteWidth=16;cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    if(FAILED(d->CreateBuffer(&cb,nullptr,&s.constants)))return false;
    return true;
}
float Halton(UINT n,UINT base){float f=1,v=0;for(;n;n/=base){f/=base;v+=f*(n%base);}return v-.5f;}
void Setup(Data& s,ID3D11DeviceContext* c,UINT w,UINT h){
    D3D11_VIEWPORT vp{0,0,float(w),float(h),0,1};c->RSSetViewports(1,&vp);c->RSSetState(nullptr);
    c->OMSetBlendState(nullptr,nullptr,~0u);c->OMSetDepthStencilState(nullptr,0);
    c->IASetInputLayout(nullptr);c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    c->VSSetShader(s.vs.Get(),nullptr,0);c->GSSetShader(nullptr,nullptr,0);c->HSSetShader(nullptr,nullptr,0);c->DSSetShader(nullptr,nullptr,0);
    c->PSSetSamplers(0,1,s.sampler.GetAddressOf());c->PSSetConstantBuffers(0,1,s.constants.GetAddressOf());FfxivPreviewCamera::Bind(c,s.camera);
}
void Blit(Data& s,ID3D11DeviceContext* c,ID3D11PixelShader* ps,Texture& target,ID3D11ShaderResourceView* input,UINT w,UINT h){
    Setup(s,c,w,h);c->OMSetRenderTargets(1,target.target.GetAddressOf(),nullptr);c->PSSetShader(ps,nullptr,0);c->PSSetShaderResources(0,1,&input);c->Draw(3,0);
    ID3D11ShaderResourceView* empty[6]{};c->PSSetShaderResources(0,6,empty);c->OMSetRenderTargets(0,nullptr,nullptr);
}
bool CreateFeature(Data& s,Slot& slot,ID3D11DeviceContext* c,UINT w,UINT h,bool inverted){
    if(!NVNGXProxy::InitDx11(s.device.Get()) || !NVNGXProxy::D3D11_AllocateParameters() || !NVNGXProxy::D3D11_CreateFeature() || !NVNGXProxy::D3D11_EvaluateFeature())return false;
    if(NVNGXProxy::D3D11_AllocateParameters()(&slot.params)!=NVSDK_NGX_Result_Success || !slot.params)return false;
    D3D11_QUERY_DESC q{D3D11_QUERY_EVENT,0};if(FAILED(s.device->CreateQuery(&q,&slot.done)))return false;
    auto p=slot.params;p->Set(NVSDK_NGX_Parameter_Width,w);p->Set(NVSDK_NGX_Parameter_Height,h);
    p->Set(NVSDK_NGX_Parameter_OutWidth,w*2);p->Set(NVSDK_NGX_Parameter_OutHeight,h*2);
    p->Set(NVSDK_NGX_Parameter_PerfQualityValue,int(NVSDK_NGX_PerfQuality_Value_MaxPerf));
    p->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,int(NVSDK_NGX_DLSS_Feature_Flags_MVLowRes|NVSDK_NGX_DLSS_Feature_Flags_AutoExposure|(inverted?NVSDK_NGX_DLSS_Feature_Flags_DepthInverted:0)));
    if(NVNGXProxy::D3D11_CreateFeature()(c,NVSDK_NGX_Feature_SuperSampling,p,&slot.handle)!=NVSDK_NGX_Result_Success)return false;
    if(!Make(slot.input,s.device.Get(),w,h,DXGI_FORMAT_R8G8B8A8_UNORM) || !Make(slot.depth,s.device.Get(),w,h,DXGI_FORMAT_R32_FLOAT) ||
       !Make(slot.motion,s.device.Get(),w,h,DXGI_FORMAT_R16G16_FLOAT) || !Make(slot.output,s.device.Get(),w*2,h*2,DXGI_FORMAT_R16G16B16A16_FLOAT) ||
       !Make(slot.composed,s.device.Get(),w*2,h*2,DXGI_FORMAT_R8G8B8A8_UNORM))return false;
    slot.width=w;slot.height=h;slot.inverted=inverted;
    LOG_INFO("CharaView DLSS: isolated feature {}x{} -> {}x{}, native alpha retained",w,h,w*2,h*2);return true;
}
void Finish(Data& s,ID3D11DeviceContext* c,ID3D11Texture2D* native,UINT w,UINT h){
    FramePacingTrace::Scope cpuTiming(FramePacingTrace::Phase::Preview);
    s.building=false;
    if(w!=s.width || h!=s.height || w*2>8192 || h*2>8192){++fallbacks;return;}
    const auto now=GetTickCount64();
    Data::Candidate* candidate=nullptr;
    for(auto& item:s.candidates)if(item.native.Get()==native){candidate=&item;break;}
    if(!candidate)for(auto& item:s.candidates)if(!item.native){candidate=&item;item.native=native;break;}
    if(candidate){candidate->seen=now;candidate->serial=s.serial;}
    Slot* slot=nullptr;
    for(auto& item:s.slots)if(item.native.Get()==native){slot=&item;break;}
    // The UI must actually sample this preview before it gets a DLSS instance.
    if(!slot || now-slot->lastUsed>250)return;
    if(!s.motion || !s.nativeDepth || !s.depthView){
        if(!s.reported[0]){s.reported[0]=true;LOG_WARN("CharaView DLSS waiting for guides: motion {}, camera {}, depth {}, depth SRV {}; native preview retained",s.motion,s.cameraCaptured,bool(s.nativeDepth),bool(s.depthView));}
        slot->valid=false;++fallbacks;return;
    }
    if(slot->failed){++fallbacks;return;}
    if(slot->handle && (slot->width!=w || slot->height!=h || slot->inverted!=s.inverted)){
        if(!Ready(*slot,c)){slot->valid=false;++fallbacks;return;}Release(*slot);slot->native=native;slot->lastUsed=now;
    }
    ComPtr<ID3D11DeviceContext1> c1;if(FAILED(c->QueryInterface(IID_PPV_ARGS(&c1)))){slot->valid=false;++fallbacks;return;}
    ComPtr<ID3D11ShaderResourceView> source;if(FAILED(s.device->CreateShaderResourceView(native,nullptr,&source))){slot->valid=false;++fallbacks;return;}
    Guard guard;ComPtr<ID3DDeviceContextState> saved;c1->SwapDeviceContextState(s.privateState.Get(),&saved);
    const auto restore=[&]{ID3D11ShaderResourceView* empty[6]{};c->PSSetShaderResources(0,6,empty);c->OMSetRenderTargets(0,nullptr,nullptr);c1->SwapDeviceContextState(saved.Get(),nullptr);};
    if(!slot->handle && !CreateFeature(s,*slot,c,w,h,s.inverted)){
        LOG_WARN("CharaView DLSS unavailable: feature creation failed; native preview retained");slot->failed=true;slot->valid=false;if(slot->done)c->End(slot->done.Get());restore();++fallbacks;return;
    }
    // NGX owns its compute bindings. Unbind them inside our private state
    // before reading/writing the same textures in fullscreen passes.
    c->ClearState();
    float settings[4]{s.jitterX,s.jitterY,1.0f/w,1.0f/h};c->UpdateSubresource(s.constants.Get(),0,nullptr,settings,0,0);
    Blit(s,c,s.copy.Get(),slot->input,source.Get(),w,h);
    auto& motion=FfxivPreviewMotion::Data();ComPtr<ID3D11ShaderResourceView> motionView;
    if(FAILED(s.device->CreateShaderResourceView(motion.texture.Get(),nullptr,&motionView))){slot->valid=false;c->End(slot->done.Get());restore();++fallbacks;return;}
    Setup(s,c,w,h);ID3D11RenderTargetView* rt[2]{slot->depth.target.Get(),slot->motion.target.Get()};c->OMSetRenderTargets(2,rt,nullptr);
    ID3D11ShaderResourceView* inputs[3]{nullptr,motionView.Get(),s.depthView.Get()};c->PSSetShaderResources(0,3,inputs);c->PSSetShader(s.guide.Get(),nullptr,0);c->Draw(3,0);
    ID3D11ShaderResourceView* empty[4]{};c->PSSetShaderResources(0,4,empty);c->OMSetRenderTargets(0,nullptr,nullptr);
    bool nrApplied=false;
    if(Config::Instance()->FfxivPreviewNR.value_or_default()){
        if(!slot->nrColour.texture && !slot->nrAllocationFailed){
            Texture next,depth,motion;
            if(Make(next,s.device.Get(),w,h,DXGI_FORMAT_R16G16B16A16_FLOAT) &&
               Make(depth,s.device.Get(),w,h,DXGI_FORMAT_R32_FLOAT) &&
               Make(motion,s.device.Get(),w,h,DXGI_FORMAT_R16G16_FLOAT)){
                slot->nrColour=std::move(next);slot->nrDepth=std::move(depth);slot->nrMotion=std::move(motion);
                LOG_INFO("CharaView NR: native {}x{} correction applied after {}x{} DLSS",w,h,w*2,h*2);
            }else{slot->nrAllocationFailed=true;LOG_WARN("CharaView NR: low-resolution surface allocation failed; clean DLSS retained");}
        }
        if(slot->nrColour.texture){
            Setup(s,c,w,h);
            ID3D11RenderTargetView* targets[]{slot->nrColour.target.Get(),slot->nrDepth.target.Get(),slot->nrMotion.target.Get()};
            ID3D11ShaderResourceView* sources[]{slot->input.view.Get(),slot->motion.view.Get(),slot->depth.view.Get()};
            c->OMSetRenderTargets(3,targets,nullptr);c->PSSetShaderResources(0,3,sources);
            c->PSSetShader(s.nrInputs.Get(),nullptr,0);c->Draw(3,0);
            ID3D11ShaderResourceView* empty[3]{};c->PSSetShaderResources(0,3,empty);c->OMSetRenderTargets(0,nullptr,nullptr);
            if(!slot->nr)slot->nr=std::make_unique<FfxivPreviewNR::Instance>();
            nrApplied=slot->nr->Process(c,slot->nrColour.texture.Get(),slot->nrDepth.texture.Get(),slot->nrMotion.texture.Get(),s.inverted,!slot->lastEval || now-slot->lastEval>250,s.frame?s.frame:s.serial);
            c->ClearState();
        }
    }else{
        FfxivPreviewNR::Retire(std::move(slot->nr));slot->nrColour={};slot->nrDepth={};slot->nrMotion={};slot->nrAllocationFailed=false;
    }
    auto p=slot->params;
    p->Set(NVSDK_NGX_Parameter_Color,static_cast<ID3D11Resource*>(slot->input.texture.Get()));
    p->Set(NVSDK_NGX_Parameter_Output,static_cast<ID3D11Resource*>(slot->output.texture.Get()));
    p->Set(NVSDK_NGX_Parameter_Depth,static_cast<ID3D11Resource*>(slot->depth.texture.Get()));
    p->Set(NVSDK_NGX_Parameter_MotionVectors,static_cast<ID3D11Resource*>(slot->motion.texture.Get()));
    p->Set(NVSDK_NGX_Parameter_Jitter_Offset_X,s.jitterX);p->Set(NVSDK_NGX_Parameter_Jitter_Offset_Y,s.jitterY);
    p->Set(NVSDK_NGX_Parameter_MV_Scale_X,float(w));p->Set(NVSDK_NGX_Parameter_MV_Scale_Y,float(h));
    p->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width,w);p->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height,h);
    p->Set(NVSDK_NGX_Parameter_DLSS_Pre_Exposure,1.0f);p->Set(NVSDK_NGX_Parameter_DLSS_Exposure_Scale,1.0f);
    p->Set(NVSDK_NGX_Parameter_Reset,int(!slot->lastEval || now-slot->lastEval>250));
    p->Set(NVSDK_NGX_Parameter_Sharpness,0.0f);
    const auto result=NVNGXProxy::D3D11_EvaluateFeature()(c,slot->handle,p,nullptr);
    slot->valid=result==NVSDK_NGX_Result_Success;
    if(slot->valid){
        c->ClearState();
        Setup(s,c,w*2,h*2);c->PSSetShaderResources(3,1,source.GetAddressOf());
        if(nrApplied){c->PSSetShaderResources(4,1,slot->nrColour.view.GetAddressOf());
            auto* reference=slot->nr->InputView();c->PSSetShaderResources(5,1,&reference);}
        Blit(s,c,nrApplied?s.alphaNR.Get():s.alpha.Get(),slot->composed,slot->output.view.Get(),w*2,h*2);
        slot->lastEval=now;slot->serial=s.serial;
        if(s.jitterX!=0 || s.jitterY!=0)++jittered;
        if(++evaluated==1 || evaluated.load()%300==0)LOG_INFO("CharaView DLSS: {} evaluations, {}x{} input -> {}x{} output, jitter {}/{}, {} jittered evaluations",evaluated.load(),w,h,w*2,h*2,s.jitterX,s.jitterY,jittered.load());
        CaptureDiagnostic(s,c,*slot);
    }else{++fallbacks;LOG_WARN("CharaView DLSS evaluate {:X}; native preview retained",UINT(result));slot->failed=true;}
    c->End(slot->done.Get());restore();
}
void Collect(Data& s,ID3D11DeviceContext* c,UINT64 now){
    Guard guard;
    SaveDiagnostic(s,c);
    FfxivPreviewNR::Collect();
    const bool enabled=Config::Instance()->FfxivPreviewDLSS.value_or_default();UINT count=0;
    for(auto& slot:s.slots){if(slot.native && (now-slot.lastUsed>2000 || !enabled) && Ready(slot,c))Release(slot);if(slot.handle)++count;}
    for(auto& item:s.candidates)if(item.native && now-item.seen>250)item={};
    instances.store(count);s.lastSweep=now;
}
} // namespace
void Tag(ID3D11PixelShader* ps,unsigned crc){if(ps && crc==4256363772u){UINT yes=1;ps->SetPrivateData(producerTag,sizeof(yes),&yes);}}
void Draw(ID3D11DeviceContext* c,const std::function<void()>& draw){
    if(privateDraw || managing || FfxivPreviewMotion::replaying || c->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE || !Config::Instance()->FfxivPreviewDLSS.value_or_default() || State::Instance().isShuttingDown){draw();return;}
    ManagerGuard manager;
    auto& s=Get();std::unique_lock lock(s.mutex);if(s.shutdown){lock.unlock();draw();return;}
    const auto now=GetTickCount64();if(s.device && !s.building && now-s.lastSweep>=100)Collect(s,c,now);
    ComPtr<ID3D11Device> device;c->GetDevice(&device);
    ComPtr<ID3D11RenderTargetView> target;ComPtr<ID3D11DepthStencilView> dsv;c->OMGetRenderTargets(1,&target,&dsv);
    ComPtr<ID3D11Resource> resource;ComPtr<ID3D11Texture2D> texture;D3D11_TEXTURE2D_DESC desc{};int entry=-1;
    if(target){target->GetResource(&resource);if(SUCCEEDED(resource.As(&texture))){texture->GetDesc(&desc);entry=FfxivCharaViewTrace::Match(FfxivCharaViewTrace::Current(),texture.Get(),desc.Width,desc.Height);}}
    bool prepass=false,freshDepth=false;
    if(dsv){
        ComPtr<ID3D11Resource> r;ComPtr<ID3D11Texture2D> depth;dsv->GetResource(&r);
        if(SUCCEEDED(r.As(&depth))){D3D11_TEXTURE2D_DESC dd{};depth->GetDesc(&dd);
            const int depthEntry=FfxivCharaViewTrace::Match(FfxivCharaViewTrace::Current(),depth.Get(),dd.Width,dd.Height);
            ComPtr<ID3D11Buffer> camera;c->VSGetConstantBuffers(0,1,&camera);D3D11_BUFFER_DESC cb{};if(camera)camera->GetDesc(&cb);
            const bool previewDepth=depthEntry==18 || depthEntry==19;
            ComPtr<ID3D11DepthStencilState> state;UINT reference;c->OMGetDepthStencilState(&state,&reference);
            D3D11_DEPTH_STENCIL_DESC ds{};if(state)state->GetDesc(&ds);else ds.DepthEnable=true;
            prepass=previewDepth && cb.ByteWidth>=944 && ds.DepthEnable && entry!=8 && entry!=13;
            if(previewDepth){auto& cycle=s.depthCycles[depthEntry-18];freshDepth=cycle.texture==depth && GetTickCount64()-cycle.cleared<250;}
            if(prepass)desc=dd;
        }
    }
    ComPtr<ID3D11PixelShader> ps;c->PSGetShader(&ps,nullptr,nullptr);UINT marked=0,bytes=sizeof(marked);
    bool producer=ps && SUCCEEDED(ps->GetPrivateData(producerTag,&bytes,&marked)) && marked;
    // Finishing shaders can be created before our device hook is installed.
    // Recover only an exact engine-owned output that samples the current
    // native geometry buffer; dimensions alone are never sufficient.
    if(!producer && s.building && entry>=0 && entry<8 && desc.Width==s.width && desc.Height==s.height && desc.Format==DXGI_FORMAT_B8G8R8A8_UNORM){
        ID3D11ShaderResourceView* finishing[16]{};c->PSGetShaderResources(0,16,finishing);
        for(auto view:finishing)if(view){
            ComPtr<ID3D11Resource> input;ComPtr<ID3D11Texture2D> t;view->GetResource(&input);
            if(SUCCEEDED(input.As(&t))){D3D11_TEXTURE2D_DESC td{};t->GetDesc(&td);
                if(FfxivCharaViewTrace::Match(FfxivCharaViewTrace::Current(),t.Get(),td.Width,td.Height)==8)producer=true;}
            view->Release();
        }
        if(producer && !s.reported[1]){s.reported[1]=true;LOG_INFO("CharaView DLSS: recovered early-created native finisher, output entry {}",entry);}
    }
    UINT ui=0,uiBytes=sizeof(ui),displayCrc=0,crcBytes=sizeof(displayCrc);
    const GUID displayTag{0xd2b564c4,0x7708,0x48f0,{0xa7,0xb5,0x55,0x22,0x19,0x42,0x31,0xf0}};
    if(ps){ps->GetPrivateData(FfxivHdrPreview::tag,&uiBytes,&ui);ps->GetPrivateData(displayTag,&crcBytes,&displayCrc);}
    if(!prepass && entry!=8 && entry!=13 && !producer && !ui && displayCrc!=2067850010u){lock.unlock();draw();return;}
    if(!Initialize(s,device.Get())){
        if(!s.failed)LOG_WARN("CharaView DLSS initialization failed: private state {}, VS {}, copy {}, guides {}, alpha {}, sampler {}, constants {}; native preview retained",bool(s.privateState),bool(s.vs),bool(s.copy),bool(s.guide),bool(s.alpha),bool(s.sampler),bool(s.constants));
        s.failed=true;lock.unlock();draw();return;
    }
    if((prepass || entry==8 || entry==13) && dsv && desc.SampleDesc.Count==1 && desc.Width<=4096 && desc.Height<=4096){
        D3D11_VIEWPORT vp{};UINT count=1;c->RSGetViewports(&count,&vp);
        if(count!=1 || vp.Width!=desc.Width || vp.Height!=desc.Height){lock.unlock();draw();return;}
        if(!s.building){
            s.building=true;s.prepassSeen=freshDepth;s.motion=false;s.cameraCaptured=false;s.width=desc.Width;s.height=desc.Height;++s.serial;
            const auto now=GetTickCount64();s.requested=std::any_of(s.slots.begin(),s.slots.end(),[&](const Slot& slot){return slot.native && !slot.failed && now-slot.lastUsed<250;});
            // Bootstrap and all failed feature paths keep native rasterisation
            // unchanged. Start temporal jitter only after NGX has evaluated.
            const bool running=s.requested && freshDepth && std::any_of(s.slots.begin(),s.slots.end(),[](const Slot& slot){return slot.valid && !slot.failed;});
            // All previews in one real frame share the phase. Advancing once
            // per portrait can otherwise make each view sample only a subset
            // of the sequence when several portraits are open together.
            const auto phase=s.frame?s.frame:s.serial;
            s.jitterX=running?Halton(UINT(phase%16)+1,2):0;s.jitterY=running?Halton(UINT(phase%16)+1,3):0;
            auto& m=FfxivPreviewMotion::Data();std::lock_guard ml(m.mutex);
            if(s.requested && FfxivPreviewMotion::Ensure(m,c,s.width,s.height)){const FLOAT zero[4]{};c->ClearRenderTargetView(m.target.Get(),zero);}
        }
        if(!s.requested){draw();return;}
        if(prepass){
            s.prepassSeen|=freshDepth;
            // The later G-buffer draw can use EQUAL against this prepass.
            // Moving only that draw rejects native geometry before DLSS sees it.
            auto shifted=vp;shifted.TopLeftX+=s.jitterX;shifted.TopLeftY+=s.jitterY;
            c->RSSetViewports(1,&shifted);draw();c->RSSetViewports(1,&vp);
            if(!s.reported[4]){s.reported[4]=true;LOG_INFO("CharaView DLSS: native preview depth stage found (colour target {}); raster jitter synchronized from depth clear",entry);}
            return;
        }
        if(!s.prepassSeen){
            s.jitterX=s.jitterY=0;
            if(!s.reported[6]){s.reported[6]=true;LOG_INFO("CharaView DLSS: no verified preview depth clear this frame; native raster positions retained");}
        }
        UINT geometry=0,tagSize=sizeof(geometry);if(ps)ps->GetPrivateData(FfxivPreviewMotion::tag,&tagSize,&geometry);
        if(entry==8 && geometry && !s.cameraCaptured && FfxivPreviewCamera::Capture(c,s.camera)){
            s.cameraCaptured=true;
            if(!s.reported[2]){s.reported[2]=true;LOG_INFO("CharaView DLSS camera captured: buffer {} bytes, first constant {}, range {} constants",s.camera.bytes,s.camera.first,s.camera.count);}
            ComPtr<ID3D11Resource> depthResource;ComPtr<ID3D11Texture2D> nextDepth;dsv->GetResource(&depthResource);depthResource.As(&nextDepth);
            if(nextDepth!=s.nativeDepth){s.nativeDepth=nextDepth;s.depthView.Reset();}
            D3D11_SHADER_RESOURCE_VIEW_DESC view{};view.Format=DXGI_FORMAT_R24_UNORM_X8_TYPELESS;view.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;view.Texture2D.MipLevels=1;
            if(s.nativeDepth && !s.depthView)s.device->CreateShaderResourceView(s.nativeDepth.Get(),&view,&s.depthView);
            ComPtr<ID3D11DepthStencilState> ds;UINT ref;c->OMGetDepthStencilState(&ds,&ref);D3D11_DEPTH_STENCIL_DESC dd{};if(ds)ds->GetDesc(&dd);
            s.inverted=ds && (dd.DepthFunc==D3D11_COMPARISON_GREATER || dd.DepthFunc==D3D11_COMPARISON_GREATER_EQUAL);
            if(!s.reported[5]){s.reported[5]=true;LOG_INFO("CharaView DLSS geometry depth comparison {}",UINT(dd.DepthFunc));}
        }
        else if(entry==8 && geometry && !s.cameraCaptured && !s.reported[3]){
            s.reported[3]=true;ComPtr<ID3D11Buffer> camera;c->PSGetConstantBuffers(2,1,&camera);D3D11_BUFFER_DESC desc{};if(camera)camera->GetDesc(&desc);
            LOG_WARN("CharaView DLSS camera capture rejected: native buffer {} bytes; native preview retained",desc.ByteWidth);
        }
        const auto before=FfxivPreviewMotion::draws.load();
        auto shifted=vp;shifted.TopLeftX+=s.jitterX;shifted.TopLeftY+=s.jitterY;c->RSSetViewports(1,&shifted);
        FfxivPreviewMotion::Around(c,draw);c->RSSetViewports(1,&vp);
        s.motion|=FfxivPreviewMotion::draws.load()>before;return;
    }
    bool depthInactive=!dsv;
    if(producer && dsv){ComPtr<ID3D11DepthStencilState> ds;UINT ref;c->OMGetDepthStencilState(&ds,&ref);D3D11_DEPTH_STENCIL_DESC dd{};if(ds)ds->GetDesc(&dd);depthInactive=ds && !dd.DepthEnable && !dd.StencilEnable;}
    if(producer && texture && depthInactive && s.building && desc.Width==s.width && desc.Height==s.height && desc.Format==DXGI_FORMAT_B8G8R8A8_UNORM){
        D3D11_VIEWPORT vp{};UINT count=1;c->RSGetViewports(&count,&vp);
        draw();
        if(count==1 && vp.TopLeftX>=0 && vp.TopLeftX<=.5f && vp.TopLeftY>=0 && vp.TopLeftY<=.5f && vp.Width==desc.Width && vp.Height==desc.Height){
            auto& motion=FfxivPreviewMotion::Data();if(Config::Instance()->FfxivPreviewGuideCapture.value_or_default()){std::lock_guard ml(motion.mutex);FfxivPreviewMotion::Report(motion,c);}
            Finish(s,c,texture.Get(),desc.Width,desc.Height);
        }
        else{s.building=false;++fallbacks;}
        return;
    }
    // Replace only textures produced by the verified native preview finisher.
    // Whole-resource SRVs only: an atlas needs separate subrect histories.
    ID3D11ShaderResourceView* raw[8]{};c->PSGetShaderResources(0,8,raw);std::array<ComPtr<ID3D11ShaderResourceView>,8> saved;bool changed=false;
    unsigned mappedHistory=UINT_MAX,mappedCount=0;
    for(UINT i=0;i<8;++i){saved[i].Attach(raw[i]);if(!raw[i])continue;raw[i]->GetResource(&resource);
        Slot* active=nullptr;
        for(auto& slot:s.slots)if(slot.native.Get()==resource.Get()){active=&slot;break;}
        if(!active)for(auto& candidate:s.candidates)if(candidate.native.Get()==resource.Get() && GetTickCount64()-candidate.seen<250){
            for(auto& slot:s.slots)if(!slot.native){slot.native=candidate.native;slot.lastUsed=GetTickCount64();active=&slot;break;}break;
        }
        if(active)active->lastUsed=GetTickCount64();
        for(unsigned history=0;history<s.slots.size();++history){auto& slot=s.slots[history];if(slot.valid && slot.native.Get()==resource.Get()){
            D3D11_SHADER_RESOURCE_VIEW_DESC view{};raw[i]->GetDesc(&view);if(view.ViewDimension!=D3D11_SRV_DIMENSION_TEXTURE2D || view.Texture2D.MostDetailedMip!=0)break;
            raw[i]=slot.composed.view.Get();slot.lastUsed=GetTickCount64();changed=true;mappedHistory=history;++mappedCount;break;
        }}
    }
    if(changed)c->PSSetShaderResources(0,8,raw);
    if(mappedCount==1 && mappedHistory<16)
        FfxivPreviewFGMap::Capture(c,mappedHistory,s.slots[mappedHistory].motion.view.Get(),draw);
    draw();if(changed){for(UINT i=0;i<8;++i)raw[i]=saved[i].Get();c->PSSetShaderResources(0,8,raw);}
}
void Copy(ID3D11DeviceContext* c,ID3D11Resource* destination,ID3D11Resource* source,bool full){
    if(!full || privateDraw || managing || c->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE || !Config::Instance()->FfxivPreviewDLSS.value_or_default())return;
    auto& s=Get();std::lock_guard lock(s.mutex);if(!s.device || s.shutdown)return;
    for(auto& cycle:s.depthCycles)if(cycle.texture.Get()==source && GetTickCount64()-cycle.cleared<250){
        ComPtr<ID3D11Texture2D> t;if(SUCCEEDED(destination->QueryInterface(IID_PPV_ARGS(&t)))){D3D11_TEXTURE2D_DESC dd{};t->GetDesc(&dd);
            const int entry=FfxivCharaViewTrace::Match(FfxivCharaViewTrace::Current(),t.Get(),dd.Width,dd.Height);
            if(entry==18 || entry==19)s.depthCycles[entry-18]={t,cycle.cleared};}
        break;
    }
    bool current=false;
    for(auto& item:s.candidates)if(item.native.Get()==source && GetTickCount64()-item.seen<250){current=item.serial==s.serial;break;}
    if(!current)return; // Never use shared geometry after another preview has replaced it.
    ComPtr<ID3D11Texture2D> texture;if(FAILED(destination->QueryInterface(IID_PPV_ARGS(&texture))))return;
    D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);
    if(desc.Width!=s.width || desc.Height!=s.height || desc.Format!=DXGI_FORMAT_B8G8R8A8_UNORM || !(desc.BindFlags&D3D11_BIND_SHADER_RESOURCE))return;
    // Portraits may copy a common finishing target into separate UI textures.
    // Give each copied texture its own history; do not share the parent's NGX feature.
    Finish(s,c,texture.Get(),desc.Width,desc.Height);
}
void ClearDepth(ID3D11DeviceContext* c,ID3D11DepthStencilView* view,UINT flags){
    if(!view || !(flags&D3D11_CLEAR_DEPTH) || privateDraw || managing || FfxivPreviewMotion::replaying || c->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE || !Config::Instance()->FfxivPreviewDLSS.value_or_default())return;
    ComPtr<ID3D11Resource> resource;ComPtr<ID3D11Texture2D> texture;view->GetResource(&resource);if(FAILED(resource.As(&texture)))return;
    D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);
    const int entry=FfxivCharaViewTrace::Match(FfxivCharaViewTrace::Current(),texture.Get(),desc.Width,desc.Height);
    if(entry!=18 && entry!=19)return;
    auto& s=Get();std::lock_guard lock(s.mutex);if(s.shutdown)return;
    s.depthCycles[entry-18]={texture,GetTickCount64()};
    if(++s.depthClears==1)LOG_INFO("CharaView DLSS: verified native depth clear entry {}, {}x{}",entry,desc.Width,desc.Height);
}
void Tick(){
    auto& s=Get();std::lock_guard lock(s.mutex);if(!s.device || s.shutdown)return;
    ComPtr<ID3D11DeviceContext> c;s.device->GetImmediateContext(&c);
    Collect(s,c.Get(),GetTickCount64());FfxivPreviewFGMap::EndFrame();s.building=false;s.depthCycles={};++s.frame;
}
void Shutdown(){
    Guard guard;
    auto& s=Get();std::lock_guard lock(s.mutex);s.shutdown=true;
    if(s.device){ComPtr<ID3D11DeviceContext> c;s.device->GetImmediateContext(&c);for(auto& slot:s.slots)if(Ready(slot,c.Get()))Release(slot);}
    instances.store(0); // In-flight work stays owned until process teardown; never release busy NGX state.
}
} // namespace
