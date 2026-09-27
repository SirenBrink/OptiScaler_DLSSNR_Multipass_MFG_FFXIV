#pragma once
#include <d3d12.h>
#include <dxgi1_6.h>
#include <string>
namespace Hdr10 {
bool Request(HWND window);
bool Active();
bool Configure(IDXGISwapChain4* chain);
void Deactivate();
std::string Status();
ID3D12Resource* Convert(ID3D12Device*, ID3D12GraphicsCommandList*, ID3D12Resource*, D3D12_RESOURCE_STATES);
// Bridge-owned lists can run before general resource tracking is installed.
HRESULT ResetCommands(ID3D12GraphicsCommandList*, ID3D12CommandAllocator*);
void ExecuteCommands(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
void Submitted(ID3D12CommandQueue*,UINT,ID3D12CommandList*const*);
void Reset(ID3D12CommandList*);
}
