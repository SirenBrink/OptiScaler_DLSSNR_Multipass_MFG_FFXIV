#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include "MipBiasPolicy.h"

// FFXIV's cached samplers cannot be updated by changing CreateSamplerState alone.
// Keep logical bindings intact (including GetSamplers/state backups) and substitute
// immutable native sampler variants at bind time. No draw hooks or GPU waits.
namespace Dx11AutomaticMipBias
{
using Microsoft::WRL::ComPtr;
using Set = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11SamplerState* const*);
using Get = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11SamplerState**);
inline constexpr unsigned Stages = 6, Slots = D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT;
inline constexpr size_t CacheLimit = 256;
struct Binding { ComPtr<ID3D11SamplerState> logical, physical; };
struct Entry { ComPtr<ID3D11SamplerState> source, variant; };
class Binder
{
    std::recursive_mutex mutex;
    ID3D11DeviceContext* context = nullptr; // identity only; released explicitly at NGX shutdown
    std::array<std::array<Binding, Slots>, Stages> bindings {};
    std::unordered_map<ID3D11SamplerState*, Entry> cache;
    std::array<Set, Stages> setters {};
    std::array<Get, Stages> getters {};
    float target = 0;
    bool enabled = false;
    std::atomic<bool> tracking {false};
    std::atomic<float> reported {0};
    std::atomic<unsigned long long> creations {0};

    ComPtr<ID3D11SamplerState> Variant(ID3D11SamplerState* source)
    {
        if (!source || !enabled) return source;
        if (auto it = cache.find(source); it != cache.end()) return it->second.variant;
        D3D11_SAMPLER_DESC desc {}; source->GetDesc(&desc);
        const float changed = MipBiasPolicy::Apply(desc.MipLODBias, desc.MinLOD, desc.MaxLOD,
            unsigned(desc.Filter), target, true, false, false);
        if (cache.size() >= CacheLimit) cache.clear();
        Entry entry; entry.source = source;
        if (changed == desc.MipLODBias) {
            entry.variant = source; cache.emplace(source, std::move(entry)); return source;
        }
        ComPtr<ID3D11Device> device; source->GetDevice(&device);
        desc.MipLODBias = changed;
        ++creations;
        // Failed creation is cached as a native fallback, avoiding retry storms.
        if (!device || FAILED(device->CreateSamplerState(&desc, &entry.variant))) entry.variant = source;
        auto result = entry.variant;
        cache.emplace(source, std::move(entry));
        return result;
    }
public:
    void Attach(ID3D11DeviceContext* c, const std::array<Set, Stages>& s, const std::array<Get, Stages>& g)
    {
        std::lock_guard lock(mutex);
        context = c; setters = s; getters = g;
    }
    void Bind(unsigned stage, ID3D11DeviceContext* c, UINT start, UINT count, ID3D11SamplerState* const* input)
    {
        if (!tracking.load(std::memory_order_relaxed) || c != context || !input || start > Slots || count > Slots-start)
        { setters[stage](c, start, count, input); return; }
        std::lock_guard lock(mutex);
        std::array<ID3D11SamplerState*, Slots> output {};
        for (UINT i=0; i<count; ++i) {
            auto& b = bindings[stage][start+i];
            if (b.logical.Get() != input[i]) {
                b.logical = input[i]; b.physical = Variant(input[i]);
            }
            output[i] = b.physical.Get();
        }
        setters[stage](c, start, count, output.data());
    }
    void Read(unsigned stage, ID3D11DeviceContext* c, UINT start, UINT count, ID3D11SamplerState** output)
    {
        getters[stage](c, start, count, output);
        if (!tracking.load(std::memory_order_relaxed) || c != context || !output || start > Slots || count > Slots-start) return;
        std::lock_guard lock(mutex);
        for (UINT i=0; i<count; ++i) {
            const auto& b = bindings[stage][start+i];
            if (output[i] && output[i] == b.physical.Get() && b.logical.Get() != output[i]) {
                output[i]->Release(); output[i] = b.logical.Get();
                if (output[i]) output[i]->AddRef();
            }
        }
    }
    bool Update(ID3D11DeviceContext* c, bool active, unsigned render, unsigned output)
    {
        if (c != context) return false;
        std::lock_guard lock(mutex);
        active = active && render && output && render <= output;
        // epsilon=1: conservative end of NVIDIA's recommended interval. This is
        // the final world-sampler target, never added to a previous override.
        const float next = active ? MipBiasPolicy::Recommended(render, output) : 0;
        if (active == enabled && (!active || std::abs(next-target)<0.0001f)) return false;
        enabled = active; target = next; reported.store(next);
        cache.clear(); tracking.store(true);
        // Update already-bound samplers too. Native Get observes ClearState,
        // ExecuteCommandList and context-state swaps without trusting a shadow.
        for (unsigned stage=0; stage<Stages; ++stage) {
            std::array<ID3D11SamplerState*, Slots> actual {}, desired {};
            getters[stage](c, 0, Slots, actual.data());
            for (unsigned i=0; i<Slots; ++i) {
                auto& b = bindings[stage][i];
                if (actual[i] != b.physical.Get()) b.logical = actual[i];
                if (actual[i]) actual[i]->Release();
                b.physical = Variant(b.logical.Get()); desired[i] = b.physical.Get();
            }
            setters[stage](c, 0, Slots, desired.data());
        }
        if (!enabled) { bindings = {}; cache.clear(); tracking.store(false); }
        return true;
    }
    void Clear(ID3D11DeviceContext* c)
    {
        Update(c, false, 0, 0);
        std::lock_guard lock(mutex);
        bindings = {}; cache.clear(); enabled = false; tracking.store(false); reported.store(0);
    }
    bool Ready() const { return tracking.load(); }
    float Target() const { return reported.load(); }
    unsigned long long Creations() const { return creations.load(); }
};
inline Binder binder;
}
