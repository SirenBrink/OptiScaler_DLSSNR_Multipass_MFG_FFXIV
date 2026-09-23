// Included in namespace DlssNr by DlssNr_Dx12.cpp, after the private NR helpers.
// Calls below are serialized by g_nrMutex. Nothing writes the game's NGX parameter block.
namespace DeferredSr
{
constexpr unsigned MarkerCount = 16;
static_assert(MarkerCount == PreSrTiming::Slots);
struct SplitRate
{
    ID3D12Resource *depth = nullptr, *motion = nullptr, *thirdScene = nullptr;
    bool guidesReadable = false, sceneReadable[3] {};
    float sceneScale[3] {1,1,1};
    ResidualFgCamera camera;
    PreSrSplitSchedule schedule;
    ~SplitRate() { for (auto* r : {depth, motion, thirdScene}) if (r) r->Release(); }
};
struct HalfRate
{
    SplitRate split;
    std::unique_ptr<ResidualFg> fg;
    ID3D12Resource *motion = nullptr, *previousMotion = nullptr, *anchorMotion = nullptr,
                   *interpolated = nullptr, *suppression = nullptr, *suppressionTexture = nullptr,
                   *zeroUpload = nullptr, *history[2] {};
    bool ready = false, failed = false, havePrevious = false, previousWasAnchor = false;
    bool motionReadable = false, previousReadable = false, anchorReadable = false;
    bool historyReadable[2] {}, interpolationReadable = false;
    bool suppressionReadable = false;
    unsigned writeIndex = 0;
    float historyScale[2] {1,1};
    unsigned long long lastEpoch = 0, createEpoch = 0, anchorId = 0;
    unsigned long long nrAnchors = 0, skippedNr = 0;
    ResidualFgCamera camera;
    ~HalfRate()
    {
        // Generation's GPU completion markers protect all of these lifetimes.
        fg.reset();
        for (auto* r : { motion, previousMotion, anchorMotion, interpolated, suppression,
                         suppressionTexture, zeroUpload, history[0], history[1] }) if (r) r->Release();
    }
    void Reset() { havePrevious = false; previousWasAnchor = false; split.schedule.Reset(); }
};
struct Generation
{
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr; // identity/reference only; no private submissions
    unsigned w = 0, h = 0, outW = 0, outH = 0, flags = 0;
    DXGI_FORMAT inputFormat {}, outputFormat {};
    ID3D12Resource *edited = nullptr, *residualInput = nullptr, *residualOutput = nullptr, *clean = nullptr,
                   *composed = nullptr, *exposure = nullptr, *readback = nullptr;
    // Lazily allocated and retained until the generation is GPU-complete.
    ID3D12Resource* boundedResidual = nullptr;
    bool trailGuard = false;
    ID3D12QueryHeap* queries = nullptr;
    volatile UINT64* completed = nullptr;
    bool occupied[MarkerCount] {};
    unsigned nextMarker = 0, lastMarker = 0;
    bool everRecorded = false, smallReadable = false, reset = true, failed = false;
    NVSDK_NGX_Parameter* parameters = nullptr;
    NVSDK_NGX_Handle* feature = nullptr;
    unsigned long long createEpoch = 0;
    unsigned long long lastBeginEpoch = 0;
    bool began = false;
    std::unique_ptr<DlssNr_Dx12> codec;
    bool halfRequested = false, approximateCamera = false;
    bool splitWork = false;
    std::string halfStatus;
    bool sampleAndHold = false;
    DlssNrResidualHold hold;
    ID3D12Resource* zeroMotion = nullptr;
    std::unique_ptr<HalfRate> half;
    // Experimental FFXIV motion-onset sampling, retired with this generation.
    ID3D12Resource* motionReadback = nullptr;
    struct MotionSample
    {
        bool pending = false, halfFloat = false;
        unsigned long long time = 0;
        float scaleX = 0, scaleY = 0;
    } motionSamples[MarkerCount];
    PreSrMotionReset::Gate motionGate;
    unsigned long long lastMotionTime = 0;
    uint64_t lightingEvent = 0;
    bool motionSamplingFailed = false;
    // A pan can be detected on a skipped NR frame. Keep its private-history
    // reset until an anchor has actually evaluated, rather than losing it.
    bool privateHistoryResetPending = false;
    PreSrTiming timing;
    PreSrCadence cadence;
    unsigned frameKind[MarkerCount] {};
    unsigned long long completedFrames = 0;

    bool Idle() const { return !everRecorded || completed[lastMarker] != 0; }
    ~Generation()
    {
        if (motionReadback) motionReadback->Release();
        half.reset();
        if (zeroMotion) zeroMotion->Release();
        if (feature && NVNGXProxy::D3D12_ReleaseFeature())
            NVNGXProxy::D3D12_ReleaseFeature()(feature);
        if (parameters && NVNGXProxy::D3D12_DestroyParameters())
            NVNGXProxy::D3D12_DestroyParameters()(parameters);
        if (readback && completed) readback->Unmap(0, nullptr);
        for (auto* r : { edited, residualInput, residualOutput, boundedResidual, clean, composed, exposure, readback })
            if (r) r->Release();
        if (queries) queries->Release();
        if (queue) queue->Release();
        if (device) device->Release();
    }
};

// Record an actual GPU completion marker after EACH seam. A later CPU frame/Present count alone
// does not prove a resource is no longer in flight. Slots aren't reused until the GPU wrote them.
struct Use
{
    Generation& g;
    ID3D12GraphicsCommandList* cmd;
    unsigned slot;
    bool valid;
    Use(Generation& gen, ID3D12GraphicsCommandList* commands) : g(gen), cmd(commands), slot(g.nextMarker)
    {
        valid = !g.occupied[slot] || g.completed[slot] != 0;
        if (!valid) return;
        if (g.occupied[slot] && g.frameKind[slot])
            g.cadence.Record(g.completed[slot], g.timing.Frequency(), g.frameKind[slot]);
        g.frameKind[slot] = 0;
        g.timing.Recycle(slot); // Only this completed slot can be read/reused.
        g.completed[slot] = 0;
        g.occupied[slot] = true;
        g.lastMarker = slot;
        g.everRecorded = true;
        g.nextMarker = (slot + 1) % MarkerCount;
    }
    ~Use()
    {
        if (!valid) return;
        cmd->EndQuery(g.queries, D3D12_QUERY_TYPE_TIMESTAMP, slot);
        cmd->ResolveQueryData(g.queries, D3D12_QUERY_TYPE_TIMESTAMP, slot, 1,
                              g.readback, slot * sizeof(UINT64));
    }
};

// A generation owns NGX features. Its destructor must never run from the CRT's
// DLL unload callbacks, where NVIDIA may already have torn down its runtime.
// Normal retirement and explicit shutdown below still release completed work.
// Unresolved ownership survives until OS process cleanup, as it does for g_nr.
std::unique_ptr<Generation>& current = *new std::unique_ptr<Generation>;
std::vector<std::unique_ptr<Generation>>& retired = *new std::vector<std::unique_ptr<Generation>>;
std::string status = "not started";
int presentationGuideDelay = -1;
struct Pending
{
    ID3D12GraphicsCommandList* cmd = nullptr;
    NVSDK_NGX_Parameter* caller = nullptr;
    ID3D12Resource* output = nullptr;
    unsigned long long epoch = 0;
    float scale = 1;
    bool skipNr = false;
    bool half = false;
} pending;

void Say(const std::string& text)
{
    if (status == text) return;
    status = text;
    LOG_INFO("DLSS-NR deferred DLSS: {}", text);
}
void Cancel()
{
    presentationGuideDelay = -1;
    pending = {};
    if (current) { current->reset = true; current->hold.Reset(); if (current->half) current->half->Reset(); }
}
void Collect()
{
    std::erase_if(retired, [](const auto& g) { return g->Idle(); });
}

unsigned UInt(NVSDK_NGX_Parameter* p, const char* key, unsigned fallback = 0)
{
    unsigned value = fallback;
    p->Get(key, &value);
    return value;
}
float Float(NVSDK_NGX_Parameter* p, const char* key, float fallback)
{
    float value = fallback;
    p->Get(key, &value);
    return std::isfinite(value) ? value : fallback;
}

bool Allocate(Generation& g)
{
    g.edited = CreateScratch(g.device, g.inputFormat, g.w, g.h);
    g.residualInput = CreateScratch(g.device, DXGI_FORMAT_R16G16B16A16_FLOAT, g.w, g.h);
    g.residualOutput = CreateScratch(g.device, DXGI_FORMAT_R16G16B16A16_FLOAT, g.outW, g.outH);
    g.clean = CreateScratch(g.device, g.outputFormat, g.outW, g.outH);
    g.composed = CreateScratch(g.device, g.outputFormat, g.outW, g.outH);
    g.exposure = CreateScratch(g.device, DXGI_FORMAT_R32_FLOAT, 1, 1);
    if (!g.edited || !g.residualInput || !g.residualOutput || !g.clean || !g.composed || !g.exposure) return false;
    g.codec = std::make_unique<DlssNr_Dx12>("Deferred NR contribution", g.device);
    if (!g.codec->IsInit()) return false;
    D3D12_QUERY_HEAP_DESC query {};
    query.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    query.Count = MarkerCount;
    if (FAILED(g.device->CreateQueryHeap(&query, IID_PPV_ARGS(&g.queries)))) return false;
    auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
    auto desc = CD3DX12_RESOURCE_DESC::Buffer(MarkerCount * sizeof(UINT64));
    if (FAILED(g.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
              D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g.readback)))) return false;
    void* mapped = nullptr;
    if (FAILED(g.readback->Map(0, nullptr, &mapped))) return false;
    g.completed = static_cast<volatile UINT64*>(mapped);
    for (unsigned i = 0; i < MarkerCount; ++i) g.completed[i] = 0;
    if (!g.timing.Init(g.device, g.queue))
        LOG_WARN("DLSS-NR PreSR: GPU cost breakdown unavailable; rendering is unchanged");
    return true;
}

// Copies nine texels, not a full texture. Use's timestamp follows these copies;
// consume only completed slots, before Use can recycle their completion marker.
bool ConsumePanOnset(Generation& g, unsigned long long now)
{
    int newest = -1;
    for (unsigned i = 0; i < MarkerCount; ++i)
        if (g.motionSamples[i].pending && g.completed[i] != 0 &&
            (newest < 0 || g.motionSamples[i].time > g.motionSamples[newest].time)) newest = i;
    if (newest < 0 || !g.motionReadback) return false;
    const auto sample = g.motionSamples[newest];
    for (auto& item : g.motionSamples)
        if (item.time <= sample.time) item.pending = false;
    const SIZE_T offset = static_cast<SIZE_T>(newest) * 9 * 512;
    D3D12_RANGE range { offset, offset + 9 * 512 };
    void* mapped = nullptr;
    if (FAILED(g.motionReadback->Map(0, &range, &mapped))) return false;
    std::array<float, 9> xs {}, ys {};
    for (unsigned i = 0; i < 9; ++i)
    {
        const auto* pixel = static_cast<const unsigned char*>(mapped) + offset + i * 512;
        if (sample.halfFloat)
        {
            uint16_t values[2]; std::memcpy(values, pixel, sizeof(values));
            xs[i] = DirectX::PackedVector::XMConvertHalfToFloat(values[0]) * sample.scaleX;
            ys[i] = DirectX::PackedVector::XMConvertHalfToFloat(values[1]) * sample.scaleY;
        }
        else
        {
            float values[2]; std::memcpy(values, pixel, sizeof(values));
            xs[i] = values[0] * sample.scaleX; ys[i] = values[1] * sample.scaleY;
        }
    }
    D3D12_RANGE written { 0, 0 }; g.motionReadback->Unmap(0, &written);
    const float speed = PreSrMotionReset::PanSpeed(xs, ys);
    const bool reset = g.motionGate.Update(speed, sample.time, now);
    if (reset) LOG_INFO("PreSR motion diagnostic: one private-history reset, panSpeed={:.3f}, sampleAge={}ms",
                       speed, now - sample.time);
    return reset;
}

void SamplePan(Generation& g, ID3D12GraphicsCommandList* cmd, ID3D12Resource* motion,
               NVSDK_NGX_Parameter* source, unsigned slot, unsigned long long now)
{
    auto& sample = g.motionSamples[slot]; sample.pending = false;
    const auto elapsed = g.lastMotionTime ? now - g.lastMotionTime : 0;
    g.lastMotionTime = now;
    if (!motion || !elapsed || elapsed > 150 || g.motionSamplingFailed) return;
    const auto desc = motion->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.DepthOrArraySize != 1 ||
        desc.MipLevels != 1 || desc.SampleDesc.Count != 1 || desc.Width < g.w || desc.Height < g.h ||
        (desc.Format != DXGI_FORMAT_R16G16_FLOAT && desc.Format != DXGI_FORMAT_R32G32_FLOAT)) return;
    if (!g.motionReadback)
    {
        auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
        auto buffer = CD3DX12_RESOURCE_DESC::Buffer(MarkerCount * 9 * 512);
        if (FAILED(g.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                   D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g.motionReadback))))
        { g.motionSamplingFailed = true; LOG_WARN("PreSR motion diagnostic: readback unavailable"); return; }
        LOG_INFO("PreSR motion diagnostic: sampling enabled; reset on pan onset only, cooldown 750ms, rearm quiet 250ms");
    }
    const auto arrival = (D3D12_RESOURCE_STATES)Config::Instance()->MVResourceBarrier.value_or(
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(cmd, motion, arrival, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION from {}; from.pResource = motion;
    from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    for (unsigned i = 0; i < 9; ++i)
    {
        const unsigned x = (i % 3 + 1) * g.w / 4, y = (i / 3 + 1) * g.h / 4;
        D3D12_BOX box { x, y, 0, x + 1, y + 1, 1 };
        D3D12_TEXTURE_COPY_LOCATION to {}; to.pResource = g.motionReadback;
        to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        to.PlacedFootprint.Offset = (static_cast<UINT64>(slot) * 9 + i) * 512;
        to.PlacedFootprint.Footprint = { desc.Format, 1, 1, 1, 256 };
        cmd->CopyTextureRegion(&to, 0, 0, 0, &from, &box);
    }
    Barrier(cmd, motion, D3D12_RESOURCE_STATE_COPY_SOURCE, arrival);
    sample = { true, desc.Format == DXGI_FORMAT_R16G16_FLOAT, now,
        Float(source, NVSDK_NGX_Parameter_MV_Scale_X, 1) / g.w * (1000.0f / elapsed),
        Float(source, NVSDK_NGX_Parameter_MV_Scale_Y, 1) / g.h * (1000.0f / elapsed) };
}

bool CreateHalfRate(Generation& g, ID3D12GraphicsCommandList* cmd, unsigned long long epoch)
{
    g.half = std::make_unique<HalfRate>();
    auto& h = *g.half;
    h.motion = CreateScratch(g.device, DXGI_FORMAT_R32G32B32A32_FLOAT, g.w, g.h);
    h.previousMotion = CreateScratch(g.device, DXGI_FORMAT_R32G32B32A32_FLOAT, g.w, g.h);
    h.anchorMotion = CreateScratch(g.device, DXGI_FORMAT_R32G32B32A32_FLOAT, g.w, g.h);
    h.interpolated = CreateScratch(g.device, DXGI_FORMAT_R16G16B16A16_FLOAT, g.outW, g.outH);
    h.suppressionTexture = CreateScratch(g.device, DXGI_FORMAT_R8_UNORM, 1, 1);
    for (auto& r : h.history) r = CreateScratch(g.device, g.outputFormat, g.outW, g.outH);
    auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    auto desc = CD3DX12_RESOURCE_DESC::Buffer(256, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    if (FAILED(g.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&h.suppression)))) return false;
    heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    desc = CD3DX12_RESOURCE_DESC::Buffer(256);
    if (FAILED(g.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&h.zeroUpload)))) return false;
    void* zero = nullptr;
    if (FAILED(h.zeroUpload->Map(0, nullptr, &zero))) return false;
    std::memset(zero, 0, 256); h.zeroUpload->Unmap(0, nullptr);
    if (!h.motion || !h.previousMotion || !h.anchorMotion || !h.interpolated ||
        !h.suppressionTexture || !h.history[0] || !h.history[1]) return false;
    ResidualFgApi api { NVNGXProxy::D3D12_GetCapabilityParameters(), NVNGXProxy::D3D12_AllocateParameters(),
        NVNGXProxy::D3D12_DestroyParameters(), NVNGXProxy::D3D12_CreateFeature(),
        [](ID3D12GraphicsCommandList* c, const NVSDK_NGX_Handle* f, NVSDK_NGX_Parameter* p,
           PFN_NVSDK_NGX_ProgressCallback cb) { return NVNGXProxy::D3D12_EvaluateFeature()(c, f, p, cb); },
        NVNGXProxy::D3D12_ReleaseFeature() };
    h.fg = std::make_unique<ResidualFg>(api);
    auto result = h.fg->Create(cmd, g.outW, g.outH, g.w, g.h);
    if (result != NVSDK_NGX_Result_Success)
    { g.halfStatus = "FG creation failed: " + std::to_string((unsigned)result); return false; }
    h.createEpoch = epoch;
    h.ready = true;
    return true;
}

// Capture owned resources, not AddRef'd game textures: the game overwrites its
// guides next frame. Full-resource depth copy also supports a depth-stencil
// source in the R32 family without an illegal partial depth copy.
bool SnapshotSplitGuides(Generation& g, ID3D12GraphicsCommandList* cmd,
                         ID3D12Resource* depth, ID3D12Resource* motion)
{
    auto& s = g.half->split;
    const auto d = depth->GetDesc();
    if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.MipLevels != 1 ||
        d.DepthOrArraySize != 1 || d.SampleDesc.Count != 1 ||
        (d.Format != DXGI_FORMAT_R32_FLOAT && d.Format != DXGI_FORMAT_R32_TYPELESS && d.Format != DXGI_FORMAT_D32_FLOAT))
        return false;
    if (!s.depth) s.depth = CreateScratch(g.device, DXGI_FORMAT_R32_FLOAT, (unsigned)d.Width, d.Height);
    if (!s.motion) s.motion = CreateScratch(g.device, DXGI_FORMAT_R32G32B32A32_FLOAT, g.w, g.h);
    if (!s.thirdScene) s.thirdScene = CreateScratch(g.device, g.outputFormat, g.outW, g.outH);
    if (!s.depth || !s.motion || !s.thirdScene || s.depth->GetDesc().Width != d.Width || s.depth->GetDesc().Height != d.Height)
        return false;
    const auto old = s.guidesReadable ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    const auto depthArrival = (D3D12_RESOURCE_STATES)Config::Instance()->DepthResourceBarrier.value_or(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(cmd, depth, depthArrival, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(cmd, s.depth, old, D3D12_RESOURCE_STATE_COPY_DEST);
    cmd->CopyResource(s.depth, depth);
    Barrier(cmd, s.depth, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(cmd, depth, D3D12_RESOURCE_STATE_COPY_SOURCE, depthArrival);
    Barrier(cmd, motion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(cmd, s.motion, old, D3D12_RESOURCE_STATE_COPY_DEST);
    cmd->CopyResource(s.motion, motion);
    Barrier(cmd, s.motion, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(cmd, motion, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    s.guidesReadable = true;
    s.camera = g.half->camera;
    return true;
}

bool PrepareHalfRate(Generation& g, ID3D12GraphicsCommandList* cmd, NVSDK_NGX_Parameter* source,
                     ID3D12Resource* motion, unsigned long long epoch, unsigned long long submittedEpoch)
{
    if (!g.halfRequested) return false;
    if (!(g.flags & NVSDK_NGX_DLSS_Feature_Flags_MVLowRes) ||
        (g.flags & NVSDK_NGX_DLSS_Feature_Flags_MVJittered))
    { g.halfStatus = "requires low-resolution, non-jittered motion"; return false; }
    // This initial bridge integration explicitly requires opt-in approximate guides.
    // Never mark invented matrices as game-supplied camera data.
    if (!g.approximateCamera)
    { g.halfStatus = "explicit approximate-camera opt-in required"; return false; }
    if (!g.half)
    {
        g.halfStatus = "initializing NVIDIA FG";
        if (!CreateHalfRate(g, cmd, submittedEpoch)) g.half->failed = true;
        return false;
    }
    auto& h = *g.half;
    if (!h.ready || h.failed || submittedEpoch == h.createEpoch) return false;
    if (g.reset || UInt(source, NVSDK_NGX_Parameter_Reset) || epoch != h.lastEpoch + 1) h.Reset();
    h.lastEpoch = epoch;
    auto desc = motion->GetDesc();
    if (desc.Width < g.w || desc.Height < g.h || desc.MipLevels != 1 || desc.SampleDesc.Count != 1)
    { h.Reset(); g.halfStatus = "unsupported motion texture"; return false; }
    if (h.motionReadable) Barrier(cmd, h.motion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    DlssNrConstants normalize {}; normalize.Mode = DlssNrMode_NormalizeMotion;
    normalize.Width = g.w; normalize.Height = g.h;
    normalize.MvScaleX = Float(source, NVSDK_NGX_Parameter_MV_Scale_X, 1) / g.w;
    normalize.MvScaleY = Float(source, NVSDK_NGX_Parameter_MV_Scale_Y, 1) / g.h;
    bool ok = g.codec->DispatchPass(cmd, normalize, motion, nullptr, nullptr, nullptr, nullptr, h.motion, nullptr);
    Barrier(cmd, h.motion, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    h.motionReadable = true;
    if (!ok) { h.Reset(); g.halfStatus = "motion normalization failed"; return false; }
    // Intervening frames only supply motion for the next anchor. Neither NR nor
    // residual FG consumes composed anchor motion on those skipped frames.
    if (h.havePrevious && !h.previousWasAnchor)
    {
        if (h.anchorReadable) Barrier(cmd, h.anchorMotion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        normalize.Mode = DlssNrMode_ComposeMotion;
        ok = g.codec->DispatchPass(cmd, normalize, h.motion, h.previousMotion, nullptr, nullptr, nullptr, h.anchorMotion, nullptr);
        Barrier(cmd, h.anchorMotion, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        h.anchorReadable = true;
        if (!ok) { h.Reset(); g.halfStatus = "motion composition failed"; return false; }
    }
    using namespace DirectX;
    const auto& cfg = *Config::Instance();
    float nearPlane = cfg.FsrCameraNear.value_or_default(), farPlane = cfg.FsrCameraFar.value_or_default();
    float fov = cfg.FsrVerticalFov.value_or_default() * XM_PI / 180.0f;
    if (!(nearPlane > 0 && farPlane > nearPlane && fov > 0.01f && fov < XM_PI - 0.01f))
    { h.Reset(); g.halfStatus = "invalid approximate camera parameters"; return false; }
    if (g.flags & NVSDK_NGX_DLSS_Feature_Flags_DepthInverted) std::swap(nearPlane, farPlane);
    auto projection = XMMatrixPerspectiveFovRH(fov, (float)g.outW/g.outH, nearPlane, farPlane);
    XMFLOAT4X4 temp;
    XMStoreFloat4x4(&temp, projection); std::memcpy(h.camera.viewToClip, &temp, sizeof(temp));
    XMStoreFloat4x4(&temp, XMMatrixInverse(nullptr, projection)); std::memcpy(h.camera.clipToView, &temp, sizeof(temp));
    XMStoreFloat4x4(&temp, XMMatrixIdentity());
    std::memcpy(h.camera.clipToPrevious, &temp, sizeof(temp)); std::memcpy(h.camera.previousToClip, &temp, sizeof(temp));
    h.camera.up[1] = h.camera.right[0] = 1; h.camera.forward[2] = -1;
    h.camera.nearPlane = nearPlane; h.camera.farPlane = farPlane;
    h.camera.fov = fov; h.camera.aspect = (float)g.outW/g.outH; h.camera.valid = true;
    g.halfStatus.clear();
    return true;
}

void Before(ID3D12GraphicsCommandList* cmd, NVSDK_NGX_Parameter* source,
            unsigned long long epoch, unsigned long long submittedEpoch,
            ID3D12CommandQueue* queue, bool privateJob = false,
            unsigned int guideSourceWidth = 0, unsigned int guideSourceHeight = 0)
{
    if (pending.cmd && current)
    {
        LOG_DEBUG("DLSS-NR deferred: Before entry with a stale pending (previous After never ran) -> reset. epoch {}", epoch);
        current->reset = true; // abandoned/failed main SR call
    }
    pending = {};
    struct ResetOnGap
    {
        unsigned long long epoch;
        ~ResetOnGap()
        {
            if (!pending.cmd && current)
            {
                LOG_DEBUG("DLSS-NR deferred: Before returned without arming a seam -> reset + hold/half cleared. epoch {}", epoch);
                current->reset = true; current->hold.Reset(); if (current->half) current->half->Reset();
            }
        }
    } resetOnGap { epoch };
    Collect();
    const auto& cfg = *Config::Instance();
    if (cfg.DlssNrUseProxy.value_or_default() || cfg.DlssNrHoldFrame.value_or_default() ||
        cfg.DlssNrDebugView.value_or_default() != 0 || cfg.DlssNrCompare.value_or_default() != 0 ||
        cfg.DlssNrShowSkinMask.value_or_default() ||
        !cfg.DlssNrApplyModel.value_or_default())
    {
        Say("inactive: disable proxy backend, frame hold/debug/compare, and enable Apply model");
        return;
    }
    if (cmd->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT ||
        (!privateJob && (cfg.RestoreComputeSignature.value_or_default() || cfg.RestoreGraphicSignature.value_or_default()) &&
         !D3D12Hooks::CanRestoreRootSignature(cmd)))
    {
        Say("inactive: requires a direct command list with restorable game state");
        return;
    }
    auto* color = GetResource(source, NVSDK_NGX_Parameter_Color, "DLSSD.Color");
    auto* output = GetResource(source, NVSDK_NGX_Parameter_Output, "DLSSD.Output");
    auto* depth = GetResource(source, NVSDK_NGX_Parameter_Depth, "DLSSD.Depth");
    auto* motion = GetResource(source, NVSDK_NGX_Parameter_MotionVectors, "DLSSD.MotionVectors");
    const bool wantsHalf = cfg.DlssNrResidualFg.value_or_default() && !privateJob;
    const bool sampleAndHold = wantsHalf && motion == nullptr;
    if (!color || !output || !depth || (!motion && !sampleAndHold) || color == output)
    {
        Say("inactive: distinct Color/Output, depth and motion are required");
        return;
    }
    for (const char* key : { NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X,
         NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y, NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X,
         NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y, NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X,
         NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y, NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X,
         NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y })
        if (UInt(source, key) != 0) { Say("inactive: non-zero colour/guide/output offsets"); return; }
    const auto inDesc = color->GetDesc(), outDesc = output->GetDesc();
    const auto active = PreSrColorExtent(inDesc,
        UInt(source, NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width),
        UInt(source, NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height));
    if (!active || !PreSrColorExtent(outDesc, 0, 0) || inDesc.MipLevels != 1 || outDesc.MipLevels != 1 ||
        active->width > outDesc.Width || active->height > outDesc.Height)
    {
        Say("inactive: unsupported active input/output dimensions");
        return;
    }
    ID3D12Device* device = nullptr;
    if (FAILED(cmd->GetDevice(IID_PPV_ARGS(&device)))) return;
    auto* ownerQueue = queue ? queue : (ID3D12CommandQueue*)State::Instance().currentCommandQueue;
    ID3D12Device* queueDevice = nullptr;
    if (!ownerQueue || ownerQueue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT ||
        FAILED(ownerQueue->GetDevice(IID_PPV_ARGS(&queueDevice))) || queueDevice != device)
    {
        if (queueDevice) queueDevice->Release();
        device->Release();
        Say("waiting for a same-device direct queue identity");
        return;
    }
    queueDevice->Release();
    unsigned flags = UInt(source, NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags) &
        (NVSDK_NGX_DLSS_Feature_Flags_DepthInverted | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
         NVSDK_NGX_DLSS_Feature_Flags_MVJittered);
    if (sampleAndHold)
        flags = (flags & NVSDK_NGX_DLSS_Feature_Flags_DepthInverted) | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
    const bool splitWork = wantsHalf && !sampleAndHold && State::Instance().gameExe == "ffxiv_dx11.exe" &&
        cfg.DlssNrSplitFrameWork.value_or_default();
    if (current && (current->device != device || current->queue != ownerQueue || current->w != active->width ||
        current->h != active->height || current->outW != outDesc.Width || current->outH != outDesc.Height ||
        current->inputFormat != inDesc.Format || current->outputFormat != outDesc.Format || current->flags != flags ||
        current->halfRequested != wantsHalf ||
        current->splitWork != splitWork ||
        current->sampleAndHold != sampleAndHold ||
        current->approximateCamera != cfg.DlssNrResidualFgApproxCamera.value_or_default()))
        retired.push_back(std::move(current));
    if (!current)
    {
        if (retired.size() >= 4) { device->Release(); Say("waiting for retired GPU work; clean SR frame retained"); return; }
        current = std::make_unique<Generation>();
        current->device = device; // take the GetDevice reference
        current->queue = ownerQueue;
        ownerQueue->AddRef();
        current->w = active->width; current->h = active->height;
        current->outW = (unsigned)outDesc.Width; current->outH = outDesc.Height;
        current->inputFormat = inDesc.Format; current->outputFormat = outDesc.Format; current->flags = flags;
        current->halfRequested = wantsHalf;
        current->splitWork = splitWork;
        current->sampleAndHold = sampleAndHold;
        current->approximateCamera = cfg.DlssNrResidualFgApproxCamera.value_or_default();
        if (!Allocate(*current)) { current->failed = true; Say("allocation failed; clean SR frame retained"); return; }
    }
    else device->Release();
    auto& g = *current;
    if (g.failed) return;
    // Native seams have a logical per-evaluate identity. Bridges retain the submitted epoch
    // so a second upscale in the same bridge submission is still rejected.
    if (g.began && g.lastBeginEpoch == epoch)
    { g.reset = true; Say("inactive: more than one upscale in a submission epoch"); return; }
    g.began = true;
    g.lastBeginEpoch = epoch;
    if (g.trailGuard != cfg.DlssNrPreSrTrailGuard.value_or_default())
    {
        g.trailGuard = cfg.DlssNrPreSrTrailGuard.value_or_default();
        g.reset = true; g.hold.Reset(); if (g.half) g.half->Reset();
        g.privateHistoryResetPending = false;
        g.motionGate = {};
        for (auto& sample : g.motionSamples) sample.pending = false;
        LOG_INFO("DLSS-NR PreSR current-edit bounds {}", g.trailGuard ? "enabled" : "disabled");
    }
    const auto motionNow = GetTickCount64();
    // Current-edit bounds replace the broad pan-onset reset. Resetting every
    // onset also resets residual FG, which can suppress the whole NR edit for
    // a frame. Keep the legacy policy when bounds are disabled; actual cuts,
    // lighting discontinuities and failures still invalidate history normally.
    const bool testMotion = !g.trailGuard && State::Instance().gameExe == "ffxiv_dx11.exe" && !privateJob &&
        (flags & NVSDK_NGX_DLSS_Feature_Flags_MVLowRes) &&
        !(flags & NVSDK_NGX_DLSS_Feature_Flags_MVJittered);
    const bool panReset = testMotion && ConsumePanOnset(g, motionNow);
    g.privateHistoryResetPending |= panReset;
    Use use(g, cmd);
    if (!use.valid) { Say("waiting for GPU completion slots; clean SR frame retained"); return; }
    if (!g.feature)
    {
        ScopedNrStateEnvelope envelope(cmd);
        if (!NVNGXProxy::InitDx12(g.device) || !NVNGXProxy::D3D12_AllocateParameters() ||
            !NVNGXProxy::D3D12_DestroyParameters() || !NVNGXProxy::D3D12_CreateFeature() ||
            !NVNGXProxy::D3D12_EvaluateFeature() || !NVNGXProxy::D3D12_ReleaseFeature() ||
            NVNGXProxy::D3D12_AllocateParameters()(&g.parameters) != NVSDK_NGX_Result_Success || !g.parameters)
        { g.failed = true; Say("NVIDIA DLSS SR runtime unavailable; no alternative upscaler used"); return; }
        auto* p = g.parameters;
        p->Set(NVSDK_NGX_Parameter_Width, g.w); p->Set(NVSDK_NGX_Parameter_Height, g.h);
        p->Set(NVSDK_NGX_Parameter_OutWidth, g.outW); p->Set(NVSDK_NGX_Parameter_OutHeight, g.outH);
        p->Set(NVSDK_NGX_Parameter_CreationNodeMask, 1u); p->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
        p->Set(NVSDK_NGX_Parameter_PerfQualityValue, (int)UInt(source, NVSDK_NGX_Parameter_PerfQualityValue,
                                                           NVSDK_NGX_PerfQuality_Value_MaxPerf));
        // LDR biased carrier, constant unit exposure, no auto-exposure/sharpening. No main-game presets
        // or feature handle are overwritten. NGX is called directly, bypassing OptiScaler's NR hooks.
        p->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, g.flags);
        const auto result = NVNGXProxy::D3D12_CreateFeature()(cmd, NVSDK_NGX_Feature_SuperSampling, p, &g.feature);
        if (result != NVSDK_NGX_Result_Success || !g.feature)
        { g.failed = true; Say("private DLSS creation failed: " + std::to_string((unsigned)result)); return; }
        DlssNrConstants unit {}; unit.Mode = DlssNrMode_UnitExposure; unit.Width = unit.Height = 1;
        if (!g.codec->DispatchPass(cmd, unit, g.edited, nullptr, nullptr, nullptr, nullptr, g.exposure, nullptr))
        { g.failed = true; Say("private exposure initialization failed"); return; }
        Barrier(cmd, g.exposure, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (g.sampleAndHold)
        {
            g.zeroMotion = CreateScratch(g.device, DXGI_FORMAT_R16G16_FLOAT, g.w, g.h);
            unit.Mode = DlssNrMode_ZeroMotion; unit.Width = g.w; unit.Height = g.h;
            if (!g.zeroMotion || !g.codec->DispatchPass(cmd, unit, g.edited, nullptr, nullptr, nullptr,
                                                       nullptr, g.zeroMotion, nullptr))
            { g.failed = true; Say("sample-and-hold guide initialization failed"); return; }
            Barrier(cmd, g.zeroMotion, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        g.createEpoch = submittedEpoch;
        Say("private DLSS created; waiting for a later submission epoch");
        return;
    }
    // Synthetic seam ticks cannot prove that a feature's creation commands were submitted.
    if (submittedEpoch == g.createEpoch)
    { LOG_DEBUG("DLSS-NR deferred: waiting after feature creation at submitted epoch {}", submittedEpoch); return; }
    if (testMotion)
    {
        ScopedNrStateEnvelope envelope(cmd);
        SamplePan(g, cmd, motion, source, use.slot, motionNow);
    }

    // Clear both the private reconstruction and any held/alternate-frame NR edit
    // on the same lighting event. Main-game DLSS and FG parameters are untouched.
    if (State::Instance().gameExe == "ffxiv_dx11.exe" &&
        State::Instance().swapchainInteropApi == SwapchainInteropApi::Dx11wDx12 &&
        cfg.DlssNrWhitePointSource.value_or_default() == 2 && cfg.DlssNrLightingHistory.value_or_default() &&
        !cfg.DlssNrHoldFrame.value_or_default() &&
        !(UInt(source, NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags) & NVSDK_NGX_DLSS_Feature_Flags_IsHDR) &&
        FfxivLightingScan::ConsumeReset(g.lightingEvent))
    {
        g.reset = true; g.hold.Reset(); if (g.half) g.half->Reset();
        LOG_INFO("DLSS-NR PreSR: native lighting event {} invalidates residual history", g.lightingEvent);
    }

    if (g.sampleAndHold)
    {
        if (g.reset || UInt(source, NVSDK_NGX_Parameter_Reset)) g.hold.Reset();
        if (g.hold.CanReuse(epoch))
        {
            pending = {cmd, source, output, epoch,
                       std::max(Float(source, NVSDK_NGX_Parameter_DLSS_Pre_Exposure, 1), 1e-4f), true, false};
            return; // Apply the held residual to CURRENT clean SR, not a delayed raster.
        }
        g.hold.Reset();
        motion = g.zeroMotion; // private NR/SR only, with temporal history reset below.
    }

    bool half = false;
    if (!g.sampleAndHold)
    {
        PreSrTiming::Scope cost(g.timing, cmd, use.slot, PreSrTiming::Guides);
        ScopedNrStateEnvelope envelope(cmd);
        half = PrepareHalfRate(g, cmd, source, motion, epoch, submittedEpoch);
    }
    if (!half && g.half) g.half->Reset();
    if (half && g.half->havePrevious && g.half->previousWasAnchor)
    {
        pending = { cmd, source, output, epoch,
                    std::max(Float(source, NVSDK_NGX_Parameter_DLSS_Pre_Exposure, 1), 1e-4f), true, true };
        return; // NR and private residual SR are both skipped; game SR still runs normally.
    }
    auto* nrMotion = half ? (g.half->havePrevious ? g.half->anchorMotion : g.half->motion) : motion;

    const auto arrival = cfg.ColorResourceBarrier.has_value() ?
        (D3D12_RESOURCE_STATES)cfg.ColorResourceBarrier.value() : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    Barrier(cmd, color, arrival, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(cmd, g.edited, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    CopyActiveColor(cmd, g.edited, color, *active);
    Barrier(cmd, color, D3D12_RESOURCE_STATE_COPY_SOURCE, arrival);
    Barrier(cmd, g.edited, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    DlssNrFrameInfo frame {};
    frame.BeforeUpscale = frame.PrivateColorCopy = true;
    frame.IndependentCommands = privateJob;
    frame.SubmissionEpoch = submittedEpoch;
    frame.RenderSubrectWidth = g.w; frame.RenderSubrectHeight = g.h;
    frame.GuideSourceWidth = guideSourceWidth; frame.GuideSourceHeight = guideSourceHeight;
    frame.DepthInverted = (flags & NVSDK_NGX_DLSS_Feature_Flags_DepthInverted) != 0;
    // Match the ordinary NR entry point. A full-size allocation can contain only
    // a render-size active MV rectangle; leaving these fields at their defaults
    // makes ResolveGuideRegions expose padded/stale vectors to the model.
    frame.MotionVectorsLowResolution = (flags & NVSDK_NGX_DLSS_Feature_Flags_MVLowRes) != 0;
    frame.OutputWidth = g.outW; frame.OutputHeight = g.outH;
    frame.ColourIsLinearHdr = (UInt(source, NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags) &
        NVSDK_NGX_DLSS_Feature_Flags_IsHDR) != 0 && FormatCanHoldLinearHdr(outDesc.Format);
    frame.Reset = UInt(source, NVSDK_NGX_Parameter_Reset) != 0 || g.reset || g.sampleAndHold || privateJob;
    frame.MvScaleX = Float(source, NVSDK_NGX_Parameter_MV_Scale_X, 1);
    frame.MvScaleY = Float(source, NVSDK_NGX_Parameter_MV_Scale_Y, 1);
    if (half) { frame.MvScaleX = (float)g.w; frame.MvScaleY = (float)g.h; }
    if (g.sampleAndHold) frame.MvScaleX = frame.MvScaleY = 1.0f;
    frame.PreExposure = std::max(Float(source, NVSDK_NGX_Parameter_DLSS_Pre_Exposure, 1), 1e-4f);
    frame.ExposureTexture = GetResource(source, NVSDK_NGX_Parameter_ExposureTexture, "ExposureTexture");
    g_nr.exposureOfferedNow = frame.ExposureTexture != nullptr;
    g_nr.exposureEverOffered = g_nr.exposureEverOffered || g_nr.exposureOfferedNow;
    ++g_nr.exposureFrames;
    if (!g_compose) g_compose = std::make_unique<DlssNr_Dx12>("Neural Rendering", g.device);
    const auto before = g_nr.successfulDispatches;
    {
        PreSrTiming::Scope cost(g.timing, cmd, use.slot, PreSrTiming::Nr);
        g_compose->Dispatch(cmd, g.edited, depth, nrMotion, g.edited, frame, queue);
    }
    const bool evaluated = g_nr.successfulDispatches != before;
    if (evaluated)
    {
        ScopedNrStateEnvelope envelope(cmd);
        if (g.smallReadable)
            Barrier(cmd, g.residualInput, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Barrier(cmd, color, arrival, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        DlssNrConstants encode {}; encode.Mode = DlssNrMode_EncodeResidual;
        encode.Width = g.w; encode.Height = g.h; encode.ExposurePreMul = frame.PreExposure;
        const bool ok = g.codec->DispatchPass(cmd, encode, color, g.edited, nullptr, nullptr, nullptr, g.residualInput, nullptr);
        Barrier(cmd, color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, arrival);
        Barrier(cmd, g.residualInput, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        g.smallReadable = true;
        if (ok)
        {
            if (half && g.splitWork)
            {
                PreSrTiming::Scope cost(g.timing, cmd, use.slot, PreSrTiming::Snapshot);
                if (!SnapshotSplitGuides(g, cmd, depth, nrMotion))
                {
                    g.failed = true; g.half->Reset();
                    Say("split scheduling guide capture failed (requires single-plane R32 depth); clean SR retained");
                    Barrier(cmd, g.edited, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    return;
                }
                g.half->split.schedule.Queue(epoch);
            }
            auto* p = g.parameters;
            p->Set(NVSDK_NGX_Parameter_Color, g.residualInput); p->Set(NVSDK_NGX_Parameter_Output, g.residualOutput);
            p->Set(NVSDK_NGX_Parameter_Depth, depth); p->Set(NVSDK_NGX_Parameter_MotionVectors, nrMotion);
            if (half && g.splitWork)
            {
                p->Set(NVSDK_NGX_Parameter_Depth, g.half->split.depth);
                p->Set(NVSDK_NGX_Parameter_MotionVectors, g.half->split.motion);
            }
            p->Set(NVSDK_NGX_Parameter_ExposureTexture, g.exposure);
            p->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, g.w);
            p->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, g.h);
            p->Set(NVSDK_NGX_Parameter_Reset, (unsigned)(frame.Reset || g.reset || g.privateHistoryResetPending));
            p->Set(NVSDK_NGX_Parameter_Jitter_Offset_X, Float(source, NVSDK_NGX_Parameter_Jitter_Offset_X, 0));
            p->Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, Float(source, NVSDK_NGX_Parameter_Jitter_Offset_Y, 0));
            p->Set(NVSDK_NGX_Parameter_MV_Scale_X, frame.MvScaleX);
            p->Set(NVSDK_NGX_Parameter_MV_Scale_Y, frame.MvScaleY);
            p->Set(NVSDK_NGX_Parameter_FrameTimeDeltaInMsec, Float(source, NVSDK_NGX_Parameter_FrameTimeDeltaInMsec, 16.67f) *
                   (half && g.half->havePrevious ? 2.0f : 1.0f));
            p->Set(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, 1.0f);
            p->Set(NVSDK_NGX_Parameter_DLSS_Exposure_Scale, 1.0f);
            p->Set(NVSDK_NGX_Parameter_Sharpness, 0.0f);
            pending = { cmd, source, output, epoch, frame.PreExposure, false, half };
            LOG_DEBUG("DLSS-NR deferred: Before armed epoch {} half {} preExp {:.4f} reset-carried {}", epoch, half,
                      frame.PreExposure, frame.Reset);
        }
    }
    else { g.reset = true; Say("waiting for NR evaluation; clean SR frame retained"); }
    Barrier(cmd, g.edited, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

// The input/output pair always enters and leaves in UAV state. Swap ownership
// after success so both ordinary composition and residual FG see the bounded
// anchor. Before rebinds NGX Output on every evaluation; skips reuse this anchor.
bool BoundResidual(Generation& g, ID3D12GraphicsCommandList* cmd)
{
    if (!g.trailGuard) return true;
    if (!g.boundedResidual)
        g.boundedResidual = CreateScratch(g.device, DXGI_FORMAT_R16G16B16A16_FLOAT, g.outW, g.outH);
    if (!g.boundedResidual) return false;
    DlssNrConstants bounds {}; bounds.Mode = DlssNrMode_BoundResidual;
    bounds.Width = g.outW; bounds.Height = g.outH;
    bounds.GuideWidth = g.w; bounds.GuideHeight = g.h;
    Barrier(cmd, g.residualOutput, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    const bool ok = g.codec->DispatchPass(cmd, bounds, g.residualOutput, g.residualInput,
                                        nullptr, nullptr, nullptr, g.boundedResidual, nullptr);
    Barrier(cmd, g.residualOutput, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (ok) std::swap(g.residualOutput, g.boundedResidual);
    return ok;
}

// Background GPU job: produce only the DLSS-upscaled residual. No raster composition,
// regular FG call, or presentation operation is recorded on this queue.
bool ResolvePrivate(ID3D12GraphicsCommandList* cmd, NVSDK_NGX_Parameter* source,
                    [[maybe_unused]] unsigned long long epoch, ID3D12Resource* destination)
{
    const auto pair = pending; pending = {};
    // No epoch match, as in After() -- see the comment there. (Dead path on this branch: async NR
    // was removed in v0.7.1 and nothing calls ResolvePrivate; kept consistent for a future revival.)
    if (!current || current->failed || pair.cmd != cmd || pair.caller != source)
        return false;
    auto& g = *current;
    Use use(g, cmd);
    if (!use.valid) { g.reset = true; return false; }
    const auto result = NVNGXProxy::D3D12_EvaluateFeature()(cmd, g.feature, g.parameters, nullptr);
    if (result != NVSDK_NGX_Result_Success)
    { g.failed = true; Say("asynchronous residual DLSS evaluation failed"); return false; }
    g.privateHistoryResetPending = false;
    if (!BoundResidual(g, cmd))
    { g.failed = true; Say("PreSR current-edit bounds failed; clean SR frame retained"); return false; }
    Barrier(cmd, g.residualOutput, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(cmd, destination, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    cmd->CopyResource(destination, g.residualOutput);
    Barrier(cmd, destination, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    Barrier(cmd, g.residualOutput, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    g.reset = false;
    return true;
}

void After(ID3D12GraphicsCommandList* cmd, NVSDK_NGX_Parameter* source, unsigned long long epoch)
{
    const auto pair = pending;
    pending = {}; // Consume once, only for the immediately matching successful upscale.
    // Match and consume the immediately preceding Before by resource identity, not Present timing.
    // The pending epoch also owns history continuity if Present changed while DLSS was recording.
    if (!current || current->failed || pair.cmd != cmd || pair.caller != source ||
        pair.output != GetResource(source, NVSDK_NGX_Parameter_Output, "DLSSD.Output"))
    {
        LOG_DEBUG("DLSS-NR deferred: After no-op -> reset. current={} failed={} cmdMatch={} callerMatch={} "
                  "epoch(pending/now)={}/{} outputMatch={}",
                  current != nullptr, current && current->failed, pair.cmd == cmd, pair.caller == source, pair.epoch,
                  epoch, pair.output == GetResource(source, NVSDK_NGX_Parameter_Output, "DLSSD.Output"));
        if (current) current->reset = true;
        return;
    }
    auto& g = *current;
    const auto& cfg = *Config::Instance();
    if ((cfg.RestoreComputeSignature.value_or_default() || cfg.RestoreGraphicSignature.value_or_default()) &&
        !D3D12Hooks::CanRestoreRootSignature(cmd))
    { g.reset = true; Say("inactive: game state cannot be restored after SR"); return; }
    Use use(g, cmd);
    if (!use.valid) { g.reset = true; Say("waiting for GPU completion slots; clean SR frame retained"); return; }
    ScopedNrStateEnvelope envelope(cmd);
    const bool split = pair.half && g.half && g.splitWork;
    const bool evaluateSr = PreSrSplitSchedule::Evaluate(split, pair.skipNr);
    if (split && pair.skipNr && !g.half->split.schedule.CanResolve(pair.epoch))
    { g.reset = true; g.half->Reset(); Say("split scheduling discontinuity; clean SR retained"); return; }
    if (evaluateSr)
    {
        PreSrTiming::Scope cost(g.timing, cmd, use.slot, PreSrTiming::Sr);
        const auto result = NVNGXProxy::D3D12_EvaluateFeature()(cmd, g.feature, g.parameters, nullptr);
        if (result != NVSDK_NGX_Result_Success)
        { g.failed = true; if (g.half) g.half->Reset(); Say("private DLSS evaluation failed: " + std::to_string((unsigned)result)); return; }
        if (g.privateHistoryResetPending && UInt(g.parameters, NVSDK_NGX_Parameter_Reset))
        {
            LOG_INFO("DLSS-NR PreSR: pan-onset reset consumed by private DLSS{}",
                     pair.half ? " and residual FG anchor" : "");
            g.privateHistoryResetPending = false;
        }
        cost.End();
        if (g.trailGuard)
        {
            PreSrTiming::Scope guardCost(g.timing, cmd, use.slot, PreSrTiming::Guard);
            if (!BoundResidual(g, cmd))
            { g.failed = true; if (g.half) g.half->Reset(); Say("PreSR current-edit bounds failed; clean SR frame retained"); return; }
        }
    }
    if (g.reset)
        LOG_DEBUG("DLSS-NR deferred: private history restart requested (split mode may evaluate next frame). "
                  "epoch {} skipNr {} half {}", epoch, pair.skipNr, pair.half);
    else
        LOG_TRACE("DLSS-NR deferred: After applied epoch {} skipNr {} half {}", epoch, pair.skipNr, pair.half);
    g.reset = false;
    Barrier(cmd, g.residualOutput, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    bool half = pair.half && g.half && !g.half->failed;
    if (half && evaluateSr)
    {
        PreSrTiming::Scope cost(g.timing, cmd, use.slot, PreSrTiming::Fg);
        auto& h = *g.half;
        Barrier(cmd, h.suppression, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        cmd->CopyBufferRegion(h.suppression, 0, h.zeroUpload, 0, 256);
        Barrier(cmd, h.suppression, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (h.interpolationReadable)
            Barrier(cmd, h.interpolated, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        const auto result = h.fg->Evaluate(cmd, g.residualOutput,
            split ? h.split.depth : GetResource(source, NVSDK_NGX_Parameter_Depth, "DLSSD.Depth"),
            split ? h.split.motion : (h.havePrevious ? h.anchorMotion : h.motion),
            h.interpolated, h.suppression, split ? h.split.camera : h.camera,
            (g.flags & NVSDK_NGX_DLSS_Feature_Flags_DepthInverted) != 0,
            (split ? !h.split.schedule.HasEdit() : !h.havePrevious) || UInt(g.parameters, NVSDK_NGX_Parameter_Reset) != 0, h.anchorId++);
        Barrier(cmd, h.interpolated, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        h.interpolationReadable = true;
        if (result != NVSDK_NGX_Result_Success)
        {
            h.failed = true; h.Reset(); half = false;
            g.halfStatus = "FG evaluate failed: " + std::to_string((unsigned)result);
            if (split)
            {
                Barrier(cmd, g.residualOutput, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                g.failed = true; Say("split residual FG failed; clean current SR retained"); return;
            }
        }
        else
        {
            if (split) h.split.schedule.Resolved();
            // NVIDIA writes a boolean to the first buffer byte. Copy it to R8_UNORM
            // for the shader: avoids CPU waiting and changing the game's predication.
            Barrier(cmd, h.suppression, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            Barrier(cmd, h.suppressionTexture, h.suppressionReadable ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE :
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
            D3D12_TEXTURE_COPY_LOCATION from {}, to {};
            from.pResource = h.suppression; from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            from.PlacedFootprint.Footprint = { DXGI_FORMAT_R8_UNORM, 1, 1, 1, 256 };
            to.pResource = h.suppressionTexture; to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            cmd->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
            Barrier(cmd, h.suppressionTexture, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Barrier(cmd, h.suppression, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            h.suppressionReadable = true;
        }
    }
    const auto arrival = cfg.OutputResourceBarrier.has_value() ?
        (D3D12_RESOURCE_STATES)cfg.OutputResourceBarrier.value() : D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    PreSrTiming::Scope composeCost(g.timing, cmd, use.slot, PreSrTiming::Compose);
    Barrier(cmd, pair.output, arrival, D3D12_RESOURCE_STATE_COPY_SOURCE);
    // In half-rate mode the current clean raster is needed only as next frame's
    // history. Copy directly into its ring slot instead of via g.clean.
    auto* cleanTarget = half ? g.half->history[g.half->writeIndex] : g.clean;
    auto cleanBefore = half && g.half->historyReadable[g.half->writeIndex] ?
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    if (half && split)
    {
        auto& s = g.half->split;
        cleanTarget = s.schedule.write == 2 ? s.thirdScene : g.half->history[s.schedule.write];
        cleanBefore = s.sceneReadable[s.schedule.write] ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }
    Barrier(cmd, cleanTarget, cleanBefore, D3D12_RESOURCE_STATE_COPY_DEST);
    cmd->CopyResource(cleanTarget, pair.output);
    Barrier(cmd, cleanTarget, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ID3D12Resource* base = cleanTarget;
    ID3D12Resource* residual = g.residualOutput;
    ID3D12Resource* suppression = nullptr;
    DlssNrConstants apply {}; apply.Mode = DlssNrMode_ApplyResidual;
    apply.Width = g.outW; apply.Height = g.outH; apply.ExposurePreMul = pair.scale;
    if (half)
    {
        auto& h = *g.half;
        if (split)
        {
            auto& s = h.split;
            s.sceneReadable[s.schedule.write] = true; s.sceneScale[s.schedule.write] = pair.scale;
            const auto index = s.schedule.Output();
            base = index == 2 ? s.thirdScene : h.history[index];
            apply.ExposurePreMul = s.sceneScale[index];
            if (s.schedule.UseMidpoint(evaluateSr))
            {
                residual = h.interpolated; suppression = h.suppressionTexture;
                apply.Mode = DlssNrMode_ApplyInterpolatedResidual;
            }
        }
        else if (h.havePrevious)
        {
            const unsigned previous = 1 - h.writeIndex;
            base = h.history[previous]; apply.ExposurePreMul = h.historyScale[previous];
            if (!pair.skipNr)
            {
                residual = h.interpolated; suppression = h.suppressionTexture;
                apply.Mode = DlssNrMode_ApplyInterpolatedResidual;
            }
        }
        if (!split) { h.historyReadable[h.writeIndex] = true; h.historyScale[h.writeIndex] = pair.scale; }
    }
    ID3D12Resource* rejectionReference = nullptr;
    if (g.trailGuard && apply.Mode == DlssNrMode_ApplyInterpolatedResidual)
    {
        rejectionReference = cleanTarget;
        apply.ReferencePreExposure = pair.scale;
        if (split)
        {
            // Current B is newer than the latest NR anchor: use the preceding
            // clean A, not the just-captured B or the displayed midpoint itself.
            auto& s = g.half->split;
            const auto anchor = (s.schedule.write + 2) % 3;
            rejectionReference = anchor == 2 ? s.thirdScene : g.half->history[anchor];
            apply.ReferencePreExposure = s.sceneScale[anchor];
        }
        apply.ResidualRejection = 1;
    }
    bool ok = true;
    if (half && split && !g.half->split.schedule.HasEdit())
    {
        // First A has no evaluated residual. Never sample its uninitialized UAV.
        Barrier(cmd, base, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Barrier(cmd, g.composed, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        cmd->CopyResource(g.composed, base);
        Barrier(cmd, g.composed, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Barrier(cmd, base, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    else ok = g.codec->DispatchPass(cmd, apply, base, residual, rejectionReference, nullptr, suppression, g.composed, nullptr);
    if (ok)
    {
        Barrier(cmd, g.composed, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Barrier(cmd, pair.output, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        cmd->CopyResource(pair.output, g.composed);
        Barrier(cmd, pair.output, D3D12_RESOURCE_STATE_COPY_DEST, arrival);
        Barrier(cmd, g.composed, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        presentationGuideDelay = half ? (split ? int(g.half->split.schedule.captured) : (g.half->havePrevious ? 1 : 0)) : -1;
        Say(g.sampleAndHold ? "running: sample-and-hold (motion unavailable); each NR residual applied to 2 current frames; no residual FG or SR delay" :
            half ? (split ? "running: SPLIT work; NR on A, private SR/FG on B; scene delayed 2 frames; APPROXIMATE camera guides" :
                           "running: NR every second frame + NVIDIA residual FG; SR delayed 1 frame; APPROXIMATE camera guides") :
            "running: " + std::to_string(g.w) + "x" + std::to_string(g.h) + " contribution -> private DLSS -> " +
            std::to_string(g.outW) + "x" + std::to_string(g.outH) + "; applied after SR" +
            (g.halfRequested ? "; residual FG inactive: " + g.halfStatus : ""));
    }
    else { Barrier(cmd, pair.output, D3D12_RESOURCE_STATE_COPY_SOURCE, arrival); Say("composition failed; clean frame retained"); }
    if (!half)
        Barrier(cmd, g.clean, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Barrier(cmd, g.residualOutput, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (g.sampleAndHold)
    {
        if (ok && !pair.skipNr) g.hold.SampleSucceeded(pair.epoch);
        else g.hold.Reset();
    }
    if (half)
    {
        auto& h = *g.half;
        // Both textures are owned by this generation and all uses run in queue
        // order. Rotate ownership/state instead of copying the normalized field.
        std::swap(h.motion, h.previousMotion);
        std::swap(h.motionReadable, h.previousReadable);
        h.havePrevious = ok; h.previousWasAnchor = !pair.skipNr;
        if (pair.skipNr) ++h.skippedNr; else ++h.nrAnchors;
        if (h.skippedNr == 8 && pair.skipNr)
            LOG_INFO("Residual FG cadence: {} NR anchor frames, {} skipped NR frames; matching clean history active",
                     h.nrAnchors, h.skippedNr);
        h.writeIndex = 1 - h.writeIndex;
        if (split && ok) h.split.schedule.Advance();
        if (!ok) { h.Reset(); g.reset = true; }
    }
    composeCost.End();
    if (ok) g.frameKind[use.slot] = half ? (pair.skipNr ? PreSrCadence::Skipped : PreSrCadence::Anchor) : PreSrCadence::Ordinary;
    if (++g.completedFrames % 120 == 0 && g.timing.Available())
    {
        const auto& t = g.timing;
        LOG_INFO("PreSR GPU cost per call (ms): guides {:.3f}, NR {:.3f}, private SR {:.3f}, bounds {:.3f}, residual FG {:.3f}, compose {:.3f}; NR anchors {}, skipped {}; snapshot {:.3f}, split {}",
            t.Get(PreSrTiming::Guides).ms, t.Get(PreSrTiming::Nr).ms, t.Get(PreSrTiming::Sr).ms,
            g.trailGuard ? t.Get(PreSrTiming::Guard).ms : 0.0, t.Get(PreSrTiming::Fg).ms, t.Get(PreSrTiming::Compose).ms,
            g.half ? g.half->nrAnchors : 0, g.half ? g.half->skippedNr : 0, t.Get(PreSrTiming::Snapshot).ms, split);
        const auto cadence = g.cadence.Get();
        LOG_INFO("PreSR real-frame GPU completion intervals (ms): anchor {:.3f} ({}), skipped {:.3f} ({}), median {:.3f}, p95 {:.3f}, max {:.3f}; {} samples; not display FPS",
            cadence.anchor, cadence.anchors, cadence.skipped, cadence.skips,
            cadence.median, cadence.p95, cadence.maximum, cadence.samples);
    }
}

void Shutdown()
{
    Cancel();
    if (current) retired.push_back(std::move(current));
    const auto total = retired.size();
    Collect();
    const auto retained = retired.size();
    // Never free feature histories, descriptors or surfaces referenced by an unsubmitted/in-flight
    // list. At shutdown only, retain uncompleted generations for process teardown rather than UAF.
    for (auto& g : retired) (void)g.release();
    retired.clear();
    LOG_INFO("DLSS-NR PreSR shutdown: released {} completed generations; retained {} pending generations",
             total - retained, retained);
}
} // namespace DeferredSr

std::string SynchronousDeferredDlssStatus()
{
    std::lock_guard<std::recursive_mutex> lock(g_nrMutex);
    auto result = DeferredSr::status;
    if (DeferredSr::current && DeferredSr::current->half && DeferredSr::current->half->havePrevious)
    {
        const auto& h = *DeferredSr::current->half;
        result += " (NR frames " + std::to_string(h.nrAnchors) + ", skipped " + std::to_string(h.skippedNr) + ")";
    }
    if (DeferredSr::current && DeferredSr::current->timing.Available())
    {
        const auto& t = DeferredSr::current->timing;
        if (t.Get(PreSrTiming::Compose).count)
            result += std::format("\nGPU ms per call: guides {:.2f}, NR {:.2f}, residual DLSS {:.2f}, bounds {:.2f}, residual FG {:.2f}, composition {:.2f}",
                t.Get(PreSrTiming::Guides).ms, t.Get(PreSrTiming::Nr).ms, t.Get(PreSrTiming::Sr).ms,
                DeferredSr::current->trailGuard ? t.Get(PreSrTiming::Guard).ms : 0.0,
                t.Get(PreSrTiming::Fg).ms, t.Get(PreSrTiming::Compose).ms);
    }
    return result;
}
