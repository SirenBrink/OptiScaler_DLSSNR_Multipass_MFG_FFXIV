#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>
#include <array>

namespace FfxivCompanion::Geometry
{
using Microsoft::WRL::ComPtr;
enum class Result { Ready, Unsupported, MissingBuffers, IndexFormat, ResourceFlags, Range, TooLarge, Budget, Allocation, Count };
// One synchronous indexed draw only. No retained context/device/engine references,
// no readback, no duplicate draw, and no changes to shaders, constants or textures.
class Scope
{
    ID3D11DeviceContext* context = nullptr;
    bool attempted = false;
    static constexpr UINT Slots = D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT;
    std::array<ComPtr<ID3D11Buffer>, Slots> vertex, ownedVertex;
    std::array<ID3D11Buffer*, Slots> originalVertices {}, replacementVertices {};
    std::array<UINT, Slots> strides {}, offsets {};
    ComPtr<ID3D11Buffer> index, ownedIndex;
    UINT indexOffset = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
public:
    uint64_t bytes = 0;
    UINT observedStride = 0, vertexBytes = 0, indexBytes = 0, slotMask = 0;
    Scope() = default;
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    Result Enter(ID3D11DeviceContext* c, UINT indexCount, UINT startIndex, uint64_t budget)
    {
        if (attempted || !c || c->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE || !indexCount)
            return Result::Unsupported;
        attempted = true;
        ComPtr<ID3D11Device> device; c->GetDevice(&device);
        if (device->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0) return Result::Unsupported;
        c->IAGetVertexBuffers(0, Slots, originalVertices.data(), strides.data(), offsets.data());
        for (UINT slot = 0; slot < Slots; ++slot)
        {
            vertex[slot].Attach(originalVertices[slot]);
            if (!vertex[slot]) continue;
            if (!slotMask) observedStride = strides[slot];
            slotMask |= 1u << slot;
        }
        c->IAGetIndexBuffer(index.ReleaseAndGetAddressOf(), &format, &indexOffset);
        if (!slotMask || !index) return Result::MissingBuffers;
        if (format != DXGI_FORMAT_R16_UINT && format != DXGI_FORMAT_R32_UINT) return Result::IndexFormat;
        std::array<D3D11_BUFFER_DESC, Slots> vd {};
        std::array<UINT, Slots> owner {};
        for (UINT slot = 0; slot < Slots; ++slot)
        {
            owner[slot] = slot;
            if (!vertex[slot]) continue;
            vertex[slot]->GetDesc(&vd[slot]);
            const auto& desc = vd[slot];
            if (!desc.ByteWidth || desc.MiscFlags || desc.StructureByteStride ||
                (desc.BindFlags & (D3D11_BIND_STREAM_OUTPUT | D3D11_BIND_UNORDERED_ACCESS))) return Result::ResourceFlags;
            if (offsets[slot] >= desc.ByteWidth) return Result::Range;
            if (desc.ByteWidth > 8 * 1024 * 1024) return Result::TooLarge;
            // Multiple slots can reference the same allocation with different
            // strides/offsets. Copy it once, retaining the aliasing relationship.
            for (UINT previous = 0; previous < slot; ++previous)
                if (vertex[slot].Get() == vertex[previous].Get()) { owner[slot] = owner[previous]; break; }
            if (owner[slot] == slot) vertexBytes += desc.ByteWidth;
        }
        D3D11_BUFFER_DESC id {}; index->GetDesc(&id); indexBytes = id.ByteWidth;
        const UINT indexSize = format == DXGI_FORMAT_R16_UINT ? 2 : 4;
        if (!id.ByteWidth || id.MiscFlags || id.StructureByteStride ||
            (id.BindFlags & (D3D11_BIND_STREAM_OUTPUT | D3D11_BIND_UNORDERED_ACCESS)))
            return Result::ResourceFlags;
        if (indexOffset % indexSize ||
            uint64_t(indexOffset) + (uint64_t(startIndex) + indexCount) * indexSize > id.ByteWidth)
            return Result::Range;
        bytes = uint64_t(vertexBytes) + id.ByteWidth;
        if (id.ByteWidth > 2 * 1024 * 1024) return Result::TooLarge;
        if (bytes > budget) return Result::Budget;
        for (UINT slot = 0; slot < Slots; ++slot)
        {
            if (!vertex[slot]) continue;
            if (owner[slot] == slot)
            {
                auto desc = vd[slot]; desc.Usage = D3D11_USAGE_DEFAULT;
                desc.CPUAccessFlags = 0; desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
                if (FAILED(device->CreateBuffer(&desc, nullptr, ownedVertex[slot].ReleaseAndGetAddressOf())))
                    return Result::Allocation;
            }
            replacementVertices[slot] = ownedVertex[owner[slot]].Get();
        }
        id.Usage = D3D11_USAGE_DEFAULT; id.CPUAccessFlags = 0; id.BindFlags = D3D11_BIND_INDEX_BUFFER;
        if (FAILED(device->CreateBuffer(&id, nullptr, ownedIndex.ReleaseAndGetAddressOf()))) return Result::Allocation;
        // Native command layouts may be converted before this draw. Preserve the
        // actual IA stride; no CPU-packet stride assumption is needed for a byte copy.
        // Same context: each copy precedes its consumer. Resources are never mapped
        // by this code; at an actual native draw the source is already unmapped.
        for (UINT slot = 0; slot < Slots; ++slot)
            if (vertex[slot] && owner[slot] == slot) c->CopyResource(ownedVertex[slot].Get(), vertex[slot].Get());
        c->CopyResource(ownedIndex.Get(), index.Get());
        c->IASetVertexBuffers(0, Slots, replacementVertices.data(), strides.data(), offsets.data());
        c->IASetIndexBuffer(ownedIndex.Get(), format, indexOffset);
        context = c;
        return Result::Ready;
    }
    ~Scope()
    {
        if (!context) return;
        context->IASetVertexBuffers(0, Slots, originalVertices.data(), strides.data(), offsets.data());
        context->IASetIndexBuffer(index.Get(), format, indexOffset);
    }
};
}
