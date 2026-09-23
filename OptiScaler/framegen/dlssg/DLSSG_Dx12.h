#pragma once

#include <framegen/IFGFeature_Dx12.h>

#include <proxies/Streamline_Proxy.h>
#include <atomic>
#include <misc/TemporalContinuity.h>

class DLSSG_Dx12 : public virtual IFGFeature_Dx12
{
  private:
    uint32_t _width = 0;
    uint32_t _height = 0;
    std::optional<bool> _haveHudless = std::nullopt;

    sl::ViewportHandle viewport { 0 };
    sl::FrameToken* frameToken = nullptr;

    ID3D12Fence* dlssgFence[BUFFER_COUNT] = {};
    UINT64 lastOptionFrame = 0;
    std::atomic<UINT64> _observedMultiplier { 0 }; // tick count in high bits, multiplier in low byte
    int _sampleMode = -1;
    TemporalContinuity::SuccessfulFrames _historyFrames;

    struct GuideSnapshot
    {
        ID3D12Resource* resource = nullptr;
        Dx12Resource view {};
        UINT64 frame = 0;
        bool valid = false;
    };
    struct GuideMetadata { float values[9] {}; float vectors[4][3] {}; double delta=0; UINT reset=0; };
    GuideMetadata _guideMetadata[BUFFER_COUNT] {};
    GuideSnapshot _guideSnapshots[BUFFER_COUNT][2] {};
    int _guideAge[BUFFER_COUNT] {-1,-1,-1,-1};
    struct RetiredGuide { ID3D12Resource* resource; ID3D12Fence* fence; UINT64 value; };
    std::vector<RetiredGuide> _retiredGuides;
    TemporalContinuity::SuccessfulFrames _sceneHistory;
    UINT64 _guideResetFrame = UINT64_MAX;
    void RetireGuide(ID3D12Resource*& resource);
    void CollectGuides();
    bool MatchPresentationGuide(Dx12Resource& resource, int index);
    bool Dispatch();

  protected:
    void ReleaseObjects() override final;
    void CreateObjects(ID3D12Device* InDevice) override final;

  public:
    // IFGFeature
    void SetPresentationGuideDelay(int age) override;
    const char* Name() override final { return "DLSSG"; };
    feature_version Version() override final;
    HWND Hwnd() override final;
    unsigned GetObservedFrameMultiplier() const override final
    {
        const auto sample = _observedMultiplier.load(std::memory_order_relaxed);
        return sample && GetTickCount64() - (sample >> 8) < 3000 ? unsigned(sample & 0xff) : 0;
    }

    // IFGFeature_Dx12
    bool CreateSwapchain(IDXGIFactory* factory, ID3D12CommandQueue* cmdQueue, DXGI_SWAP_CHAIN_DESC* desc,
                         IDXGISwapChain** swapChain, bool readyToRelease) override final;
    bool CreateSwapchain1(IDXGIFactory* factory, ID3D12CommandQueue* cmdQueue, HWND hwnd, DXGI_SWAP_CHAIN_DESC1* desc,
                          DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc, IDXGISwapChain1** swapChain,
                          bool readyToRelease) override final;

    bool ReleaseSwapchain(HWND hwnd) override final;

    void CreateContext(ID3D12Device* device, FG_Constants& fgConstants) override final;
    void Activate() override final;
    void Deactivate() override final;
    void DestroyFGContext() override final;
    bool Shutdown() override final;

    void EvaluateState(ID3D12Device* device, FG_Constants& fgConstants) override final;

    bool Present() override final;

    bool SetResource(Dx12Resource* inputResource) override final;
    void SetCommandQueue(FG_ResourceType type, ID3D12CommandQueue* queue) override final;

    void* FrameGenerationContext() override final;
    void* SwapchainContext() override final;

    DLSSG_Dx12() : IFGFeature_Dx12(), IFGFeature()
    {
        if (StreamlineProxy::Module() == nullptr)
            StreamlineProxy::LoadStreamline();

        if (StreamlineProxy::Module() != nullptr && !StreamlineProxy::IsD3D12Inited() &&
            State::Instance().currentD3D12Device != nullptr)
        {
            StreamlineProxy::InitWithD3D12(State::Instance().currentD3D12Device);
        }
    }

    ~DLSSG_Dx12();

    // Inherited via IFGFeature_Dx12
    bool SetInterpolatedFrameCount(UINT interpolatedFrameCount) override;
};
