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
        _dlssInitedDx12 = NVNGXProxy::InitDx12(Device);

        if (!_dlssInitedDx12)
            return false;

        _moduleLoaded =
            (NVNGXProxy::D3D12_Init_ProjectID() != nullptr || NVNGXProxy::D3D12_Init_Ext() != nullptr) &&
            (NVNGXProxy::D3D12_Shutdown() != nullptr || NVNGXProxy::D3D12_Shutdown1() != nullptr) &&
            (NVNGXProxy::D3D12_GetParameters() != nullptr || NVNGXProxy::D3D12_AllocateParameters() != nullptr) &&
            NVNGXProxy::D3D12_DestroyParameters() != nullptr && NVNGXProxy::D3D12_CreateFeature() != nullptr &&
            NVNGXProxy::D3D12_ReleaseFeature() != nullptr && NVNGXProxy::D3D12_EvaluateFeature() != nullptr;

        // delay between init and create feature
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    LOG_INFO("Creating DLSS feature");

    if (NVNGXProxy::D3D12_CreateFeature() != nullptr)
    {
        ProcessInitParams(InParameters);
        auto& cfg=*Config::Instance();
        _sceneHdrInput=DlssSceneInput::bridgeInitialising && cfg.FfxivHDR.value_or_default() && cfg.FfxivHDRMode.value_or_default()==1 && cfg.FfxivHDRDLSSSceneInput.value_or_default();
        unsigned int originalFlags=0;InParameters->Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,&originalFlags);
        if(_sceneHdrInput){unsigned int flags=originalFlags;
            InParameters->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,flags|NVSDK_NGX_DLSS_Feature_Flags_IsHDR|NVSDK_NGX_DLSS_Feature_Flags_AutoExposure);
            LOG_INFO("DLSS scene HDR input enabled: linear FP16 reconstruction, SDR output for ReShade");}


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
        auto* colour = colourBinding.original;
        auto* output = outputBinding.original;
        unsigned int reset=0;
        std::shared_ptr<DlssSceneInput::Packet> hdrPacket;
        if(_sceneHdrInput){
            InParameters->Get(NVSDK_NGX_Parameter_Reset,&reset);
            auto scene=FfxivSceneHdr::Open(Device,RenderWidth(),RenderHeight(),false);
            const char* failure="unknown";
            hdrPacket=DlssSceneInput::Prepare(Device,InCommandList,colour,output,RenderWidth(),RenderHeight(),std::move(scene),&failure);
            if(!hdrPacket){
                LOG_ERROR("DLSS scene HDR preparation failed: {}; render {}x{}. Recreating ordinary DLSS for this session.",failure,RenderWidth(),RenderHeight());
                for(auto* resource:{colour,output})if(resource){auto desc=resource->GetDesc();LOG_ERROR("DLSS scene HDR resource: {}x{}, format {}, flags {}, mips {}, samples {}, array {}",desc.Width,desc.Height,(UINT)desc.Format,(UINT)desc.Flags,desc.MipLevels,desc.SampleDesc.Count,desc.DepthOrArraySize);}
                Config::Instance()->FfxivHDRDLSSSceneInput.set_volatile_value(false);
                State::Instance().changeBackend[Handle()->Id]=true;
                return false;
            }
            const int available=hdrPacket->scene.hdr?1:0;
            if(available!=_sceneHdrAvailable){InParameters->Set(NVSDK_NGX_Parameter_Reset,1u);_sceneHdrAvailable=available;LOG_INFO("DLSS scene HDR: {}",available?"native HDR highlights entering DLSS":"linear SDR fallback (no same-frame scene)");}
            colourBinding.Set(hdrPacket->input.Get());
            outputBinding.Set(hdrPacket->output.Get());
            exposureBinding.Set(nullptr);
        }
        nvResult = NVNGXProxy::D3D12_EvaluateFeature()(InCommandList, _p_dlssHandle, InParameters, NULL);
        if(hdrPacket){
            colourBinding.Restore();
            outputBinding.Restore();
            exposureBinding.Restore();
            InParameters->Set(NVSDK_NGX_Parameter_Reset,reset);
            const bool success=nvResult==NVSDK_NGX_Result_Success;
            DlssSceneInput::Finish(hdrPacket,InCommandList,success);
            if(success)FfxivSceneHdr::PublishReconstructed(DlssSceneInput::Image(hdrPacket));
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
