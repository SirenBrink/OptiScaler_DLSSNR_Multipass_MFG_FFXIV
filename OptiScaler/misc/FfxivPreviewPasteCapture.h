#pragma once
#include <shaders/hdr/HdrScreenshotReadback.h>
#include <array>
#include <filesystem>
#include <fstream>
#include <memory>

namespace FfxivPreviewPasteCapture
{
struct Job
{
    std::array<Hdr10::Screenshot::Readback, 6> images;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> finalAllocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> finalCommands;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
    std::shared_ptr<void> guideOwner;
    std::shared_ptr<Job> pendingOwnership;
    std::filesystem::path directory;
    float phase = 0;
    UINT64 batch = 0, current = 0, previous = 0;
    bool finalRecorded = false;
};
struct Worker { std::shared_ptr<Job> job; HMODULE module; };

inline DWORD WINAPI Save(void* argument)
{
    auto* work = static_cast<Worker*>(argument);
    auto& job = *work->job;
    const auto module = work->module;
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    const bool complete = event && SUCCEEDED(job.images[0].fence->SetEventOnCompletion(1, event)) &&
                          WaitForSingleObject(event, 10000) == WAIT_OBJECT_0;
    if (event) CloseHandle(event);
    if (complete && job.images[0].fence->GetCompletedValue() != UINT64_MAX)
    {
        try
        {
        std::error_code error;
        std::filesystem::create_directories(job.directory, error);
        bool saved = !error;
        const char* names[]{"previous", "current", "guide", "before", "after", "final"};
        for (unsigned i = 0; saved && i < job.images.size(); ++i)
        {
            if (i == 5 && !job.finalRecorded) continue;
            auto& image = job.images[i];
            const auto& layout = image.footprint.Footprint;
            void* data = nullptr;
            D3D12_RANGE range{0, SIZE_T(image.bytes)};
            if (FAILED(image.readback->Map(0, &range, &data))) { saved = false; break; }
            std::ofstream file(job.directory / (std::string(names[i]) + ".bin"), std::ios::binary);
            const UINT header[]{layout.Width, layout.Height, UINT(layout.Format)};
            file.write(reinterpret_cast<const char*>(header), sizeof(header));
            for (UINT y = 0; y < layout.Height; ++y)
                file.write(static_cast<const char*>(data) + image.footprint.Offset + y * layout.RowPitch,
                           size_t(layout.Width) * 4);
            file.flush(); saved = bool(file);
            const D3D12_RANGE written{0, 0}; image.readback->Unmap(0, &written);
        }
        std::ofstream info(job.directory / "capture.txt");
        info << "phase=" << job.phase << "\nbatch=" << job.batch << "\ncurrent=" << job.current
             << "\nprevious=" << job.previous << "\n";
        info << "final=" << job.finalRecorded << "\n";
        LOG_INFO("CharaView FG pixel capture: saved {}, phase {}, batch {}, previous {}, current {}, final stage {}",
                 saved && bool(info), job.phase, job.batch, job.previous, job.current, job.finalRecorded);
        }
        catch (...)
        {
            LOG_WARN("CharaView FG pixel capture: file output failed");
        }
        job.pendingOwnership.reset();
    }
    else
        LOG_WARN("CharaView FG pixel capture: GPU completion unavailable; submitted resources retained");
    delete work;
    FreeLibraryAndExitThread(module, 0);
}

// One bounded capture with logging enabled. The copies use the existing
// command list and their own fence; disk output never blocks presentation.
inline std::shared_ptr<Job> Prepare(ID3D12Device* device, ID3D12Resource* previous,
    ID3D12Resource* current, ID3D12Resource* guide, ID3D12Resource* output,
    const std::shared_ptr<void>& owner, float phase, UINT64 batch, UINT64 previousFrame, UINT64 currentFrame)
try
{
    static bool attempted = false;
    static ULONGLONG firstPair = 0;
    if (attempted || !Config::Instance()->LogToFile.value_or_default() ||
        !Config::Instance()->FfxivPreviewGuideCapture.value_or_default() || !previous || !current || !guide)
        return {};
    const auto size = current->GetDesc();
    if (size.Width * size.Height > 8388608) return {}; // bound diagnostic readback memory
    const auto now = GetTickCount64();
    if (!firstPair) firstPair = now;
    if (now - firstPair < 5000 || phase < 0.3f || phase > 0.75f) return {};
    attempted = true;
    auto job = std::make_shared<Job>();
    ID3D12Resource* sources[]{previous, current, guide, output, output, output};
    for (unsigned i = 0; i < job->images.size(); ++i)
    {
        const bool hdr = sources[i]->GetDesc().Format == DXGI_FORMAT_R10G10B10A2_UNORM;
        if (FAILED(job->images[i].Allocate(device, sources[i], hdr)))
        {
            LOG_WARN("CharaView FG pixel capture: readback allocation unavailable");
            return {};
        }
    }
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&job->finalAllocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, job->finalAllocator.Get(), nullptr,
            IID_PPV_ARGS(&job->finalCommands)))) return {};
    job->guideOwner = owner;
    job->phase = phase; job->batch = batch; job->previous = previousFrame; job->current = currentFrame;
    job->directory = Util::DllPath().parent_path() / L"OptiScaler" / L"PreviewFGDiagnostics";
    return job;
}
catch (...)
{
    LOG_WARN("CharaView FG pixel capture: preparation failed");
    return {};
}

inline void RecordBefore(const std::shared_ptr<Job>& job, ID3D12GraphicsCommandList* list,
                         D3D12_RESOURCE_STATES imageState)
{
    if (!job) return;
    job->images[0].RecordPreservingState(list, imageState);
    job->images[1].RecordPreservingState(list, imageState);
    job->images[2].RecordPreservingState(list, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    job->images[3].RecordPreservingState(list, D3D12_RESOURCE_STATE_RENDER_TARGET);
}
inline void RecordAfter(const std::shared_ptr<Job>& job, ID3D12GraphicsCommandList* list)
{
    if (job) job->images[4].RecordPreservingState(list, D3D12_RESOURCE_STATE_RENDER_TARGET);
}
inline void Submit(const std::shared_ptr<Job>& job, ID3D12CommandQueue* queue)
{
    if (!job) return;
    job->pendingOwnership = job;
    if (FAILED(queue->Signal(job->images[0].fence.Get(), 1))) return;
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
        reinterpret_cast<LPCWSTR>(&Save), &module)) return;
    auto* work = new (std::nothrow) Worker{job, module};
    if (!work) return;
    HANDLE thread = CreateThread(nullptr, 0, Save, work, 0, nullptr);
    if (thread) CloseHandle(thread); // Retain submitted resources if worker creation fails.
}

inline std::shared_ptr<Job>& PendingFinal()
{
    static thread_local std::shared_ptr<Job> pending;
    return pending;
}
inline void DeferFinal(const std::shared_ptr<Job>& job, ID3D12CommandQueue* queue)
{
    if (!job) return;
    // Retain submitted copies until final presentation establishes completion.
    // An aborted presentation quarantines this one bounded capture on exit.
    job->pendingOwnership = job;
    job->queue = queue;
    PendingFinal() = job;
}
inline void FinishFinal()
{
    auto job = std::move(PendingFinal());
    if (!job) return;
    job->images[5].RecordPreservingState(job->finalCommands.Get(), D3D12_RESOURCE_STATE_PRESENT);
    if (SUCCEEDED(job->finalCommands->Close()))
    {
        ID3D12CommandList* lists[]{job->finalCommands.Get()};
        job->queue->ExecuteCommandLists(1, lists);
        job->finalRecorded = true;
    }
    Submit(job, job->queue.Get());
}
}
