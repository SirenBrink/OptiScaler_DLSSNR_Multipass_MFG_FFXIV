#include "pch.h"
#include "HdrScreenshot.h"
#include "Hdr10.h"
#include "HdrScreenshotImage.h"
#include "HdrScreenshotReadback.h"
#include <Config.h>
#include <Util.h>
#include <atomic>
#include <mutex>
#include <new>

namespace Hdr10::Screenshot {
using Microsoft::WRL::ComPtr;
namespace {
struct State {
    std::atomic<bool> busy{false};
    std::atomic<ULONGLONG> requested{0};
    std::mutex mutex;
    std::string message = "Screenshots save to game/OptiScaler/Screenshots";
};
State& S() { static auto* state = new State; return *state; }
void Message(std::string value) { std::lock_guard lock(S().mutex); S().message = std::move(value); }
}
struct Job : Readback {
    std::filesystem::path path;
    bool hdr = false;
    bool iccOnly = false;
    // Established before any post-submission allocations. Failure paths keep
    // the submitted copy alive even if worker allocation itself fails.
    std::shared_ptr<Job> pendingOwnership;
    ~Job() { S().busy = false; }
};
std::string Status() { std::lock_guard lock(S().mutex); return S().message; }
void Request()
{
    if (Hdr10::Active() && !S().busy.load()) S().requested = GetTickCount64();
}
std::shared_ptr<Job> Prepare(HWND window, ID3D12Device* device, ID3D12GraphicsCommandList* commands,
    ID3D12Resource* hdrSource, ID3D12Resource* sdrSource, D3D12_RESOURCE_STATES sdrState)
{
    const auto requested = S().requested.exchange(0);
    if (!requested || GetTickCount64()-requested > 1000 || !device || !commands) return {};
    const auto key = Config::Instance()->FfxivHDRScreenshotKey.value_or_default();
    if (key <= 0 || key >= 256) return {};
    if (!window || GetAncestor(GetForegroundWindow(), GA_ROOT) != GetAncestor(window, GA_ROOT)) return {};
    if (S().busy.exchange(true)) return {};
    std::shared_ptr<Job> job;
    try {
        job = std::make_shared<Job>();
        job->hdr = Config::Instance()->FfxivHDRScreenshotFormat.value_or_default() == 1;
        job->iccOnly = job->hdr && Config::Instance()->FfxivHDRScreenshotIccOnly.value_or_default();
        ScreenshotImage::Check(job->Allocate(device, job->hdr ? hdrSource : sdrSource, job->hdr));
        SYSTEMTIME t{}; GetLocalTime(&t);
        job->path = Util::DllPath().parent_path() / L"OptiScaler" / L"Screenshots" /
            std::format(L"OptiHDR-{:04}{:02}{:02}-{:02}{:02}{:02}-{:03}-{}-{}.png",
                t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond,t.wMilliseconds,GetTickCount64(),job->hdr?L"HDR":L"SDR");
        // Finish potentially throwing preparation before recording a GPU use.
        Message(job->hdr ? "HDR screenshot queued" : "SDR screenshot queued");
        job->RecordPreservingState(commands, job->hdr ? D3D12_RESOURCE_STATE_COPY_SOURCE : sdrState);
        return job;
    } catch (...) { if (!job) S().busy = false; Message("HDR screenshot allocation failed (see log)"); LOG_ERROR("OptiHDR screenshot allocation failed"); return {}; }
}
namespace {
struct Worker { std::shared_ptr<Job> job; HMODULE module; };
DWORD WINAPI Encode(void* parameter)
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    auto* work = static_cast<Worker*>(parameter);
    const HMODULE module = work->module;
    auto& job = *work->job;
    bool com = false, mapped = false;
    try {
        // Dedicated worker: never block Present, never release a pending GPU readback.
        while (job.fence->GetCompletedValue() == 0) Sleep(10);
        if (job.fence->GetCompletedValue() == UINT64_MAX) throw DXGI_ERROR_DEVICE_REMOVED;
        ScreenshotImage::Check(CoInitializeEx(nullptr, COINIT_MULTITHREADED)); com = true;
        void* data = nullptr; D3D12_RANGE read{0, SIZE_T(job.bytes)};
        ScreenshotImage::Check(job.readback->Map(0, &read, &data)); mapped = true;
        std::filesystem::create_directories(job.path.parent_path());
        const auto* pixels = static_cast<BYTE*>(data)+job.footprint.Offset;
        const auto& size = job.footprint.Footprint;
        ScreenshotImage::SavePng(job.path, pixels, size.Width, size.Height, size.RowPitch, job.hdr, size.Format, job.iccOnly);
        Message("Saved " + job.path.filename().string());
        LOG_INFO("OptiHDR screenshot saved: {} (colour metadata: {})", job.path.string(), job.hdr ? (job.iccOnly ? "BT.2100 PQ ICC only" : "BT.2100 PQ cICP") : "sRGB");
    } catch (HRESULT error) {
        Message("HDR screenshot failed (see OptiScaler.log)");
        LOG_ERROR("OptiHDR screenshot failed: {:08X}, path {}", static_cast<unsigned>(error), job.path.string());
        std::error_code ignored; std::filesystem::remove(job.path, ignored);
    } catch (...) {
        Message("HDR screenshot failed (see OptiScaler.log)");
        LOG_ERROR("OptiHDR screenshot encoding/readback failed");
        std::error_code error; std::filesystem::remove(job.path, error);
    }
    if (mapped) { D3D12_RANGE none{0,0}; job.readback->Unmap(0, &none); }
    if (com) CoUninitialize();
    job.pendingOwnership.reset(); // The worker still owns job until delete.
    delete work;
    // Keep this DLL mapped until the worker has released all resources.
    FreeLibraryAndExitThread(module, 0);
}
}
void Submit(const std::shared_ptr<Job>& job, ID3D12CommandQueue* queue)
{
    if (!job) return;
    job->pendingOwnership = job;
    if (!queue || FAILED(queue->Signal(job->fence.Get(), 1))) {
        // Submitted resources cannot be freed without completion proof.
        Message("HDR screenshot disabled: GPU completion signal failed");
        LOG_ERROR("OptiHDR screenshot fence signal failed; pending resources retained");
        return;
    }
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
        reinterpret_cast<LPCWSTR>(&Encode), &module)) {
        Message("HDR screenshot worker unavailable"); return;
    }
    auto* work = new (std::nothrow) Worker{job, module};
    if (!work) {
        // Keep the module pin and pre-established pending ownership.
        Message("HDR screenshot worker allocation failed; capture disabled");
        return;
    }
    HANDLE thread = CreateThread(nullptr, 0, Encode, work, 0, nullptr);
    if (thread) CloseHandle(thread);
    else {
        // Retain work (and module) if no worker can wait for the submitted copy.
        Message("HDR screenshot worker unavailable");
        LOG_ERROR("OptiHDR screenshot worker creation failed; pending resources retained");
    }
}
}
