#pragma once
#include <d3d12.h>
#include <filesystem>
#include <string>

namespace AmdPreSr
{
struct Frame
{
    ID3D12Resource *colour = nullptr, *motion = nullptr, *depth = nullptr, *exposure = nullptr;
    UINT width = 0, height = 0;
    float motionScaleX = 1, motionScaleY = 1;
    // Active display extent, excluding allocation padding; zero means render-resolution vectors.
    UINT motionWidth = 0, motionHeight = 0;
    float preExposure = 1, exposureScale = 1;
    bool reset = false, depthInverted = false;
    D3D12_RESOURCE_STATES colourState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    D3D12_RESOURCE_STATES motionState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    D3D12_RESOURCE_STATES depthState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    D3D12_RESOURCE_STATES exposureState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
};
struct Settings
{
    UINT encoding = 0; // Auto, Linear, sRGB, Gamma 2.2
    bool toneChannels = false;
    float modelScale = 1;
    UINT passes = 1;
    float tone = 0, structure = 1, skin = 1;
};
// Process lifetime owner: intentionally not destroyed/unloaded while HIP threads exist.
class Backend
{
    struct Impl;
    Impl* p;

  public:
    Backend(ID3D12Device*, ID3D12CommandQueue*, const std::filesystem::path& directory);
    // Records pre-SR work. Returns a FP16 input for the upscaler, or nullptr on skip/failure.
    ID3D12Resource* Record(ID3D12GraphicsCommandList*, const Frame&, const Settings&);
    // Bind the actual render queue BEFORE submission; do not launch GPU work yet.
    int PendingListIndex(UINT, ID3D12CommandList* const*) const;
    void Submitting(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
    // Must run immediately AFTER real queue submission, including non-upscale lists.
    void Submitted(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
    bool Ready();
    bool Shutdown(); // call before loader-lock teardown, after all submissions
    void InvalidateHistory(); // applied at the next safe recording boundary
    std::string Status() const;
    UINT64 RecordedFrames() const;
};
} // namespace AmdPreSr
