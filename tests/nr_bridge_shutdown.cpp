// Built by run_nr_bridge_shutdown.ps1 against the production generation owners,
// destructor, retirement and bridge suspend/resume functions. NGX is simulated
// so an attempted release after runtime teardown produces exit code 99.
#include <windows.h>
#include <d3d12.h>
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include <cstdio>
#include <dlssnr/ResidualFg.h>
#include <shaders/dlssnr/DlssNr_Common.h>
#include <shaders/dlssnr/PreSrMotionReset.h>

#define LOG_INFO(...) ((void)0)
static bool runtimeAlive = true;
static unsigned released = 0, destroyed = 0;
static NVSDK_NGX_Result MockRelease(NVSDK_NGX_Handle*)
{
    if (!runtimeAlive) std::_Exit(99);
    ++released;
    return NVSDK_NGX_Result_Success;
}
static NVSDK_NGX_Result MockDestroy(NVSDK_NGX_Parameter*)
{
    if (!runtimeAlive) std::_Exit(99);
    ++destroyed;
    return NVSDK_NGX_Result_Success;
}
namespace NVNGXProxy
{
auto D3D12_ReleaseFeature() { return &MockRelease; }
auto D3D12_DestroyParameters() { return &MockDestroy; }
}
class DlssNr_Dx12 {};
static std::recursive_mutex g_nrMutex;
static std::atomic_bool g_bridgeSuspended {false};
static struct { bool reset = false, passNeedsReset[30] {}; } g_nr;
static std::optional<double> g_lastGpuTime, g_lastNgxTime;
namespace DlssNr
{
#include "nr_shutdown_production.inl"
}

static std::unique_ptr<DlssNr::DeferredSr::Generation> Make(bool recorded, UINT64* completion)
{
    auto g = std::make_unique<DlssNr::DeferredSr::Generation>();
    // Fake handles are consumed only by the mock runtime functions above.
    g->feature = reinterpret_cast<NVSDK_NGX_Handle*>(1);
    g->parameters = reinterpret_cast<NVSDK_NGX_Parameter*>(2);
    g->everRecorded = recorded;
    g->completed = completion;
    return g;
}

int main(int argc, char**)
{
    namespace D = DlssNr::DeferredSr;
    UINT64 pending = 0, complete = 1;
    if (argc > 1)
    {
        // Simulate missed early shutdown. No NGX call may come from CRT cleanup.
        D::current = Make(false, nullptr);
        D::retired.push_back(Make(false, nullptr));
        runtimeAlive = false;
        return 0;
    }

    D::current = Make(true, &complete);
    D::retired.push_back(Make(true, &pending));
    D::retired.push_back(Make(false, nullptr));
    D::pending.epoch = 42;
    g_lastGpuTime = 2.0;
    g_lastNgxTime = 1.0;
    DlssNr::SuspendForBridgeShutdown();
    assert(g_bridgeSuspended && released == 2 && destroyed == 2);
    assert(!D::current && D::retired.empty() && D::pending.epoch == 0);
    assert(g_nr.reset && std::all_of(std::begin(g_nr.passNeedsReset), std::end(g_nr.passNeedsReset),
                                   [](bool reset) { return reset; }));
    assert(!g_lastGpuTime && !g_lastNgxTime);
    DlssNr::SuspendForBridgeShutdown(); // Repeated shutdown cannot double-release.
    assert(released == 2 && destroyed == 2);
    DlssNr::ResumeAfterBridgeInit();
    assert(!g_bridgeSuspended);
    D::current = Make(true, &pending);
    DlssNr::SuspendForBridgeShutdown(); // Pending/unsubmitted work must survive.
    assert(released == 2 && destroyed == 2);
    runtimeAlive = false;
    std::puts("NR shutdown: completed/pending retirement, repeat shutdown, reinitialization and history reset passed");
}
