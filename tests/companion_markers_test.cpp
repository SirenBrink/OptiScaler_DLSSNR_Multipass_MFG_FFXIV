#include "../OptiScaler/misc/companion/CompanionMarkers.h"
#include <cassert>
#include <cstdio>
#include <vector>
#pragma comment(lib, "d3d11.lib")
using Microsoft::WRL::ComPtr;
using namespace FfxivCompanion;
int main()
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    assert(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &device, nullptr, &context)));
    D3D11_TEXTURE2D_DESC td {};
    td.Width = td.Height = 64; td.MipLevels = td.ArraySize = td.SampleDesc.Count = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.BindFlags = D3D11_BIND_RENDER_TARGET;
    std::vector<uint32_t> pixels(64 * 64, 0xFF302010);
    D3D11_SUBRESOURCE_DATA data {pixels.data(), 64 * 4, 0};
    ComPtr<ID3D11Texture2D> source, target, readback;
    assert(SUCCEEDED(device->CreateTexture2D(&td, &data, &source)));
    assert(SUCCEEDED(device->CreateTexture2D(&td, &data, &target)));
    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    assert(SUCCEEDED(device->CreateTexture2D(&td, nullptr, &readback)));
    ComPtr<ID3D11RenderTargetView> originalView;
    assert(SUCCEEDED(device->CreateRenderTargetView(source.Get(), nullptr, &originalView)));
    auto* view = originalView.Get(); context->OMSetRenderTargets(1, &view, nullptr);
    D3D11_VIEWPORT viewport {3, 4, 40, 30, 0, 1}; context->RSSetViewports(1, &viewport);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    Snapshot s; s.frame = {1, 1, 64, 64, 1, Preview};
    auto& p = s.plates[0]; p.objectId = 1; p.flags = BoundsValid;
    p.anchorX = p.anchorY = 32; p.left = p.top = 10; p.right = p.bottom = 54;
    context->CopyResource(target.Get(), source.Get());
    assert(PaintMarkers(context.Get(), target.Get(), s));
    ComPtr<ID3D11RenderTargetView> bound;
    context->OMGetRenderTargets(1, &bound, nullptr); assert(bound.Get() == originalView.Get());
    UINT count = 1; D3D11_VIEWPORT after {}; context->RSGetViewports(&count, &after);
    assert(count == 1 && after.TopLeftX == 3 && after.TopLeftY == 4 && after.Width == 40);
    D3D11_PRIMITIVE_TOPOLOGY topology; context->IAGetPrimitiveTopology(&topology);
    assert(topology == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    auto pixel = [&](ID3D11Texture2D* texture, unsigned x, unsigned y) {
        context->CopyResource(readback.Get(), texture); D3D11_MAPPED_SUBRESOURCE mapped {};
        assert(SUCCEEDED(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped)));
        uint32_t value; memcpy(&value, static_cast<char*>(mapped.pData) + y * mapped.RowPitch + 4 * x, 4);
        context->Unmap(readback.Get(), 0); return value;
    };
    assert(pixel(target.Get(), 32, 32) == 0xFF80FF40);
    assert(pixel(target.Get(), 10, 15) == 0xFF80FF40);
    assert(pixel(target.Get(), 20, 20) == 0xFF302010);
    assert(pixel(source.Get(), 32, 32) == 0xFF302010);
    context->CopyResource(target.Get(), source.Get());
    s.frame.width = 128; assert(!PaintMarkers(context.Get(), target.Get(), s));
    s.frame.width = 64; s.frame.count = 0; assert(!PaintMarkers(context.Get(), target.Get(), s));
    s.frame.count = 1; p.anchorX = p.anchorY = -1000; p.flags = 0;
    assert(!PaintMarkers(context.Get(), target.Get(), s));
    assert(pixel(target.Get(), 32, 32) == 0xFF302010);
    p.flags = BoundsValid; p.left = -10; p.top = 20; p.right = 12; p.bottom = 40;
    assert(PaintMarkers(context.Get(), target.Get(), s));
    assert(pixel(target.Get(), 0, 20) == 0xFF80FF40);
    assert(pixel(target.Get(), 0, 30) == 0xFF302010);
    puts("PASS: WARP pixels, original-image preservation, pipeline-state preservation, resize and offscreen guards");
}
