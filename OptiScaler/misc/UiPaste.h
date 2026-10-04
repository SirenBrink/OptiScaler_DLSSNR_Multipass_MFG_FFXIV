#pragma once

// Post frame generation UI paste (DX11 games through the Dx11wDx12 bridge + external HUD-less).
//
// DLSS-G's own UI recomposition still interpolates the UI layer, so menus morph between frames.
// This module keeps the UI static instead:
//
//  1. Producer (Dx11wDx12SC::Present, game thread, FG queue): for every real frame that has a
//     HUD-less copy, a compute pass builds a "paste image" from (final, HUD-less):
//       alpha 1.0  pixel is UI on this frame              -> final colour
//       alpha 0.5  pixel was UI on the previous frame only -> final colour (clears fading ghosts)
//       alpha 0    leave the generated frame alone
//     Images live in a small ring. The producer never waits on the GPU: it only reuses a slot
//     whose previous write and every paste reading it have already completed.
//
//  2. Consumer (LocalPresent of DLSS-G's native swapchain, i.e. every output present including
//     generated ones): draws the newest *completed* paste image over the current backbuffer on the
//     presenting queue, right before Present. Choosing only completed images means the paste never
//     stalls frame pacing, and the UI shown can only move forward in time, never back.

#include <d3d12.h>
#include <dxgi1_6.h>
#include <cstdint>

namespace UiPaste
{
struct Status
{
    uint64_t produced = 0; // paste images built (real frames)
    uint64_t pasted = 0;   // output presents (real or generated) the UI was drawn on
    uint64_t skipped = 0;  // output presents left untouched although frame generation was active
    char lastMessage[160] = {};
};

struct ProduceParams
{
    float threshold = 0.008f; // how different final and HUD-less must be to count as UI
    uint32_t dilation = 1;    // grow the UI area by this many pixels (max 4)
    bool hdr = false;        // SDR mask, PQ pixels from the production OptiHDR converter
    bool cleanup = true;      // also paste where the previous frame had UI but this one does not
};

// Game thread, once per real frame, after `queue` waited for the interop copy of both images.
// `finalImage` and `hudless` rest in D3D12_RESOURCE_STATE_COMMON and are returned to it.
void Produce(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Resource* finalImage, DXGI_FORMAT finalFormat,
             ID3D12Resource* hudless, DXGI_FORMAT hudlessFormat, const ProduceParams& params);

// Game thread, once per real frame that does not call Produce. Stops pasting until the next Produce.
void Invalidate(const char* reason);
// UI-free input is allowed only after a matching completed paste is available.
bool Ready(ID3D12Device* device, UINT width, UINT height);

// Resize / teardown: drops the images (freed once the GPU is done with them).
void Release();

// Any thread that presents DLSS-G's native swapchain. Records and submits on `presentQueue`.
void Paste(IDXGISwapChain* swapChain, ID3D12CommandQueue* presentQueue);

void SetDebugTint(bool enabled);
bool DebugTint();

Status Snapshot();
} // namespace UiPaste
