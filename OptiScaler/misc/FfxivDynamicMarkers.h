#pragma once
#include <Config.h>
#include <hooks/Reflex_Hooks.h>
#include <proxies/Streamline_Proxy.h>
#include <mutex>

// Experimental bridge intervals, not engine simulation/latency measurements.
// Only native dynamic MFG in FFXIV uses these approximations.
namespace FfxivDynamicMarkers
{
inline std::mutex mutex;
inline uint64_t simulationFrame = UINT64_MAX;
inline uint64_t renderFrame = UINT64_MAX;
inline bool Enabled()
{
    auto& state = State::Instance();
    return state.gameExe == "ffxiv_dx11.exe" && !state.isShuttingDown &&
           state.activeFgInput == FGInput::Upscaler && state.activeFgOutput == FGOutput::DLSSG &&
           Config::Instance()->FGEnabled.value_or_default() &&
           Config::Instance()->FGDLSSGForceDMFG.value_or_default() &&
           !ReflexHooks::gameIsSendingMarkers() && StreamlineProxy::IsD3D12Inited() &&
           StreamlineProxy::GetNewFrameToken() && StreamlineProxy::PCLSetMarker();
}
inline bool Mark(sl::PCLMarker marker, uint64_t frame)
{
    sl::FrameToken* token = nullptr;
    const uint32_t id = static_cast<uint32_t>(frame);
    if (StreamlineProxy::GetNewFrameToken()(token, &id) != sl::Result::eOk || !token) return false;
    struct MarkerScope
    {
        bool previous = ReflexHooks::bridgeMarkerCall;
        MarkerScope() { ReflexHooks::bridgeMarkerCall = true; }
        ~MarkerScope() { ReflexHooks::bridgeMarkerCall = previous; }
    } scope;
    const auto result = StreamlineProxy::PCLSetMarker()(marker, *token);
    LOG_TRACE("FFXIV dynamic marker: frame={} marker={} result={}", frame, static_cast<int>(marker), static_cast<int>(result));
    return result == sl::Result::eOk;
}
inline void NextFrame(uint64_t frame)
{
    std::lock_guard<std::mutex> lock(mutex);
    simulationFrame = renderFrame = UINT64_MAX;
    if (Enabled() && Mark(sl::PCLMarker::eSimulationStart, frame)) simulationFrame = frame;
}
inline void Upscale(uint64_t frame)
{
    std::lock_guard<std::mutex> lock(mutex);
    if (!Enabled()) { simulationFrame = renderFrame = UINT64_MAX; return; }
    if (simulationFrame != frame) return; // Drop incomplete or skipped intervals.
    simulationFrame = UINT64_MAX;
    if (Mark(sl::PCLMarker::eSimulationEnd, frame) && Mark(sl::PCLMarker::eRenderSubmitStart, frame))
        renderFrame = frame;
}
inline void BeforePresent(uint64_t frame)
{
    std::lock_guard<std::mutex> lock(mutex);
    if (Enabled() && renderFrame == frame) Mark(sl::PCLMarker::eRenderSubmitEnd, frame);
    renderFrame = UINT64_MAX;
}
}
