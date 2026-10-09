#pragma once
#include <d3d12.h>
#include <memory>
#include <string>
namespace Hdr10::Screenshot {
struct Job;
void Request(); // Called by the normal configurable-key input path.
bool PendingHDR(); // Full native UI is captured only when an HDR shot is requested.
// HDR source is in COPY_SOURCE; SDR source is restored to its supplied state.
// Submit only after ExecuteCommandLists.
std::shared_ptr<Job> Prepare(HWND, ID3D12Device*, ID3D12GraphicsCommandList*,
    ID3D12Resource* hdrSource, ID3D12Resource* sdrSource, D3D12_RESOURCE_STATES sdrState);
void Submit(const std::shared_ptr<Job>&, ID3D12CommandQueue*);
std::string Status();
}
