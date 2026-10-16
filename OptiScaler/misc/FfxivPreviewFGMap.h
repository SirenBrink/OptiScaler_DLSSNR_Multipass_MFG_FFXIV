#pragma once
#include <d3d11.h>
#include <d3d12.h>
#include <functional>
#include <memory>

namespace FfxivPreviewFGMap {
struct Image {
    ID3D12Resource* resource = nullptr;
    std::shared_ptr<void> owner;
};
void Capture(ID3D11DeviceContext* context, unsigned history,
             ID3D11ShaderResourceView* previewMotion,
             const std::function<void()>& nativeDraw);
void PublishForHudPaste();
void EndFrame();
Image Open(ID3D12Device* device, unsigned width, unsigned height);
}
