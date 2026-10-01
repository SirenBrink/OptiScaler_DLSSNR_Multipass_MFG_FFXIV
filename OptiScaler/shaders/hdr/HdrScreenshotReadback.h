#pragma once
#include <d3d12.h>
#include <wrl/client.h>
namespace Hdr10::Screenshot {
inline bool IsSdrFormat(DXGI_FORMAT format)
{
    switch(format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        return true;
    default: return false;
    }
}
struct Readback {
    Microsoft::WRL::ComPtr<ID3D12Resource> readback, source;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 bytes = 0;
    HRESULT Allocate(ID3D12Device* device, ID3D12Resource* image, bool hdr = true)
    {
        if (!device || !image) return E_INVALIDARG;
        auto desc = image->GetDesc();
        if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
            (hdr ? desc.Format != DXGI_FORMAT_R10G10B10A2_UNORM : !IsSdrFormat(desc.Format)) ||
            desc.SampleDesc.Count != 1 || desc.DepthOrArraySize != 1 || !desc.Width || !desc.Height ||
            desc.Width > 32768 || desc.Height > 32768) return E_INVALIDARG;
        device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
        D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC buffer{}; buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = bytes; buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        auto hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback));
        if (FAILED(hr)) return hr;
        hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
        if (SUCCEEDED(hr)) source = image;
        return hr;
    }
    // Caller must put source in COPY_SOURCE and retain this object until its
    // queue fence completes (or the unsubmitted command list is discarded).
    void Record(ID3D12GraphicsCommandList* commands)
    {
        D3D12_TEXTURE_COPY_LOCATION from{}, to{};
        from.pResource = source.Get(); from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        to.pResource = readback.Get(); to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        to.PlacedFootprint = footprint;
        commands->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    }
    void RecordPreservingState(ID3D12GraphicsCommandList* commands, D3D12_RESOURCE_STATES state)
    {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition={source.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,state,D3D12_RESOURCE_STATE_COPY_SOURCE};
        if(state!=D3D12_RESOURCE_STATE_COPY_SOURCE) commands->ResourceBarrier(1,&barrier);
        Record(commands);
        barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_SOURCE;
        barrier.Transition.StateAfter=state;
        if(state!=D3D12_RESOURCE_STATE_COPY_SOURCE) commands->ResourceBarrier(1,&barrier);
    }
};
}
