#include <pch.h>
#include "DLSSFeature_Dx12.h"
#include <dxgi1_4.h>
#include <Config.h>
#include <misc/FfxivSceneHdr.h>
#include <misc/NgxResourceBinding.h>
#include <shaders/hdr/DlssSceneInput.h>

bool DLSSFeatureDx12::InitInternal(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters)
{
    if (IsInited())
        return true;

    return InitDLSS(InCommandList, InParameters);
}

bool DLSSFeatureDx12::InitDLSS(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters)
{
    if (NVNGXProxy::NVNGXModule() == nullptr)
    {
        LOG_ERROR("nvngx.dll not loaded!");
        return false;
    }

    if (!_dlssInitedDx12)
    {
        const bool runtimeAlreadyInitialized = NVNGXProxy::IsDx12Inited();
        _dlssInitedDx12 = NVNGXProxy::InitDx12(Device);

        if (!_dlssInitedDx12)
            return false;

        _moduleLoaded =
            (NVNGXProxy::D3D12_Init_ProjectID() != nullptr || NVNGXProxy::D3D12_Init_Ext() != nullptr) &&
            (NVNGXProxy::D3D12_Shutdown() != nullptr || NVNGXProxy::D3D12_Shutdown1() != nullptr) &&
            (NVNGXProxy::D3D12_GetParameters() != nullptr || NVNGXProxy::D3D12_AllocateParameters() != nullptr) &&
            NVNGXProxy::D3D12_DestroyParameters() != nullptr && NVNGXProxy::D3D12_CreateFeature() != nullptr &&
            NVNGXProxy::D3D12_ReleaseFeature() != nullptr && NVNGXProxy::D3D12_EvaluateFeature() != nullptr;

        // Preserve the initial runtime warm-up, but rebuilding a feature does
        // not reinitialize the shared runtime and needs no second fixed pause.
        if (!runtimeAlreadyInitialized)
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    LOG_INFO("Creating DLSS feature");

    if (NVNGXProxy::D3D12_CreateFeature() != nullptr)
    {
        ProcessInitParams(InParameters);
        _hdrHistory.reset();
        _sceneHdrAvailable=-1;
        _hdrStabilityState=-1;
        _hdrNrMode=-1;
        auto& cfg=*Config::Instance();
        _sceneHdrInput=DlssSceneInput::bridgeInitialising && cfg.FfxivHDR.value_or_default() && cfg.FfxivHDRMode.value_or_default()==1 && cfg.FfxivHDRDLSSSceneInput.value_or_default();
        unsigned int originalFlags=0;InParameters->Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,&originalFlags);
        if(_sceneHdrInput){unsigned int flags=originalFlags;
            InParameters->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,flags|NVSDK_NGX_DLSS_Feature_Flags_IsHDR|NVSDK_NGX_DLSS_Feature_Flags_AutoExposure);
            LOG_INFO("DLSS scene HDR input enabled: matched SDR reference normalization, linear FP16 reconstruction, SDR output for ReShade");}


        _p_dlssHandle = &_dlssHandle;

        NVSDK_NGX_Result nvResult;
        {
            ScopedSkipHeapCapture skipHeapCapture {};

            nvResult = NVNGXProxy::D3D12_CreateFeature()(InCommandList, NVSDK_NGX_Feature_SuperSampling, InParameters,
                                                         &_p_dlssHandle);
        }

        if(_sceneHdrInput)InParameters->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,originalFlags);
        if (nvResult != NVSDK_NGX_Result_Success)
        {
            LOG_ERROR("_CreateFeature result: {0:X}", (unsigned int) nvResult);
            return false;
        }
        else
        {
            LOG_INFO("_CreateFeature result: NVSDK_NGX_Result_Success");
        }
    }
    else
    {
        LOG_ERROR("_CreateFeature is nullptr");
        return false;
    }

    ReadVersion();

    SetInit(true);
    return true;
}

bool DLSSFeatureDx12::EvaluateInternal(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters)
{
    if (!_moduleLoaded)
    {
        LOG_ERROR("nvngx.dll or _nvngx.dll is not loaded!");
        return false;
    }

    NVSDK_NGX_Result nvResult;

    if (NVNGXProxy::D3D12_EvaluateFeature() != nullptr)
    {
        ProcessEvaluateParams(InParameters);

        NgxResourceBinding::Binding colourBinding(InParameters, NVSDK_NGX_Parameter_Color);
        NgxResourceBinding::Binding outputBinding(InParameters, NVSDK_NGX_Parameter_Output);
        NgxResourceBinding::Binding exposureBinding(InParameters, NVSDK_NGX_Parameter_ExposureTexture);
        NgxResourceBinding::Binding depthBinding(InParameters, NVSDK_NGX_Parameter_Depth);
        NgxResourceBinding::Binding motionBinding(InParameters, NVSDK_NGX_Parameter_MotionVectors);
        NgxResourceBinding::FloatBinding preExposure(InParameters,NVSDK_NGX_Parameter_DLSS_Pre_Exposure);
        NgxResourceBinding::FloatBinding exposureScale(InParameters,NVSDK_NGX_Parameter_DLSS_Exposure_Scale);
        auto* colour = colourBinding.original;
        auto* output = outputBinding.original;
        unsigned int reset=0;
        std::shared_ptr<DlssSceneInput::Packet> hdrPacket;
        // Retain the last image before Prepare selects a reusable work packet.
        auto hdrHistory=_sceneHdrInput?_hdrHistory.lock():nullptr;
        if(_sceneHdrInput){
            InParameters->Get(NVSDK_NGX_Parameter_Reset,&reset);
            const auto& cfg=*Config::Instance();
            const int nrMode=!cfg.DlssNrEnabled.value_or_default()?0:
                (cfg.DlssNrDeferredDlss.value_or_default()?2:cfg.DlssNrRunBeforeSr.value_or_default()?1:3) |
                (cfg.FfxivHDRNRSceneInput.value_or_default()?4:0);
            if(_hdrNrMode>=0&&_hdrNrMode!=nrMode){
                InParameters->Set(NVSDK_NGX_Parameter_Reset,1u);hdrHistory.reset();_hdrHistory.reset();
                LOG_INFO("DLSS HDR stability: NR path changed {} -> {}; resetting DLSS and clean HDR history",_hdrNrMode,nrMode);
            }
            _hdrNrMode=nrMode;
            auto scene=FfxivSceneHdr::Open(Device,RenderWidth(),RenderHeight(),false);
            const char* failure="unknown";
            hdrPacket=DlssSceneInput::Prepare(Device,InCommandList,colour,output,RenderWidth(),RenderHeight(),std::move(scene),&failure);
            if(!hdrPacket){
                LOG_ERROR("DLSS scene HDR preparation failed: {}; render {}x{}. Recreating ordinary DLSS for this session.",failure,RenderWidth(),RenderHeight());
                for(auto* resource:{colour,output})if(resource){auto desc=resource->GetDesc();LOG_ERROR("DLSS scene HDR resource: {}x{}, format {}, flags {}, mips {}, samples {}, array {}",desc.Width,desc.Height,(UINT)desc.Format,(UINT)desc.Flags,desc.MipLevels,desc.SampleDesc.Count,desc.DepthOrArraySize);}
                Config::Instance()->FfxivHDRDLSSSceneInput.set_volatile_value(false);
                _hdrHistory.reset();
                State::Instance().changeBackend[Handle()->Id]=true;
                return false;
            }
            const int available=hdrPacket->scene.hdr?1:0;
            if(available!=_sceneHdrAvailable){InParameters->Set(NVSDK_NGX_Parameter_Reset,1u);_sceneHdrAvailable=available;LOG_INFO("DLSS scene HDR: {}",available?"native HDR highlights entering DLSS":"linear SDR fallback (no same-frame scene)");}
            colourBinding.Set(hdrPacket->input.Get());
            outputBinding.Set(hdrPacket->output.Get());
            exposureBinding.Set(nullptr);
            // RenoDX captures are already exposed. Native exposure must not be applied a second time.
            preExposure.Set(1.0f);exposureScale.Set(1.0f);
        }
        nvResult = NVNGXProxy::D3D12_EvaluateFeature()(InCommandList, _p_dlssHandle, InParameters, NULL);
        if(hdrPacket){
            const bool success=nvResult==NVSDK_NGX_Result_Success;
            if(success){
                unsigned effectiveReset=0;InParameters->Get(NVSDK_NGX_Parameter_Reset,&effectiveReset);
                float mx=0,my=0,jx=0,jy=0;
                InParameters->Get(NVSDK_NGX_Parameter_MV_Scale_X,&mx);InParameters->Get(NVSDK_NGX_Parameter_MV_Scale_Y,&my);
                InParameters->Get(NVSDK_NGX_Parameter_Jitter_Offset_X,&jx);InParameters->Get(NVSDK_NGX_Parameter_Jitter_Offset_Y,&jy);
                if(effectiveReset || (hdrHistory && GetTickCount64()-hdrHistory->lastUse>250))hdrHistory.reset();
                DlssSceneInput::Stabilize(hdrPacket,InCommandList,std::move(hdrHistory),depthBinding.original,motionBinding.original,mx,my,jx,jy,LowResMV()&&!JitteredMV(),Config::Instance()->FfxivHDRAdaptiveStabilizer.value_or_default());
                const int stabilityState=hdrPacket->hasDepth?1:0;
                if(stabilityState!=_hdrStabilityState){
                    _hdrStabilityState=stabilityState;
                    LOG_DEBUG("DLSS HDR temporal stability: {}",stabilityState?"compatible guides available":"unsupported guide/output layout");
                }
            }
            colourBinding.Restore();
            outputBinding.Restore();
            exposureBinding.Restore();
            preExposure.Restore();exposureScale.Restore();
            InParameters->Set(NVSDK_NGX_Parameter_Reset,reset);
            DlssSceneInput::Finish(hdrPacket,InCommandList,success);
            if(success){
                _hdrHistory=hdrPacket;
                DlssSceneInput::bridgeHdrShoulder=bool(hdrPacket->scene.hdr);
                FfxivSceneHdr::PublishReconstructed(DlssSceneInput::Image(hdrPacket));
            }else _hdrHistory.reset();
        }

        if (nvResult != NVSDK_NGX_Result_Success)
        {
            LOG_ERROR("_EvaluateFeature result: {0:X}", (unsigned int) nvResult);
            return false;
        }
    }
    else
    {
        LOG_ERROR("_EvaluateFeature is nullptr");
        return false;
    }

    _frameCount++;

    return true;
}

void DLSSFeatureDx12::Shutdown(ID3D12Device* InDevice)
{
    if (_dlssInitedDx12)
    {
        if (NVNGXProxy::D3D12_Shutdown() != nullptr)
            NVNGXProxy::D3D12_Shutdown()();
        else if (NVNGXProxy::D3D12_Shutdown1() != nullptr)
            NVNGXProxy::D3D12_Shutdown1()(InDevice);
    }

    DLSSFeature::Shutdown();
}

DLSSFeatureDx12::DLSSFeatureDx12(unsigned int InHandleId, NVSDK_NGX_Parameter* InParameters)
    : IFeature(InHandleId, InParameters), IFeature_Dx12(InHandleId, InParameters), DLSSFeature(InHandleId, InParameters)
{
    if (NVNGXProxy::NVNGXModule() == nullptr)
    {
        LOG_INFO("nvngx.dll not loaded, now loading");
        NVNGXProxy::InitNVNGX();
    }

    LOG_INFO("binding complete!");
}

DLSSFeatureDx12::~DLSSFeatureDx12()
{
    if (State::Instance().isShuttingDown)
        return;

    if (NVNGXProxy::D3D12_ReleaseFeature() != nullptr && _p_dlssHandle != nullptr)
        NVNGXProxy::D3D12_ReleaseFeature()(_p_dlssHandle);
}
