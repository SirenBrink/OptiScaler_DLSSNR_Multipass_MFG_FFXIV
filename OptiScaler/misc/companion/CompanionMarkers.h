#pragma once
#include "CompanionCore.h"
#include <d3d11_1.h>
#include <wrl/client.h>
#include <algorithm>

namespace FfxivCompanion
{
// A bounded diagnostic renderer for the DX11 source image. No pipeline bindings,
// shader compilation, GPU readback, camera prediction or extra presentation.
// ClearView paints opaque line rectangles without changing the immediate context's state.
inline bool PaintMarkers(ID3D11DeviceContext* context, ID3D11Texture2D* target, const Snapshot& snapshot)
{
    if (!context || !target || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) return false;
    const auto& f = snapshot.frame;
    if (!(f.flags & Preview) || !f.count || f.count > MaxPlates) return false;
    D3D11_TEXTURE2D_DESC desc {};
    target->GetDesc(&desc);
    // Never rescale coordinates from a different source frame after a resolution change.
    if (desc.Width != f.width || desc.Height != f.height || !desc.Width || !desc.Height ||
        desc.Width > 16384 || desc.Height > 16384 || desc.ArraySize != 1 || desc.SampleDesc.Count != 1 ||
        !(desc.BindFlags & D3D11_BIND_RENDER_TARGET)) return false;
    DXGI_FORMAT format;
    switch (desc.Format)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        format = DXGI_FORMAT_R8G8B8A8_UNORM; break;
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        format = DXGI_FORMAT_B8G8R8A8_UNORM; break;
    default: return false; // This runs on the SDR DX11 copy before OptiHDR conversion.
    }
    std::array<D3D11_RECT, MaxPlates * 6> rects {};
    UINT count = 0;
    auto rect = [&](float left, float top, float right, float bottom)
    {
        if (!std::isfinite(left) || !std::isfinite(top) || !std::isfinite(right) || !std::isfinite(bottom)) return;
        // Clamp before integer conversion, and clip each original edge independently.
        D3D11_RECT r {
            static_cast<LONG>(std::floor(std::clamp(left, 0.0f, float(desc.Width)))),
            static_cast<LONG>(std::floor(std::clamp(top, 0.0f, float(desc.Height)))),
            static_cast<LONG>(std::ceil(std::clamp(right, 0.0f, float(desc.Width)))),
            static_cast<LONG>(std::ceil(std::clamp(bottom, 0.0f, float(desc.Height))))};
        if (r.left < r.right && r.top < r.bottom && count < rects.size()) rects[count++] = r;
    };
    for (uint32_t i = 0; i < f.count; ++i)
    {
        const auto& p = snapshot.plates[i];
        rect(p.anchorX - 5, p.anchorY, p.anchorX + 6, p.anchorY + 1);
        rect(p.anchorX, p.anchorY - 5, p.anchorX + 1, p.anchorY + 6);
        if (p.flags & BoundsValid)
        {
            rect(p.left, p.top, p.right, p.top + 1);
            rect(p.left, p.bottom - 1, p.right, p.bottom);
            rect(p.left, p.top, p.left + 1, p.bottom);
            rect(p.right - 1, p.top, p.right, p.bottom);
        }
    }
    // ClearView with ZERO rectangles clears the entire view. Never call it for an empty list.
    if (!count) return false;
    using Microsoft::WRL::ComPtr;
    ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    D3D11_FEATURE_DATA_D3D11_OPTIONS options {};
    if (FAILED(device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &options, sizeof(options))) ||
        !options.ClearView) return false;
    ComPtr<ID3D11DeviceContext1> context1;
    if (FAILED(context->QueryInterface(IID_PPV_ARGS(&context1)))) return false;
    D3D11_RENDER_TARGET_VIEW_DESC viewDesc {};
    viewDesc.Format = format;
    viewDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11RenderTargetView> view;
    if (FAILED(device->CreateRenderTargetView(target, &viewDesc, &view))) return false;
    const FLOAT green[4] {64.0f / 255.0f, 1.0f, 128.0f / 255.0f, 1.0f};
    context1->ClearView(view.Get(), green, rects.data(), count);
    return true;
}
}
