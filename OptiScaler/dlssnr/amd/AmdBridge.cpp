#include "pch.h"

#include "AmdBridge.h"
#include "AmdPreSr.h"
#include "AmdPrerequisites.h"

#include <Config.h>
#include <State.h>
#include <Util.h>
#include <wrl/client.h>
#include <misc/SkipSpoof.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>

namespace DlssNr::AmdBridge
{
namespace
{
std::atomic<AmdPreSr::Backend*> backend { nullptr };
std::mutex initMutex;
bool initializationFailed = false;
ID3D12Device* backendDevice = nullptr;
std::mutex frameMutex;
std::mutex messageMutex;
std::string message = "AMD pre-SR: waiting for a DirectX 12 SR frame";
std::string prerequisiteError;
std::atomic_bool amdHardware { false };

struct FrameIdentity
{
    UINT width = 0;
    UINT height = 0;
    float scale = 1.0f;
};

FrameIdentity lastFrame {};
UINT stableFrames = 0;
ULONGLONG settlingSince = 0;

std::filesystem::path Directory() { return Util::DllPath().parent_path(); }

void Message(const char* text)
{
    std::lock_guard lock(messageMutex);
    if (*text != '\0' && message != text)
    {
        std::ofstream log(Util::DllPath().parent_path() / L"amd_bridge.log", std::ios::app);
        log << GetTickCount64() << " thread=" << GetCurrentThreadId() << " " << text << '\n';
    }
    message = text;
}

void ReportPrerequisite(std::string error)
{
    std::lock_guard lock(messageMutex);
    if (prerequisiteError == error)
        return;

    prerequisiteError = std::move(error);
    if (!prerequisiteError.empty())
    {
        std::ofstream log(Directory() / L"amd_bridge.log", std::ios::app);
        log << GetTickCount64() << " thread=" << GetCurrentThreadId() << " " << prerequisiteError << '\n';
        LOG_ERROR("{}", prerequisiteError);
    }
}

ID3D12Resource* Resource(NVSDK_NGX_Parameter* parameters, const char* name)
{
    ID3D12Resource* resource = nullptr;
    if (parameters->Get(name, &resource) != NVSDK_NGX_Result_Success)
        parameters->Get(name, reinterpret_cast<void**>(&resource));
    return resource;
}

bool IsAmd(ID3D12Device* device)
{
    struct PhysicalAdapterScope
    {
        uint64_t id = SkipSpoof::AddEntry(SkipSpoofType::Thread);
        ~PhysicalAdapterScope() { SkipSpoof::RemoveEntry(id); }
    } physicalAdapterScope;

    Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) ||
        FAILED(factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&adapter))))
        return false;

    DXGI_ADAPTER_DESC1 desc {};
    const bool amd = SUCCEEDED(adapter->GetDesc1(&desc)) && desc.VendorId == 0x1002;
    LOG_INFO("AMD pre-SR physical adapter vendor: {:04X}, AMD: {}", desc.VendorId, amd);
    return amd;
}

AmdPreSr::Backend* BackendFor(ID3D12Device* device, ID3D12CommandQueue* queue)
{


    std::lock_guard lock(initMutex);
    if (auto* existing = backend.load(std::memory_order_relaxed))
    {
        if (device != backendDevice)
        {
            Message("AMD pre-SR: graphics device changed; restart required");
            return nullptr;
        }
        return existing;
    }
    if (initializationFailed) return nullptr;

    try
    {
        auto* created = new AmdPreSr::Backend(device, queue, Directory());
        backendDevice = device; // Backend holds the device reference.
        backend.store(created, std::memory_order_release);
        return created;
    }
    catch (const std::exception& error)
    {
        initializationFailed = true;
        Message(error.what());
        LOG_ERROR("AMD pre-SR initialization failed: {}", error.what());
        return nullptr;
    }
}
} // namespace

bool HasFiles()
{
    std::error_code ec;
    return std::filesystem::exists(Directory() / L"dlssnr_amd_pass1.dll", ec);
}

bool IsAmdDevice(ID3D12Device* device) { return device && IsAmd(device); }

bool CanUse(ID3D12Device* device)
{
    if (!device)
        return false;

    static std::mutex cacheMutex;
    static LUID cachedLuid {};
    static bool cached = false;
    static bool cachedResult = false;
    const LUID luid = device->GetAdapterLuid();
    std::lock_guard lock(cacheMutex);
    if (!cached || luid.HighPart != cachedLuid.HighPart || luid.LowPart != cachedLuid.LowPart)
    {
        cachedLuid = luid;
        cachedResult = IsAmd(device);
        amdHardware.store(cachedResult);
        cached = true;
    }
    if (!cachedResult)
    {
        ReportPrerequisite({});
        return false;
    }

    const auto* config = Config::Instance();
    ReportPrerequisite(config->DlssNrEnabled.value_or_default() &&
                               config->DlssNrRunBeforeSr.value_or_default()
                           ? MissingPrerequisite(Directory())
                           : std::string {});
    return HasFiles() && MissingPrerequisite(Directory()).empty();
}

std::string PrerequisiteError()
{
    std::lock_guard lock(messageMutex);
    return prerequisiteError;
}

ID3D12Resource* Prepare(ID3D12GraphicsCommandList* commandList, NVSDK_NGX_Parameter* parameters,
                        ID3D12CommandQueue* queue, unsigned int featureFlags, bool interop)
{
    std::lock_guard frameLock(frameMutex);
    if (!commandList || !parameters)
        return nullptr;

    Microsoft::WRL::ComPtr<ID3D12Device> device;
    if (FAILED(commandList->GetDevice(IID_PPV_ARGS(&device))) || !CanUse(device.Get()))
        return nullptr;

    if (!queue)
        queue = reinterpret_cast<ID3D12CommandQueue*>(State::Instance().currentCommandQueue);
    if (!queue)
    {
        Message("AMD pre-SR: waiting for the game command queue");
        return nullptr;
    }

    auto* owner = BackendFor(device.Get(), queue);
    if (!owner)
        return nullptr;

    AmdPreSr::Frame frame {};
    frame.colour = Resource(parameters, NVSDK_NGX_Parameter_Color);
    frame.motion = Resource(parameters, NVSDK_NGX_Parameter_MotionVectors);
    frame.depth = Resource(parameters, NVSDK_NGX_Parameter_Depth);
    frame.exposure = Resource(parameters, NVSDK_NGX_Parameter_ExposureTexture);
    if (!frame.colour || !frame.motion || !frame.depth)
    {
        Message("AMD pre-SR: missing colour, motion or depth input");
        return nullptr;
    }

    parameters->Get(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, &frame.preExposure);
    parameters->Get(NVSDK_NGX_Parameter_DLSS_Exposure_Scale, &frame.exposureScale);
    parameters->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, &frame.width);
    parameters->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, &frame.height);
    if (!frame.width) frame.width = static_cast<UINT>(frame.colour->GetDesc().Width);
    if (!frame.height) frame.height = frame.colour->GetDesc().Height;

    UINT colorX = 0, colorY = 0, reset = 0;
    parameters->Get(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X, &colorX);
    parameters->Get(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y, &colorY);
    if (colorX || colorY)
    {
        Message("AMD pre-SR: nonzero colour subrect origin unsupported");
        return nullptr;
    }

    if (!(featureFlags & NVSDK_NGX_DLSS_Feature_Flags_MVLowRes))
    {
        parameters->Get(NVSDK_NGX_Parameter_OutWidth, &frame.motionWidth);
        parameters->Get(NVSDK_NGX_Parameter_OutHeight, &frame.motionHeight);
        if (!frame.motionWidth) frame.motionWidth = static_cast<UINT>(frame.motion->GetDesc().Width);
        if (!frame.motionHeight) frame.motionHeight = frame.motion->GetDesc().Height;
    }

    const auto& config = *Config::Instance();
    const float scale = std::clamp(config.DlssNrWorkingScale.value_or_default(), 0.25f, 2.0f);
    const FrameIdentity current { frame.width, frame.height, scale };
    const auto now = GetTickCount64();
    if (current.width != lastFrame.width || current.height != lastFrame.height || current.scale != lastFrame.scale)
    {
        lastFrame = current;
        stableFrames = 0;
        settlingSince = now;
        owner->InvalidateHistory();
    }
    if (settlingSince == 0)
        settlingSince = now;
    if (now - settlingSince < 300 || (stableFrames < 2 && ++stableFrames < 2))
    {
        Message("AMD pre-SR: warming up after an upscaler or resolution change");
        return nullptr;
    }

    frame.depthInverted = (featureFlags & NVSDK_NGX_DLSS_Feature_Flags_DepthInverted) != 0;
    parameters->Get(NVSDK_NGX_Parameter_Reset, &reset);
    frame.reset = reset != 0;
    parameters->Get(NVSDK_NGX_Parameter_MV_Scale_X, &frame.motionScaleX);
    parameters->Get(NVSDK_NGX_Parameter_MV_Scale_Y, &frame.motionScaleY);

    const auto state = [interop](const auto& setting) {
        return !interop && setting.has_value() ? (D3D12_RESOURCE_STATES)setting.value()
                                               : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    };
    frame.colourState = state(config.ColorResourceBarrier);
    frame.depthState = state(config.DepthResourceBarrier);
    frame.motionState = state(config.MVResourceBarrier);
    frame.exposureState = state(config.ExposureResourceBarrier);

    AmdPreSr::Settings settings {};
    settings.modelScale = scale;
    settings.passes = std::clamp(config.DlssNrPasses.value_or_default(), 1u, 3u);
    settings.structure = config.DlssNrLocalStructure.value_or_default();
    settings.skin = config.DlssNrSkinStructure.value_or_default();
    if (settings.skin < 0.0f)
        settings.skin = settings.structure;

    auto* replacement = owner->Record(commandList, frame, settings);
    if (replacement)
        Message("");
    return replacement;
}

int PendingListIndex(UINT count, ID3D12CommandList* const* lists)
{
    if (auto* owner = backend.load(std::memory_order_acquire))
        return owner->PendingListIndex(count, lists);
    return -1;
}

void Submitting(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists)
{
    if (auto* owner = backend.load(std::memory_order_acquire))
        owner->Submitting(queue, count, lists);
}

void Submitted(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists)
{
    if (auto* owner = backend.load(std::memory_order_acquire))
        owner->Submitted(queue, count, lists);
}

void InvalidateHistory()
{
    if (auto* owner = backend.load(std::memory_order_acquire))
        owner->InvalidateHistory();
}

std::string Status()
{
    if (!amdHardware.load()) return {};
    {
        std::lock_guard lock(messageMutex);
        if (!message.empty())
            return message;
    }
    if (auto* owner = backend.load(std::memory_order_acquire))
        return owner->Status();
    return "AMD pre-SR: idle";
}
} // namespace DlssNr::AmdBridge
