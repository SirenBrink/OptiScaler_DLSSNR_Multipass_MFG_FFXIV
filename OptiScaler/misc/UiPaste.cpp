#include "pch.h"
#include <misc/FramePacingTrace.h>
#include "UiPaste.h"

#include <State.h>
#include <Config.h>
#include <menu/menu_overlay_base.h>
#include <Util.h>
#include "ExternalHudless.h"
#include "FfxivPreviewFGMap.h"
#include "FfxivPreviewPresentation.h"
#include "FfxivPreviewPasteCapture.h"
#include "FfxivPreviewFlow.h"
#include <shaders/hdr/Hdr10.h>
#include <misc/FfxivSceneHdr.h>

#include <shaders/Shader_Common.h>
#include <d3dx/d3dx12.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

namespace
{
// A preview needs three retained captures when its latest write is still on
// the GPU. Extra full-size images are created only while preview FG is active.
constexpr UINT SlotCount = 7;

// Consumer command storage must cover high MFG factors as well as base-frame GPU backlog.
// 32 entries cover four base frames at 8x without allocating additional full-size UI images.
// Never reset an allocator or overwrite descriptors while their fence is incomplete.
constexpr UINT PasteRing = 32;

// Producer descriptor table: t0 final, t1 HUD-less, t2 previous paste image,
// t3 HDR final, t4 preview guide, u0 output.
constexpr UINT ProduceDescriptors = 6;

constexpr D3D12_RESOURCE_STATES ImageRestState = static_cast<D3D12_RESOURCE_STATES>(
    static_cast<int>(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) |
    static_cast<int>(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));

// Do not paste an image this old (game paused presenting, loading screen, alt-tab).
constexpr double StaleImageMs = 250.0;

const char* ProduceShaderCode = R"(
cbuffer Params : register(b0)
{
    float Threshold;
    uint Radius;
    uint Width;
    uint Height;
    uint HasPrevious;
    uint Cleanup;
    uint UseHDR;
    uint UsePreviewMap;
};

Texture2D<float4> FinalTexture : register(t0);
Texture2D<float4> HudlessTexture : register(t1);
Texture2D<float4> PreviousPaste : register(t2);
Texture2D<float4> HdrFinalTexture : register(t3);
Texture2D<float4> PreviewMotion : register(t4);
RWTexture2D<float4> PasteTexture : register(u0);

float PixelDifference(int2 p)
{
    p = clamp(p, int2(0, 0), int2((int) Width - 1, (int) Height - 1));
    float3 d = abs(FinalTexture.Load(int3(p, 0)).rgb - HudlessTexture.Load(int3(p, 0)).rgb);
    return max(max(d.r, d.g), d.b);
}

[numthreads(16, 16, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= Width || id.y >= Height)
        return;

    int2 p = int2(id.xy);
    int r = (int) Radius;
    bool isUi = false;

    // The 3D character can be present in both the final and REST HUD-less
    // images. Keep its mapped preview area in the paste so the two captured
    // poses can actually reach the generated frames.
    if (UsePreviewMap != 0)
        isUi = PreviewMotion.Load(int3(p, 0)).a > 0.75;

    [loop]
    for (int y = -r; y <= r && !isUi; ++y)
    {
        [loop]
        for (int x = -r; x <= r; ++x)
        {
            if (PixelDifference(p + int2(x, y)) > Threshold)
            {
                isUi = true;
                break;
            }
        }
    }

    float3 finalColor = UseHDR != 0 ? HdrFinalTexture.Load(int3(p, 0)).rgb : FinalTexture.Load(int3(p, 0)).rgb;

    if (isUi)
        PasteTexture[id.xy] = float4(finalColor, 1.0);
    else if (HasPrevious != 0 && Cleanup != 0 && PreviousPaste.Load(int3(p, 0)).a > 0.75)
        PasteTexture[id.xy] = float4(finalColor, 0.5);
    else
        PasteTexture[id.xy] = float4(0.0, 0.0, 0.0, 0.0);
}
)";

const char* PasteShaderCode = R"(
cbuffer Params : register(b0)
{
    uint DebugTint;
    uint UsePreviewMotion;
    float PreviewPhase;
    uint Reserved;
    uint UsePreviewFlow;
};

Texture2D<float4> PasteTexture : register(t0);
Texture2D<float4> PreviewMotion : register(t1);
Texture2D<float4> PreviewTexture : register(t2);
Texture2D<float4> PreviousPreviewTexture : register(t3);
Texture2D<float4> PreviewFlow : register(t4);
SamplerState PreviewLinear : register(s0);

bool InPreview(float2 p, float history, uint2 size)
{
    if (any(p < 0.0) || any(p >= float2(size))) return false;
    int2 pixel = clamp(int2(round(p)), int2(0,0), int2(size)-1);
    float4 guide = PreviewMotion.Load(int3(pixel,0));
    return guide.a > .75 && abs(guide.b - history) < .03;
}

float4 VSMain(uint vid : SV_VertexID) : SV_Position
{
    float2 uv = float2((vid << 1) & 2, vid & 2);
    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float4 PSMain(float4 pos : SV_Position) : SV_Target
{
    int2 p = int2(pos.xy);
    float4 c = PasteTexture.Load(int3(p, 0));
    if (UsePreviewMotion != 0)
    {
        float4 guide = PreviewMotion.Load(int3(p, 0));
        if (guide.a > 0.75)
        {
            uint w, h; PasteTexture.GetDimensions(w, h);
            float2 motion = (guide.rg * 255.0 - 128.0) * (128.0 / 255.0);
            if (UsePreviewFlow != 0)
            {
                uint fw, fh; PreviewFlow.GetDimensions(fw, fh);
                float4 flow = PreviewFlow.SampleLevel(PreviewLinear,
                    float2(p) / (float2(fw,fh) * 4.0), 0);
                if (Reserved != 0 && flow.a > 0.99 && flow.b > 0.5 &&
                    InPreview(float2(p) + flow.rg, guide.b, uint2(w,h)))
                {
                    // Quarter-size flow is filtered across silhouette edges.
                    // Validate it against the full-size pixel before replacing
                    // native geometry motion; background flow must not pull
                    // the character outline into a neighbouring surface.
                    float3 current = PreviewTexture.Load(int3(p,0)).rgb;
                    float4 refinedOld = PreviousPreviewTexture.SampleLevel(PreviewLinear,
                        (float2(p) + flow.rg + .5) / float2(w,h), 0);
                    float4 nativeOld = PreviousPreviewTexture.SampleLevel(PreviewLinear,
                        (float2(p) + motion + .5) / float2(w,h), 0);
                    float refinedError = dot(abs(refinedOld.rgb-current), float3(1.0/3.0,1.0/3.0,1.0/3.0));
                    float nativeError = dot(abs(nativeOld.rgb-current), float3(1.0/3.0,1.0/3.0,1.0/3.0));
                    if (refinedOld.a > .75 && refinedError <= max(.002, nativeError * .85))
                        motion = flow.rg;
                }
            }
            if (Reserved != 0)
            {
                // The guide is previous minus current in output pixels. Move
                // both real poses toward the desired time, then blend only the
                // preview region. The rest of the HUD stays on PasteTexture.
                float2 oldPosition = float2(p) + motion * PreviewPhase;
                float2 newPosition = float2(p) - motion * (1.0 - PreviewPhase);
                if (InPreview(oldPosition, guide.b, uint2(w,h)) &&
                    InPreview(newPosition, guide.b, uint2(w,h)))
                {
                    float4 oldPose = PreviousPreviewTexture.SampleLevel(PreviewLinear,
                        (oldPosition + 0.5) / float2(w, h), 0);
                    float4 newPose = PreviewTexture.SampleLevel(PreviewLinear,
                        (newPosition + 0.5) / float2(w, h), 0);
                    if (oldPose.a > 0.75 && newPose.a > 0.75)
                    {
                        // A large disagreement after warping is usually an
                        // occlusion or a bad match, not a surface to dissolve.
                        // Prefer the nearer pose there, preserving both exact
                        // endpoints and normal blending for aligned detail.
                        float3 difference = abs(oldPose.rgb - newPose.rgb);
                        float rejection = smoothstep(.025, .10, max(difference.r,max(difference.g,difference.b)));
                        float weight = lerp(PreviewPhase, step(.5,PreviewPhase), rejection);
                        c = lerp(oldPose, newPose, weight);
                    }
                    else if (newPose.a > 0.75)
                        c = newPose;
                    else if (oldPose.a > 0.75)
                        c = oldPose;
                }
                else
                    c = PreviewTexture.Load(int3(p,0)); // retain this panel at its boundary
            }
            else
            {
                float2 source = float2(p) + motion * PreviewPhase;
                if (all(source >= 0.0) && all(source < float2(w, h)))
                {
                    int2 guidePixel = clamp(int2(round(source)), int2(0, 0), int2(w, h) - 1);
                    float4 sourceGuide = PreviewMotion.Load(int3(guidePixel, 0));
                    if (sourceGuide.a > 0.75 && abs(sourceGuide.b - guide.b) < 0.03)
                    {
                        float4 moved = PreviewTexture.SampleLevel(PreviewLinear,
                            (source + 0.5) / float2(w, h), 0);
                        if (moved.a > 0.75)
                            c = moved;
                    }
                }
            }
        }
    }

    if (c.a <= 0.0)
        discard;

    if (DebugTint != 0)
        c.rgb = lerp(c.rgb, c.a > 0.75 ? float3(1.0, 0.0, 1.0) : float3(0.0, 1.0, 1.0), 0.35);

    return float4(c.rgb, 1.0);
}
)";

struct ProduceConstants
{
    float threshold;
    uint32_t radius;
    uint32_t width;
    uint32_t height;
    uint32_t hasPrevious;
    uint32_t cleanup;
    uint32_t useHDR;
    uint32_t usePreviewMap;
};

enum class SlotState
{
    Empty,
    Ready,
};

struct Slot
{
    ID3D12Resource* image = nullptr;
    ID3D12Resource* previewMotion = nullptr; // owned by previewMotionOwner
    std::shared_ptr<void> previewMotionOwner;
    ID3D12Resource* previewFlow = nullptr;
    ID3D12DescriptorHeap* flowHeap = nullptr;
    UINT64 flowPreviousFrame = 0;
    SlotState state = SlotState::Empty;
    UINT64 frame = 0;
    UINT64 producedValue = 0; // g_produceFence value that completes the write
    UINT64 lastReadValue = 0; // g_pasteFence value that completes the last paste that read it
    double producedAtMs = 0.0;

    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12DescriptorHeap* heap = nullptr;
};

struct Retired
{
    ID3D12Resource* resource = nullptr;
    std::shared_ptr<void> previewMotionOwner;
    UINT64 producedValue = 0;
    UINT64 readValue = 0;
};

std::mutex g_mutex;

ID3D12Device* g_device = nullptr;
bool g_failed = false; // shader/pipeline creation failed on this device

// Producer
ID3D12RootSignature* g_produceRootSig = nullptr;
ID3D12PipelineState* g_producePso = nullptr;
ID3D12PipelineState* g_previewFlowPso = nullptr;
bool g_previewFlowFailed = false;
ID3D12GraphicsCommandList* g_produceList = nullptr;
ID3D12Fence* g_produceFence = nullptr;
UINT64 g_produceValue = 0;
UINT g_csuIncrement = 0;

std::array<Slot, SlotCount> g_slots {};
std::vector<Retired> g_retired;
UINT64 g_width = 0;
UINT g_height = 0;
DXGI_FORMAT g_imageFormat = DXGI_FORMAT_UNKNOWN;
UINT g_missingSamples = 0;
UINT64 g_frame = 0;      // one tick per real frame (Produce or Invalidate)
int g_lastWritten = -1;  // slot written by the previous Produce, if that was the previous frame

// Consumer
ID3D12RootSignature* g_pasteRootSig = nullptr;
ID3DBlob* g_pasteVs = nullptr;
ID3DBlob* g_pastePs = nullptr;
ID3D12PipelineState* g_pastePso = nullptr;
DXGI_FORMAT g_pastePsoFormat = DXGI_FORMAT_UNKNOWN;
ID3D12CommandAllocator* g_pasteAllocators[PasteRing] = {};
UINT64 g_pasteAllocatorValues[PasteRing] = {};
ID3D12GraphicsCommandList* g_pasteList = nullptr;
ID3D12DescriptorHeap* g_pasteCsuHeap = nullptr;
ID3D12DescriptorHeap* g_pasteRtvHeap = nullptr;
UINT g_rtvIncrement = 0;
ID3D12Fence* g_pasteFence = nullptr;
UINT64 g_pasteValue = 0;
UINT64 g_pasteCounter = 0;
UINT64 g_lastPastedFrame = 0;
UINT g_sameFramePastes = 0;
// The game's character preview can update at a different cadence than the
// scene. Preserve its last image and guide across intervening real frames.
int g_previewSlot = -1;
int g_previousPreviewSlot = -1;
int g_olderPreviewSlot = -1;
FfxivPreviewPresentation::Clock g_previewClock;
UINT64 g_previewPairFrame = 0;
UINT64 g_previousPreviewPairFrame = 0;
bool g_previewBatchInterpolate = false;
float g_previewPhaseLow = 1.0f;
float g_previewPhaseHigh = 0.0f;
double g_previewLastUpdateMs = 0.0;
double g_previewPeriodMs = 33.3;
UINT64 g_previewGuidedPastes = 0;
UINT64 g_previewPairedPastes = 0;
UINT64 g_previewUnguidedPastes = 0;
UINT64 g_previewFlowBuilt = 0;
UINT64 g_previewFlowPasted = 0;
UINT g_previewTracedOutputs = 0;
UINT64 g_previewAdvancedBatches = 0;
UINT64 g_previewHeldBatches = 0;
UINT64 g_previewPendingBatches = 0;
UINT64 g_previewMaxSourceLag = 0;
ID3D12CommandQueue* g_checkedQueue = nullptr;
bool g_checkedQueueOk = false;
bool g_loggedFirstPaste = false;
bool g_loggedPreviewMotion = false;

std::atomic<bool> g_debugTint { false };
// Device changes are exceptional; preserve any shared map held by abandoned
// GPU work just as the existing raw D3D12 images are preserved.
std::vector<std::shared_ptr<void>> g_orphanedPreviewMaps;

UiPaste::Status g_status {};
double g_lastSummaryMs = 0.0;

void SetMessageLocked(const char* message)
{
    if (message == nullptr)
        message = "";

    if (strncmp(g_status.lastMessage, message, sizeof(g_status.lastMessage)) == 0)
        return;

    strncpy_s(g_status.lastMessage, sizeof(g_status.lastMessage), message, _TRUNCATE);
    LOG_DEBUG("UI paste: {}", g_status.lastMessage);
}

template <typename T> void ReleaseCom(T*& value)
{
    if (value != nullptr)
    {
        value->Release();
        value = nullptr;
    }
}

UINT64 Completed(ID3D12Fence* fence) { return fence != nullptr ? fence->GetCompletedValue() : 0; }

bool DeviceRemoved()
{
    return (g_produceFence != nullptr && g_produceFence->GetCompletedValue() == UINT64_MAX) ||
           (g_pasteFence != nullptr && g_pasteFence->GetCompletedValue() == UINT64_MAX);
}

DXGI_FORMAT TypedFormat(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default:
        return format;
    }
}

// Same precision as the final image, in a format that supports typed UAV stores everywhere.
DXGI_FORMAT ImageFormatFor(DXGI_FORMAT finalFormat)
{
    switch (TypedFormat(finalFormat))
    {
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        return DXGI_FORMAT_R10G10B10A2_UNORM; // 2-bit alpha still holds 0 / ~0.33-0.67 / 1
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    }
}

bool CreateRootSignature(ID3D12Device* device, const D3D12_ROOT_SIGNATURE_DESC& desc, ID3D12RootSignature** out,
                         const char* name)
{
    ID3DBlob* blob = nullptr;
    ID3DBlob* error = nullptr;
    auto hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error);

    if (FAILED(hr))
    {
        LOG_ERROR("UI paste: {} root signature serialization failed: {:X} {}", name, (UINT) hr,
                  error != nullptr ? (const char*) error->GetBufferPointer() : "");
        ReleaseCom(error);
        ReleaseCom(blob);
        return false;
    }

    ReleaseCom(error);
    hr = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(out));
    blob->Release();

    if (FAILED(hr))
    {
        LOG_ERROR("UI paste: {} CreateRootSignature failed: {:X}", name, (UINT) hr);
        return false;
    }

    return true;
}

ID3D12DescriptorHeap* CreateHeap(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type, UINT count, bool shaderVisible)
{
    ScopedSkipHeapCapture skipHeapCapture {};

    D3D12_DESCRIPTOR_HEAP_DESC desc = {};
    desc.Type = type;
    desc.NumDescriptors = count;
    desc.Flags = shaderVisible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

    ID3D12DescriptorHeap* heap = nullptr;
    if (FAILED(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap))))
        return nullptr;

    return heap;
}

void CreateSrv(ID3D12Resource* resource, DXGI_FORMAT format, D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC desc = {};
    desc.Format = TypedFormat(format);
    desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    desc.Texture2D.MipLevels = 1;
    g_device->CreateShaderResourceView(resource, &desc, handle);
}

void Barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
             D3D12_RESOURCE_STATES after)
{
    if (before == after)
        return;

    auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(resource, before, after);
    list->ResourceBarrier(1, &barrier);
}

void CollectRetiredLocked()
{
    const auto produced = Completed(g_produceFence);
    const auto read = Completed(g_pasteFence);

    std::erase_if(g_retired,
                  [&](Retired& item)
                  {
                      if (item.producedValue > produced || item.readValue > read)
                          return false;

                      ReleaseCom(item.resource);
                      return true;
                  });
}

void RetireImagesLocked()
{
    for (auto& slot : g_slots)
    {
        if (slot.image != nullptr || slot.previewMotionOwner)
            g_retired.push_back({ slot.image, std::move(slot.previewMotionOwner), slot.producedValue, slot.lastReadValue });
        if (slot.previewFlow != nullptr)
            g_retired.push_back({slot.previewFlow, {}, slot.producedValue, slot.lastReadValue});

        slot.image = nullptr;
        slot.previewFlow = nullptr;
        slot.flowPreviousFrame = 0;
        slot.previewMotion = nullptr;
        slot.state = SlotState::Empty;
        slot.frame = 0;
    }

    g_lastWritten = -1;
    g_previewSlot = -1;
    g_previousPreviewSlot = -1;
    g_olderPreviewSlot = -1;
    g_previewClock.Reset();
    g_previewPairFrame = g_previousPreviewPairFrame = 0;
    g_previewLastUpdateMs = 0.0;
    g_previewPeriodMs = 33.3;
    g_width = 0;
    g_height = 0;
    g_imageFormat = DXGI_FORMAT_UNKNOWN;
}

void ForgetDeviceLocked()
{
    // Only on a device change (should not happen during a session). Objects of the old device are
    // dropped without Release: they may still be referenced by work we cannot wait for here.
    g_produceRootSig = nullptr;
    g_producePso = nullptr;
    g_previewFlowPso = nullptr;
    g_previewFlowFailed = false;
    g_produceList = nullptr;
    g_produceFence = nullptr;
    g_produceValue = 0;
    for(auto& slot:g_slots)if(slot.previewMotionOwner)g_orphanedPreviewMaps.push_back(std::move(slot.previewMotionOwner));
    for(auto& item:g_retired)if(item.previewMotionOwner)g_orphanedPreviewMaps.push_back(std::move(item.previewMotionOwner));
    g_slots = {};
    g_retired.clear();
    g_width = 0;
    g_height = 0;
    g_imageFormat = DXGI_FORMAT_UNKNOWN;
    g_lastWritten = -1;
    g_previewSlot = -1;
    g_previousPreviewSlot = -1;
    g_olderPreviewSlot = -1;
    g_previewClock.Reset();
    g_previewPairFrame = g_previousPreviewPairFrame = 0;
    g_previewLastUpdateMs = 0.0;
    g_previewPeriodMs = 33.3;

    g_pasteRootSig = nullptr;
    g_pastePso = nullptr;
    g_pastePsoFormat = DXGI_FORMAT_UNKNOWN;
    for (UINT i = 0; i < PasteRing; ++i)
    {
        g_pasteAllocators[i] = nullptr;
        g_pasteAllocatorValues[i] = 0;
    }
    g_pasteList = nullptr;
    g_pasteCsuHeap = nullptr;
    g_pasteRtvHeap = nullptr;
    g_pasteFence = nullptr;
    g_pasteValue = 0;
    g_checkedQueue = nullptr;
    g_checkedQueueOk = false;
    g_failed = false;
}

bool EnsureProducerLocked(ID3D12Device* device)
{
    if (g_device != device)
    {
        if (g_device != nullptr)
            LOG_WARN("UI paste: D3D12 device changed, recreating");

        ForgetDeviceLocked();
        g_device = device;
    }

    if (g_failed)
        return false;

    if (g_producePso != nullptr)
        return true;

    CD3DX12_DESCRIPTOR_RANGE ranges[2];
    ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 5, 0);
    ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);

    CD3DX12_ROOT_PARAMETER params[2];
    params[0].InitAsConstants(sizeof(ProduceConstants) / 4, 0);
    params[1].InitAsDescriptorTable(2, ranges);

    CD3DX12_STATIC_SAMPLER_DESC flowSampler(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
    CD3DX12_ROOT_SIGNATURE_DESC rootDesc(2, params, 1, &flowSampler, D3D12_ROOT_SIGNATURE_FLAG_NONE);
    if (!CreateRootSignature(device, rootDesc, &g_produceRootSig, "produce"))
    {
        g_failed = true;
        return false;
    }

    ID3DBlob* cs = CompileShader(ProduceShaderCode, "CSMain", "cs_5_0");
    if (cs == nullptr)
    {
        LOG_ERROR("UI paste: failed to compile the paste image shader");
        g_failed = true;
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = g_produceRootSig;
    psoDesc.CS = { cs->GetBufferPointer(), cs->GetBufferSize() };
    auto hr = device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&g_producePso));
    cs->Release();

    if (FAILED(hr))
    {
        LOG_ERROR("UI paste: CreateComputePipelineState failed: {:X}", (UINT) hr);
        g_failed = true;
        return false;
    }

    hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_produceFence));
    if (FAILED(hr))
    {
        LOG_ERROR("UI paste: producer CreateFence failed: {:X}", (UINT) hr);
        ReleaseCom(g_producePso);
        g_failed = true;
        return false;
    }

    g_produceValue = 0;
    g_csuIncrement = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    LOG_INFO("UI paste: producer ready");
    return true;
}

void RecordPreviewFlowLocked(Slot& slot, int previous, ID3D12GraphicsCommandList* list)
{
    slot.flowPreviousFrame = 0;
    if (!slot.previewMotion)
    {
        // Target selection has proved all prior reads/writes completed.
        ReleaseCom(slot.previewFlow);
        return;
    }
    if (previous < 0 || !g_slots[previous].previewMotion || g_previewFlowFailed)
        return;
    if (!g_previewFlowPso)
    {
        auto* shader = CompileShader(FfxivPreviewFlow::FlowShaderCode, "CSMain", "cs_5_0");
        if (!shader) { g_previewFlowFailed = true; return; }
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = g_produceRootSig;
        desc.CS = {shader->GetBufferPointer(), shader->GetBufferSize()};
        const auto result = g_device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&g_previewFlowPso));
        shader->Release();
        if (FAILED(result))
        {
            LOG_WARN("CharaView FG: image-motion pipeline unavailable: {:X}; native guides retained", UINT(result));
            g_previewFlowFailed = true;
            return;
        }
        LOG_INFO("CharaView FG: quarter-resolution image-motion refinement ready");
    }
    const UINT width = (UINT(g_width) + 3) / 4, height = (g_height + 3) / 4;
    if (!slot.previewFlow)
    {
        CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_DEFAULT);
        const auto desc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R16G16B16A16_FLOAT,
            width, height, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        if (FAILED(g_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            ImageRestState, nullptr, IID_PPV_ARGS(&slot.previewFlow)))) return;
        slot.previewFlow->SetName(L"CharaView image-motion refinement");
    }
    if (!slot.flowHeap)
        slot.flowHeap = CreateHeap(g_device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, ProduceDescriptors, true);
    if (!slot.flowHeap) return;

    CD3DX12_CPU_DESCRIPTOR_HANDLE descriptor(slot.flowHeap->GetCPUDescriptorHandleForHeapStart());
    ID3D12Resource* inputs[]{slot.image, g_slots[previous].image, slot.image, slot.image, slot.previewMotion};
    for (unsigned i = 0; i < 5; ++i)
    {
        CreateSrv(inputs[i], i == 4 ? DXGI_FORMAT_R8G8B8A8_UNORM : g_imageFormat, descriptor);
        descriptor.Offset(1, g_csuIncrement);
    }
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    g_device->CreateUnorderedAccessView(slot.previewFlow, nullptr, &uav, descriptor);
    Barrier(list, slot.previewFlow, ImageRestState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Barrier(list, slot.previewMotion, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ID3D12DescriptorHeap* heaps[]{slot.flowHeap};
    const UINT constants[]{UINT(g_width), g_height, width, height};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(g_produceRootSig);
    list->SetPipelineState(g_previewFlowPso);
    list->SetComputeRoot32BitConstants(0, 4, constants, 0);
    list->SetComputeRootDescriptorTable(1, slot.flowHeap->GetGPUDescriptorHandleForHeapStart());
    list->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
    Barrier(list, slot.previewFlow, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, ImageRestState);
    Barrier(list, slot.previewMotion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    slot.flowPreviousFrame = g_slots[previous].frame;
}

bool EnsureConsumerLocked(DXGI_FORMAT backBufferFormat)
{
    if (g_failed || g_device == nullptr)
        return false;

    if (g_pasteRootSig == nullptr)
    {
        CD3DX12_DESCRIPTOR_RANGE range;
        range.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 5, 0);

        CD3DX12_ROOT_PARAMETER params[2];
        params[0].InitAsConstants(5, 0, 0, D3D12_SHADER_VISIBILITY_PIXEL);
        params[1].InitAsDescriptorTable(1, &range, D3D12_SHADER_VISIBILITY_PIXEL);

        CD3DX12_STATIC_SAMPLER_DESC previewSampler(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
        CD3DX12_ROOT_SIGNATURE_DESC rootDesc(2, params, 1, &previewSampler, D3D12_ROOT_SIGNATURE_FLAG_NONE);
        if (!CreateRootSignature(g_device, rootDesc, &g_pasteRootSig, "paste"))
        {
            g_failed = true;
            return false;
        }
    }

    if (g_pasteVs == nullptr)
        g_pasteVs = CompileShader(PasteShaderCode, "VSMain", "vs_5_0");

    if (g_pastePs == nullptr)
        g_pastePs = CompileShader(PasteShaderCode, "PSMain", "ps_5_0");

    if (g_pasteVs == nullptr || g_pastePs == nullptr)
    {
        LOG_ERROR("UI paste: failed to compile the paste shaders");
        g_failed = true;
        return false;
    }

    if (g_pasteFence == nullptr)
    {
        if (FAILED(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_pasteFence))))
        {
            LOG_ERROR("UI paste: paste CreateFence failed");
            g_failed = true;
            return false;
        }

        g_pasteValue = 0;
    }

    if (g_pasteAllocators[0] == nullptr)
    {
        for (UINT i = 0; i < PasteRing; ++i)
        {
            if (FAILED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                        IID_PPV_ARGS(&g_pasteAllocators[i]))))
            {
                LOG_ERROR("UI paste: CreateCommandAllocator failed");
                for (auto& allocator : g_pasteAllocators)
                    ReleaseCom(allocator);
                g_failed = true;
                return false;
            }

            g_pasteAllocatorValues[i] = 0;
        }
    }

    if (g_pasteList == nullptr)
    {
        if (FAILED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_pasteAllocators[0], nullptr,
                                               IID_PPV_ARGS(&g_pasteList))))
        {
            LOG_ERROR("UI paste: CreateCommandList failed");
            g_failed = true;
            return false;
        }

        g_pasteList->Close();
    }

    if (g_pasteCsuHeap == nullptr)
    {
        g_pasteCsuHeap = CreateHeap(g_device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, PasteRing * 5, true);
        g_pasteRtvHeap = CreateHeap(g_device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, PasteRing, false);

        if (g_pasteCsuHeap == nullptr || g_pasteRtvHeap == nullptr)
        {
            LOG_ERROR("UI paste: descriptor heap creation failed");
            ReleaseCom(g_pasteCsuHeap);
            ReleaseCom(g_pasteRtvHeap);
            g_failed = true;
            return false;
        }

        g_rtvIncrement = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        g_csuIncrement = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }

    if (g_pastePso == nullptr || g_pastePsoFormat != backBufferFormat)
    {
        // A previous PSO may still be referenced by in-flight lists; format changes are rare, keep it.
        g_pastePso = nullptr;

        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.pRootSignature = g_pasteRootSig;
        psoDesc.VS = { g_pasteVs->GetBufferPointer(), g_pasteVs->GetBufferSize() };
        psoDesc.PS = { g_pastePs->GetBufferPointer(), g_pastePs->GetBufferSize() };
        psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask =
            D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN | D3D12_COLOR_WRITE_ENABLE_BLUE;
        psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        psoDesc.DepthStencilState.DepthEnable = FALSE;
        psoDesc.DepthStencilState.StencilEnable = FALSE;
        psoDesc.SampleMask = UINT_MAX;
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoDesc.NumRenderTargets = 1;
        psoDesc.RTVFormats[0] = backBufferFormat;
        psoDesc.SampleDesc = { 1, 0 };

        auto hr = g_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&g_pastePso));
        if (FAILED(hr))
        {
            LOG_ERROR("UI paste: CreateGraphicsPipelineState for format {} failed: {:X}", (UINT) backBufferFormat,
                      (UINT) hr);
            g_pastePso = nullptr;
            g_pastePsoFormat = DXGI_FORMAT_UNKNOWN;
            return false;
        }

        g_pastePsoFormat = backBufferFormat;
        LOG_INFO("UI paste: paste pipeline ready for backbuffer format {}", (UINT) backBufferFormat);
    }

    return true;
}

// Newest Ready slot (by frame), regardless of GPU completion. -1 if none.
int NewestReadyLocked()
{
    int best = -1;
    for (int i = 0; i < (int) SlotCount; ++i)
    {
        if (g_slots[i].state == SlotState::Ready && (best < 0 || g_slots[i].frame > g_slots[best].frame))
            best = i;
    }

    return best;
}

bool CreateImageLocked(Slot& slot, UINT index)
{
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = g_width;
    desc.Height = g_height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = g_imageFormat;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    auto hr = g_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, ImageRestState, nullptr,
                                                IID_PPV_ARGS(&slot.image));
    if (FAILED(hr) || slot.image == nullptr)
    {
        LOG_ERROR("UI paste: creating paste image {} ({}x{}, format {}) failed: {:X}", index, g_width, g_height,
                  (UINT) g_imageFormat, (UINT) hr);
        slot.image = nullptr;
        return false;
    }

    slot.image->SetName(std::format(L"UI paste image [{}]", index).c_str());
    slot.lastReadValue = 0;
    LOG_INFO("UI paste: image {} created: {}x{} format {}", index, g_width, g_height, (UINT) g_imageFormat);
    return true;
}

void LogSummaryLocked()
{
    const double now = Util::MillisecondsNow();
    if (now - g_lastSummaryMs < 10000.0)
        return;

    g_lastSummaryMs = now;
    LOG_INFO("UI paste: built {}, pasted {}, skipped {}; last: {}", g_status.produced, g_status.pasted,
             g_status.skipped, g_status.lastMessage);
    if (Config::Instance()->FfxivPreviewFG.value_or_default() && g_previewGuidedPastes)
        LOG_INFO("CharaView FG: guided pastes {}, paired {}, unguided {}, preview period {:.1f} ms; output phases [{:.3f}, {:.3f}]; image-motion built {}, pasted {}",
                  g_previewGuidedPastes, g_previewPairedPastes, g_previewUnguidedPastes, g_previewPeriodMs,
                  g_previewPhaseLow, g_previewPhaseHigh, g_previewFlowBuilt, g_previewFlowPasted);
    g_previewPhaseLow = 1.0f;
    g_previewPhaseHigh = 0.0f;
    if (g_previewAdvancedBatches || g_previewHeldBatches)
        LOG_INFO("CharaView FG timing: advanced {} batches, held {}, GPU-dependent {}, maximum source lag {} real frames",
                 g_previewAdvancedBatches, g_previewHeldBatches, g_previewPendingBatches, g_previewMaxSourceLag);
    g_previewAdvancedBatches = g_previewHeldBatches = g_previewPendingBatches = g_previewMaxSourceLag = 0;
}
} // namespace

void UiPaste::Produce(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Resource* finalImage,
                      DXGI_FORMAT finalFormat, ID3D12Resource* hudless, DXGI_FORMAT hudlessFormat,
                      const ProduceParams& params)
{
    FramePacingTrace::Scope cpuTiming(FramePacingTrace::Phase::HudProduce);
    if (device == nullptr || queue == nullptr || finalImage == nullptr || hudless == nullptr)
        return;

    std::lock_guard lock(g_mutex);
    g_missingSamples=0;

    ++g_frame;
    const int previousWritten = g_lastWritten;
    g_lastWritten = -1;

    if (!EnsureProducerLocked(device))
    {
        SetMessageLocked("Paste shaders failed to initialise (see OptiScaler.log)");
        return;
    }

    if (DeviceRemoved())
        return;

    CollectRetiredLocked();
    LogSummaryLocked();

    const auto finalDesc = finalImage->GetDesc();
    const auto imageFormat = params.hdr ? DXGI_FORMAT_R10G10B10A2_UNORM : ImageFormatFor(finalFormat);

    if (finalDesc.Width != g_width || finalDesc.Height != g_height || imageFormat != g_imageFormat)
    {
        RetireImagesLocked();
        g_width = finalDesc.Width;
        g_height = finalDesc.Height;
        g_imageFormat = imageFormat;
    }

    // The previous frame's image, for clearing UI that disappeared since then. Same queue as this
    // pass, so it is complete by the time this dispatch runs.
    const int previous =
        (previousWritten >= 0 && g_slots[previousWritten].state == SlotState::Ready &&
         g_slots[previousWritten].frame + 1 == g_frame && g_slots[previousWritten].image != nullptr)
            ? previousWritten
            : -1;
    const int newest = NewestReadyLocked();

    // Reuse only a slot whose previous write and every paste of it have completed: never a GPU wait.
    const auto produced = Completed(g_produceFence);
    const auto read = Completed(g_pasteFence);
    int target = -1;

    const bool previewHistoryActive = g_previewSlot >= 0 || g_previousPreviewSlot >= 0 ||
                                      g_olderPreviewSlot >= 0;
    const int slotLimit = previewHistoryActive ? (int) SlotCount : 4;
    for (int i = 0; i < slotLimit; ++i)
    {
        if (i == previous || i == newest)
            continue;

        // A preview commonly updates at 30 Hz while the scene runs faster.
        // Keep its last image until the next preview update, provided it is
        // still fresh; it is also read by the presentation queue below.
        if ((i == g_previewSlot || i == g_previousPreviewSlot || i == g_olderPreviewSlot) &&
            Config::Instance()->FfxivPreviewFG.value_or_default() &&
            Util::MillisecondsNow() - g_slots[i].producedAtMs < 200.0)
            continue;

        // producedValue also guards the slot's allocator and descriptor heap, even after a resize
        // retired the image itself.
        const auto& slot = g_slots[i];
        if (slot.producedValue > produced || (slot.image != nullptr && slot.lastReadValue > read))
            continue;

        if (target < 0 || slot.state == SlotState::Empty ||
            (g_slots[target].state != SlotState::Empty && slot.frame < g_slots[target].frame))
        {
            target = i;
        }
    }

    // Preserve the main HUD if the other slots are all in flight. The
    // preview then uses the unwarped HUD image until a new guide is captured.
    for (int heldIndex : {g_olderPreviewSlot, g_previousPreviewSlot, g_previewSlot})
    {
        if (target >= 0 || heldIndex < 0 || heldIndex == previous || heldIndex == newest)
            continue;
        const auto& held = g_slots[heldIndex];
        if (held.producedValue <= produced && held.lastReadValue <= read)
            target = heldIndex;
    }

    if (target < 0)
    {
        SetMessageLocked("No free UI image this frame (presents are running behind)");
        return;
    }

    auto& slot = g_slots[target];
    if (target == g_previewSlot)
        g_previewSlot = -1;
    if (target == g_previousPreviewSlot)
        g_previousPreviewSlot = -1;
    if (target == g_olderPreviewSlot)
        g_olderPreviewSlot = -1;
    slot.state = SlotState::Empty;
    slot.flowPreviousFrame = 0;
    slot.previewMotion = nullptr;
    slot.previewMotionOwner.reset(); // prior paste reads completed before target selection

    if (slot.image == nullptr && !CreateImageLocked(slot, (UINT) target))
    {
        SetMessageLocked("Could not create the UI paste image");
        return;
    }

    if (slot.allocator == nullptr &&
        FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&slot.allocator))))
    {
        SetMessageLocked("Could not create a command allocator");
        return;
    }

    if (slot.heap == nullptr)
    {
        slot.heap = CreateHeap(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, ProduceDescriptors, true);
        if (slot.heap == nullptr)
        {
            SetMessageLocked("Could not create a descriptor heap");
            return;
        }
    }

    // The slot's previous producer work has completed (checked above), so the allocator is free.
    if (FAILED(slot.allocator->Reset()))
    {
        SetMessageLocked("Command allocator reset failed");
        return;
    }

    HRESULT hr = S_OK;
    if (g_produceList == nullptr)
    {
        hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, slot.allocator, g_producePso,
                                       IID_PPV_ARGS(&g_produceList));
    }
    else
    {
        hr = g_produceList->Reset(slot.allocator, g_producePso);
    }

    if (FAILED(hr))
    {
        LOG_ERROR("UI paste: producer command list reset failed: {:X}", (UINT) hr);
        SetMessageLocked("Command list reset failed");
        return;
    }

    Hdr10::Reset(g_produceList);
    ID3D12Resource* hdrFinal = nullptr;
    if (params.hdr) {
        auto scene = FfxivSceneHdr::Open(device, (UINT)g_width, g_height);
        hdrFinal = Hdr10::Convert(device, g_produceList, finalImage, D3D12_RESOURCE_STATE_COMMON, scene);
        if (!hdrFinal) {
            g_produceList->Close(); Hdr10::Reset(g_produceList);
            SetMessageLocked("HDR HUD conversion unavailable");
            return;
        }
    }
    // Hold the guide before submitting the producer. Its valid pixels identify
    // the preview even when REST's image difference does not include it.
    auto preview = FfxivPreviewFGMap::Open(device, (UINT)g_width, g_height);
    slot.previewMotion = preview.resource;
    slot.previewMotionOwner = std::move(preview.owner);

    CD3DX12_CPU_DESCRIPTOR_HANDLE cpu(slot.heap->GetCPUDescriptorHandleForHeapStart());
    CreateSrv(finalImage, finalFormat, cpu);
    cpu.Offset(1, g_csuIncrement);
    CreateSrv(hudless, hudlessFormat, cpu);
    cpu.Offset(1, g_csuIncrement);
    if (previous >= 0)
        CreateSrv(g_slots[previous].image, g_imageFormat, cpu);
    else
        CreateSrv(finalImage, finalFormat, cpu); // placeholder, HasPrevious = 0
    cpu.Offset(1, g_csuIncrement);

    CreateSrv(hdrFinal ? hdrFinal : finalImage, hdrFinal ? DXGI_FORMAT_R10G10B10A2_UNORM : finalFormat, cpu);
    cpu.Offset(1, g_csuIncrement);
    CreateSrv(slot.previewMotion ? slot.previewMotion : finalImage,
              slot.previewMotion ? DXGI_FORMAT_R8G8B8A8_UNORM : finalFormat, cpu);
    cpu.Offset(1, g_csuIncrement);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.Format = g_imageFormat;
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device->CreateUnorderedAccessView(slot.image, nullptr, &uavDesc, cpu);

    ProduceConstants constants {};
    constants.threshold = std::clamp(params.threshold, 0.0f, 1.0f);
    constants.radius = std::min<uint32_t>(params.dilation, 4);
    constants.width = (uint32_t) g_width;
    constants.height = g_height;
    constants.hasPrevious = previous >= 0 ? 1 : 0;
    constants.cleanup = params.cleanup ? 1 : 0;
    constants.useHDR = params.hdr ? 1 : 0;
    constants.usePreviewMap = slot.previewMotion ? 1 : 0;

    auto list = g_produceList;

    // Both inputs rest in COMMON between uses (interop copy / Streamline tagging / UI extraction).
    Barrier(list, finalImage, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(list, hudless, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (slot.previewMotion)
        Barrier(list, slot.previewMotion, D3D12_RESOURCE_STATE_COMMON,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(list, slot.image, ImageRestState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    ID3D12DescriptorHeap* heaps[] = { slot.heap };
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(g_produceRootSig);
    list->SetPipelineState(g_producePso);
    list->SetComputeRoot32BitConstants(0, sizeof(ProduceConstants) / 4, &constants, 0);
    list->SetComputeRootDescriptorTable(1, slot.heap->GetGPUDescriptorHandleForHeapStart());
    list->Dispatch((UINT) ((g_width + 15) / 16), (g_height + 15) / 16, 1);

    Barrier(list, slot.image, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, ImageRestState);
    if (slot.previewMotion)
        Barrier(list, slot.previewMotion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_COMMON);
    Barrier(list, hudless, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    Barrier(list, finalImage, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);

    RecordPreviewFlowLocked(slot, previous, list);

    hr = list->Close();
    if (FAILED(hr))
    {
        LOG_ERROR("UI paste: producer command list close failed: {:X}", (UINT) hr);
        SetMessageLocked("Command list close failed");
        return;
    }

    ID3D12CommandList* lists[] = { list };
    Hdr10::ExecuteCommands(queue, 1, lists);

    const auto value = ++g_produceValue;
    hr = queue->Signal(g_produceFence, value);
    if (FAILED(hr))
    {
        // The write is submitted but has no completion token: never reuse or paste this slot.
        LOG_ERROR("UI paste: producer Signal failed: {:X}", (UINT) hr);
        slot.producedValue = UINT64_MAX;
        SetMessageLocked("Fence signal failed");
        return;
    }

    slot.state = SlotState::Ready;
    slot.frame = g_frame;
    slot.producedValue = value;
    slot.producedAtMs = Util::MillisecondsNow();
    if (slot.flowPreviousFrame) ++g_previewFlowBuilt;
    if (slot.previewMotion)
    {
        if (g_previewLastUpdateMs > 0.0)
        {
            const double period = slot.producedAtMs - g_previewLastUpdateMs;
            if (period >= 18.0 && period <= 100.0)
                g_previewPeriodMs = std::clamp(g_previewPeriodMs * 0.75 + period * 0.25, 20.0, 100.0);
        }
        g_previewLastUpdateMs = slot.producedAtMs;
        g_olderPreviewSlot = g_previousPreviewSlot;
        g_previousPreviewSlot = g_previewSlot;
        g_previewSlot = target;
    }
    if(slot.previewMotion && !g_loggedPreviewMotion){
        g_loggedPreviewMotion=true;
        LOG_INFO("UI paste: first CharaView motion map attached to a real frame ({}x{})",g_width,g_height);
    }
    g_lastWritten = target;

    if (g_status.produced++ == 0)
        LOG_INFO("UI paste: first UI image built ({}x{})", g_width, g_height);

    SetMessageLocked("Active");
}

void UiPaste::MissingSample()
{
    std::lock_guard lock(g_mutex);
    ++g_frame;
    ++g_missingSamples;
    const auto now=Util::MillisecondsNow();
    // Keep at most two missing real frames and 100 ms. Never resurrect expired
    // images, and preserve their GPU fences and monotonic presentation order.
    for(auto& slot:g_slots)if(slot.state==SlotState::Ready &&
        (g_missingSamples>2 || now-slot.producedAtMs>100.0))slot.state=SlotState::Empty;
    // A retained HUD sample must stay still when its matching real-frame
    // preview guides are missing. Keep the owner until prior GPU reads retire.
    for(auto& slot:g_slots)slot.previewMotion=nullptr;
    g_previewSlot=-1;
    g_previousPreviewSlot=-1;
    g_olderPreviewSlot=-1;
    g_previewClock.Reset();
    g_previewPairFrame = g_previousPreviewPairFrame = 0;
    g_lastWritten=-1; // recovery must not use stale mask feedback
    SetMessageLocked(g_missingSamples<=2?"Brief HUD capture gap":"No HUD-less image this frame");
}

void UiPaste::Invalidate(const char* reason)
{
    std::lock_guard lock(g_mutex);

    ++g_frame;
    g_missingSamples=0;
    g_lastWritten = -1;

    for (auto& slot : g_slots)
    {
        if (slot.state == SlotState::Ready)
            slot.state = SlotState::Empty;
        slot.previewMotion = nullptr;
    }
    g_previewSlot = -1;
    g_previousPreviewSlot = -1;
    g_olderPreviewSlot = -1;
    g_previewClock.Reset();
    g_previewPairFrame = g_previousPreviewPairFrame = 0;

    SetMessageLocked(reason);
}

bool UiPaste::Ready(ID3D12Device* device, UINT width, UINT height)
{
    std::lock_guard lock(g_mutex);
    if(g_failed || DeviceRemoved() || g_device!=device || g_width!=width || g_height!=height) return false;
    const auto completed=Completed(g_produceFence);
    const auto now=Util::MillisecondsNow();
    for(const auto& slot:g_slots)
        if(slot.state==SlotState::Ready && slot.image && slot.producedValue<=completed &&
            now-slot.producedAtMs<=StaleImageMs) return true;
    return false;
}

void UiPaste::Release()
{
    if (State::Instance().isShuttingDown)
        return;

    std::lock_guard lock(g_mutex);
    RetireImagesLocked();
    CollectRetiredLocked();
}

void UiPaste::Paste(IDXGISwapChain* swapChain, ID3D12CommandQueue* presentQueue)
{
    FramePacingTrace::Scope cpuTiming(FramePacingTrace::Phase::HudPaste);
    if (swapChain == nullptr || presentQueue == nullptr)
        return;

    auto& state = State::Instance();
    if (!ExternalHudless::Active() || state.isShuttingDown || !Config::Instance()->FGExternalUIPasteAfterFG.value_or_default())
        return;

    // Only DLSS-G is known to route every generated frame through this present.
    if (state.activeFgOutput != FGOutput::DLSSG)
        return;

    // "Show Detected UI" tints the frame DLSS-G receives; pasting untinted UI would hide that view.
    if (state.fgHudlessCompare)
        return;

    auto fg = state.currentFG;
    if (fg == nullptr || !fg->IsActive() || fg->IsPaused())
        return;

    std::lock_guard lock(g_mutex);

    if (g_device == nullptr || g_produceFence == nullptr || g_status.produced == 0 || g_failed)
        return;

    if (DeviceRemoved())
        return;

    // Pick the newest image whose write has completed, never older than one already shown.
    // Steady timing caps the pick at one real frame behind the newest frame the game submitted:
    // that image is almost always complete, so the switch to the next image happens at the same
    // point of every base frame instead of whenever the newest image happens to finish on the GPU
    // (which made world-anchored UI such as nameplates step unevenly). Costs ~1 base frame of UI latency.
    const bool steady = Config::Instance()->FGExternalUIPasteSteadyTiming.value_or_default();
    const UINT64 newestAllowed = steady ? (g_frame > 0 ? g_frame - 1 : 0) : UINT64_MAX;
    const auto produced = Completed(g_produceFence);
    const double now = Util::MillisecondsNow();
    int pick = -1;

    for (int i = 0; i < (int) SlotCount; ++i)
    {
        const auto& slot = g_slots[i];
        if (slot.state != SlotState::Ready || slot.image == nullptr || slot.producedValue > produced)
            continue;

        if (slot.frame > newestAllowed)
            continue;

        if (slot.frame + 2 < g_frame || slot.frame < g_lastPastedFrame || now - slot.producedAtMs > StaleImageMs)
            continue;

        if (pick < 0 || slot.frame > g_slots[pick].frame)
            pick = i;
    }

    // In UI-free frame generation DLSS-G's frames carry no UI at all, so skipping a paste would make
    // the UI blink out. Fall back to the newest completed image, even an older one.
    const auto& cfg = *Config::Instance();
    if (pick < 0 && cfg.FGExternalUIFreeFrameGen.value_or_default())
    {
        for (int i = 0; i < (int) SlotCount; ++i)
        {
            const auto& slot = g_slots[i];
            if (slot.state != SlotState::Ready || slot.image == nullptr || slot.producedValue > produced ||
                now - slot.producedAtMs > StaleImageMs)
            {
                continue;
            }

            if (pick < 0 || slot.frame > g_slots[pick].frame)
                pick = i;
        }
    }

    if (pick < 0)
    {
        ++g_status.skipped;
        return;
    }

    if (presentQueue != g_checkedQueue)
    {
        g_checkedQueue = presentQueue;
        g_checkedQueueOk = false;

        ID3D12Device* queueDevice = nullptr;
        if (SUCCEEDED(presentQueue->GetDevice(IID_PPV_ARGS(&queueDevice))) && queueDevice != nullptr)
        {
            g_checkedQueueOk = queueDevice == g_device;
            queueDevice->Release();
        }

        const auto queueDesc = presentQueue->GetDesc();
        LOG_INFO("UI paste: presenting queue {:X} (type {}) {}", (size_t) presentQueue, (int) queueDesc.Type,
                 g_checkedQueueOk ? "accepted" : "is on another D3D12 device, not pasting");
    }

    if (!g_checkedQueueOk)
    {
        ++g_status.skipped;
        SetMessageLocked("The presenting queue is on another D3D12 device");
        return;
    }

    IDXGISwapChain3* swapChain3 = nullptr;
    if (FAILED(swapChain->QueryInterface(IID_PPV_ARGS(&swapChain3))) || swapChain3 == nullptr)
        return;

    const UINT backBufferIndex = swapChain3->GetCurrentBackBufferIndex();
    ID3D12Resource* backBuffer = nullptr;
    auto hr = swapChain3->GetBuffer(backBufferIndex, IID_PPV_ARGS(&backBuffer));
    swapChain3->Release();

    if (FAILED(hr) || backBuffer == nullptr)
    {
        ++g_status.skipped;
        SetMessageLocked("Could not get the presenting backbuffer");
        return;
    }

    // Holding a backbuffer reference past this call would make ResizeBuffers fail.
    struct BackBufferRef
    {
        ID3D12Resource* resource;
        ~BackBufferRef() { resource->Release(); }
    } backBufferRef { backBuffer };

    const auto backBufferDesc = backBuffer->GetDesc();
    auto& slot = g_slots[pick];
    // Keep the chosen pair stable through the native present burst. A newer
    // GPU capture completing halfway through it must not reset preview motion.
    const bool samePreviewBatch = g_previewClock.Batch() == g_frame;
    int previewPick = -1;
    // Avoid adding fresh-producer dependencies from native present callbacks
    // while the menu slows presentation. The completed HUD retains the
    // DLSS/NR preview; interpolation resumes when the menu closes.
    if (Config::Instance()->FfxivPreviewFG.value_or_default() && !MenuOverlayBase::IsVisible())
    {
        for (int i = 0; i < (int) SlotCount; ++i)
        {
            const auto& candidate = g_slots[i];
            // The ordinary HUD is deliberately one real frame behind for
            // stable timing. The preview has its own interpolation clock and
            // must use the newest submitted capture, even if it is one frame
            // newer than the selected HUD image. Otherwise its paired pose
            // check almost never succeeds at a 30 Hz preview cadence. Ready
            // means ExecuteCommandLists and Signal have already been queued.
            // The present queue waits for that token below; requiring CPU-
            // observed completion here repeats old pairs then skips poses
            // whenever the rendering queue has more than one frame in flight.
            if (candidate.state != SlotState::Ready || !candidate.image || !candidate.previewMotion ||
                candidate.producedValue == UINT64_MAX || candidate.frame > g_frame ||
                now - candidate.producedAtMs > 150.0)
                continue;
            if (samePreviewBatch && candidate.frame != g_previewPairFrame)
                continue;
            if (previewPick < 0 || candidate.frame > g_slots[previewPick].frame)
                previewPick = i;
        }
    }
    int previousPreviewPick = -1;
    if (previewPick >= 0)
    {
        for (int i = 0; i < (int) SlotCount; ++i)
        {
            const auto& earlier = g_slots[i];
            const double previewGap = g_slots[previewPick].producedAtMs - earlier.producedAtMs;
            if (earlier.state != SlotState::Ready || !earlier.image || !earlier.previewMotion ||
                earlier.producedValue == UINT64_MAX || earlier.frame >= g_slots[previewPick].frame ||
                previewGap < 4.0 || previewGap > 100.0 || now - earlier.producedAtMs > 200.0)
                continue;
            if (samePreviewBatch && earlier.frame != g_previousPreviewPairFrame)
                continue;
            if (previousPreviewPick < 0 || earlier.frame > g_slots[previousPreviewPick].frame)
                previousPreviewPick = i;
        }
    }

    if (backBufferDesc.Width != g_width || backBufferDesc.Height != g_height || backBufferDesc.SampleDesc.Count != 1)
    {
        ++g_status.skipped;
        SetMessageLocked("Backbuffer size differs from the UI image (resizing?)");
        return;
    }

    const auto rtvFormat = TypedFormat(backBufferDesc.Format);
    if (!EnsureConsumerLocked(rtvFormat))
    {
        ++g_status.skipped;
        SetMessageLocked("Paste pipeline failed to initialise (see OptiScaler.log)");
        return;
    }

    const UINT ring = (UINT) (g_pasteCounter % PasteRing);
    if (g_pasteAllocatorValues[ring] > Completed(g_pasteFence))
    {
        ++g_status.skipped;
        SetMessageLocked("Presenting queue is running behind, skipped a paste");
        return;
    }

    auto allocator = g_pasteAllocators[ring];
    if (FAILED(allocator->Reset()) || FAILED(g_pasteList->Reset(allocator, g_pastePso)))
    {
        ++g_status.skipped;
        SetMessageLocked("Paste command list reset failed");
        return;
    }

    CD3DX12_CPU_DESCRIPTOR_HANDLE srv(g_pasteCsuHeap->GetCPUDescriptorHandleForHeapStart(), (INT) ring * 5,
                                      g_csuIncrement);
    CD3DX12_GPU_DESCRIPTOR_HANDLE srvGpu(g_pasteCsuHeap->GetGPUDescriptorHandleForHeapStart(), (INT) ring * 5,
                                         g_csuIncrement);
    CD3DX12_CPU_DESCRIPTOR_HANDLE rtv(g_pasteRtvHeap->GetCPUDescriptorHandleForHeapStart(), (INT) ring,
                                      g_rtvIncrement);

    CreateSrv(slot.image, g_imageFormat, srv);
    CD3DX12_CPU_DESCRIPTOR_HANDLE motionSrv(srv, 1, g_csuIncrement);
    CreateSrv(previewPick >= 0 ? g_slots[previewPick].previewMotion : slot.image,
              previewPick >= 0 ? DXGI_FORMAT_R8G8B8A8_UNORM : g_imageFormat, motionSrv);
    CD3DX12_CPU_DESCRIPTOR_HANDLE previewSrv(motionSrv, 1, g_csuIncrement);
    CreateSrv(previewPick >= 0 ? g_slots[previewPick].image : slot.image, g_imageFormat, previewSrv);
    CD3DX12_CPU_DESCRIPTOR_HANDLE previousPreviewSrv(previewSrv, 1, g_csuIncrement);
    CreateSrv(previousPreviewPick >= 0 ? g_slots[previousPreviewPick].image : slot.image,
              g_imageFormat, previousPreviewSrv);
    const bool usePreviewFlow = previewPick >= 0 && previousPreviewPick >= 0 &&
        g_slots[previewPick].previewFlow &&
        g_slots[previewPick].flowPreviousFrame == g_slots[previousPreviewPick].frame;
    CD3DX12_CPU_DESCRIPTOR_HANDLE flowSrv(previousPreviewSrv, 1, g_csuIncrement);
    CreateSrv(usePreviewFlow ? g_slots[previewPick].previewFlow : slot.image,
              usePreviewFlow ? DXGI_FORMAT_R16G16B16A16_FLOAT : g_imageFormat, flowSrv);

    D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
    rtvDesc.Format = rtvFormat;
    rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    g_device->CreateRenderTargetView(backBuffer, &rtvDesc, rtv);

    auto list = g_pasteList;

    // A presentable backbuffer is in PRESENT (== COMMON) whenever Present is called.
    Barrier(list, backBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    if(previewPick >= 0)
        Barrier(list, g_slots[previewPick].previewMotion, D3D12_RESOURCE_STATE_COMMON,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    ID3D12DescriptorHeap* heaps[] = { g_pasteCsuHeap };
    list->SetDescriptorHeaps(1, heaps);
    list->SetGraphicsRootSignature(g_pasteRootSig);
    list->SetPipelineState(g_pastePso);

    struct PasteConstants { UINT debugTint; UINT usePreviewMotion; float phase; UINT reserved; UINT useFlow; };
    const UINT64 previewFrame = previewPick >= 0 ? g_slots[previewPick].frame : 0;
    unsigned multiplier = fg->GetObservedFrameMultiplier();
    if (!multiplier)
        multiplier = std::clamp(fg->GetInterpolatedFrameCount(), 0u, 7u) + 1;
    const bool continuousPair = previousPreviewPick >= 0 && g_previewBatchInterpolate &&
        g_slots[previousPreviewPick].frame == g_previewPairFrame && previewFrame != g_previewPairFrame;
    const auto previewStep = g_previewClock.Plan(g_frame, multiplier, continuousPair);
    // Repeated captures must hold the current pose instead of replaying the
    // previous-to-current transition on the next real-frame batch.
    const bool interpolateBatch = samePreviewBatch ? g_previewBatchInterpolate :
                                  previewFrame != g_previewPairFrame;
    const float previewPhase = previousPreviewPick < 0 ? 0.0f :
                               interpolateBatch ? previewStep.Phase() : 1.0f;
    const PasteConstants constants{g_debugTint.load(std::memory_order_relaxed) ? 1u : 0u,
                                  previewPick >= 0 ? 1u : 0u,
                                  previewPhase,
                                  previousPreviewPick >= 0 ? 1u : 0u,
                                  usePreviewFlow ? 1u : 0u};
    if (previousPreviewPick >= 0 && g_previewFlowBuilt >= 120 && g_previewTracedOutputs < 64 &&
        Config::Instance()->LogToFile.value_or_default())
        LOG_INFO("CharaView FG output {}: batch {}, pair {} -> {}, output {}/{}, phase {:.3f}, flow {}, buffer {} @ {:X}, queue {:X}",
                 ++g_previewTracedOutputs, g_frame, g_slots[previousPreviewPick].frame, previewFrame,
                 previewStep.output, previewStep.count, previewPhase, usePreviewFlow,
                 backBufferIndex, size_t(backBuffer), size_t(presentQueue));
    list->SetGraphicsRoot32BitConstants(0, 5, &constants, 0);
    list->SetGraphicsRootDescriptorTable(1, srvGpu);

    D3D12_VIEWPORT viewport = { 0.0f, 0.0f, (float) g_width, (float) g_height, 0.0f, 1.0f };
    D3D12_RECT scissor = { 0, 0, (LONG) g_width, (LONG) g_height };
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    auto previewCapture = previousPreviewPick >= 0 && previewPick >= 0 ?
        FfxivPreviewPasteCapture::Prepare(g_device, g_slots[previousPreviewPick].image,
            g_slots[previewPick].image, g_slots[previewPick].previewMotion, backBuffer,
            g_slots[previewPick].previewMotionOwner, previewPhase, g_frame,
            g_slots[previousPreviewPick].frame, g_slots[previewPick].frame) : nullptr;
    FfxivPreviewPasteCapture::RecordBefore(previewCapture, list, ImageRestState);
    list->DrawInstanced(3, 1, 0, 0);
    FfxivPreviewPasteCapture::RecordAfter(previewCapture, list);
    if(previewPick >= 0)
        Barrier(list, g_slots[previewPick].previewMotion, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_COMMON);

    Barrier(list, backBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);

    hr = list->Close();
    if (FAILED(hr))
    {
        LOG_ERROR("UI paste: paste command list close failed: {:X}", (UINT) hr);
        ++g_status.skipped;
        SetMessageLocked("Paste command list close failed");
        return;
    }

    // The ordinary HUD is already complete. Preview images may still be in
    // flight: their producer and signal were submitted before this FG batch,
    // so wait on the GPU rather than selecting a stale CPU-completed pose.
    UINT64 requiredValue = slot.producedValue;
    if (previewPick >= 0)
        requiredValue = std::max(requiredValue, g_slots[previewPick].producedValue);
    if (previousPreviewPick >= 0)
        requiredValue = std::max(requiredValue, g_slots[previousPreviewPick].producedValue);
    hr = presentQueue->Wait(g_produceFence, requiredValue);
    if (FAILED(hr))
    {
        LOG_ERROR("UI paste: producer dependency failed: {:X}", (UINT) hr);
        ++g_status.skipped;
        SetMessageLocked("Present queue could not wait for the HUD image");
        return;
    }

    ID3D12CommandList* lists[] = { list };
    presentQueue->ExecuteCommandLists(1, lists);
    FfxivPreviewPasteCapture::DeferFinal(previewCapture, presentQueue);

    const auto value = ++g_pasteValue;
    hr = presentQueue->Signal(g_pasteFence, value);
    if (FAILED(hr))
    {
        // Submitted without a completion token: keep this slot and allocator out of reuse.
        LOG_ERROR("UI paste: paste Signal failed: {:X}", (UINT) hr);
        slot.lastReadValue = UINT64_MAX;
        if (previewPick >= 0)
            g_slots[previewPick].lastReadValue = UINT64_MAX;
        if (previousPreviewPick >= 0)
            g_slots[previousPreviewPick].lastReadValue = UINT64_MAX;
        g_pasteAllocatorValues[ring] = UINT64_MAX;
        ++g_pasteCounter;
        return;
    }

    slot.lastReadValue = value;
    if (previewPick >= 0)
    {
        g_slots[previewPick].lastReadValue = value;
        if (!samePreviewBatch)
        {
            if (interpolateBatch) ++g_previewAdvancedBatches;
            else ++g_previewHeldBatches;
            if (g_slots[previewPick].producedValue > produced) ++g_previewPendingBatches;
            g_previewMaxSourceLag = std::max(g_previewMaxSourceLag, g_frame - previewFrame);
        }
        ++g_previewGuidedPastes;
        if (usePreviewFlow) ++g_previewFlowPasted;
        if (previousPreviewPick >= 0)
        {
            g_slots[previousPreviewPick].lastReadValue = value;
            ++g_previewPairedPastes;
        }
        g_previewClock.Commit(previewStep);
        g_previewPairFrame = previewFrame;
        g_previousPreviewPairFrame = previousPreviewPick >= 0 ? g_slots[previousPreviewPick].frame : 0;
        g_previewBatchInterpolate = interpolateBatch;
        if (previousPreviewPick >= 0)
        {
            g_previewPhaseLow = std::min(g_previewPhaseLow, previewPhase);
            g_previewPhaseHigh = std::max(g_previewPhaseHigh, previewPhase);
        }
    }
    else
    {
        ++g_previewUnguidedPastes;
        g_previewClock.Reset();
        g_previewPairFrame = g_previousPreviewPairFrame = 0;
    }
    g_pasteAllocatorValues[ring] = value;
    ++g_pasteCounter;
    g_sameFramePastes = slot.frame == g_lastPastedFrame ? g_sameFramePastes + 1 : 1;
    g_lastPastedFrame = slot.frame;
    ++g_status.pasted;

    if (!g_loggedFirstPaste)
    {
        g_loggedFirstPaste = true;
        LOG_INFO("UI paste: first paste after frame generation ({}x{}, backbuffer format {})", g_width, g_height,
                 (UINT) rtvFormat);
    }
}

void UiPaste::SetDebugTint(bool enabled) { g_debugTint.store(enabled, std::memory_order_relaxed); }

void UiPaste::FinishPresentDiagnostics() { FfxivPreviewPasteCapture::FinishFinal(); }

bool UiPaste::DebugTint() { return g_debugTint.load(std::memory_order_relaxed); }

UiPaste::Status UiPaste::Snapshot()
{
    std::lock_guard lock(g_mutex);
    return g_status;
}
