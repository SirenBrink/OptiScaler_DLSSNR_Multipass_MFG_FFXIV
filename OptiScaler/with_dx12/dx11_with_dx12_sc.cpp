#include "pch.h"
#include <shaders/hdr/Hdr10.h>
#include <shaders/hdr/HdrScreenshot.h>
#include <misc/FfxivLightingCapture.h>
#include <misc/companion/Companion.h>
#include <misc/ExternalHudless.h>
#include <misc/ExternalHudlessNormalize.h>
#include <misc/UiPaste.h>
#include <shaders/ui_extract/UE_Dx12.h>
#include <framegen/IFGFeature_Dx12.h>
#include "dx11_with_dx12_sc.h"

#include <with_dx12/with_dx12.h>

#include <hooks/FG_Hooks.h>
#include <menu/menu_overlay_dx.h>

#include <Util.h>
#include <Config.h>

#include <d3d11.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#pragma intrinsic(_ReturnAddress)

static int scCount = 0;

static DXGI_FORMAT ExternalHudlessTypedFormat(DXGI_FORMAT format);

namespace
{
template <typename T> void SafeRelease(T*& value)
{
    if (value != nullptr)
    {
        value->Release();
        value = nullptr;
    }
}

void SafeCloseHandle(HANDLE& value)
{
    if (value != nullptr)
    {
        CloseHandle(value);
        value = nullptr;
    }
}

void TransitionResource(ID3D12GraphicsCommandList* commandList, ID3D12Resource* resource,
                        D3D12_RESOURCE_STATES beforeState, D3D12_RESOURCE_STATES afterState)
{
    if (commandList == nullptr || resource == nullptr || beforeState == afterState)
        return;

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = beforeState;
    barrier.Transition.StateAfter = afterState;

    commandList->ResourceBarrier(1, &barrier);
}

UINT ResolveBufferCount(IDXGISwapChain* swapchain, IDXGISwapChain1* swapchain1)
{
    DXGI_SWAP_CHAIN_DESC desc = {};
    if (swapchain != nullptr && SUCCEEDED(swapchain->GetDesc(&desc)) && desc.BufferCount > 0)
        return desc.BufferCount;

    DXGI_SWAP_CHAIN_DESC1 desc1 = {};
    if (swapchain1 != nullptr && SUCCEEDED(swapchain1->GetDesc1(&desc1)) && desc1.BufferCount > 0)
        return desc1.BufferCount;

    return 2;
}

DXGI_FORMAT ResolveBufferFormat(IDXGISwapChain* swapchain, IDXGISwapChain1* swapchain1)
{
    DXGI_SWAP_CHAIN_DESC desc = {};
    if (swapchain != nullptr && SUCCEEDED(swapchain->GetDesc(&desc)) && desc.BufferDesc.Format != DXGI_FORMAT_UNKNOWN)
        return desc.BufferDesc.Format;

    DXGI_SWAP_CHAIN_DESC1 desc1 = {};
    if (swapchain1 != nullptr && SUCCEEDED(swapchain1->GetDesc1(&desc1)))
        return desc1.Format;

    return DXGI_FORMAT_UNKNOWN;
}
} // namespace

Dx11wDx12SC::Dx11wDx12SC(IDXGISwapChain* real, IDXGISwapChain4* fgSC, ID3D11Device* pDevice, HWND hWnd, UINT flags)
    : _real(real), _fgSwapChain(fgSC), _dx11Device(pDevice), _handle(hWnd), _refcount(1)
{
    _id = ++scCount;
    _lastFlags = flags;

    if (_real != nullptr)
    {
        _real->AddRef();
        _real->QueryInterface(IID_PPV_ARGS(&_real1));
        _real->QueryInterface(IID_PPV_ARGS(&_real2));
        _real->QueryInterface(IID_PPV_ARGS(&_real3));
        _real->QueryInterface(IID_PPV_ARGS(&_real4));
    }

    if (_fgSwapChain != nullptr)
        _fgSwapChain->AddRef();

    if (_dx11Device != nullptr)
    {
        _dx11Device->AddRef();
        auto device5Result = _dx11Device->QueryInterface(IID_PPV_ARGS(&_dx11Device5));
        if (FAILED(device5Result))
            LOG_WARN("ID3D11Device5 unavailable: {:X}", (UINT) device5Result);

        _dx11Device->GetImmediateContext(&_dx11Context);
        if (_dx11Context != nullptr)
        {
            auto context4Result = _dx11Context->QueryInterface(IID_PPV_ARGS(&_dx11Context4));
            if (FAILED(context4Result))
                LOG_WARN("ID3D11DeviceContext4 unavailable: {:X}", (UINT) context4Result);
        }
    }

    if (WithDx12::PrepareD3D12ForD3D11(_dx11Device, D3D_FEATURE_LEVEL_11_0))
    {
        _dx12Device = WithDx12::GetD3D12Device();
        _dx12CommandQueue = WithDx12::GetD3D12CommandQueue();
    }
    else
    {
        LOG_ERROR("failed to resolve D3D12 device/queue");
    }

    State::Instance().swapchainInteropApi = SwapchainInteropApi::Dx11wDx12;

    _RefreshCachedSwapchainDesc();
    DXGI_SWAP_CHAIN_DESC1 hdrDesc {};
    if (Config::Instance()->FfxivHDR.value_or_default() && _fgSwapChain &&
        SUCCEEDED(_fgSwapChain->GetDesc1(&hdrDesc)) && hdrDesc.Format == DXGI_FORMAT_R10G10B10A2_UNORM)
    {
        _hdrOutput = Hdr10::Configure(_fgSwapChain);
        if (!_hdrOutput) _fgSwapChain->ResizeBuffers(0, 0, 0, _bufferFormat, hdrDesc.Flags);
    }

    LOG_INFO("Dx11wDx12SC {} created, real: {:X}, fg: {:X}, dx11: {:X}, dx12: {:X}, queue: {:X}", _id, (UINT64) _real,
             (UINT64) _fgSwapChain, (UINT64) _dx11Device, (UINT64) _dx12Device, (UINT64) _dx12CommandQueue);
}

Dx11wDx12SC::~Dx11wDx12SC()
{
    if (_hdrOutput) Hdr10::Deactivate();
    // Drain completed tiny native readbacks on normal bridge teardown. Never
    // block for unfinished GPU work or release them from DLL detach/loader lock.
    if (_dx11Context && _dx11Context == FfxivLightingScan::State().context)
        FfxivLightingScan::Tick(_dx11Context, false);
    MenuOverlayDx::CleanupRenderTarget(true, _handle);
    _ReleaseInteropObjects();

    SafeRelease(_real4);
    SafeRelease(_real3);
    SafeRelease(_real2);
    SafeRelease(_real1);
    SafeRelease(_fgSwapChain);
    SafeRelease(_real);
    SafeRelease(_dx11Context4);
    SafeRelease(_dx11Context);
    SafeRelease(_dx11Device5);
    SafeRelease(_dx11Device);
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::QueryInterface(REFIID riid, void** ppvObject)
{
    LOG_TRACE("Caller: {}", Util::WhoIsTheCaller(_ReturnAddress()));

    if (ppvObject == nullptr)
        return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) || riid == __uuidof(IDXGIDeviceSubObject) ||
        riid == __uuidof(IDXGISwapChain))
    {
        AddRef();
        *ppvObject = static_cast<IDXGISwapChain*>(this);
        return S_OK;
    }

    if (riid == __uuidof(IDXGISwapChain1))
    {
        if (_real1 == nullptr)
            return E_NOINTERFACE;

        AddRef();
        *ppvObject = static_cast<IDXGISwapChain1*>(this);
        return S_OK;
    }

    if (riid == __uuidof(IDXGISwapChain2))
    {
        if (_real2 == nullptr)
            return E_NOINTERFACE;

        AddRef();
        *ppvObject = static_cast<IDXGISwapChain2*>(this);
        return S_OK;
    }

    if (riid == __uuidof(IDXGISwapChain3))
    {
        if (_real3 == nullptr)
            return E_NOINTERFACE;

        AddRef();
        *ppvObject = static_cast<IDXGISwapChain3*>(this);
        return S_OK;
    }

    if (riid == __uuidof(IDXGISwapChain4))
    {
        if (_real4 == nullptr && _fgSwapChain == nullptr)
            return E_NOINTERFACE;

        AddRef();
        *ppvObject = static_cast<IDXGISwapChain4*>(this);
        return S_OK;
    }

    if (riid == __uuidof(Dx11wDx12SC))
    {
        AddRef();
        *ppvObject = this;
        return S_OK;
    }

    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE Dx11wDx12SC::AddRef()
{
    auto count = InterlockedIncrement(&_refcount);
    LOG_TRACE("Count: {}, caller: {}", count, Util::WhoIsTheCaller(_ReturnAddress()));
    return count;
}

ULONG STDMETHODCALLTYPE Dx11wDx12SC::Release()
{
    ULONG ret = InterlockedDecrement(&_refcount);

    LOG_TRACE("Count: {}, caller: {}", ret, Util::WhoIsTheCaller(_ReturnAddress()));

    if (ret == 0)
    {
        if (State::Instance().currentSwapchain == this)
            State::Instance().currentSwapchain = nullptr;

        if (State::Instance().currentWrappedSwapchain == this)
            State::Instance().currentWrappedSwapchain = nullptr;

        if (State::Instance().currentRealSwapchain == _real)
            State::Instance().currentRealSwapchain = nullptr;

        FGHooks::ClearDx12InteropPresentSC(_fgSwapChain);

        if (State::Instance().currentFGSwapchain == _fgSwapChain)
            State::Instance().currentFGSwapchain = nullptr;

        if (State::Instance().currentD3D11Device == _dx11Device)
            State::Instance().currentD3D11Device = nullptr;

        State::Instance().swapchainInteropApi = SwapchainInteropApi::None;

        auto fg = State::Instance().currentFG;
        if (fg != nullptr && fg->Mutex.getOwner() != 1 && fg->SwapchainContext() != nullptr)
        {
            fg->Deactivate();
            fg->ReleaseSwapchain(_handle);
        }

        delete this;
    }

    return ret;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::SetPrivateData(REFGUID Name, UINT DataSize, const void* pData)
{
    return _real != nullptr ? _real->SetPrivateData(Name, DataSize, pData) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::SetPrivateDataInterface(REFGUID Name, const IUnknown* pUnknown)
{
    return _real != nullptr ? _real->SetPrivateDataInterface(Name, pUnknown) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetPrivateData(REFGUID Name, UINT* pDataSize, void* pData)
{
    return _real != nullptr ? _real->GetPrivateData(Name, pDataSize, pData) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetParent(REFIID riid, void** ppParent)
{
    return _real != nullptr ? _real->GetParent(riid, ppParent) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetDevice(REFIID riid, void** ppDevice)
{
    return _real != nullptr ? _real->GetDevice(riid, ppDevice) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::Present(UINT SyncInterval, UINT Flags)
{
    if (_real == nullptr || _fgSwapChain == nullptr)
        return DXGI_ERROR_DEVICE_REMOVED;

    if (_fg == nullptr)
        _fg = State::Instance().currentFG;

    if ((Flags & DXGI_PRESENT_TEST) != 0)
        return _real->Present(SyncInterval, Flags);

    struct SceneFrame { ~SceneFrame(){FfxivSceneHdr::EndFrame();} } sceneFrame;
    if (!_InitInteropObjects())
        return DXGI_ERROR_DEVICE_REMOVED;

    auto dx11Index = _GetDx11BackBufferIndexForPresent();

    if (!_RequestSharedBackBuffer(dx11Index))
        return DXGI_ERROR_DEVICE_REMOVED;

    if (!_CopyDx11BackBufferToShared(dx11Index))
        return DXGI_ERROR_DEVICE_REMOVED;

    // Recorded on the D3D11 context before the interop fence below, so the same
    // DX11 -> DX12 wait covers both the backbuffer and the HUD-less copy.
    _CopyExternalHudlessToShared();

    if (!_WaitDx11ThenDx12())
        return DXGI_ERROR_DEVICE_REMOVED;

    // UI-free frame generation: hand DLSS-G the HUD-less frame, the UI paste adds the UI back on
    // every output frame (see misc/UiPaste.h). DLSS-G then has no UI of its own to interpolate.
    _uiFreeThisFrame = _hudlessPending && _UiFreeFrameGenWanted();

    if (!_CopyDx11SharedToDx12FGBackBuffer(dx11Index))
        return DXGI_ERROR_DEVICE_REMOVED;

    if (!_WaitForInteropCopyOnPresentQueue())
        return DXGI_ERROR_DEVICE_REMOVED;

    // The FG queue now waits for the interop copy (which itself waited for D3D11),
    // so the shared HUD-less texture is complete for anything recorded from here on.
    const bool hudlessThisFrame = _hudlessPending;
    const UINT hudlessSlotThisFrame = _hudlessSlot;
    _TagExternalHudless();

    // Builds the image LocalPresent pastes over every DLSS-G output frame (see misc/UiPaste.h).
    _ProduceUiPaste(hudlessThisFrame, hudlessSlotThisFrame);
    // Failed/invalidated producers must not leave a scene-only base frame without its UI.
    if (_uiFreeThisFrame && !_UiPasteReady()) {
        _uiFreeThisFrame=false;
        if (!_CopyDx11SharedToDx12FGBackBuffer(dx11Index) || !_WaitForInteropCopyOnPresentQueue())
            return DXGI_ERROR_DEVICE_REMOVED;
    }

    const bool fgHookedPresenter =
        State::Instance().currentFGSwapchain == _fgSwapChain && !FGHooks::IsDx12InteropPresentSC(_fgSwapChain);

    // The game-facing DX11 swapchain is never presented in this wrapper.
    // For a plain external DX12 presenter, draw Opti's overlay here.
    // For a real FG swapchain, FGHooks::FGPresent/LocalPresent owns overlay/present-side work;
    // drawing it here would double-enter the overlay path before the FG present hook.
    if (!fgHookedPresenter)
        MenuOverlayDx::Present(_fgSwapChain, SyncInterval, Flags, nullptr, _dx12CommandQueue, _handle, false);
    else
        LOG_TRACE("real FG presenter detected; skipping wrapper overlay path");

    LOG_DEBUG("frame {}, dx11Index {}, real3Index {}, fakeIndex {}, bufferCount {}", State::Instance().frameCount,
              dx11Index, _real3 != nullptr ? _real3->GetCurrentBackBufferIndex() : 0xFFFFFFFF, _currentFakeIndex,
              _bufferCount);

    if (_real != nullptr)
    {
        UINT realFlags = Flags;

        // Do not wait for it
        auto realPresentResult = _real->Present(0, realFlags);

        if (FAILED(realPresentResult))
            LOG_WARN("hidden real DX11 Present failed: {:X}", (UINT) realPresentResult);
    }

    auto result = _fgSwapChain->Present(SyncInterval, Flags);

    if (SUCCEEDED(result))
        _AdvanceFakeBackBufferIndex();
    else
        LOG_ERROR("fg Present failed: {:X}", (UINT) result);

    return result;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetBuffer(UINT Buffer, REFIID riid, void** ppSurface)
{
    return _real != nullptr ? _real->GetBuffer(Buffer, riid, ppSurface) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::SetFullscreenState(BOOL Fullscreen, IDXGIOutput* pTarget)
{
    LOG_DEBUG("Dx11wDx12SC SetFullscreenState: {}, target: {:X}, caller: {}", Fullscreen, (size_t) pTarget,
              Util::WhoIsTheCaller(_ReturnAddress()));

    State::Instance().realExclusiveFullscreen = Fullscreen;

    if (_fgSwapChain != nullptr)
        return _fgSwapChain->SetFullscreenState(Fullscreen, pTarget);

    return _real != nullptr ? _real->SetFullscreenState(Fullscreen, pTarget) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetFullscreenState(BOOL* pFullscreen, IDXGIOutput** ppTarget)
{
    if (_fgSwapChain != nullptr)
        return _fgSwapChain->GetFullscreenState(pFullscreen, ppTarget);

    return _real != nullptr ? _real->GetFullscreenState(pFullscreen, ppTarget) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetDesc(DXGI_SWAP_CHAIN_DESC* pDesc)
{
    if (_real == nullptr)
        return DXGI_ERROR_DEVICE_REMOVED;

    auto result = _real->GetDesc(pDesc);
    if (SUCCEEDED(result) && pDesc != nullptr)
        pDesc->OutputWindow = _handle;

    return result;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::ResizeBuffers(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat,
                                                     UINT SwapChainFlags)
{
    MenuOverlayDx::ScopedResize overlayResize;
    LOG_DEBUG("Dx11wDx12SC ResizeBuffers: count {}, size {}x{}, format {}, flags {:X}", BufferCount, Width, Height,
              (UINT) NewFormat, SwapChainFlags);

    if (!_WaitForCopyQueueIdle())
        LOG_WARN("continuing ResizeBuffers after copy fence wait failure");

    MenuOverlayDx::CleanupRenderTarget(true, _handle);
    _ReleaseInteropBackBuffers();

    HRESULT realResult = _real != nullptr ? _real->ResizeBuffers(BufferCount, Width, Height, NewFormat, SwapChainFlags)
                                          : DXGI_ERROR_DEVICE_REMOVED;

    HRESULT fgResult = DXGI_ERROR_DEVICE_REMOVED;
    if (SUCCEEDED(realResult) && _fgSwapChain != nullptr)
        fgResult = _fgSwapChain->ResizeBuffers(BufferCount, Width, Height, _hdrOutput ? DXGI_FORMAT_R10G10B10A2_UNORM : NewFormat, SwapChainFlags);

    LOG_DEBUG("Dx11wDx12SC ResizeBuffers results: real {:X}, fg {:X}", (UINT) realResult, (UINT) fgResult);

    if (SUCCEEDED(realResult) && SUCCEEDED(fgResult))
    {
        if (_hdrOutput && !Hdr10::Configure(_fgSwapChain)) return DXGI_ERROR_UNSUPPORTED;
        _RefreshCachedSwapchainDesc();

        if (Config::Instance()->FGEnabled.value_or_default())
        {
            State::Instance().fgResetCapturedResources = true;
            State::Instance().fgOnlyUseCapturedResources = false;
            State::Instance().fgChanged = true;
        }

        State::Instance().scChanged = true;
        State::Instance().SCAllowTearing = (SwapChainFlags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) != 0;

        if (State::Instance().currentFeature == nullptr)
        {
            State::Instance().screenWidth = static_cast<float>(Width);
            State::Instance().screenHeight = static_cast<float>(Height);
            State::Instance().lastMipBias = 100.0f;
            State::Instance().lastMipBiasMax = -100.0f;
        }
    }

    return FAILED(realResult) ? realResult : fgResult;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::ResizeTarget(const DXGI_MODE_DESC* pNewTargetParameters)
{
    return _fgSwapChain != nullptr
               ? _fgSwapChain->ResizeTarget(pNewTargetParameters)
               : (_real != nullptr ? _real->ResizeTarget(pNewTargetParameters) : DXGI_ERROR_DEVICE_REMOVED);
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetContainingOutput(IDXGIOutput** ppOutput)
{
    return _fgSwapChain != nullptr
               ? _fgSwapChain->GetContainingOutput(ppOutput)
               : (_real != nullptr ? _real->GetContainingOutput(ppOutput) : DXGI_ERROR_DEVICE_REMOVED);
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetFrameStatistics(DXGI_FRAME_STATISTICS* pStats)
{
    return _fgSwapChain != nullptr ? _fgSwapChain->GetFrameStatistics(pStats)
                                   : (_real != nullptr ? _real->GetFrameStatistics(pStats) : DXGI_ERROR_DEVICE_REMOVED);
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetLastPresentCount(UINT* pLastPresentCount)
{
    return _fgSwapChain != nullptr
               ? _fgSwapChain->GetLastPresentCount(pLastPresentCount)
               : (_real != nullptr ? _real->GetLastPresentCount(pLastPresentCount) : DXGI_ERROR_DEVICE_REMOVED);
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetDesc1(DXGI_SWAP_CHAIN_DESC1* pDesc)
{
    return _real1 != nullptr ? _real1->GetDesc1(pDesc) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pDesc)
{
    if (_fgSwapChain != nullptr)
        return _fgSwapChain->GetFullscreenDesc(pDesc);

    return _real1 != nullptr ? _real1->GetFullscreenDesc(pDesc) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetHwnd(HWND* pHwnd)
{
    if (pHwnd == nullptr)
        return DXGI_ERROR_INVALID_CALL;

    *pHwnd = _handle;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetCoreWindow(REFIID refiid, void** ppUnk)
{
    return _real1 != nullptr ? _real1->GetCoreWindow(refiid, ppUnk) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::Present1(UINT SyncInterval, UINT Flags,
                                                const DXGI_PRESENT_PARAMETERS* pPresentParameters)
{
    UNREFERENCED_PARAMETER(pPresentParameters);
    return Present(SyncInterval, Flags);
}

BOOL STDMETHODCALLTYPE Dx11wDx12SC::IsTemporaryMonoSupported(void)
{
    return _real1 != nullptr ? _real1->IsTemporaryMonoSupported() : FALSE;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetRestrictToOutput(IDXGIOutput** ppRestrictToOutput)
{
    return _real1 != nullptr ? _real1->GetRestrictToOutput(ppRestrictToOutput) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::SetBackgroundColor(const DXGI_RGBA* pColor)
{
    return _real1 != nullptr ? _real1->SetBackgroundColor(pColor) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetBackgroundColor(DXGI_RGBA* pColor)
{
    return _real1 != nullptr ? _real1->GetBackgroundColor(pColor) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::SetRotation(DXGI_MODE_ROTATION Rotation)
{
    return _real1 != nullptr ? _real1->SetRotation(Rotation) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetRotation(DXGI_MODE_ROTATION* pRotation)
{
    return _real1 != nullptr ? _real1->GetRotation(pRotation) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::SetSourceSize(UINT Width, UINT Height)
{
    return _real2 != nullptr ? _real2->SetSourceSize(Width, Height) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetSourceSize(UINT* pWidth, UINT* pHeight)
{
    return _real2 != nullptr ? _real2->GetSourceSize(pWidth, pHeight) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::SetMaximumFrameLatency(UINT MaxLatency)
{
    if (_fgSwapChain != nullptr)
        return _fgSwapChain->SetMaximumFrameLatency(MaxLatency);

    return _real2 != nullptr ? _real2->SetMaximumFrameLatency(MaxLatency) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetMaximumFrameLatency(UINT* pMaxLatency)
{
    if (_fgSwapChain != nullptr)
        return _fgSwapChain->GetMaximumFrameLatency(pMaxLatency);

    return _real2 != nullptr ? _real2->GetMaximumFrameLatency(pMaxLatency) : DXGI_ERROR_DEVICE_REMOVED;
}

HANDLE STDMETHODCALLTYPE Dx11wDx12SC::GetFrameLatencyWaitableObject(void)
{
    if (_fgSwapChain != nullptr)
        return _fgSwapChain->GetFrameLatencyWaitableObject();

    return _real2 != nullptr ? _real2->GetFrameLatencyWaitableObject() : nullptr;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::SetMatrixTransform(const DXGI_MATRIX_3X2_F* pMatrix)
{
    return _real2 != nullptr ? _real2->SetMatrixTransform(pMatrix) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetMatrixTransform(DXGI_MATRIX_3X2_F* pMatrix)
{
    return _real2 != nullptr ? _real2->GetMatrixTransform(pMatrix) : DXGI_ERROR_DEVICE_REMOVED;
}

UINT STDMETHODCALLTYPE Dx11wDx12SC::GetCurrentBackBufferIndex(void)
{
    if (_real3 != nullptr)
        return _real3->GetCurrentBackBufferIndex();

    return _currentFakeIndex;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE ColorSpace,
                                                              UINT* pColorSpaceSupport)
{
    if (_fgSwapChain != nullptr)
        return _fgSwapChain->CheckColorSpaceSupport(ColorSpace, pColorSpaceSupport);

    return _real3 != nullptr ? _real3->CheckColorSpaceSupport(ColorSpace, pColorSpaceSupport)
                             : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::SetColorSpace1(DXGI_COLOR_SPACE_TYPE ColorSpace)
{
    if (_hdrOutput) return Hdr10::Configure(_fgSwapChain) ? S_OK : DXGI_ERROR_UNSUPPORTED;
    State::Instance().isHdrActive = ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ||
                                    ColorSpace == DXGI_COLOR_SPACE_YCBCR_FULL_GHLG_TOPLEFT_P2020 ||
                                    ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P2020 ||
                                    ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;

    if (_fgSwapChain != nullptr)
        return _fgSwapChain->SetColorSpace1(ColorSpace);

    return _real3 != nullptr ? _real3->SetColorSpace1(ColorSpace) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::ResizeBuffers1(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT Format,
                                                      UINT SwapChainFlags, const UINT* pCreationNodeMask,
                                                      IUnknown* const* ppPresentQueue)
{
    MenuOverlayDx::ScopedResize overlayResize;
    LOG_DEBUG("Dx11wDx12SC ResizeBuffers1: count {}, size {}x{}, format {}, flags {:X}", BufferCount, Width, Height,
              (UINT) Format, SwapChainFlags);

    if (!_WaitForCopyQueueIdle())
        LOG_WARN("continuing ResizeBuffers1 after copy fence wait failure");

    MenuOverlayDx::CleanupRenderTarget(true, _handle);
    _ReleaseInteropBackBuffers();

    HRESULT realResult = _real3 != nullptr ? _real3->ResizeBuffers1(BufferCount, Width, Height, Format, SwapChainFlags,
                                                                    pCreationNodeMask, ppPresentQueue)
                                           : ResizeBuffers(BufferCount, Width, Height, Format, SwapChainFlags);

    HRESULT fgResult = DXGI_ERROR_DEVICE_REMOVED;
    if (SUCCEEDED(realResult) && _fgSwapChain != nullptr)
    {
        // The game's ppPresentQueue is not valid for the DX12 FG swapchain. Use ResizeBuffers for phase 1.
        fgResult = _fgSwapChain->ResizeBuffers(BufferCount, Width, Height, _hdrOutput ? DXGI_FORMAT_R10G10B10A2_UNORM : Format, SwapChainFlags);
    }

    LOG_DEBUG("Dx11wDx12SC ResizeBuffers1 results: real {:X}, fg {:X}", (UINT) realResult, (UINT) fgResult);

    if (SUCCEEDED(realResult) && SUCCEEDED(fgResult))
    {
        if (_hdrOutput && !Hdr10::Configure(_fgSwapChain)) return DXGI_ERROR_UNSUPPORTED;
        _RefreshCachedSwapchainDesc();

        if (Config::Instance()->FGEnabled.value_or_default())
        {
            State::Instance().fgResetCapturedResources = true;
            State::Instance().fgOnlyUseCapturedResources = false;
            State::Instance().fgChanged = true;
        }

        State::Instance().scChanged = true;
        State::Instance().SCAllowTearing = (SwapChainFlags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) != 0;
    }

    return FAILED(realResult) ? realResult : fgResult;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::SetHDRMetaData(DXGI_HDR_METADATA_TYPE Type, UINT Size, void* pMetaData)
{
    if (_fgSwapChain != nullptr)
        return _fgSwapChain->SetHDRMetaData(Type, Size, pMetaData);

    return _real4 != nullptr ? _real4->SetHDRMetaData(Type, Size, pMetaData) : DXGI_ERROR_DEVICE_REMOVED;
}

bool Dx11wDx12SC::_InitInteropObjects()
{
    if (_interopInitialized)
    {
        _RefreshCachedSwapchainDesc();

        if (_bufferCount == 0)
        {
            LOG_ERROR("InitInteropObjects resolved zero backbuffers");
            return false;
        }

        if (_sharedDx11BackBufferCopies.size() != _bufferCount)
            _sharedDx11BackBufferCopies.resize(_bufferCount, nullptr);

        if (_openedDx11BackBuffers.size() != _bufferCount)
            _openedDx11BackBuffers.resize(_bufferCount, nullptr);

        if (_sharedBackBufferHandles.size() != _bufferCount)
            _sharedBackBufferHandles.resize(_bufferCount, nullptr);

        if (_openedDx11BackBufferStates.size() != _bufferCount)
            _openedDx11BackBufferStates.resize(_bufferCount, D3D12_RESOURCE_STATE_COMMON);

        return true;
    }

    if (_dx11Device == nullptr || _dx11Device5 == nullptr || _dx11Context4 == nullptr || _dx12Device == nullptr ||
        _dx12CommandQueue == nullptr)
    {
        LOG_ERROR("interop objects missing: dx11 {}, dx11-5 {}, ctx4 {}, dx12 {}, queue {}", (UINT64) _dx11Device,
                  (UINT64) _dx11Device5, (UINT64) _dx11Context4, (UINT64) _dx12Device, (UINT64) _dx12CommandQueue);
        return false;
    }

    HRESULT result = S_OK;

    const UINT copyAllocatorCount = std::max<UINT>(_bufferCount != 0 ? _bufferCount : 3, 3);

    if (_copyAllocators.size() != copyAllocatorCount)
    {
        for (auto& allocator : _copyAllocators)
            SafeRelease(allocator);

        _copyAllocators.assign(copyAllocatorCount, nullptr);
    }

    if (_copyAllocatorFenceValues.size() != copyAllocatorCount)
        _copyAllocatorFenceValues.assign(copyAllocatorCount, 0);

    if (_copyCommandLists.size() != copyAllocatorCount)
    {
        for (auto& commandList : _copyCommandLists)
            SafeRelease(commandList);

        _copyCommandLists.assign(copyAllocatorCount, nullptr);
    }

    for (UINT i = 0; i < copyAllocatorCount; ++i)
    {
        if (_copyAllocators[i] != nullptr)
            continue;

        result = _dx12Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&_copyAllocators[i]));
        if (FAILED(result))
        {
            LOG_ERROR("CreateCommandAllocator[{}] failed: {:X}", i, (UINT) result);
            return false;
        }
    }

    for (UINT i = 0; i < copyAllocatorCount; ++i)
    {
        if (_copyCommandLists[i] != nullptr)
            continue;

        result = _dx12Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, _copyAllocators[i], nullptr,
                                                IID_PPV_ARGS(&_copyCommandLists[i]));
        if (FAILED(result))
        {
            LOG_ERROR("CreateCommandList failed: {:X}", (UINT) result);
            return false;
        }

        _copyCommandLists[i]->Close();
    }

    if (_copyFence == nullptr)
    {
        result = _dx12Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_copyFence));
        if (FAILED(result))
        {
            LOG_ERROR("Create copy fence failed: {:X}", (UINT) result);
            return false;
        }
    }

    if (_copyFenceEvent == nullptr)
    {
        _copyFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (_copyFenceEvent == nullptr)
        {
            LOG_ERROR("CreateEvent for copy fence failed");
            return false;
        }
    }

    if (_dx11Fence == nullptr)
    {
        result = _dx11Device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&_dx11Fence));
        if (FAILED(result))
        {
            LOG_ERROR("D3D11 CreateFence failed: {:X}", (UINT) result);
            return false;
        }
    }

    if (_sharedFenceHandle == nullptr)
    {
        result = _dx11Fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &_sharedFenceHandle);
        if (FAILED(result))
        {
            LOG_ERROR("CreateSharedHandle for D3D11 fence failed: {:X}", (UINT) result);
            return false;
        }
    }

    if (_dx12SharedFence == nullptr)
    {
        result = _dx12Device->OpenSharedHandle(_sharedFenceHandle, IID_PPV_ARGS(&_dx12SharedFence));
        if (FAILED(result))
        {
            LOG_ERROR("OpenSharedHandle for D3D12 fence failed: {:X}", (UINT) result);
            return false;
        }
    }

    _RefreshCachedSwapchainDesc();

    if (_bufferCount == 0)
    {
        LOG_ERROR("InitInteropObjects resolved zero backbuffers");
        return false;
    }

    _sharedDx11BackBufferCopies.assign(_bufferCount, nullptr);
    _openedDx11BackBuffers.assign(_bufferCount, nullptr);
    _sharedBackBufferHandles.assign(_bufferCount, nullptr);
    _openedDx11BackBufferStates.assign(_bufferCount, D3D12_RESOURCE_STATE_COMMON);

    _interopInitialized = true;
    return true;
}

bool Dx11wDx12SC::_RequestSharedBackBuffer(UINT index)
{
    if (_real == nullptr || _dx11Device == nullptr || _dx12Device == nullptr)
        return false;

    if (_bufferCount == 0)
        _RefreshCachedSwapchainDesc();

    if (index >= _bufferCount)
    {
        LOG_ERROR("backbuffer index {} out of range {}", index, _bufferCount);
        return false;
    }

    if (_sharedDx11BackBufferCopies.size() <= index)
        _sharedDx11BackBufferCopies.resize(_bufferCount, nullptr);

    if (_openedDx11BackBuffers.size() <= index)
        _openedDx11BackBuffers.resize(_bufferCount, nullptr);

    if (_sharedBackBufferHandles.size() <= index)
        _sharedBackBufferHandles.resize(_bufferCount, nullptr);

    if (_openedDx11BackBufferStates.size() <= index)
        _openedDx11BackBufferStates.resize(_bufferCount, D3D12_RESOURCE_STATE_COMMON);

    if (_openedDx11BackBuffers[_currentFakeIndex] != nullptr)
        return true;

    // Read Dx11 sc backbuffer
    ID3D11Texture2D* sourceTexture = nullptr;
    auto result = _real->GetBuffer(index, IID_PPV_ARGS(&sourceTexture));
    if (FAILED(result) || sourceTexture == nullptr)
    {
        LOG_ERROR("GetBuffer({}) failed: {:X}", index, (UINT) result);
        return false;
    }

    // Create shared copy
    IDXGIResource1* dxgiResource = nullptr;
    if (_sharedBackBufferHandles[_currentFakeIndex] == nullptr)
    {
        D3D11_TEXTURE2D_DESC desc = {};
        sourceTexture->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.CPUAccessFlags = 0;
        // Readable as a texture: HDR10 conversion and the external-HUD-less UI extraction sample it.
        desc.BindFlags |= D3D11_BIND_SHADER_RESOURCE;
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

        result = _dx11Device->CreateTexture2D(&desc, nullptr, &_sharedDx11BackBufferCopies[_currentFakeIndex]);
        if (FAILED(result) || _sharedDx11BackBufferCopies[_currentFakeIndex] == nullptr)
        {
            LOG_ERROR("CreateTexture2D shadow buffer {} failed: {:X}", _currentFakeIndex, (UINT) result);
            sourceTexture->Release();
            return false;
        }

        result = _sharedDx11BackBufferCopies[_currentFakeIndex]->QueryInterface(IID_PPV_ARGS(&dxgiResource));
        if (FAILED(result) || dxgiResource == nullptr)
        {
            LOG_ERROR("shadow IDXGIResource1 query {} failed: {:X}", _currentFakeIndex, (UINT) result);
            sourceTexture->Release();
            return false;
        }

        result = dxgiResource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ, nullptr,
                                                  &_sharedBackBufferHandles[_currentFakeIndex]);
        dxgiResource->Release();

        if (FAILED(result) || _sharedBackBufferHandles[_currentFakeIndex] == nullptr)
        {
            LOG_ERROR("shadow CreateSharedHandle {} failed: {:X}", _currentFakeIndex, (UINT) result);
            sourceTexture->Release();
            return false;
        }
    }

    // Dx12 handle to Dx11 shared texture
    result = _dx12Device->OpenSharedHandle(_sharedBackBufferHandles[_currentFakeIndex],
                                           IID_PPV_ARGS(&_openedDx11BackBuffers[_currentFakeIndex]));
    sourceTexture->Release();

    if (FAILED(result) || _openedDx11BackBuffers[_currentFakeIndex] == nullptr)
    {
        LOG_ERROR("OpenSharedHandle for backbuffer {} failed: {:X}", _currentFakeIndex, (UINT) result);
        return false;
    }

    _openedDx11BackBufferStates[_currentFakeIndex] = D3D12_RESOURCE_STATE_COMMON;
    return true;
}

bool Dx11wDx12SC::_CopyDx11BackBufferToShared(UINT index)
{
    if (_currentFakeIndex >= _sharedDx11BackBufferCopies.size() ||
        _sharedDx11BackBufferCopies[_currentFakeIndex] == nullptr)
    {
        return false;
    }

    if (_dx11Context == nullptr || _real == nullptr)
        return false;

    ID3D11Texture2D* sourceTexture = nullptr;
    auto result = _real->GetBuffer(index, IID_PPV_ARGS(&sourceTexture));
    if (FAILED(result) || sourceTexture == nullptr)
    {
        LOG_ERROR("GetBuffer for shadow copy {} failed: {:X}", index, (UINT) result);
        return false;
    }

    LOG_DEBUG("Copying DX11 backbuffer {} sourceTexture: {:X} to shadow copy {:X}", index, (size_t) sourceTexture,
              (size_t) _sharedDx11BackBufferCopies[_currentFakeIndex]);

    _dx11Context->CopyResource(_sharedDx11BackBufferCopies[_currentFakeIndex], sourceTexture);
    // Bake the diagnostic marks beside the native HUD, once per source frame, before
    // the interop fence and HDR/FG. A late overlay on each generated Present can pair
    // newer CPU coordinates with an older HUD image and appear to wander.
    if (State::Instance().gameExe == "ffxiv_dx11.exe")
        FfxivCompanion::DrawSourceMarkers(_dx11Context, _sharedDx11BackBufferCopies[_currentFakeIndex]);
    sourceTexture->Release();
    return true;
}

bool Dx11wDx12SC::_WaitDx11ThenDx12()
{
    if (_dx11Context4 == nullptr || _dx11Fence == nullptr || _dx12CommandQueue == nullptr ||
        _dx12SharedFence == nullptr)
        return false;

    const auto waitValue = _sharedFenceValue++;

    auto result = _dx11Context4->Signal(_dx11Fence, waitValue);
    if (FAILED(result))
    {
        LOG_ERROR("DX11 Signal failed: {:X}", (UINT) result);
        return false;
    }

    _dx11Context4->Flush();

    // Important:
    // Do not block the FG/present queue on the D3D11 shared fence.
    // Isolate the cross-API wait on the interop copy queue.
    result = _dx12CommandQueue->Wait(_dx12SharedFence, waitValue);
    if (FAILED(result))
    {
        LOG_ERROR("interop copy queue Wait on D3D11 fence failed: {:X}", (UINT) result);
        return false;
    }

    return true;
}

bool Dx11wDx12SC::_WaitForCopyAllocator(UINT slot)
{
    if (_copySubmissionFailed) return false;

    if (slot >= _copyAllocatorFenceValues.size())
    {
        LOG_ERROR("copy allocator slot {} out of range {}", slot, _copyAllocatorFenceValues.size());
        return false;
    }

    const auto fenceValue = _copyAllocatorFenceValues[slot];

    if (fenceValue == 0)
        return true;

    if (_copyFence == nullptr || _copyFenceEvent == nullptr) return false;
    const auto completedValue = _copyFence->GetCompletedValue();
    if (completedValue == UINT64_MAX) return false;
    if (completedValue >= fenceValue)
        return true;

    auto result = _copyFence->SetEventOnCompletion(fenceValue, _copyFenceEvent);
    if (FAILED(result))
    {
        LOG_ERROR("copy allocator fence SetEventOnCompletion failed. slot {}, fence {}, completed {}, result {:X}",
                  slot, fenceValue, completedValue, (UINT) result);
        return false;
    }

    const auto waitResult = WaitForSingleObject(_copyFenceEvent, 5000);
    if (waitResult != WAIT_OBJECT_0)
    {
        LOG_ERROR("copy allocator fence wait failed. slot {}, fence {}, completed {}, waitResult {:X}", slot,
                  fenceValue, _copyFence->GetCompletedValue(), waitResult);
        return false;
    }

    const auto retiredValue = _copyFence->GetCompletedValue();
    if (retiredValue == UINT64_MAX || retiredValue < fenceValue) return false;

    return true;
}

bool Dx11wDx12SC::_CopyDx11SharedToDx12FGBackBuffer(UINT dx11Index)
{
    if (_copyAllocators.empty() || _copyCommandLists.empty() || _dx12CommandQueue == nullptr || _copyFence == nullptr ||
        _fgSwapChain == nullptr || _currentFakeIndex >= _openedDx11BackBuffers.size() ||
        _openedDx11BackBuffers[_currentFakeIndex] == nullptr)
    {
        return false;
    }

    const UINT copySlot = _currentFakeIndex;
    auto allocator = _copyAllocators[copySlot];

    if (allocator == nullptr)
        return false;

    if (!_WaitForCopyAllocator(copySlot))
        return false;

    auto result = allocator->Reset();
    if (FAILED(result))
    {
        LOG_ERROR("copy allocator[{}] reset failed: {:X}", copySlot, (UINT) result);
        return false;
    }

    result = _hdrOutput ? Hdr10::ResetCommands(_copyCommandLists[copySlot], allocator)
                        : _copyCommandLists[copySlot]->Reset(allocator, nullptr);
    if (FAILED(result))
    {
        LOG_ERROR("copy command list reset failed: {:X}", (UINT) result);
        return false;
    }

    UINT fgIndex = _fgSwapChain->GetCurrentBackBufferIndex();
    LOG_DEBUG("dx11Index {}, fgIndex {}", dx11Index, fgIndex);

    ID3D12Resource* fgBackBuffer = nullptr;
    result = _fgSwapChain->GetBuffer(fgIndex, IID_PPV_ARGS(&fgBackBuffer));
    if (FAILED(result) || fgBackBuffer == nullptr)
    {
        LOG_ERROR("FG GetBuffer({}) failed: {:X}", fgIndex, (UINT) result);
        _copyCommandLists[copySlot]->Close();
        return false;
    }

    auto sourceBefore = _openedDx11BackBufferStates[copySlot];
    ID3D12Resource* copySource = _openedDx11BackBuffers[copySlot];
    bool hudlessSource = false;

    if (_uiFreeThisFrame)
    {
        ID3D12Resource* hudless = _hudlessSlot == copySlot && copySlot < _openedHudless.size()
                                      ? _openedHudless[copySlot]
                                      : nullptr;
        const auto fgDesc = fgBackBuffer->GetDesc();
        if (hudless != nullptr)
        {
            const auto hudlessDesc = hudless->GetDesc();
            if (hudlessDesc.Width == fgDesc.Width && hudlessDesc.Height == fgDesc.Height &&
                (_hdrOutput || ExternalHudlessTypedFormat(hudlessDesc.Format) == ExternalHudlessTypedFormat(fgDesc.Format)))
            {
                copySource = hudless;
                sourceBefore = D3D12_RESOURCE_STATE_COMMON; // shared HUD-less copies rest in COMMON
                hudlessSource = true;
            }
        }

        if (!hudlessSource)
            _uiFreeThisFrame = false; // fall back to the normal frame (UI baked in) for this frame
    }
    if (_hdrOutput)
    {
        auto scene=FfxivSceneHdr::Open(_dx12Device,(UINT)copySource->GetDesc().Width,copySource->GetDesc().Height);
        copySource = Hdr10::Convert(_dx12Device, _copyCommandLists[copySlot], copySource, sourceBefore,scene);
        if(copySource && Config::Instance()->FfxivHDRMode.value_or_default()==1 && _fg && _fg->IsActive()){
            // The original HUD-less copy was recorded on the upscaler list.
            // This queue now waits on DX11 (and its upscaler interop completion),
            // so both colour conversions can use the same immutable scene pair.
            Microsoft::WRL::ComPtr<ID3D12Resource> nativeHudless;Dx12Resource hudless{};
            const auto frame=_fg->GetIndex();
            {auto input=_fg->GetResource(FG_ResourceType::HudlessColor,frame);
             if(input && !input->hdrEncoded){hudless=*input.resource;nativeHudless=input->GetResource();}}
            if(nativeHudless){
                auto* hdrHudless=Hdr10::Convert(_dx12Device,_copyCommandLists[copySlot],nativeHudless.Get(),hudless.state,scene);
                if(hdrHudless){hudless.resource=hdrHudless;hudless.copy=nullptr;hudless.hdrEncoded=true;
                    hudless.cmdList=_copyCommandLists[copySlot];hudless.state=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                    hudless.validity=FG_ResourceValidity::ValidNow;hudless.frameIndex=frame;
                    _fg->SetResource(&hudless);}
            }
        }
        if (!copySource) { fgBackBuffer->Release(); _copyCommandLists[copySlot]->Close(); return false; }
        sourceBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    }

    TransitionResource(_copyCommandLists[copySlot], copySource, sourceBefore,
                       D3D12_RESOURCE_STATE_COPY_SOURCE);
    TransitionResource(_copyCommandLists[copySlot], fgBackBuffer, D3D12_RESOURCE_STATE_PRESENT,
                       D3D12_RESOURCE_STATE_COPY_DEST);

    _copyCommandLists[copySlot]->CopyResource(fgBackBuffer, copySource);
    // SDR captures preserve the pre-HDR ReShade image; HDR captures use the
    // converted frame. Opti's menu and external Companion window are later.
    auto hdrCapture = _hdrOutput ? Hdr10::Screenshot::Prepare(_handle, _dx12Device,
        _copyCommandLists[copySlot], copySource, _openedDx11BackBuffers[copySlot],
        _openedDx11BackBufferStates[copySlot]) : nullptr;

    TransitionResource(_copyCommandLists[copySlot], fgBackBuffer, D3D12_RESOURCE_STATE_COPY_DEST,
                       D3D12_RESOURCE_STATE_PRESENT);
    TransitionResource(_copyCommandLists[copySlot], copySource, D3D12_RESOURCE_STATE_COPY_SOURCE,
                       sourceBefore);

    if (!hudlessSource)
        _openedDx11BackBufferStates[copySlot] = D3D12_RESOURCE_STATE_COMMON;

    fgBackBuffer->Release();

    result = _copyCommandLists[copySlot]->Close();
    if (FAILED(result))
    {
        LOG_ERROR("copy command list close failed: {:X}", (UINT) result);
        return false;
    }

    ID3D12CommandList* lists[] = { _copyCommandLists[copySlot] };
    if (_hdrOutput) Hdr10::ExecuteCommands(_dx12CommandQueue, 1, lists);
    else _dx12CommandQueue->ExecuteCommandLists(1, lists);
    Hdr10::Screenshot::Submit(hdrCapture, _dx12CommandQueue);

    const auto signalValue = ++_copyFenceValue;

    result = _dx12CommandQueue->Signal(_copyFence, signalValue);
    if (FAILED(result))
    {
        // Work was submitted, but its completion cannot be proved. Never
        // reset its allocator or destroy its resources using an older fence.
        _copySubmissionFailed = true;
        LOG_ERROR("interop copy fence signal failed: {:X}", (UINT) result);
        return false;
    }

    _copyAllocatorFenceValues[copySlot] = signalValue;
    _lastInteropCopyFenceValue = signalValue;

    return true;
}

bool Dx11wDx12SC::_WaitForInteropCopyOnPresentQueue()
{
    if (_copySubmissionFailed || _copyFence == nullptr)
        return false;

    // A plain HDR presenter uses the same queue as the interop copy. Queue
    // ordering already covers the copy; no FG instance or second queue exists.
    if (FGHooks::IsDx12InteropPresentSC(_fgSwapChain))
        return true;
    if (_fg == nullptr || _fg->GetCommandQueue() == nullptr)
        return false;

    if (_lastInteropCopyFenceValue == 0)
        return true;

    auto result = _fg->GetCommandQueue()->Wait(_copyFence, _lastInteropCopyFenceValue);
    if (FAILED(result))
    {
        LOG_ERROR("present queue Wait on interop copy fence failed: {:X}", (UINT) result);
        return false;
    }

    return true;
}

bool Dx11wDx12SC::_WaitForCopyQueueIdle()
{
    if (_copySubmissionFailed) return false;

    UINT64 waitValue = _lastInteropCopyFenceValue;

    for (const auto fenceValue : _copyAllocatorFenceValues)
        waitValue = std::max(waitValue, fenceValue);

    if (waitValue == 0)
        return true;

    if (_copyFence == nullptr || _copyFenceEvent == nullptr) return false;
    const auto completedValue = _copyFence->GetCompletedValue();
    if (completedValue == UINT64_MAX) return false;
    if (completedValue >= waitValue)
        return true;

    auto result = _copyFence->SetEventOnCompletion(waitValue, _copyFenceEvent);
    if (FAILED(result))
    {
        LOG_ERROR("copy queue idle SetEventOnCompletion failed. fence {}, completed {}, result {:X}", waitValue,
                  completedValue, (UINT) result);
        return false;
    }

    const auto waitResult = WaitForSingleObject(_copyFenceEvent, 5000);
    if (waitResult != WAIT_OBJECT_0)
    {
        LOG_ERROR("copy queue idle wait failed. fence {}, completed {}, waitResult {:X}", waitValue,
                  _copyFence->GetCompletedValue(), waitResult);
        return false;
    }

    const auto retiredValue = _copyFence->GetCompletedValue();
    if (retiredValue == UINT64_MAX || retiredValue < waitValue) return false;

    for (auto& fenceValue : _copyAllocatorFenceValues)
        fenceValue = 0;

    _lastInteropCopyFenceValue = 0;

    return true;
}

void Dx11wDx12SC::_ReleaseInteropBackBuffers()
{
    _interopInitialized = false;

    _ReleaseExternalHudless();

    for (auto& resource : _openedDx11BackBuffers)
        SafeRelease(resource);

    for (auto& texture : _sharedDx11BackBufferCopies)
        SafeRelease(texture);

    for (auto& handle : _sharedBackBufferHandles)
        SafeCloseHandle(handle);

    _openedDx11BackBufferStates.clear();
    _openedDx11BackBuffers.clear();
    _sharedDx11BackBufferCopies.clear();
    _sharedBackBufferHandles.clear();
}

void Dx11wDx12SC::_ReleaseInteropObjects()
{
    if (!_WaitForCopyQueueIdle())
    {
        // Raw COM ownership is intentionally retained on this failure path.
        // A timeout or failed signal cannot justify freeing GPU-visible memory.
        _interopInitialized = false;
        LOG_ERROR("Interop cleanup deferred: copy completion could not be established");
        return;
    }
    _ReleaseInteropBackBuffers();

    _lastInteropCopyFenceValue = 0;

    SafeRelease(_dx12SharedFence);
    SafeRelease(_dx11Fence);
    SafeCloseHandle(_dx11FenceEvent);
    SafeCloseHandle(_sharedFenceHandle);

    for (auto& cmdList : _copyCommandLists)
        SafeRelease(cmdList);

    _copyCommandLists.clear();

    for (auto& allocator : _copyAllocators)
        SafeRelease(allocator);

    _copyAllocators.clear();
    _copyAllocatorFenceValues.clear();

    SafeRelease(_copyFence);
    SafeCloseHandle(_copyFenceEvent);
    _copyFenceValue = 1;

    _interopInitialized = false;
}

void Dx11wDx12SC::_RefreshCachedSwapchainDesc()
{
    _bufferCount = ResolveBufferCount(_real, _real1);
    _bufferFormat = ResolveBufferFormat(_real, _real1);
    _currentFakeIndex = _bufferCount > 0 ? _currentFakeIndex : 0;
}

UINT Dx11wDx12SC::_GetDx11BackBufferIndexForPresent() const
{
    if (_real3 != nullptr)
        return _real3->GetCurrentBackBufferIndex();

    return _bufferCount > 0 ? _currentFakeIndex : 0;
}

void Dx11wDx12SC::_AdvanceFakeBackBufferIndex() { _currentFakeIndex = (_currentFakeIndex + 1) % _bufferCount; }

// ---------------------------------------------------------------------------------------------
// External HUD-less (ReShade add-on)
// ---------------------------------------------------------------------------------------------

static DXGI_FORMAT ExternalHudlessTypedFormat(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default:
        return format;
    }
}

void Dx11wDx12SC::_ReleaseExternalHudlessSlot(UINT slot)
{
    if (slot < _openedHudless.size())
        SafeRelease(_openedHudless[slot]);

    if (slot < _sharedHudlessCopies.size())
        SafeRelease(_sharedHudlessCopies[slot]);

    if (slot < _sharedHudlessHandles.size())
        SafeCloseHandle(_sharedHudlessHandles[slot]);
}

void Dx11wDx12SC::_ReleaseExternalHudless()
{
    // The HUD-less copies are read on the FG queue (not the interop copy queue), so drain
    // that queue before releasing them. Only happens on resize/teardown.
    bool anyOpened = false;
    for (auto resource : _openedHudless)
        anyOpened |= resource != nullptr;

    auto fg = State::Instance().currentFG;
    auto fgQueue = fg != nullptr ? fg->GetCommandQueue() : nullptr;

    bool completed = !anyOpened;
    if (anyOpened && fgQueue != nullptr && _dx12Device != nullptr)
    {
        ID3D12Fence* fence = nullptr;
        if (SUCCEEDED(_dx12Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))) && fence != nullptr)
        {
            HANDLE event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
            if (event != nullptr && SUCCEEDED(fgQueue->Signal(fence, 1)) &&
                SUCCEEDED(fence->SetEventOnCompletion(1, event)))
            {
                completed = WaitForSingleObject(event, 2000) == WAIT_OBJECT_0 &&
                    fence->GetCompletedValue() != UINT64_MAX && fence->GetCompletedValue() >= 1;
            }

            if (event != nullptr)
                CloseHandle(event);

            fence->Release();
        }
    }

    if (!completed) {
        // Keep raw owning references for process lifetime when completion cannot be proved.
        // Clear the wrapper slots so resize cannot overwrite/release those abandoned resources.
        LOG_WARN("External HUD-less cleanup: retaining unresolved GPU ownership for process lifetime");
        _openedHudless.clear(); _sharedHudlessCopies.clear(); _sharedHudlessHandles.clear();
        _uiImages.clear(); _uiImageStates.clear(); _hudlessPending=false;
        (void)_uiExtract.release(); // Its descriptors/PSO may be referenced by the unresolved recording too.
        UiPaste::Release(); ExternalHudless::Clear(); return;
    }

    for (UINT i = 0; i < (UINT) _sharedHudlessCopies.size(); ++i)
        _ReleaseExternalHudlessSlot(i);

    _openedHudless.clear();
    _sharedHudlessCopies.clear();
    _sharedHudlessHandles.clear();
    _hudlessPending = false;

    for (auto& image : _uiImages)
        SafeRelease(image);

    _uiImages.clear();
    _uiImageStates.clear();

    UiPaste::Release();

    ExternalHudless::Clear();
}

void Dx11wDx12SC::_CopyExternalHudlessToShared()
{
    _hudlessPending = false;

    if (!ExternalHudless::Active()) { ExternalHudless::Clear(); return; }
    ID3D11Texture2D* source = ExternalHudless::Take();
    if (source == nullptr)
        return;

    auto fg = State::Instance().currentFG;
    if (fg == nullptr || !fg->IsActive() || fg->IsPaused() || !Config::Instance()->FGEnabled.value_or_default())
    {
        source->Release();
        ExternalHudless::MarkRejected("Frame generation is not active");
        return;
    }

    const UINT slot = _currentFakeIndex;
    if (_dx11Context == nullptr || _dx12Device == nullptr || slot >= _sharedDx11BackBufferCopies.size() ||
        _sharedDx11BackBufferCopies[slot] == nullptr)
    {
        source->Release();
        ExternalHudless::MarkRejected("Interop backbuffer is not ready");
        return;
    }

    D3D11_TEXTURE2D_DESC backBufferDesc {};
    _sharedDx11BackBufferCopies[slot]->GetDesc(&backBufferDesc);

    D3D11_TEXTURE2D_DESC sourceDesc {};
    source->GetDesc(&sourceDesc);

    ExternalHudless::NoteFormats(sourceDesc.Width, sourceDesc.Height, (uint32_t) sourceDesc.Format,
                                 (uint32_t) backBufferDesc.Format);

    if (sourceDesc.Width != backBufferDesc.Width || sourceDesc.Height != backBufferDesc.Height)
    {
        source->Release();
        auto message = std::format("Size {}x{} does not match the backbuffer {}x{}", sourceDesc.Width,
                                   sourceDesc.Height, backBufferDesc.Width, backBufferDesc.Height);
        ExternalHudless::MarkRejected(message.c_str());
        return;
    }

    if (sourceDesc.SampleDesc.Count != 1 || sourceDesc.ArraySize != 1)
    {
        source->Release();
        ExternalHudless::MarkRejected("Multisampled or array textures are not supported");
        return;
    }

    // A HUD-less image must be the display-ready frame minus UI. A different format (typically
    // R16G16B16A16_FLOAT) means it was captured from the HDR scene buffer earlier in the frame.
    const bool normalize = ExternalHudlessNeedsNormalization(ExternalHudlessTypedFormat(sourceDesc.Format),
                                                             ExternalHudlessTypedFormat(backBufferDesc.Format));
    if (!normalize && ExternalHudlessTypedFormat(sourceDesc.Format) != ExternalHudlessTypedFormat(backBufferDesc.Format))
    {
        source->Release();
        auto message = std::format("Format {} does not match the backbuffer format {} (marker is in a Toggler "
                                   "group that runs too early)",
                                   (UINT) sourceDesc.Format, (UINT) backBufferDesc.Format);
        ExternalHudless::MarkRejected(message.c_str());
        return;
    }

    if (_sharedHudlessCopies.size() < _bufferCount)
    {
        _sharedHudlessCopies.resize(_bufferCount, nullptr);
        _openedHudless.resize(_bufferCount, nullptr);
        _sharedHudlessHandles.resize(_bufferCount, nullptr);
    }

    if (slot >= _sharedHudlessCopies.size())
    {
        source->Release();
        ExternalHudless::MarkRejected("Backbuffer slot out of range");
        return;
    }

    const DXGI_FORMAT sharedFormat = normalize ? DXGI_FORMAT_R8G8B8A8_UNORM : ExternalHudlessTypedFormat(sourceDesc.Format);

    // Recreate the slot if the incoming texture changed shape or format (resolution change, preset swap).
    if (_sharedHudlessCopies[slot] != nullptr)
    {
        D3D11_TEXTURE2D_DESC sharedDesc {};
        _sharedHudlessCopies[slot]->GetDesc(&sharedDesc);

        if (sharedDesc.Width != sourceDesc.Width || sharedDesc.Height != sourceDesc.Height ||
            sharedDesc.Format != sharedFormat ||
            bool(sharedDesc.BindFlags & D3D11_BIND_UNORDERED_ACCESS) != (sharedFormat == DXGI_FORMAT_R8G8B8A8_UNORM))
        {
            LOG_INFO("External HUD-less slot {} changed, recreating", slot);
            _ReleaseExternalHudlessSlot(slot);
        }
    }

    if (_sharedHudlessCopies[slot] == nullptr)
    {
        D3D11_TEXTURE2D_DESC desc {};
        desc.Width = sourceDesc.Width;
        desc.Height = sourceDesc.Height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = sharedFormat;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | (sharedFormat == DXGI_FORMAT_R8G8B8A8_UNORM ? D3D11_BIND_UNORDERED_ACCESS : 0); // normalized output is directly shared
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

        auto result = _dx11Device->CreateTexture2D(&desc, nullptr, &_sharedHudlessCopies[slot]);
        if (FAILED(result) || _sharedHudlessCopies[slot] == nullptr)
        {
            LOG_ERROR("CreateTexture2D external hudless slot {} failed: {:X}", slot, (UINT) result);
            source->Release();
            ExternalHudless::MarkRejected("Could not create shared HUD-less texture");
            return;
        }

        IDXGIResource1* dxgiResource = nullptr;
        result = _sharedHudlessCopies[slot]->QueryInterface(IID_PPV_ARGS(&dxgiResource));
        if (SUCCEEDED(result) && dxgiResource != nullptr)
        {
            result = dxgiResource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ, nullptr,
                                                      &_sharedHudlessHandles[slot]);
            dxgiResource->Release();
        }

        if (SUCCEEDED(result) && _sharedHudlessHandles[slot] != nullptr)
            result = _dx12Device->OpenSharedHandle(_sharedHudlessHandles[slot], IID_PPV_ARGS(&_openedHudless[slot]));

        if (FAILED(result) || _openedHudless[slot] == nullptr)
        {
            LOG_ERROR("Sharing external hudless slot {} with D3D12 failed: {:X}", slot, (UINT) result);
            _ReleaseExternalHudlessSlot(slot);
            source->Release();
            ExternalHudless::MarkRejected("Could not share HUD-less texture with D3D12");
            return;
        }

        _openedHudless[slot]->SetName(std::format(L"External HUD-less [{}]", slot).c_str());
        LOG_INFO("External HUD-less slot {} created: {}x{} format {}", slot, desc.Width, desc.Height,
                 (UINT) desc.Format);
    }

    // Same type group (typeless -> typed) is a legal CopyResource.
    if (normalize) {
        if (!_hudlessNormalize) _hudlessNormalize = std::make_unique<ExternalHudlessNormalize>();
        if (!_hudlessNormalize->Copy(_dx11Device, _dx11Context, source, _sharedHudlessCopies[slot])) {
            source->Release(); ExternalHudless::MarkRejected("BGRA to RGBA normalization failed"); return;
        }
    } else _dx11Context->CopyResource(_sharedHudlessCopies[slot], source);
    source->Release();

    _hudlessPending = true;
    _hudlessSlot = slot;
}

void Dx11wDx12SC::_TagExternalHudless()
{
    if (!_hudlessPending)
        return;

    _hudlessPending = false;

    auto fg = State::Instance().currentFG;
    if (fg == nullptr || !fg->IsActive() || fg->IsPaused())
    {
        ExternalHudless::MarkRejected("Frame generation became inactive before Present");
        return;
    }

    if (_hudlessSlot >= _openedHudless.size() || _openedHudless[_hudlessSlot] == nullptr)
    {
        ExternalHudless::MarkRejected("Shared HUD-less slot missing");
        return;
    }

    // Tag it on the frame that is about to be dispatched, i.e. the frame whose backbuffer was
    // just copied above. FG Present executes this UI command list on the FG queue before Dispatch.
    if (_hdrOutput) {
        // The base frame was already converted to PQ. Never tag an SDR REST capture as HDR.
        if (_uiFreeThisFrame) ExternalHudless::MarkTagged();
        return;
    }
    const int fIndex = fg->GetIndexWillBeDispatched();
    auto cmdList = fg->GetUICommandList(fIndex);
    if (cmdList == nullptr)
    {
        ExternalHudless::MarkRejected("Frame generator has no command list for this frame");
        return;
    }

    auto desc = _openedHudless[_hudlessSlot]->GetDesc();

    Dx12Resource resource {};
    resource.type = FG_ResourceType::HudlessColor;
    resource.resource = _openedHudless[_hudlessSlot];
    resource.cmdList = cmdList;
    resource.state = D3D12_RESOURCE_STATE_COMMON;
    resource.validity = FG_ResourceValidity::ValidNow;
    resource.width = desc.Width;
    resource.height = desc.Height;
    resource.frameIndex = fIndex;

    if (fg->SetResource(&resource))
    {
        ExternalHudless::MarkTagged();
        if (_uiFreeThisFrame)
            ExternalHudless::MarkUiRejected("Not needed: frame generation gets the HUD-less frame (UI-free frame generation)");
        else
            _TagExternalUi(fg, fIndex, cmdList);
    }
    else if (Config::Instance()->FGDisableHudless.value_or_default())
        ExternalHudless::MarkRejected("\"Disable HUDless\" is checked in the OptiScaler menu");
    else
        ExternalHudless::MarkRejected("Frame generator refused the HUD-less texture (see OptiScaler.log)");
}

void Dx11wDx12SC::_TagExternalUi(IFGFeature_Dx12* fg, int fIndex, ID3D12GraphicsCommandList* cmdList)
{
    auto& cfg = *Config::Instance();

    if (!cfg.FGExternalUIFromHudless.value_or_default())
        return;

    if (_hdrOutput || Hdr10::Active())
    {
        ExternalHudless::MarkUiRejected("Off while FFXIV HDR10 output is on (DLSS-G refuses a UI image in HDR here)");
        return;
    }

    if (cfg.FGDisableUI.value_or_default())
    {
        ExternalHudless::MarkUiRejected("\"DisableUI\" is true in OptiScaler.ini");
        return;
    }

    const UINT slot = _hudlessSlot;
    if (slot >= _openedDx11BackBuffers.size() || _openedDx11BackBuffers[slot] == nullptr ||
        slot >= _openedHudless.size() || _openedHudless[slot] == nullptr)
    {
        ExternalHudless::MarkUiRejected("Backbuffer or HUD-less copy missing for this slot");
        return;
    }

    ID3D12Resource* finalImage = _openedDx11BackBuffers[slot];
    ID3D12Resource* hudless = _openedHudless[slot];

    const auto finalDesc = finalImage->GetDesc();
    const auto hudlessDesc = hudless->GetDesc();

    if ((finalDesc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) != 0 ||
        (hudlessDesc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) != 0)
    {
        ExternalHudless::MarkUiRejected("Backbuffer copy is not readable as a texture");
        return;
    }

    if (finalDesc.Width != hudlessDesc.Width || finalDesc.Height != hudlessDesc.Height)
    {
        ExternalHudless::MarkUiRejected("Backbuffer and HUD-less sizes differ");
        return;
    }

    if (_uiExtract == nullptr)
        _uiExtract = std::make_unique<UE_Dx12>("UI extract", _dx12Device);

    if (!_uiExtract->IsInit())
    {
        ExternalHudless::MarkUiRejected("UI extraction shader failed to initialise (see OptiScaler.log)");
        return;
    }

    if (_uiImages.size() < _bufferCount)
    {
        _uiImages.resize(_bufferCount, nullptr);
        _uiImageStates.resize(_bufferCount, D3D12_RESOURCE_STATE_COMMON);
    }

    if (slot >= _uiImages.size())
    {
        ExternalHudless::MarkUiRejected("Backbuffer slot out of range");
        return;
    }

    auto& uiImage = _uiImages[slot];

    if (uiImage != nullptr)
    {
        const auto uiDesc = uiImage->GetDesc();
        if (uiDesc.Width != finalDesc.Width || uiDesc.Height != finalDesc.Height)
            SafeRelease(uiImage);
    }

    if (uiImage == nullptr)
    {
        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = finalDesc.Width;
        desc.Height = finalDesc.Height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        auto result = _dx12Device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                           IID_PPV_ARGS(&uiImage));
        if (FAILED(result) || uiImage == nullptr)
        {
            LOG_ERROR("CreateCommittedResource for external UI image slot {} failed: {:X}", slot, (UINT) result);
            ExternalHudless::MarkUiRejected("Could not create the UI image");
            return;
        }

        uiImage->SetName(std::format(L"External UI [{}]", slot).c_str());
        _uiImageStates[slot] = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        LOG_INFO("External UI image slot {} created: {}x{}", slot, desc.Width, desc.Height);
    }

    // Both inputs rest in COMMON between uses (interop copy queue / Streamline tag copy).
    TransitionResource(cmdList, finalImage, D3D12_RESOURCE_STATE_COMMON,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    TransitionResource(cmdList, hudless, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    TransitionResource(cmdList, uiImage, _uiImageStates[slot], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    const bool dispatched = _uiExtract->Dispatch(
        cmdList, finalImage, ExternalHudlessTypedFormat(finalDesc.Format), hudless,
        ExternalHudlessTypedFormat(hudlessDesc.Format), uiImage, cfg.FGExternalUIThreshold.value_or_default(),
        (uint32_t) std::max(0, cfg.FGExternalUIDilation.value_or_default()));

    TransitionResource(cmdList, uiImage, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    _uiImageStates[slot] = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    TransitionResource(cmdList, hudless, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    TransitionResource(cmdList, finalImage, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_COMMON);

    if (!dispatched)
    {
        ExternalHudless::MarkUiRejected("UI extraction dispatch failed");
        return;
    }

    Dx12Resource resource {};
    resource.type = FG_ResourceType::UIColor;
    resource.resource = uiImage;
    resource.cmdList = cmdList;
    resource.state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    resource.validity = FG_ResourceValidity::ValidNow;
    resource.width = finalDesc.Width;
    resource.height = finalDesc.Height;
    resource.frameIndex = fIndex;

    if (fg->SetResource(&resource))
        ExternalHudless::MarkUiTagged();
    else
        ExternalHudless::MarkUiRejected("Frame generator refused the UI image (see OptiScaler.log)");
}

void Dx11wDx12SC::_ProduceUiPaste(bool hudlessReady, UINT slot)
{
    auto& cfg = *Config::Instance();

    if (!ExternalHudless::Active() || !cfg.FGExternalUIPasteAfterFG.value_or_default())
    {
        UiPaste::Invalidate("Off in the menu");
        return;
    }

    if (!hudlessReady)
    {
        UiPaste::Invalidate("No HUD-less image this frame");
        return;
    }


    if (FGHooks::IsDx12InteropPresentSC(_fgSwapChain) || State::Instance().currentFGSwapchain != _fgSwapChain)
    {
        UiPaste::Invalidate("No frame generation presenter");
        return;
    }

    auto fg = _fg != nullptr ? _fg : State::Instance().currentFG;
    if (fg == nullptr || !fg->IsActive() || fg->IsPaused() || fg->GetCommandQueue() == nullptr)
    {
        UiPaste::Invalidate("Frame generation is not active");
        return;
    }

    if (slot >= _openedDx11BackBuffers.size() || _openedDx11BackBuffers[slot] == nullptr ||
        slot >= _openedHudless.size() || _openedHudless[slot] == nullptr)
    {
        UiPaste::Invalidate("Backbuffer or HUD-less copy missing for this slot");
        return;
    }

    ID3D12Resource* finalImage = _openedDx11BackBuffers[slot];
    ID3D12Resource* hudless = _openedHudless[slot];

    const auto finalDesc = finalImage->GetDesc();
    const auto hudlessDesc = hudless->GetDesc();

    if ((finalDesc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) != 0 ||
        (hudlessDesc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) != 0)
    {
        UiPaste::Invalidate("Backbuffer copy is not readable as a texture");
        return;
    }

    if (finalDesc.Width != hudlessDesc.Width || finalDesc.Height != hudlessDesc.Height)
    {
        UiPaste::Invalidate("Backbuffer and HUD-less sizes differ");
        return;
    }

    UiPaste::ProduceParams params {};
    params.hdr = _hdrOutput;
    params.threshold = cfg.FGExternalUIThreshold.value_or_default();
    params.dilation = (uint32_t) std::max(0, cfg.FGExternalUIDilation.value_or_default());
    params.cleanup = cfg.FGExternalUIPasteCleanup.value_or_default() && !_uiFreeThisFrame;

    // Same queue that just waited for the interop copy, so both inputs are complete. This is
    // submitted before FG Present executes the UI command list and DLSS-G's own work.
    UiPaste::Produce(_dx12Device, fg->GetCommandQueue(), finalImage, ExternalHudlessTypedFormat(finalDesc.Format),
                     hudless, ExternalHudlessTypedFormat(hudlessDesc.Format), params);
}

bool Dx11wDx12SC::_UiFreeFrameGenWanted()
{
    auto& cfg = *Config::Instance();
    auto& state = State::Instance();

    if (!ExternalHudless::Active() || !cfg.FGExternalUIPasteAfterFG.value_or_default() || !cfg.FGExternalUIFreeFrameGen.value_or_default())
        return false;

    // Same gates as UiPaste::Paste, so the UI is never removed from a frame nobody pastes it back onto.
    if (state.activeFgOutput != FGOutput::DLSSG || state.fgHudlessCompare || state.isShuttingDown)
        return false;


    if (FGHooks::IsDx12InteropPresentSC(_fgSwapChain) || state.currentFGSwapchain != _fgSwapChain)
        return false;

    auto fg = _fg != nullptr ? _fg : state.currentFG;
    return fg != nullptr && fg->IsActive() && !fg->IsPaused() &&
        _UiPasteReady();
}

bool Dx11wDx12SC::_UiPasteReady() {
    if(_hudlessSlot>=_openedHudless.size() || !_openedHudless[_hudlessSlot])return false;
    const auto desc=_openedHudless[_hudlessSlot]->GetDesc();
    return UiPaste::Ready(_dx12Device, (UINT)desc.Width, desc.Height);
}
