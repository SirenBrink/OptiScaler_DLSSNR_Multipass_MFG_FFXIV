#pragma once

#include <d3d12.h>
#include <nvsdk_ngx.h>

#include <string>

namespace DlssNr::AmdBridge
{
bool HasFiles();
bool IsAmdDevice(ID3D12Device* device);
bool CanUse(ID3D12Device* device);
std::string PrerequisiteError();
ID3D12Resource* Prepare(ID3D12GraphicsCommandList* commandList, NVSDK_NGX_Parameter* parameters,
                        ID3D12CommandQueue* queue, unsigned int featureFlags, bool interop);
int PendingListIndex(UINT count, ID3D12CommandList* const* lists);
void Submitting(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists);
void Submitted(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists);
void InvalidateHistory();
std::string Status();
} // namespace DlssNr::AmdBridge
