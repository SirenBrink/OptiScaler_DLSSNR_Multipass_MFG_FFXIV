#pragma once
#include <array>
#include <chrono>
#include <cstdint>

// CPU timings only: queued GPU work is not measured as GPU execution time.
// No per-frame output, CPU/GPU synchronization, allocation or shared mutex.
// Enabled with file logging at Debug/Trace; summaries are per thread and phase.
namespace FramePacingTrace {
enum class Phase { NativePresent, InteropSubmit, CopyAllocatorWait, HudProduce,
                   HudPaste, FGDispatch, OutputPresent, ReflexSleep, Preview, PreviewNR, SceneNR, HiddenPresent, InteropSignal, InteropFlush, InteropQueueWait, Count };
inline constexpr std::array<const char*, size_t(Phase::Count)> Names {
    "native present", "DX11 interop submit", "copy allocator wait", "HUD produce",
    "HUD paste", "FG dispatch", "output present", "Reflex sleep", "preview DLSS+NR", "preview NR", "scene NR recording", "hidden DX11 present", "DX11 interop signal", "DX11 interop flush", "DX12 interop queue wait" };
struct Accumulator {
    uint64_t calls=0, sumUs=0, maxUs=0, over8=0, over16=0, over33=0;
    void Add(uint64_t us) {
        ++calls;sumUs+=us;if(us>maxUs)maxUs=us;
        over8+=us>=8000;over16+=us>=16000;over33+=us>=33000;
    }
};
inline bool Enabled() {
    const auto* cfg=Config::Instance();
    return State::Instance().gameExe == "ffxiv_dx11.exe" && cfg->LogToFile.value_or_default() && cfg->LogLevel.value_or_default()<=1;
}
class Scope {
    using Clock=std::chrono::steady_clock;
    Phase phase;
    bool enabled;
    Clock::time_point start;
public:
    explicit Scope(Phase p):phase(p),enabled(Enabled()) {
        if(enabled)start=Clock::now();
    }
    Scope(const Scope&)=delete;
    Scope& operator=(const Scope&)=delete;
    ~Scope() {
        if(!enabled)return;
        const auto now=Clock::now();
        struct Entry { Accumulator stats;Clock::time_point report; };
        static thread_local std::array<Entry,size_t(Phase::Count)> entries{};
        auto& entry=entries[size_t(phase)];
        if(entry.report==Clock::time_point{})entry.report=now;
        entry.stats.Add(uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(now-start).count()));
        if(now-entry.report<std::chrono::seconds(10))return;
        const auto stats=entry.stats;
        entry={};entry.report=now;
        LOG_INFO("Frame pacing CPU [{}]: calls {}, mean {:.3f} ms, max {:.3f} ms, >=8ms {}, >=16ms {}, >=33ms {}",
                 Names[size_t(phase)],stats.calls,double(stats.sumUs)/double(stats.calls)/1000.0,
                 double(stats.maxUs)/1000.0,stats.over8,stats.over16,stats.over33);
    }
};
}
