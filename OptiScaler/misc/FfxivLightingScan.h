#pragma once
#include "LightingHistory.h"
#include <d3d11_1.h>
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <cstring>
#include <mutex>

// Served by the verified shader hooks in FfxivLightingCapture. Native FFXIV
// DX11 only. This is a lighting-history signal, never an extra exposure multiply.
namespace FfxivLightingScan
{
using Microsoft::WRL::ComPtr;
inline std::atomic<bool> enabled {false}, armed {false};
struct Reading
{
    uint64_t sampleTime = 0, samples = 0, events = 0, eventTime = 0, dropped = 0;
    float gain = 0, changeStops = 0;
    LightingHistory::Curve curve {};
    bool valid = false, failed = false;
    const char* reason = "Waiting for the verified native shader chain.";
};
struct Slot
{
    ComPtr<ID3D11Texture2D> gain, lut;
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11Query> ready;
    uint64_t time = 0, generation = 0;
    bool pending = false;
};
struct Scan
{
    std::array<Slot, 4> slots;
    int collecting = -1;
    uintptr_t adapted = 0, lut = 0, color = 0;
    bool haveGain = false, haveLut = false, haveTone = false, active = false, failed = false;
    std::array<bool, 4> slotLinear {};
    uint64_t samplerKey = UINT64_MAX;
    uint64_t generation = 0, next = 0, events = 0;
    ID3D11DeviceContext* context = nullptr; // Identity only; no owned context/device reference.
    LightingHistory::Gate gate;
    std::mutex mutex;
    Reading reading;
};
inline Scan& State()
{
    // No COM teardown under the loader lock. Completed slots are released on
    // the rendering thread when disabled; in-flight slots drain without waiting.
    static auto* state = new Scan;
    return *state;
}
inline Reading Latest()
{
    auto& s = State(); std::lock_guard lock(s.mutex); return s.reading;
}
inline bool Fresh(const Reading& r, uint64_t now)
{
    return enabled.load() && r.valid && now >= r.sampleTime && now - r.sampleTime <= LightingHistory::MaxAgeMs;
}
inline bool ConsumeReset(uint64_t& cursor)
{
    const auto r = Latest(); const auto now = GetTickCount64();
    if (!Fresh(r, now)) { cursor = r.events; return false; }
    return LightingHistory::Consume(r.events, r.eventTime, now, cursor);
}
inline void Drop(const char* reason = "No complete linked shader chain in the sampled frame.")
{
    auto& s = State(); armed.store(false); s.collecting = -1;
    std::lock_guard lock(s.mutex); ++s.reading.dropped; s.reading.reason = reason;
    if (s.reading.dropped <= 3 || s.reading.dropped % 100 == 0)
        LOG_INFO("FFXIV native lighting: skipped sample {}: {}", s.reading.dropped, reason);
}
inline bool Allocate(ID3D11DeviceContext* c, Slot& slot)
{
    if (slot.ready) return true;
    ComPtr<ID3D11Device> d; c->GetDevice(&d);
    Slot fresh;
    D3D11_TEXTURE2D_DESC td {}; td.Width = td.Height = td.ArraySize = td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R32_FLOAT; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(d->CreateTexture2D(&td, nullptr, &fresh.gain))) return false;
    td.Width = 1024; td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    if (FAILED(d->CreateTexture2D(&td, nullptr, &fresh.lut))) return false;
    D3D11_BUFFER_DESC bd {}; bd.ByteWidth = 48; bd.Usage = D3D11_USAGE_STAGING; bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(d->CreateBuffer(&bd, nullptr, &fresh.constants))) return false;
    D3D11_QUERY_DESC q {D3D11_QUERY_EVENT, 0};
    if (FAILED(d->CreateQuery(&q, &fresh.ready))) return false;
    slot = std::move(fresh); return true;
}
inline bool Texture(ID3D11Resource* resource, UINT width, DXGI_FORMAT format)
{
    ComPtr<ID3D11Texture2D> t;
    if (!resource || FAILED(resource->QueryInterface(IID_PPV_ARGS(&t)))) return false;
    D3D11_TEXTURE2D_DESC d {}; t->GetDesc(&d);
    return d.Width == width && d.Height == 1 && d.ArraySize == 1 && d.MipLevels == 1 &&
           d.SampleDesc.Count == 1 && d.Format == format;
}
inline ComPtr<ID3D11Resource> Srv(ID3D11DeviceContext* c, UINT slot, DXGI_FORMAT format)
{
    ComPtr<ID3D11ShaderResourceView> v; c->PSGetShaderResources(slot, 1, &v);
    if (!v) return {};
    D3D11_SHADER_RESOURCE_VIEW_DESC d {}; v->GetDesc(&d);
    if (d.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D || d.Format != format ||
        d.Texture2D.MostDetailedMip != 0) return {};
    ComPtr<ID3D11Resource> r; v->GetResource(&r); return r;
}
inline ComPtr<ID3D11Resource> Target(ID3D11DeviceContext* c, DXGI_FORMAT format)
{
    ComPtr<ID3D11RenderTargetView> v; c->OMGetRenderTargets(1, &v, nullptr);
    if (!v) return {};
    D3D11_RENDER_TARGET_VIEW_DESC d {}; v->GetDesc(&d);
    if (d.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D || d.Format != format || d.Texture2D.MipSlice != 0) return {};
    ComPtr<ID3D11Resource> r; v->GetResource(&r); return r;
}
inline bool Constants(ID3D11DeviceContext* c, UINT index, UINT bytes, ID3D11Buffer* dest, UINT offset)
{
    ComPtr<ID3D11DeviceContext1> c1; c->QueryInterface(IID_PPV_ARGS(&c1));
    ComPtr<ID3D11Buffer> b; UINT first = 0, count = 4096;
    if (c1) c1->PSGetConstantBuffers1(index, 1, &b, &first, &count);
    else c->PSGetConstantBuffers(index, 1, &b);
    if (!b) return false;
    D3D11_BUFFER_DESC d {}; b->GetDesc(&d);
    if (uint64_t(first) * 16 + bytes > d.ByteWidth || uint64_t(count) * 16 < bytes) return false;
    D3D11_BOX box {first * 16, 0, 0, first * 16 + bytes, 1, 1};
    c->CopySubresourceRegion(dest, 0, offset, 0, 0, b.Get(), 0, &box); return true;
}
inline bool Predicated(ID3D11DeviceContext* c)
{
    ComPtr<ID3D11Predicate> p; BOOL value; c->GetPredication(&p, &value); return p != nullptr;
}
inline bool MagnificationFilter(D3D11_FILTER filter, bool& linear)
{
    // The probes are uniform neutral fields, so their LUT coordinates have zero
    // derivatives: use MAG, independent of MIN/MIP settings. The verified LUT
    // has only one mip. Comparison and min/max reduction samplers are excluded.
    switch (filter)
    {
    case D3D11_FILTER_MIN_MAG_MIP_POINT:
    case D3D11_FILTER_MIN_MAG_POINT_MIP_LINEAR:
    case D3D11_FILTER_MIN_LINEAR_MAG_MIP_POINT:
    case D3D11_FILTER_MIN_LINEAR_MAG_POINT_MIP_LINEAR:
        linear = false; return true;
    case D3D11_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT:
    case D3D11_FILTER_MIN_POINT_MAG_MIP_LINEAR:
    case D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT:
    case D3D11_FILTER_MIN_MAG_MIP_LINEAR:
    case D3D11_FILTER_ANISOTROPIC:
        linear = true; return true;
    default: return false;
    }
}
// ids are one-based exact signatures, not shader names or texture heuristics.
inline void Before(ID3D11DeviceContext* c, UINT id)
{
    auto& s = State();
    if (s.collecting < 0 || id != 1 || !s.haveLut || s.haveTone) return;
    if (Predicated(c)) { Drop(); return; }
    auto lut = Srv(c, 1, DXGI_FORMAT_R16G16B16A16_FLOAT);
    auto color = Target(c, DXGI_FORMAT_R16G16B16A16_FLOAT);
    if (!lut || !color || reinterpret_cast<uintptr_t>(lut.Get()) != s.lut) { Drop("ToneMapping resource link or format changed."); return; }
    ComPtr<ID3D11SamplerState> sampler; c->PSGetSamplers(1, 1, &sampler);
    if (!sampler) { Drop(); return; }
    D3D11_SAMPLER_DESC sd {}; sampler->GetDesc(&sd);
    const uint64_t samplerKey = uint64_t(sd.Filter) | (uint64_t(sd.AddressU) << 32) | (uint64_t(sd.AddressV) << 40);
    if (s.samplerKey != samplerKey)
    {
        s.samplerKey = samplerKey;
        LOG_INFO("FFXIV native lighting: LUT sampler filter=0x{:X}, addressU={}, addressV={}",
            unsigned(sd.Filter), unsigned(sd.AddressU), unsigned(sd.AddressV));
    }
    bool linear = false;
    if (!MagnificationFilter(sd.Filter, linear))
    { Drop("Unsupported tone-map lookup sampler."); return; }
    auto& slot = s.slots[s.collecting];
    if (!Constants(c, 0, 16, slot.constants.Get(), 0) || !Constants(c, 1, 32, slot.constants.Get(), 16)) { Drop("Missing tone-map constants."); return; }
    c->CopyResource(slot.lut.Get(), lut.Get());
    s.slotLinear[s.collecting] = linear;
    s.color = reinterpret_cast<uintptr_t>(color.Get()); s.haveTone = true;
}
inline void After(ID3D11DeviceContext* c, UINT id)
{
    auto& s = State(); if (s.collecting < 0) return;
    if (id == 6 && !s.haveGain)
    {
        if (Predicated(c)) { Drop(); return; }
        auto r = Target(c, DXGI_FORMAT_R32_FLOAT);
        if (!Texture(r.Get(), 1, DXGI_FORMAT_R32_FLOAT)) { Drop("Unsupported adaptation output."); return; }
        c->CopyResource(s.slots[s.collecting].gain.Get(), r.Get());
        s.adapted = reinterpret_cast<uintptr_t>(r.Get()); s.haveGain = true;
    }
    else if (id == 9 && s.haveGain && !s.haveLut)
    {
        if (Predicated(c)) { Drop(); return; }
        auto a = Srv(c, 0, DXGI_FORMAT_R32_FLOAT);
        auto r = Target(c, DXGI_FORMAT_R16G16B16A16_FLOAT);
        if (reinterpret_cast<uintptr_t>(a.Get()) != s.adapted || !Texture(r.Get(), 1024, DXGI_FORMAT_R16G16B16A16_FLOAT))
        { Drop("Adaptation-to-LUT resource link or format changed."); return; }
        s.lut = reinterpret_cast<uintptr_t>(r.Get()); s.haveLut = true;
    }
}
inline void Boundary(ID3D11DeviceContext* c, ID3D11Resource* color, bool hdr)
{
    auto& s = State(); if (s.collecting < 0) return;
    if (hdr || !s.haveTone || reinterpret_cast<uintptr_t>(color) != s.color)
    { Drop(hdr ? "HDR or unknown DLSS colour flags; native lighting rejection disabled." : "Tone-mapped output did not match DLSS input."); return; }
    auto& slot = s.slots[s.collecting];
    slot.generation = s.generation; slot.pending = true; c->End(slot.ready.Get());
    s.collecting = -1; armed.store(false);
}
inline float Half(uint16_t bits)
{
    const int e = (bits >> 10) & 31, m = bits & 1023;
    if (e == 31) return NAN;
    const float v = e ? std::ldexp(float(1024 + m), e - 25) : std::ldexp(float(m), -24);
    return bits & 0x8000 ? -v : v;
}
inline LightingHistory::Curve Curve(const std::array<uint16_t, 4096>& lut, const std::array<float, 12>& cb, bool linear)
{
    LightingHistory::Curve result {};
    for (float v : cb) if (!std::isfinite(v)) return result;
    if (cb[1] <= 0 || cb[6] <= 0 || cb[8] < 0 || cb[9] > 1 || cb[8] >= cb[9]) return result;
    constexpr std::array<float, 4> scene {0.03f, 0.10f, 0.30f, 0.60f};
    for (size_t i = 0; i < result.size(); ++i)
    {
        const float value = scene[i] * cb[1], lum = value * value;
        const float u = std::clamp(lum * cb[6], 0.0f, 1.0f) * (cb[9] - cb[8]) + cb[8];
        // Both interpolation taps must remain inside the one-row LUT. Under
        // this condition wrap, mirror, border and clamp give identical samples;
        // rejecting all non-clamp samplers unnecessarily disabled the live scan.
        if (u < 0.5f / 1024 || u > 1023.5f / 1024) return {};
        const float x = u * 1024 - 0.5f;
        const int left = int(std::floor(x)), point = std::clamp(int(u * 1024), 0, 1023);
        constexpr float weights[3] {0.29891f, 0.58661f, 0.11448f};
        for (int channel = 0; channel < 3; ++channel)
        {
            const float a = Half(lut[std::clamp(left, 0, 1023) * 4 + channel]);
            const float b = Half(lut[std::clamp(left + 1, 0, 1023) * 4 + channel]);
            const float gain = linear ? a + (b - a) * (x - left) : Half(lut[point * 4 + channel]);
            if (!std::isfinite(gain) || gain <= 0) return {};
            result[i] += std::sqrt(lum * gain) * weights[channel];
        }
    }
    return result;
}
inline bool Read(ID3D11DeviceContext* c, ID3D11Resource* r, void* dest, size_t bytes)
{
    D3D11_MAPPED_SUBRESOURCE m {};
    if (FAILED(c->Map(r, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m))) return false;
    std::memcpy(dest, m.pData, bytes); c->Unmap(r, 0); return true;
}
inline void Tick(ID3D11DeviceContext* c, bool wanted)
{
    auto& s = State(); const auto now = GetTickCount64(); armed.store(false);
    if (s.context && s.context != c)
    {
        s.failed = true; enabled.store(false); std::lock_guard lock(s.mutex);
        s.reading.failed = true; s.reading.valid = false; return;
    }
    s.context = c;
    if (s.collecting >= 0) Drop(); // Incomplete shader chain; never publish a guessed association.
    if (s.active != wanted)
    {
        s.active = wanted; s.next = 0; ++s.generation; s.gate = {};
        std::lock_guard lock(s.mutex); s.reading.valid = false; s.reading.eventTime = 0;
        LOG_INFO("FFXIV native lighting scan: {}", wanted ? "enabled" : "disabled");
    }
    enabled.store(wanted && !s.failed);
    // Consume in submission order, even when the fixed ring wraps.
    for (size_t n = 0; n < s.slots.size(); ++n)
    {
        int index = -1;
        for (int i = 0; i < int(s.slots.size()); ++i)
            if (s.slots[i].pending && (index < 0 || s.slots[i].time < s.slots[index].time)) index = i;
        if (index < 0) break;
        auto& slot = s.slots[index]; BOOL done = FALSE;
        auto hr = c->GetData(slot.ready.Get(), &done, sizeof(done), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (hr == S_FALSE || (hr == S_OK && !done))
        {
            if (now - slot.time > 2000) s.failed = true; // Keep in-flight resources, stop new work.
            break;
        }
        slot.pending = false;
        if (FAILED(hr)) { s.failed = true; break; }
        if (wanted && !s.failed && slot.generation == s.generation && now - slot.time <= LightingHistory::MaxAgeMs)
        {
            float gain = 0; std::array<float, 12> constants {}; std::array<uint16_t, 4096> lut {};
            if (Read(c, slot.gain.Get(), &gain, sizeof(gain)) && Read(c, slot.constants.Get(), constants.data(), sizeof(constants)) &&
                Read(c, slot.lut.Get(), lut.data(), sizeof(lut)) && std::isfinite(gain) && gain > 0 && gain < 10000)
            {
                const auto curve = Curve(lut, constants, s.slotLinear[index]);
                if (LightingHistory::Valid(curve))
                {
                    const bool cut = s.gate.Observe(curve, slot.time);
                    std::lock_guard lock(s.mutex); auto& r = s.reading;
                    r.sampleTime = slot.time; r.gain = gain; r.curve = curve; r.changeStops = s.gate.lastDifference;
                    r.valid = true; r.reason = "Verified native DX11 lighting."; ++r.samples;
                    if (cut)
                    {
                        r.events = ++s.events; r.eventTime = slot.time;
                        LOG_INFO("FFXIV native lighting: abrupt tone change {:.3f} stops, gain {:.4f}, event {}", r.changeStops, gain, r.events);
                    }
                    if (r.samples == 1 || r.samples % 100 == 0)
                        LOG_INFO("FFXIV native lighting: {} samples, gain {:.4f}, response {:.4f}/{:.4f}/{:.4f}/{:.4f}, {} cuts, {} dropped",
                            r.samples, gain, curve[0], curve[1], curve[2], curve[3], r.events, r.dropped);
                }
                else { std::lock_guard lock(s.mutex); ++s.reading.dropped; s.reading.reason = "Invalid tone-map response."; }
            }
            else { std::lock_guard lock(s.mutex); ++s.reading.dropped; s.reading.reason = "Readback unavailable or invalid adaptation."; }
        }
    }
    if (s.failed)
    {
        enabled.store(false); std::lock_guard lock(s.mutex);
        if (!s.reading.failed) LOG_ERROR("FFXIV native lighting: readback failed or timed out; scan stopped for this session");
        s.reading.failed = true; s.reading.valid = false;
        return;
    }
    if (!wanted)
    {
        for (auto& slot : s.slots) if (!slot.pending) slot = {};
        return;
    }
    if (now < s.next) return;
    s.next = now + 100; // At most ten tiny readbacks per second, no full-frame copies or file output.
    for (int i = 0; i < int(s.slots.size()); ++i)
    {
        auto& slot = s.slots[i]; if (slot.pending) continue;
        if (!Allocate(c, slot))
        {
            s.failed = true; enabled.store(false); std::lock_guard lock(s.mutex);
            s.reading.failed = true; s.reading.valid = false;
            LOG_ERROR("FFXIV native lighting: unable to allocate readbacks; scan stopped"); return;
        }
        slot.time = now; s.collecting = i;
        s.haveGain = s.haveLut = s.haveTone = false;
        s.adapted = s.lut = s.color = 0; armed.store(true); return;
    }
}
inline void Stop() { enabled.store(false); armed.store(false); }
}
