#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <functional>
#include <memory>
#include <array>
#include <atomic>
namespace Hdr10 {
// An absent callback must never make previously recorded GPU work look completed.
inline bool ReusableRecordedWork(bool recorded,const std::function<bool()>& probe) {
    if(!recorded)return true; // Allocated only; no commands have referenced it.
    if(!probe)return false;
    const bool completedAndRecordingRetired=probe();
    if(!completedAndRecordingRetired)return false;
    return true;
}

enum class NrSceneState { Off, Ordinary, PreSR, Alternating, WaitingScene, WaitingGpu, WaitingNR, UnsupportedFormat, AllocationFailed, UnavailableHooks, RequiresBeforeSR, HDRInactive, NRDisabled };
inline std::atomic<NrSceneState> nrSceneState{NrSceneState::Off};
inline const char* NrSceneText(NrSceneState state) {
 switch(state) {
 case NrSceneState::Ordinary:return "linear HDR active (NR before upscaling)";
 case NrSceneState::PreSR:return "linear HDR active (PreSR)";
 case NrSceneState::Alternating:return "linear HDR active (alternating NR anchors)";
 case NrSceneState::WaitingScene:return "SDR fallback: no matching scene this frame";
 case NrSceneState::WaitingGpu:return "SDR fallback: HDR work buffers still in flight";
 case NrSceneState::WaitingNR:return "waiting for NR / PreSR initialization";
 case NrSceneState::UnsupportedFormat:return "SDR fallback: unsupported colour texture format or layout";
 case NrSceneState::AllocationFailed:return "SDR fallback: HDR work buffer allocation failed";
 case NrSceneState::UnavailableHooks:return "SDR fallback: GPU tracking or restorable command state unavailable";
 case NrSceneState::RequiresBeforeSR:return "inactive: enable NR before upscaling or PreSR";
 case NrSceneState::HDRInactive:return "inactive: enable scene HDR output";
 case NrSceneState::NRDisabled:return "inactive: NR is disabled";
 default:return "disabled";
 }
}
// A same-frame pre-clipping scene and its original game reference. Both remain
// owned until the conversion command list finishes, including abandoned lists.
struct SceneInput {
    Microsoft::WRL::ComPtr<ID3D12Resource> hdr, reference;
    std::shared_ptr<void> owner;
    std::array<float,4> rect{0,0,1,1}; // Normalised offset/size of the rendered viewport.
    std::function<void(std::function<bool()>)> retire;
    bool referenceSrgb=false; // Native captures use gamma 2.2; DLSS compatibility output uses sRGB.
    Microsoft::WRL::ComPtr<ID3D12Resource> previewMask; // Native UI preview transmittance, in output coordinates.
    bool referenceHdrShoulder=false; // SDR reference was derived with the HDR compatibility shoulder.
};
}
