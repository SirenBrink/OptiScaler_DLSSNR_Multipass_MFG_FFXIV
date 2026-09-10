#include <pch.h>
#include <Config.h>
#include <NVNGX_Parameter.h>
#include "IFeature.h"
#include <detours/detours.h>
#include <atomic>
#include <misc/FfxivNativeQuality.h>
#include <TlHelp32.h>

void IFeature::SetHandle(unsigned int InHandleId)
{
    _handle = new NVSDK_NGX_Handle { InHandleId };
    LOG_INFO("Handle: {0}", _handle->Id);
}

// Neural Rendering between the halves of the upscaler: the upscaler writes at render resolution and
// the enlargement to display resolution becomes a later stage, with the model between the two. For ray
// reconstruction that makes the feature a denoiser and nothing else, which is the point -- the frame
// handed to the model is clean, temporally settled, and a ninth of the pixels at Ultra Performance.
//
// Only where there is something to split. At render == display the upscaler is already 1:1 and the
// model would run on the frame it runs on today, at the cost it costs today.
bool IFeature::DualFeatureSplit() const
{
    return !_isEnlargementStage && _splitOutputWidth > 0 && _splitOutputHeight > 0;
}

// Not one virtual call in here, deliberately.
//
// Several upscalers call SetInitParameters from a constructor -- XeSSFeature, FFXFeature and
// FSR31Feature all do -- and at that moment the object is only built as far as that class. Name() goes
// through GetUpscalerType(), which those classes leave pure and only their leaves define, so calling it
// there is a pure virtual call and the process is gone. DLSSFeature happens to define its own, which is
// why an identity built this way survived DLSS and killed XeSS the moment it was used as an enlarger.
//
// So this reads members only. The name is logged separately from Init, where the object is whole.
// ---------------------------------------------------------------------------------------------
// FFXIV probe -- read only, and only in FINAL FANTASY XIV.
//
// FUN_1403746e0 in ffxiv_dx11.exe writes the DLSS render size, the
// PerfQualityValue and the render preset hint. It takes the quality from a single byte in the game's
// graphics settings block:
//
//     if (((*(char *)(SETTINGS + 0x54) == 2) || (*(char *)(SETTINGS + 0x44) != 0)) &&
//         (*(char *)(SETTINGS + 0x45) != 0))
//         switch (*(byte *)(SETTINGS + 0x55)) { ... }        // else DLAA
//
// and the render size from either the quality table or, when a flag on the DLSS context is set,
// display multiplied by the float at SETTINGS + 0x4c -- the manual 3D resolution scale.
//
// Verified against executable MD5 ca3fe5c8673fe54d1961d856ade51fcc.
// SETTINGS is a pointer held in a global at image base + 0x28f6470.
//
// This reads those five fields and reports them when they change. Nothing is written. The point is to
// compare the live state with the game configuration before adding a quality override.
// Matching instruction bytes are required before using this build-specific layout.
// ---------------------------------------------------------------------------------------------
namespace
{
constexpr uintptr_t kFfxivSettingsPointerRva = 0x28f6470;

bool ReadFfxivMemory(const void* address, void* output, size_t bytes)
{
    SIZE_T copied = 0;
    return ReadProcessMemory(GetCurrentProcess(), address, output, bytes, &copied) && copied == bytes;
}

bool MatchesFfxivSettingsInstructions(uintptr_t base)
{
    // RIP-relative settings load, then gates +0x54/+0x44/+0x45 and selector +0x55.
    // Includes the displacement to the expected global; fail closed after an incompatible update.
    constexpr unsigned char expected[] = {
        0x4c, 0x8b, 0x05, 0x42, 0x1d, 0x58, 0x02, 0x45, 0x33, 0xe4,
        0x41, 0x80, 0x78, 0x54, 0x02, 0x74, 0x06, 0x45, 0x38, 0x60,
        0x44, 0x74, 0x71, 0x45, 0x38, 0x60, 0x45, 0x74, 0x6b,
        0x41, 0x0f, 0xb6, 0x40, 0x55, 0x83, 0xf8, 0x06
    };
    unsigned char actual[sizeof(expected)] {};
    return base != 0 && ReadFfxivMemory((const void*) (base + 0x374727), actual, sizeof(actual)) &&
           memcmp(actual, expected, sizeof(expected)) == 0;
}

std::atomic<bool> nativeQualityAvailable {false};
std::atomic<int> nativeRequestedQuality {-1};
std::atomic<unsigned int> nativeRequestSequence {0};
using FfxivRendererUpdate = void (*)(uintptr_t, float);
using FfxivResizeCallbacks = unsigned char (*)(uintptr_t);
FfxivRendererUpdate originalRendererUpdate = nullptr;
FfxivResizeCallbacks dispatchResizeCallbacks = nullptr;

void ProcessFfxivQualityRequest(uintptr_t renderer)
{
    static unsigned int handled = 0;
    const auto sequence = nativeRequestSequence.load();
    if (sequence == handled) return;
    const auto base = (uintptr_t) GetModuleHandleW(nullptr);
    uintptr_t settings = 0, manager = 0, context = 0, device = 0, callbacks = 0;
    unsigned char type = 0, flags = 0;
    if (!ReadFfxivMemory((void*) (base + 0x28f6470), &settings, sizeof(settings)) || settings == 0 ||
        !ReadFfxivMemory((void*) (settings + 0x54), &type, sizeof(type)) || type != 2 ||
        !ReadFfxivMemory((void*) (base + 0x28f6498), &manager, sizeof(manager)) || manager == 0 ||
        !ReadFfxivMemory((void*) (manager + 0x4220), &context, sizeof(context)) || context == 0 ||
        !ReadFfxivMemory((void*) (context + 0x181), &flags, sizeof(flags)) ||
        (flags & 1) == 0 || (flags & 0x30) != 0 ||
        !ReadFfxivMemory((void*) (base + 0x28efd00), &device, sizeof(device)) || device == 0 ||
        !ReadFfxivMemory((void*) (device + 0x30), &callbacks, sizeof(callbacks)) || callbacks == 0)
        return;

    // Invalidate only the quality table's display-size cache. The native callback then queries
    // NGX again and propagates the result to rendering textures on the game's update thread.
    const unsigned long long invalidDisplay = 0;
    SIZE_T written = 0;
    if (!WriteProcessMemory(GetCurrentProcess(), (void*) (context + 0x150), &invalidDisplay,
                            sizeof(invalidDisplay), &written) || written != sizeof(invalidDisplay))
        return;
    const auto result = dispatchResizeCallbacks(callbacks);
    handled = sequence;
    unsigned int size[2] {};
    ReadFfxivMemory((void*) (renderer + 0x428), size, sizeof(size));
    LOG_INFO("FFXIV native quality: request={} preset={} callbacks={} scene={}x{} thread={}",
             sequence, nativeRequestedQuality.load(), result, size[0], size[1], GetCurrentThreadId());
}

void ObserveFfxivRendererUpdate(uintptr_t renderer, float elapsed)
{
    ProcessFfxivQualityRequest(renderer);
    originalRendererUpdate(renderer, elapsed);
}


using FfxivSelectSize = void (*)(uintptr_t, unsigned int*, unsigned char);
using FfxivQuerySize = uintptr_t (*)(const unsigned int*, unsigned int*, unsigned int*, unsigned int*);
FfxivSelectSize originalSelectSize = nullptr;
FfxivQuerySize originalQuerySize = nullptr;
std::atomic<unsigned long long> sceneSelections {0};
struct FfxivSceneObservation
{
    bool called = false;
    bool succeeded = false;
    unsigned int optimal[2] {}, minimum[2] {}, maximum[2] {};
};
thread_local FfxivSceneObservation* activeSceneObservation = nullptr;

uintptr_t ObserveFfxivQuery(const unsigned int* display, unsigned int* optimal,
                          unsigned int* minimum, unsigned int* maximum)
{
    const auto result = originalQuerySize(display, optimal, minimum, maximum);
    // Constrain the native scene selector, rather than just NGX feature-creation metadata.
    const int requested = nativeRequestedQuality.load();
    if ((result & 0xff) != 0 && requested >= 0 && requested <= 5)
    {
        const auto answer = LastQualityAnswer();
        unsigned int target[2] {}, output[2] {};
        if (ReadFfxivMemory(optimal, target, sizeof(target)) && ReadFfxivMemory(display, output, sizeof(output)) &&
            answer.quality == requested && answer.renderWidth == target[0] && answer.renderHeight == target[1] &&
            target[0] > 0 && target[1] > 0 && target[0] <= output[0] && target[1] <= output[1])
        {
            memcpy(minimum, target, sizeof(target));
            memcpy(maximum, target, sizeof(target));
        }
    }
    if (activeSceneObservation != nullptr)
    {
        auto& observation = *activeSceneObservation;
        observation.called = true;
        observation.succeeded = (result & 0xff) != 0;
        if (observation.succeeded)
        {
            ReadFfxivMemory(optimal, observation.optimal, sizeof(observation.optimal));
            ReadFfxivMemory(minimum, observation.minimum, sizeof(observation.minimum));
            ReadFfxivMemory(maximum, observation.maximum, sizeof(observation.maximum));
        }
    }
    return result;
}

void ObserveFfxivSelection(uintptr_t renderer, unsigned int* size, unsigned char mode)
{
    unsigned int before[2] {}, after[2] {};
    const bool beforeValid = ReadFfxivMemory(size, before, sizeof(before));
    FfxivSceneObservation observation;
    auto* previous = activeSceneObservation;
    activeSceneObservation = &observation;
    originalSelectSize(renderer, size, mode);
    activeSceneObservation = previous;
    const auto count = sceneSelections.fetch_add(1) + 1;
    const bool afterValid = ReadFfxivMemory(size, after, sizeof(after));
    // Bound logging even if the game invokes this every frame.
    if (count > 100 && count % 300 != 0)
        return;
    unsigned short heights[4] {};
    ReadFfxivMemory((void*) (renderer + 0x700), heights, sizeof(heights));
    try
    {
        LOG_INFO("FFXIV scene select #{} thread={} mode={} readable={}/{}: {}x{} -> {}x{}; "
                 "query called={} success={} optimal={}x{} min={}x{} max={}x{}; heights applied/current/max/min={}/{}/{}/{}",
                 count, GetCurrentThreadId(), mode, beforeValid, afterValid, before[0], before[1], after[0], after[1],
                 observation.called, observation.succeeded, observation.optimal[0], observation.optimal[1],
                 observation.minimum[0], observation.minimum[1], observation.maximum[0], observation.maximum[1],
                 heights[0], heights[1], heights[2], heights[3]);
    }
    catch (...) {} // Logging must not escape into the game's native callback.
}

void InstallFfxivSceneDiagnostic(uintptr_t base)
{
    static std::once_flag once;
    std::call_once(once, [base]() {
        constexpr unsigned char selectBytes[] = {0x48,0x89,0x5c,0x24,0x18,0x55,0x56,0x57,
            0x41,0x56,0x41,0x57,0x48,0x83,0xec,0x20,0x8b,0x42,0x04,0x0f,0x57,0xc0,0x48,0x8b,0xd9};
        constexpr unsigned char queryBytes[] = {0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x74,0x24,0x10,
            0x48,0x89,0x7c,0x24,0x18,0x41,0x56,0x48,0x83,0xec,0x20,0x48,0x8b,0x05,0xdc,0x3f,0x5a,0x02};
        unsigned char actualSelect[sizeof(selectBytes)] {}, actualQuery[sizeof(queryBytes)] {};
        if (!ReadFfxivMemory((void*) (base + 0x2db890), actualSelect, sizeof(actualSelect)) ||
            !ReadFfxivMemory((void*) (base + 0x3524a0), actualQuery, sizeof(actualQuery)) ||
            memcmp(actualSelect, selectBytes, sizeof(selectBytes)) != 0 ||
            memcmp(actualQuery, queryBytes, sizeof(queryBytes)) != 0)
        {
            LOG_WARN("FFXIV scene diagnostic: instruction mismatch; hooks disabled");
            return;
        }
        constexpr unsigned char updateBytes[] = {0x4c,0x8b,0xdc,0x57,0x48,0x81,0xec,0xd0,0x00,0x00,0x00,0x48,0x8b,0x05,0xd6,0x94,0x5f,0x02};
        constexpr unsigned char dispatchBytes[] = {0x48,0x89,0x5c,0x24,0x10,0x48,0x89,0x6c,0x24,0x18,0x48,0x89,0x74,0x24,0x20,0x57,0x48,0x83,0xec,0x20};
        unsigned char updateActual[sizeof(updateBytes)] {}, dispatchActual[sizeof(dispatchBytes)] {};
        if (!ReadFfxivMemory((void*) (base + 0x2da790), updateActual, sizeof(updateActual)) ||
            !ReadFfxivMemory((void*) (base + 0x236ca0), dispatchActual, sizeof(dispatchActual)) ||
            memcmp(updateActual, updateBytes, sizeof(updateBytes)) != 0 ||
            memcmp(dispatchActual, dispatchBytes, sizeof(dispatchBytes)) != 0)
        {
            LOG_WARN("FFXIV native quality: update instruction mismatch; hooks disabled");
            return;
        }
        originalRendererUpdate = (FfxivRendererUpdate) (base + 0x2da790);
        dispatchResizeCallbacks = (FfxivResizeCallbacks) (base + 0x236ca0);
        originalSelectSize = (FfxivSelectSize) (base + 0x2db890);
        originalQuerySize = (FfxivQuerySize) (base + 0x3524a0);
        // Enlist existing game threads so Detours can relocate any instruction pointer in a patched prologue.
        std::vector<HANDLE> threads;
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE) return;
        THREADENTRY32 entry {};
        entry.dwSize = sizeof(entry);
        bool readable = Thread32First(snapshot, &entry) != FALSE;
        bool threadsReady = readable;
        while (readable)
        {
            if (entry.th32OwnerProcessID == GetCurrentProcessId() && entry.th32ThreadID != GetCurrentThreadId())
            {
                HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, entry.th32ThreadID);
                if (thread != nullptr) threads.push_back(thread);
                else if (GetLastError() != ERROR_INVALID_PARAMETER) threadsReady = false;
            }
            readable = Thread32Next(snapshot, &entry) != FALSE;
        }
        CloseHandle(snapshot);
        if (!threadsReady)
        {
            for (auto thread : threads) CloseHandle(thread);
            LOG_WARN("FFXIV scene diagnostic: unable to enlist game threads; hooks disabled");
            return;
        }
        LONG result = DetourTransactionBegin();
        if (result == NO_ERROR)
        {
            result = DetourUpdateThread(GetCurrentThread());
            for (auto thread : threads)
            {
                if (result != NO_ERROR) break;
                result = DetourUpdateThread(thread);
            }
            if (result == NO_ERROR)
                result = DetourAttach(&(PVOID&) originalSelectSize, ObserveFfxivSelection);
            if (result == NO_ERROR)
                result = DetourAttach(&(PVOID&) originalQuerySize, ObserveFfxivQuery);
            if (result == NO_ERROR)
                result = DetourAttach(&(PVOID&) originalRendererUpdate, ObserveFfxivRendererUpdate);
            if (result == NO_ERROR)
                result = DetourTransactionCommit();
            else
                DetourTransactionAbort();
        }
        for (auto thread : threads) CloseHandle(thread);
        if (result == NO_ERROR)
        {
            nativeQualityAvailable.store(true);
            const int forced = Config::Instance()->ForcePerfQuality.value_or_default();
            if (forced >= 0 && forced <= 5) FfxivNativeQuality::Request(forced);
        }
        LOG_INFO("FFXIV native quality v2: hook transaction result={} (0=installed); native scene updates enabled", result);
    });
}

void ReportFfxivSceneSnapshot(uintptr_t base)
{
    // Called under the settings-probe mutex. Limit snapshots to once per two seconds.
    static ULONGLONG next = 0;
    const auto now = GetTickCount64();
    if (now < next) return;
    next = now + 2000;
    uintptr_t renderer = 0;
    unsigned int scene[2] {};
    unsigned short heights[4] {};
    if (!ReadFfxivMemory((void*) (base + 0x28f6490), &renderer, sizeof(renderer)) || renderer == 0 ||
        !ReadFfxivMemory((void*) (renderer + 0x428), scene, sizeof(scene)) ||
        !ReadFfxivMemory((void*) (renderer + 0x700), heights, sizeof(heights))) return;
    LOG_INFO("FFXIV scene snapshot: selections={} scene={}x{} heights applied/current/max/min={}/{}/{}/{}",
             sceneSelections.load(), scene[0], scene[1], heights[0], heights[1], heights[2], heights[3]);
}


const char* FfxivQualityName(unsigned char value)
{
    switch (value)
    {
    case 0:
        return "Auto (chosen from pixel count)";
    case 1:
        return "DLAA -> NGX 5";
    case 2:
        return "Ultra Quality -> NGX 4";
    case 3:
        return "Quality -> NGX 2";
    case 4:
        return "Balanced -> NGX 1";
    case 5:
        return "Performance -> NGX 0";
    case 6:
        return "Ultra Performance -> NGX 3";
    default:
        return "not a value this build maps";
    }
}

void ReportFfxivQualitySetting()
{
    if (State::Instance().gameExe != "ffxiv_dx11.exe")
        return;

    static std::mutex probeMutex;
    const std::lock_guard<std::mutex> lock(probeMutex);
    const auto base = (uintptr_t) GetModuleHandleW(nullptr);
    static const bool matchingInstructions = MatchesFfxivSettingsInstructions(base);
    if (!matchingInstructions)
    {
        static bool reported = false;
        if (!reported)
        {
            LOG_WARN("FFXIV probe: unsupported executable instructions; settings read disabled");
            reported = true;
        }
        return;
    }

    InstallFfxivSceneDiagnostic(base);
    ReportFfxivSceneSnapshot(base);

    // Resolve the pointer and copy a fresh snapshot on every call. A settings reload can replace
    // or free the previous block; ReadProcessMemory fails safely if either read becomes inaccessible.
    static const unsigned char* lastSettings = nullptr;
    static unsigned int attempts = 0;
    static bool gaveUp = false;
    static bool everReported = false;

    if (gaveUp)
        return;

    const unsigned char* settings = nullptr;
    unsigned char snapshot[0x56] {};
    const bool validSnapshot =
        ReadFfxivMemory((const void*) (base + kFfxivSettingsPointerRva), &settings, sizeof(settings)) &&
        settings != nullptr && ReadFfxivMemory(settings, snapshot, sizeof(snapshot)) &&
        snapshot[0x54] <= 8 && snapshot[0x55] <= 6 && snapshot[0x44] <= 1 && snapshot[0x45] <= 1;

    // Bound consecutive failures independently of the game's frame rate.
    constexpr unsigned int kMaxAttempts = 1200;

    if (!validSnapshot)
    {
        ++attempts;
        everReported = false;
        if (attempts == 1 || attempts == kMaxAttempts)
            LOG_WARN("FFXIV probe: attempt {} of {} -- no readable, plausible settings snapshot at base+{:#x}",
                     attempts, kMaxAttempts, kFfxivSettingsPointerRva);
        if (attempts >= kMaxAttempts)
        {
            LOG_WARN("FFXIV probe: settings unavailable after {} attempts; disabling diagnostic", attempts);
            gaveUp = true;
        }
        return;
    }

    if (settings != lastSettings)
    {
        LOG_INFO("FFXIV probe: verified settings instructions; graphics settings block at {:#x}, "
                 "pointer RVA {:#x}, module base {:#x}", (uintptr_t) settings, kFfxivSettingsPointerRva, base);
        lastSettings = settings;
        everReported = false;
    }
    attempts = 0;

    const unsigned char gateA = snapshot[0x54];
    const unsigned char gateB = snapshot[0x44];
    const unsigned char gateC = snapshot[0x45];
    const unsigned char quality = snapshot[0x55];

    float scale = 0.0f;
    memcpy(&scale, snapshot + 0x4c, sizeof(scale));

    static unsigned char lastGateA = 0, lastGateB = 0, lastGateC = 0, lastQuality = 0;
    static float lastScale = 0.0f;

    if (everReported && gateA == lastGateA && gateB == lastGateB && gateC == lastGateC && quality == lastQuality &&
        scale == lastScale)
        return;

    everReported = true;
    lastGateA = gateA;
    lastGateB = gateB;
    lastGateC = gateC;
    lastQuality = quality;
    lastScale = scale;

    const bool gatePasses = (gateA == 2 || gateB != 0) && gateC != 0;

    LOG_INFO("FFXIV probe: quality byte +0x55 = {} ({}), 3D resolution scale +0x4c = {:.4f}, gates +0x54={} "
             "+0x44={} +0x45={} -> {}",
             quality, FfxivQualityName(quality), scale, gateA, gateB, gateC,
             gatePasses ? "the game reads the quality byte" : "the game forces DLAA");
}
} // namespace

bool FfxivNativeQuality::Available() { return nativeQualityAvailable.load(); }
void FfxivNativeQuality::Request(int quality)
{
    nativeRequestedQuality.store(quality >= 0 && quality <= 5 ? quality : -1);
    nativeRequestSequence.fetch_add(1);
}

std::string IFeature::FeatureIdentity() const
{
    return std::format("#{} [{}] render {}x{} display {}x{} target {}x{}{}",
                       _handle != nullptr ? _handle->Id : 0u,
                       _isEnlargementStage ? "enlargement half" : "upscaler", _renderWidth, _renderHeight,
                       _displayWidth, _displayHeight, _targetWidth, _targetHeight,
                       DualFeatureSplit() ? " split" : "");
}

bool IFeature::SetInitParameters(NVSDK_NGX_Parameter* InParameters)
{
    ReportFfxivQualitySetting();

    unsigned int width = 0;
    unsigned int outWidth = 0;
    unsigned int height = 0;
    unsigned int outHeight = 0;
    int pqValue = 0;

    if (InParameters->Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, &_featureFlags) == NVSDK_NGX_Result_Success)
    {
        if (Config::Instance()->HDR.has_value())
        {
            LOG_INFO("HDR flag overrided by user: {}", Config::Instance()->HDR.value());
            _initFlags.IsHdr = Config::Instance()->HDR.value();
        }
        else
        {
            _initFlags.IsHdr = _featureFlags & NVSDK_NGX_DLSS_Feature_Flags_IsHDR;
        }

        if (Config::Instance()->OverrideSharpness.has_value())
        {
            LOG_INFO("SharpenEnabled flag overrided by user: {}", Config::Instance()->OverrideSharpness.value());
            _initFlags.SharpenEnabled = Config::Instance()->OverrideSharpness.value();
        }
        else
        {
            _initFlags.SharpenEnabled = _featureFlags & NVSDK_NGX_DLSS_Feature_Flags_DoSharpening;
        }

        if (Config::Instance()->DepthInverted.has_value())
        {
            LOG_INFO("DepthInverted flag overrided by user: {}", Config::Instance()->DepthInverted.value());
            _initFlags.DepthInverted = Config::Instance()->DepthInverted.value();
        }
        else
        {
            _initFlags.DepthInverted = _featureFlags & NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
        }

        if (Config::Instance()->JitterCancellation.has_value())
        {
            LOG_INFO("JitteredMV flag overrided by user: {}", Config::Instance()->JitterCancellation.value());
            _initFlags.JitteredMV = Config::Instance()->JitterCancellation.value();
        }
        else
        {
            _initFlags.JitteredMV = _featureFlags & NVSDK_NGX_DLSS_Feature_Flags_MVJittered;
        }

        if (Config::Instance()->DisplayResolution.has_value())
        {
            LOG_INFO("LowResMV flag overrided by user: {}", !Config::Instance()->DisplayResolution.value());
            _initFlags.LowResMV = !Config::Instance()->DisplayResolution.value();
        }
        else
        {
            _initFlags.LowResMV = _featureFlags & NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
        }

        // First check state to prevent upscaler re-init loops
        if (State::Instance().autoExposure.has_value())
        {
            LOG_INFO("AutoExposure flag overrided by OptiScaler: {}", State::Instance().autoExposure.value());
            _initFlags.AutoExposure = State::Instance().autoExposure.value();
        }
        else if (Config::Instance()->AutoExposure.has_value())
        {
            LOG_INFO("AutoExposure flag overrided by user: {}", Config::Instance()->AutoExposure.value());
            _initFlags.AutoExposure = Config::Instance()->AutoExposure.value();
        }
        else
        {
            _initFlags.AutoExposure = _featureFlags & NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
        }

        LOG_INFO("Init Flag AutoExposure: {}", _initFlags.AutoExposure);
        LOG_INFO("Init Flag DepthInverted: {}", _initFlags.DepthInverted);
        LOG_INFO("Init Flag IsHdr: {}", _initFlags.IsHdr);
        LOG_INFO("Init Flag JitteredMV: {}", _initFlags.JitteredMV);
        LOG_INFO("Init Flag LowResMV: {}", _initFlags.LowResMV);
        LOG_INFO("Init Flag SharpenEnabled: {}", _initFlags.SharpenEnabled);

        if (State::Instance().activeFgInput == FGInput::Upscaler)
        {
            Config::Instance()->FGXeFGDepthInverted = _initFlags.DepthInverted;
            Config::Instance()->FGXeFGJitteredMV = _initFlags.JitteredMV;
            Config::Instance()->FGXeFGHighResMV = !_initFlags.LowResMV;
            LOG_DEBUG("XeFG DepthInverted: {}", Config::Instance()->FGXeFGDepthInverted.value_or_default());
            LOG_DEBUG("XeFG JitteredMV: {}", Config::Instance()->FGXeFGJitteredMV.value_or_default());
            LOG_DEBUG("XeFG HighResMV: {}", Config::Instance()->FGXeFGHighResMV.value_or_default());
            Config::Instance()->SaveXeFG();
        }
    }

    if (InParameters->Get(NVSDK_NGX_Parameter_OutWidth, &outWidth) == NVSDK_NGX_Result_Success &&
        InParameters->Get(NVSDK_NGX_Parameter_OutHeight, &outHeight) == NVSDK_NGX_Result_Success)
    {
        InParameters->Get(NVSDK_NGX_Parameter_Width, &width);
        InParameters->Get(NVSDK_NGX_Parameter_Height, &height);
        InParameters->Get(NVSDK_NGX_Parameter_PerfQualityValue, &pqValue);

        // The same substitution the optimal-settings query made, repeated where the feature is
        // actually built.
        //
        // The query moved the render resolution; this moves the label that travels with it. Leaving
        // them apart creates a feature that declares DLAA while being handed a Balanced-sized render
        // target, and a mismatched pair is what the runtime rejects -- so the two have to agree.
        //
        // They only agree when this create descends from that query, which is not every create. A
        // game may rebuild the feature from dimensions it decided earlier, without asking again:
        // FFXIV recomputes its per-quality table only when the display size changes, yet recreates
        // the feature at several other moments -- leaving group pose, changing a character's
        // appearance. Those recreates carry the previous table's dimensions. Forcing a different
        // quality into one of them hands the runtime a render target sized for one preset and a
        // PerfQualityValue naming another; it answers BAD00005, the feature is gone for the rest of
        // the session, and all the overlay can report is that the upscaler is not in use.
        //
        // So the override applies to a create the last answer accounts for, and otherwise stands
        // down. Standing down costs a preset change that lands late -- it takes effect the next time
        // the game asks -- which is the smaller loss by a wide margin.
        if (const int forcedPq = Config::Instance()->ForcePerfQuality.value_or_default();
            forcedPq >= 0 && forcedPq <= (int) NVSDK_NGX_PerfQuality_Value_DLAA && forcedPq != pqValue)
        {
            const auto answer = LastQualityAnswer();

            // No query yet means no answer to contradict. Games that never ask for optimal settings
            // reach this path with nothing else setting the quality, so the override is all there is.
            const bool nothingToCheck = answer.queries == 0;

            // Near, not equal.
            //
            // Exact equality assumes the game creates the feature at precisely the size we answered.
            // Plenty round it first, to a multiple of 8 or 16, and the answer is far more often odd
            // than 16:9 testing suggests: at 3440x1440 three of the six presets land on an odd width,
            // at 5120x1440 the same, and even 4K is odd at Ultra Quality. A game that rounds 2023 to
            // 2024 would fail an exact match, the override would quietly never apply, and the setting
            // would look broken on exactly the displays least likely to be tested.
            //
            // The mismatch this guard exists to catch is a stale render size -- a whole preset away,
            // hundreds of pixels. Thirty-two absorbs any rounding without coming close to that.
            constexpr unsigned int kRoundingSlack = 32;

            // Not named "near": windef.h still defines that away to nothing for the 16-bit memory
            // models, so the declaration becomes "const auto = ..." and the compiler asks what
            // variable you meant.
            const auto withinSlack = [](unsigned int a, unsigned int b)
            { return (a > b ? a - b : b - a) <= kRoundingSlack; };

            const bool matchesLastAnswer =
                withinSlack(answer.renderWidth, width) && withinSlack(answer.renderHeight, height) &&
                withinSlack(answer.displayWidth, outWidth) && withinSlack(answer.displayHeight, outHeight);

            if (nothingToCheck || matchesLastAnswer)
            {
                LOG_INFO("PerfQualityValue overrided by user: {} (game asked for {})", forcedPq, pqValue);
                pqValue = forcedPq;
                InParameters->Set(NVSDK_NGX_Parameter_PerfQualityValue, pqValue);
            }
            else
            {
                LOG_WARN("Leaving PerfQualityValue at the game's {}: this feature is being built for "
                         "{}x{} -> {}x{}, but the last optimal-settings answer was {}x{} -> {}x{} for "
                         "quality {}. Forcing {} onto dimensions it did not produce is what the runtime "
                         "rejects. The override applies again once the game asks.",
                         pqValue, width, height, outWidth, outHeight, answer.renderWidth, answer.renderHeight,
                         answer.displayWidth, answer.displayHeight, answer.quality, forcedPq);
            }
        }

        GetDynamicOutputResolution(InParameters, &outWidth, &outHeight);

        // Thanks to Crytek added these checks
        if (width > 16384 || width < 20)
            width = 0;

        if (height > 16384 || height < 20)
            height = 0;

        if (outWidth > 16384 || outWidth < 20)
            outWidth = 0;

        if (outHeight > 16384 || outHeight < 20)
            outHeight = 0;

        if (pqValue > 5 || pqValue < 0)
            pqValue = 1;

        // When using extended limits render res might be bigger than display res
        // it might create rendering issues but extending limits is an advanced option after all
        if (!Config::Instance()->ExtendedLimits.value_or_default())
        {
            _displayWidth = width > outWidth ? width : outWidth;
            _displayHeight = height > outHeight ? height : outHeight;
            _targetWidth = _displayWidth;
            _targetHeight = _displayHeight;
            _renderWidth = width < outWidth ? width : outWidth;
            _renderHeight = height < outHeight ? height : outHeight;
        }
        else
        {
            _displayWidth = outWidth;
            _displayHeight = outHeight;
            _targetWidth = _displayWidth;
            _targetHeight = _displayHeight;
            _renderWidth = width;
            _renderHeight = height;
        }

        const bool createSplit = _renderWidth > 0 && _renderHeight > 0 && _renderWidth < _displayWidth &&
                                 Config::Instance()->DlssNrDualFeatureActive() &&
                                 Config::Instance()->DlssNrEnabled.value_or_default();
        _splitOutputWidth = createSplit ? _renderWidth : 0;
        _splitOutputHeight = createSplit ? _renderHeight : 0;

        _perfQualityValue = (NVSDK_NGX_PerfQuality_Value) pqValue;

        if (DualFeatureSplit())
            LOG_INFO("DLSS-NR dual feature: upscaler targets {}x{}, enlargement to {}x{} runs after the model",
                     _renderWidth, _renderHeight, _displayWidth, _displayHeight);

        // Every feature that is initialised, split or not, says who it is and what it was handed. With a
        // bridge this is the only place the inner and outer objects can be told apart, and the question
        // the dual-feature failure keeps raising is which of them received which resolutions.
        LOG_DEBUG("Feature initialised: {} (asked for {}x{} -> {}x{}, quality {}, dual feature setting {})",
                  FeatureIdentity(), width, height, outWidth, outHeight, pqValue,
                  Config::Instance()->DlssNrDualFeatureActive());

        LOG_INFO("Render Resolution: {0}x{1}, Display Resolution {2}x{3}, Quality: {4}", _renderWidth, _renderHeight,
                 _displayWidth, _displayHeight, pqValue);

        // If output scaling is enabled and render res is equal to display res, enable low res MVs
        // because we will use display res MV as render res and upscale with it
        if (Config::Instance()->OutputScalingEnabled.value_or_default() && !_initFlags.LowResMV &&
            _renderWidth == _displayWidth)
        {
            LOG_INFO("Output Scaling is active with render size equal to display size, enabling low res MVs");
            _initFlags.LowResMV = true;
        }

        return true;
    }

    LOG_ERROR("Can't set parameters!");
    return false;
}

void IFeature::GetRenderResolution(const NVSDK_NGX_Parameter* InParameters, unsigned int* OutWidth,
                                   unsigned int* OutHeight)
{
    ReportFfxivQualitySetting();
    if (State::Instance().gameExe == "ffxiv_dx11.exe")
    {
        static thread_local ULONGLONG nextSample = 0;
        const auto now = GetTickCount64();
        if (now >= nextSample)
        {
            nextSample = now + 2000;
            unsigned int submittedWidth = 0, submittedHeight = 0;
            const auto widthResult = InParameters->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, &submittedWidth);
            const auto heightResult = InParameters->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, &submittedHeight);
            LOG_INFO("FFXIV scene evaluate: feature={} init={}x{} display={}x{} submitted={}x{} readable={}/{}",
                     _handle ? _handle->Id : 0u, _renderWidth, _renderHeight, _displayWidth, _displayHeight,
                     submittedWidth, submittedHeight, widthResult == NVSDK_NGX_Result_Success,
                     heightResult == NVSDK_NGX_Result_Success);
        }
    }

    if (InParameters->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, OutWidth) !=
            NVSDK_NGX_Result_Success ||
        InParameters->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, OutHeight) !=
            NVSDK_NGX_Result_Success)
    {
        LOG_WARN("No subrect dimension info!");

        unsigned int width;
        unsigned int height;
        unsigned int outWidth;
        unsigned int outHeight;

        do
        {
            if (InParameters->Get(NVSDK_NGX_Parameter_Width, &width) == NVSDK_NGX_Result_Success &&
                InParameters->Get(NVSDK_NGX_Parameter_Height, &height) == NVSDK_NGX_Result_Success)
            {
                if (InParameters->Get(NVSDK_NGX_Parameter_OutWidth, &outWidth) == NVSDK_NGX_Result_Success &&
                    InParameters->Get(NVSDK_NGX_Parameter_OutHeight, &outHeight) == NVSDK_NGX_Result_Success)
                {
                    if (width < outWidth)
                    {
                        *OutWidth = width;
                        *OutHeight = height;
                        break;
                    }

                    *OutWidth = outWidth;
                    *OutHeight = outHeight;
                }
                else
                {
                    if (width < RenderWidth())
                    {
                        *OutWidth = width;
                        *OutHeight = height;
                        break;
                    }

                    *OutWidth = RenderWidth();
                    *OutHeight = RenderHeight();
                    return;
                }
            }

            *OutWidth = RenderWidth();
            *OutHeight = RenderHeight();

        } while (false);
    }

    // The render subrect, or Width/Height -- see Config's RenderSizeFromWidthHeight.
    //
    // Off, this only reports a disagreement. On, it takes the smaller value per axis: neither
    // parameter can exceed the frame's real size under either convention, and per axis rather than as
    // a pair because letterboxing scales one axis and not the other.
    unsigned int paramWidth = 0, paramHeight = 0;

    const bool haveParams = InParameters->Get(NVSDK_NGX_Parameter_Width, &paramWidth) == NVSDK_NGX_Result_Success &&
                            InParameters->Get(NVSDK_NGX_Parameter_Height, &paramHeight) == NVSDK_NGX_Result_Success &&
                            paramWidth > 0 && paramHeight > 0;

    if (haveParams && (paramWidth != *OutWidth || paramHeight != *OutHeight))
    {
        const bool trustParams = Config::Instance()->RenderSizeFromWidthHeight.value_or_default();

        const unsigned int resolvedWidth = trustParams && paramWidth < *OutWidth ? paramWidth : *OutWidth;
        const unsigned int resolvedHeight = trustParams && paramHeight < *OutHeight ? paramHeight : *OutHeight;

        if (_renderWidth != resolvedWidth || _renderHeight != resolvedHeight || !_reportedEvaluateGeometry)
            LOG_INFO("Evaluate geometry: the game reports a {}x{} subrect beside Width/Height of {}x{}. Using "
                     "{}x{} ({}). This feature was created for render {}x{} display {}x{}.",
                     *OutWidth, *OutHeight, paramWidth, paramHeight, resolvedWidth, resolvedHeight,
                     trustParams ? "Hotfix.RenderSizeFromWidthHeight is on" : "the subrect, as NGX documents it",
                     _renderWidth, _renderHeight, _displayWidth, _displayHeight);

        *OutWidth = resolvedWidth;
        *OutHeight = resolvedHeight;
    }

    _reportedEvaluateGeometry = true;

    _renderWidth = *OutWidth;
    _renderHeight = *OutHeight;

    // Should not be needed but who knows
    // if (_renderHeight == _displayHeight && _renderWidth == _displayWidth && _perfQualityValue !=
    // NVSDK_NGX_PerfQuality_Value_DLAA)
    //{
    //	InParameters->Set(NVSDK_NGX_Parameter_PerfQualityValue, 5);
    //	InParameters->Set(NVSDK_NGX_Parameter_Scale, 1.0f);
    //	InParameters->Set(NVSDK_NGX_Parameter_SuperSampling_ScaleFactor, 1.0f);
    // }

    JitterInfo ji {};
    if (_jitterInfo.size() < 350 &&
        InParameters->Get(NVSDK_NGX_Parameter_Jitter_Offset_X, &ji.x) == NVSDK_NGX_Result_Success &&
        InParameters->Get(NVSDK_NGX_Parameter_Jitter_Offset_Y, &ji.y) == NVSDK_NGX_Result_Success)
    {
        _jitterInfo.insert(std::make_pair(ji.x, ji.y));
    }
}

float IFeature::GetSharpness(const NVSDK_NGX_Parameter* InParameters)
{
    if (Config::Instance()->OverrideSharpness.value_or_default())
        return Config::Instance()->Sharpness.value_or_default();

    float sharpness = 0.0f;

    if (InParameters->Get(NVSDK_NGX_Parameter_Sharpness, &sharpness) == NVSDK_NGX_Result_Success)
    {
        if (sharpness < 0.0f)
            sharpness = 0.0f;
        else if (sharpness > 1.0f)
            sharpness = 1.0f;
    }

    return sharpness;
}

void IFeature::TickFrozenCheck()
{
    static long updatesWithoutFramecountChange = 0;

    if (_isInited)
    {
        static auto lastFrameCount = _frameCount;

        if (_frameCount == lastFrameCount)
            updatesWithoutFramecountChange++;
        else
            updatesWithoutFramecountChange = 0;

        lastFrameCount = _frameCount;

        // Ticked once per present, but _frameCount only advances on an evaluate. Frame generation
        // presents its generated frames between evaluates, so the count reaches the multiplier every
        // real frame with nothing wrong. Scale the threshold by it.
        const auto presentsPerEvaluate = std::max(1, State::Instance().dlssgDetectedInterpolationCount + 1);

        _featureFrozen = updatesWithoutFramecountChange > 10L * presentsPerEvaluate;
    }
}

bool IFeature::UpdateOutputResolution(const NVSDK_NGX_Parameter* InParameters)
{
    // Check for FSR's dynamic resolution output
    auto fsrDynamicOutputWidth = 0;
    auto fsrDynamicOutputHeight = 0;

    InParameters->Get("FSR.upscaleSize.width", &fsrDynamicOutputWidth);
    InParameters->Get("FSR.upscaleSize.height", &fsrDynamicOutputHeight);

    if (Config::Instance()->OutputScalingEnabled.value_or_default())
    {
        if (_targetWidth == fsrDynamicOutputWidth || _targetHeight == fsrDynamicOutputHeight)
            return false;

        if (fsrDynamicOutputWidth > 0 && fsrDynamicOutputHeight > 0 &&
            ((unsigned int) (fsrDynamicOutputWidth * Config::Instance()->OutputScalingMultiplier.value_or_default()) !=
                 _targetWidth ||
             fsrDynamicOutputWidth != _displayWidth ||
             (unsigned int) (fsrDynamicOutputHeight * Config::Instance()->OutputScalingMultiplier.value_or_default()) !=
                 _targetHeight ||
             fsrDynamicOutputHeight != _displayHeight))
        {
            _targetWidth = static_cast<unsigned int>(fsrDynamicOutputWidth *
                                                     Config::Instance()->OutputScalingMultiplier.value_or_default());
            _displayWidth = fsrDynamicOutputWidth;
            _targetHeight = static_cast<unsigned int>(fsrDynamicOutputHeight *
                                                      Config::Instance()->OutputScalingMultiplier.value_or_default());
            _displayHeight = fsrDynamicOutputHeight;

            return true;
        }
    }
    else
    {
        if (fsrDynamicOutputWidth > 0 && fsrDynamicOutputHeight > 0 &&
            (fsrDynamicOutputWidth != _targetWidth || fsrDynamicOutputWidth != _displayWidth ||
             fsrDynamicOutputHeight != _targetHeight || fsrDynamicOutputHeight != _displayHeight))
        {
            _targetWidth = fsrDynamicOutputWidth;
            _displayWidth = fsrDynamicOutputWidth;
            _targetHeight = fsrDynamicOutputHeight;
            _displayHeight = fsrDynamicOutputHeight;

            return true;
        }
    }

    return false;
}

void IFeature::GetDynamicOutputResolution(NVSDK_NGX_Parameter* InParameters, unsigned int* width, unsigned int* height)
{
    // FSR 3.1 uses upscaleSize for this, max size should stay the same
    int supportsUpscaleSize = 0;
    InParameters->Get("OptiScaler.SupportsUpscaleSize", &supportsUpscaleSize);
    if (supportsUpscaleSize)
    {
        InParameters->Set("OptiScaler.SupportsUpscaleSize", 0);
        return;
    }

    // Check for FSR's dynamic resolution output
    auto fsrDynamicOutputWidth = 0;
    auto fsrDynamicOutputHeight = 0;

    InParameters->Get("FSR.upscaleSize.width", &fsrDynamicOutputWidth);
    InParameters->Get("FSR.upscaleSize.height", &fsrDynamicOutputHeight);

    if (fsrDynamicOutputWidth > 0 && fsrDynamicOutputHeight > 0)
    {
        *width = fsrDynamicOutputWidth;
        *height = fsrDynamicOutputHeight;
    }
}
