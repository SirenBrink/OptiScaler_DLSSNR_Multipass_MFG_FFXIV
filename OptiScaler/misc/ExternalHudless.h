#pragma once

// External HUD-less input for DX11 games running through the Dx11wDx12 bridge.
//
// A ReShade add-on (OptiScalerHudless.addon64) captures the frame at the exact point
// before the game draws its UI (placed there by ReshadeEffectShaderToggler) and hands
// the D3D11 texture to OptiScaler through the exports below. Dx11wDx12SC::Present copies
// it across to D3D12 next to the backbuffer and tags it as HUD-less color for the frame
// generator, so generated frames can keep the UI out of interpolation.

#include <d3d11.h>
#include <cstdint>

namespace ExternalHudless
{
inline constexpr uint32_t ApiVersion = 1;

// Layout is part of the export ABI. Only append fields.
struct StatusV1
{
    uint32_t size;       // caller sets sizeof(StatusV1)
    uint32_t apiVersion; // ApiVersion
    uint64_t submitted;  // textures received from the add-on
    uint64_t consumed;   // picked up by the Dx11wDx12 bridge at Present
    uint64_t tagged;     // accepted by the frame generator
    uint64_t rejected;   // dropped before or by the frame generator
    uint32_t lastWidth;
    uint32_t lastHeight;
    uint32_t lastFormat; // DXGI_FORMAT of the last submitted texture
    uint32_t backBufferFormat;
    char lastMessage[160];
};

// Thread-safe. Takes a reference on the texture; the newest submission wins.
void Submit(ID3D11Texture2D* texture);

// Returns the pending texture (caller owns one reference) or nullptr. Clears the slot.
ID3D11Texture2D* Take();

void NoteFormats(uint32_t width, uint32_t height, uint32_t format, uint32_t backBufferFormat);
void MarkTagged();
void MarkRejected(const char* reason);

// Drops any pending texture (swapchain teardown/resize).
void Clear();

StatusV1 Snapshot();
} // namespace ExternalHudless
