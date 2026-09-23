#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <array>
#include <algorithm>

namespace DlssNr
{
// Shares the owning Generation's completion slots. Recycle only AFTER its GPU
// completion marker is visible, before that marker is reused. Never waits/maps
// on the frame path; each End resolves before Use writes its completion marker.
class PreSrTiming
{
public:
    enum Stage : unsigned { Guides, Nr, Sr, Guard, Fg, Compose, Snapshot, StageCount };
    // Four consecutive Before/After slots sample both anchor and skipped frames
    // in each 16-slot cycle. Avoid adding queries to every rendered frame.
    static constexpr unsigned Slots = 16, SampledSlots = 4, Stride = StageCount * 2;
    struct Sample { double ms = 0; unsigned long long count = 0; };
private:
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> queries;
    Microsoft::WRL::ComPtr<ID3D12Resource> readback;
    const UINT64* data = nullptr;
    UINT64 frequency = 0;
    std::array<unsigned, Slots> pending {};
    std::array<Sample, StageCount> samples {};
public:
    ~PreSrTiming() { if (readback && data) readback->Unmap(0, nullptr); }
    bool Init(ID3D12Device* device, ID3D12CommandQueue* queue)
    {
        if (FAILED(queue->GetTimestampFrequency(&frequency)) || !frequency) return false;
        D3D12_QUERY_HEAP_DESC q {}; q.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP; q.Count = Slots * Stride;
        if (FAILED(device->CreateQueryHeap(&q, IID_PPV_ARGS(&queries)))) return false;
        D3D12_RESOURCE_DESC d {}; d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width = Slots * Stride * sizeof(UINT64); d.Height = d.DepthOrArraySize = d.MipLevels = 1;
        d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES heap {}; heap.Type = D3D12_HEAP_TYPE_READBACK;
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)))) return false;
        void* mapped = nullptr;
        if (FAILED(readback->Map(0, nullptr, &mapped))) return false;
        data = static_cast<const UINT64*>(mapped);
        return true;
    }
    const Sample& Get(Stage stage) const { return samples[stage]; }
    bool Available() const { return data != nullptr; }
    UINT64 Frequency() const { return frequency; }
    void Recycle(unsigned slot)
    {
        if (!data || slot >= Slots || !pending[slot]) return;
        for (unsigned stage = 0; stage < StageCount; ++stage)
        {
            if (!(pending[slot] & (1u << stage))) continue;
            const auto offset = slot * Stride + stage * 2;
            const auto begin = data[offset], end = data[offset + 1];
            if (!begin || end < begin) continue;
            const double ms = double(end - begin) * 1000.0 / double(frequency);
            auto& sample = samples[stage];
            ++sample.count;
            // Smooth recent per-call cost; skipped NR/SR/FG calls record nothing.
            sample.ms += (ms - sample.ms) / double(std::min(sample.count, 60ull));
        }
        pending[slot] = 0;
    }
    class Scope
    {
        PreSrTiming& owner;
        ID3D12GraphicsCommandList* cmd;
        unsigned slot, stage;
        bool active;
    public:
        Scope(PreSrTiming& timing, ID3D12GraphicsCommandList* commands, unsigned index, Stage step)
            : owner(timing), cmd(commands), slot(index), stage(step),
              active(timing.Available() && index < SampledSlots)
        {
            if (active) cmd->EndQuery(owner.queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * Stride + stage * 2);
        }
        ~Scope() { End(); }
        void End()
        {
            if (!active) return;
            active = false;
            const auto index = slot * Stride + stage * 2;
            cmd->EndQuery(owner.queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, index + 1);
            cmd->ResolveQueryData(owner.queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, index, 2,
                                 owner.readback.Get(), index * sizeof(UINT64));
            owner.pending[slot] |= 1u << stage;
        }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
    };
};
}
