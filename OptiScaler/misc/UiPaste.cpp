#include "pch.h"
#include "UiPaste.h"

#include <State.h>
#include <Config.h>
#include <Util.h>
#include "ExternalHudless.h"

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
// Paste images: newest-ready + previous (read by the producer) + one being written + one spare.
constexpr UINT SlotCount = 4;

// Consumer command storage must cover high MFG factors as well as base-frame GPU backlog.
// 32 entries cover four base frames at 8x without allocating additional full-size UI images.
// Never reset an allocator or overwrite descriptors while their fence is incomplete.
constexpr UINT PasteRing = 32;

// Producer descriptor table: t0 final, t1 HUD-less, t2 previous paste image, u0 output.
constexpr UINT ProduceDescriptors = 4;

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
};

Texture2D<float4> FinalTexture : register(t0);
Texture2D<float4> HudlessTexture : register(t1);
Texture2D<float4> PreviousPaste : register(t2);
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

    float3 finalColor = FinalTexture.Load(int3(p, 0)).rgb;

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
};

Texture2D<float4> PasteTexture : register(t0);

float4 VSMain(uint vid : SV_VertexID) : SV_Position
{
    float2 uv = float2((vid << 1) & 2, vid & 2);
    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float4 PSMain(float4 pos : SV_Position) : SV_Target
{
    float4 c = PasteTexture.Load(int3(int2(pos.xy), 0));

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
};

enum class SlotState
{
    Empty,
    Ready,
};

struct Slot
{
    ID3D12Resource* image = nullptr;
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
    UINT64 producedValue = 0;
    UINT64 readValue = 0;
};

std::mutex g_mutex;

ID3D12Device* g_device = nullptr;
bool g_failed = false; // shader/pipeline creation failed on this device

// Producer
ID3D12RootSignature* g_produceRootSig = nullptr;
ID3D12PipelineState* g_producePso = nullptr;
ID3D12GraphicsCommandList* g_produceList = nullptr;
ID3D12Fence* g_produceFence = nullptr;
UINT64 g_produceValue = 0;
UINT g_csuIncrement = 0;

std::array<Slot, SlotCount> g_slots {};
std::vector<Retired> g_retired;
UINT64 g_width = 0;
UINT g_height = 0;
DXGI_FORMAT g_imageFormat = DXGI_FORMAT_UNKNOWN;
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
ID3D12CommandQueue* g_checkedQueue = nullptr;
bool g_checkedQueueOk = false;
bool g_loggedFirstPaste = false;

std::atomic<bool> g_debugTint { false };

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
        if (slot.image != nullptr)
            g_retired.push_back({ slot.image, slot.producedValue, slot.lastReadValue });

        slot.image = nullptr;
        slot.state = SlotState::Empty;
        slot.frame = 0;
    }

    g_lastWritten = -1;
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
    g_produceList = nullptr;
    g_produceFence = nullptr;
    g_produceValue = 0;
    g_slots = {};
    g_retired.clear();
    g_width = 0;
    g_height = 0;
    g_imageFormat = DXGI_FORMAT_UNKNOWN;
    g_lastWritten = -1;

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
    ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0);
    ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);

    CD3DX12_ROOT_PARAMETER params[2];
    params[0].InitAsConstants(sizeof(ProduceConstants) / 4, 0);
    params[1].InitAsDescriptorTable(2, ranges);

    CD3DX12_ROOT_SIGNATURE_DESC rootDesc(2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE);
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

bool EnsureConsumerLocked(DXGI_FORMAT backBufferFormat)
{
    if (g_failed || g_device == nullptr)
        return false;

    if (g_pasteRootSig == nullptr)
    {
        CD3DX12_DESCRIPTOR_RANGE range;
        range.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);

        CD3DX12_ROOT_PARAMETER params[2];
        params[0].InitAsConstants(1, 0, 0, D3D12_SHADER_VISIBILITY_PIXEL);
        params[1].InitAsDescriptorTable(1, &range, D3D12_SHADER_VISIBILITY_PIXEL);

        CD3DX12_ROOT_SIGNATURE_DESC rootDesc(2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE);
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
        g_pasteCsuHeap = CreateHeap(g_device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, PasteRing, true);
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
}
} // namespace

void UiPaste::Produce(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Resource* finalImage,
                      DXGI_FORMAT finalFormat, ID3D12Resource* hudless, DXGI_FORMAT hudlessFormat,
                      const ProduceParams& params)
{
    if (device == nullptr || queue == nullptr || finalImage == nullptr || hudless == nullptr)
        return;

    std::lock_guard lock(g_mutex);

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
    const auto imageFormat = ImageFormatFor(finalFormat);

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

    for (int i = 0; i < (int) SlotCount; ++i)
    {
        if (i == previous || i == newest)
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

    if (target < 0)
    {
        SetMessageLocked("No free UI image this frame (presents are running behind)");
        return;
    }

    auto& slot = g_slots[target];
    slot.state = SlotState::Empty;

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

    auto list = g_produceList;

    // Both inputs rest in COMMON between uses (interop copy / Streamline tagging / UI extraction).
    Barrier(list, finalImage, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(list, hudless, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(list, slot.image, ImageRestState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    ID3D12DescriptorHeap* heaps[] = { slot.heap };
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(g_produceRootSig);
    list->SetPipelineState(g_producePso);
    list->SetComputeRoot32BitConstants(0, sizeof(ProduceConstants) / 4, &constants, 0);
    list->SetComputeRootDescriptorTable(1, slot.heap->GetGPUDescriptorHandleForHeapStart());
    list->Dispatch((UINT) ((g_width + 15) / 16), (g_height + 15) / 16, 1);

    Barrier(list, slot.image, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, ImageRestState);
    Barrier(list, hudless, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    Barrier(list, finalImage, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);

    hr = list->Close();
    if (FAILED(hr))
    {
        LOG_ERROR("UI paste: producer command list close failed: {:X}", (UINT) hr);
        SetMessageLocked("Command list close failed");
        return;
    }

    ID3D12CommandList* lists[] = { list };
    queue->ExecuteCommandLists(1, lists);

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
    g_lastWritten = target;

    if (g_status.produced++ == 0)
        LOG_INFO("UI paste: first UI image built ({}x{})", g_width, g_height);

    SetMessageLocked("Active");
}

void UiPaste::Invalidate(const char* reason)
{
    std::lock_guard lock(g_mutex);

    ++g_frame;
    g_lastWritten = -1;

    for (auto& slot : g_slots)
    {
        if (slot.state == SlotState::Ready)
            slot.state = SlotState::Empty;
    }

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

    CD3DX12_CPU_DESCRIPTOR_HANDLE srv(g_pasteCsuHeap->GetCPUDescriptorHandleForHeapStart(), (INT) ring,
                                      g_csuIncrement);
    CD3DX12_GPU_DESCRIPTOR_HANDLE srvGpu(g_pasteCsuHeap->GetGPUDescriptorHandleForHeapStart(), (INT) ring,
                                         g_csuIncrement);
    CD3DX12_CPU_DESCRIPTOR_HANDLE rtv(g_pasteRtvHeap->GetCPUDescriptorHandleForHeapStart(), (INT) ring,
                                      g_rtvIncrement);

    CreateSrv(slot.image, g_imageFormat, srv);

    D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
    rtvDesc.Format = rtvFormat;
    rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    g_device->CreateRenderTargetView(backBuffer, &rtvDesc, rtv);

    auto list = g_pasteList;

    // A presentable backbuffer is in PRESENT (== COMMON) whenever Present is called.
    Barrier(list, backBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);

    ID3D12DescriptorHeap* heaps[] = { g_pasteCsuHeap };
    list->SetDescriptorHeaps(1, heaps);
    list->SetGraphicsRootSignature(g_pasteRootSig);
    list->SetPipelineState(g_pastePso);

    const UINT debugTint = g_debugTint.load(std::memory_order_relaxed) ? 1u : 0u;
    list->SetGraphicsRoot32BitConstants(0, 1, &debugTint, 0);
    list->SetGraphicsRootDescriptorTable(1, srvGpu);

    D3D12_VIEWPORT viewport = { 0.0f, 0.0f, (float) g_width, (float) g_height, 0.0f, 1.0f };
    D3D12_RECT scissor = { 0, 0, (LONG) g_width, (LONG) g_height };
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->DrawInstanced(3, 1, 0, 0);

    Barrier(list, backBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);

    hr = list->Close();
    if (FAILED(hr))
    {
        LOG_ERROR("UI paste: paste command list close failed: {:X}", (UINT) hr);
        ++g_status.skipped;
        SetMessageLocked("Paste command list close failed");
        return;
    }

    // Already completed (checked above), so this never delays the present; it only makes the
    // dependency explicit on the GPU timeline.
    presentQueue->Wait(g_produceFence, slot.producedValue);

    ID3D12CommandList* lists[] = { list };
    presentQueue->ExecuteCommandLists(1, lists);

    const auto value = ++g_pasteValue;
    hr = presentQueue->Signal(g_pasteFence, value);
    if (FAILED(hr))
    {
        // Submitted without a completion token: keep this slot and allocator out of reuse.
        LOG_ERROR("UI paste: paste Signal failed: {:X}", (UINT) hr);
        slot.lastReadValue = UINT64_MAX;
        g_pasteAllocatorValues[ring] = UINT64_MAX;
        ++g_pasteCounter;
        return;
    }

    slot.lastReadValue = value;
    g_pasteAllocatorValues[ring] = value;
    ++g_pasteCounter;
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

bool UiPaste::DebugTint() { return g_debugTint.load(std::memory_order_relaxed); }

UiPaste::Status UiPaste::Snapshot()
{
    std::lock_guard lock(g_mutex);
    return g_status;
}
