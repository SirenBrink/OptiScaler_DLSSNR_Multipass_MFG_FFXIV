#pragma once
#include "CompanionGeometry.h"
#include "CompanionPackets.h"
#include <array>
#include <atomic>
#include <mutex>

namespace FfxivCompanion::Gpu
{
inline constexpr GUID shaderTag {0x2303cb05,0x21d7,0x4365,{0x9c,0x53,0xd4,0xf7,0x7b,0x82,0x2a,0x41}};
inline std::atomic<bool> requested {false};
inline std::atomic<uint64_t> matched {0}, copied {0}, unsupported {0}, budgetSkips {0}, allocationFailures {0};
inline std::atomic<uint64_t> copiedBytes {0}, cpuUs {0}, cpuPeakUs {0}, lastAgeMs {0}, publications {0};
inline std::array<std::atomic<uint64_t>,4> layouts {};
inline std::array<std::atomic<uint64_t>,4> tagged {};
inline std::array<std::atomic<uint64_t>,4> copiedLayouts {};
inline std::array<std::atomic<UINT>,4> observedStrides {}, observedVertexBytes {}, observedIndexBytes {};
inline std::array<std::atomic<UINT>,4> observedSlotMasks {};
inline std::array<std::atomic<uint64_t>,static_cast<size_t>(Geometry::Result::Count)> results {};
inline std::atomic<uint64_t> readFailures {0}, busy {0}, stale {0};
inline thread_local bool processing = false;
struct SamplingWindow
{
    ULONGLONG started = 0;
    UINT layouts = 0;
    uint64_t bytes = 0;
    bool Allow(ULONGLONG now, UINT bit)
    {
        if (now - started >= 100) { started = now; layouts = 0; bytes = 0; }
        if (layouts & bit) return false;
        layouts |= bit;
        return true;
    }
};
inline thread_local SamplingWindow sampling;
struct Identity { uintptr_t texture; UINT layout; };
inline std::mutex mutex;
inline std::array<Identity, Packets::MaxPackets> identities {};
inline size_t identityCount = 0;
inline ULONGLONG publishedAt = 0;
inline UINT Identify(const void* data, SIZE_T size)
{
    if (!data || size < 32 || size > 256 * 1024 || std::memcmp(data, "DXBC", 4)) return 0;
    UINT crc = ~0u;
    for (auto p = static_cast<const unsigned char*>(data); size--; ++p)
    { crc ^= *p; for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1))); }
    switch (~crc)
    {
    case 3865947726u: return 1; // native layout 0
    case 1823062889u: return 5; // native layout 4
    case 3759127293u: return 6; // native layout 5
    case 2966694105u: return 7; // native layout 6
    default: return 0;
    }
}
inline void TagShader(ID3D11PixelShader* shader, const void* code, SIZE_T size)
{
    if (auto id = Identify(code, size); shader && id)
        if (SUCCEEDED(shader->SetPrivateData(shaderTag, sizeof(id), &id))) ++tagged[id == 1 ? 0 : id - 4];
}
inline void Clear()
{
    std::lock_guard lock(mutex); identityCount = 0; publishedAt = 0;
}
inline void Publish(const std::vector<Packets::Packet>& packets)
{
    if (!requested.load(std::memory_order_relaxed)) return;
    std::array<Identity, Packets::MaxPackets> next {}; size_t count = 0;
    for (const auto& p : packets)
    {
        uintptr_t wrapper = 0, texture = 0; UINT flags = 0;
        memcpy(&wrapper, p.header.data() + 0x10, 8); memcpy(&flags, p.header.data() + 0x30, 4);
        // Identity hint only. Never call COM through this pointer; match a live API reference below.
        if (!wrapper || !Packets::Read(wrapper + 0x68, texture) || !texture) { ++readFailures; continue; }
        bool duplicate = false;
        for (size_t i = 0; i < count; ++i) duplicate |= next[i].texture == texture && next[i].layout == (flags & 15);
        if (!duplicate && count < next.size()) next[count++] = {texture, flags & 15};
    }
    std::unique_lock lock(mutex, std::try_to_lock);
    if (!lock.owns_lock()) { ++busy; return; }
    identities = next; identityCount = count; publishedAt = GetTickCount64(); ++publications;
}
// Called only through the existing immediate-context draw hook. Material identity
// is not exact plate ownership: shared HUD draws can match too. All such draws keep
// their own arguments and order; this path must NEVER hide/suppress a layer by identity.
template<class F> void AroundIndexed(ID3D11DeviceContext* c, UINT count, UINT start, F&& original)
{
    if (!requested.load(std::memory_order_relaxed) || processing) { original(); return; }
    struct Guard { Guard() { processing = true; } ~Guard() { processing = false; } } guard;
    UINT id = 0, size = sizeof(id);
    Geometry::ComPtr<ID3D11PixelShader> shader; c->PSGetShader(&shader, nullptr, nullptr);
    if (!shader || FAILED(shader->GetPrivateData(shaderTag, &size, &id)) || size != sizeof(id) ||
        (id != 1 && id != 5 && id != 6 && id != 7)) { original(); return; }
    Geometry::ComPtr<ID3D11ShaderResourceView> view; c->PSGetShaderResources(0, 1, &view);
    Geometry::ComPtr<ID3D11Resource> resource; if (view) view->GetResource(&resource);
    if (!resource) { original(); return; }
    const auto now = GetTickCount64(); bool match = false;
    {
        std::unique_lock lock(mutex, std::try_to_lock);
        if (!lock.owns_lock()) ++busy;
        else if (!publishedAt || now - publishedAt > 250) ++stale;
        else for (size_t i = 0; i < identityCount; ++i)
            if (identities[i].texture == reinterpret_cast<uintptr_t>(resource.Get()) && identities[i].layout == id - 1)
            { match = true; lastAgeMs.store(now - publishedAt); break; }
    }
    if (!match) { original(); return; }
    const UINT layoutIndex = id == 1 ? 0 : id - 4;
    ++matched; ++layouts[layoutIndex];
    // Diagnostic sampling, not a permanent renderer: at most four attempts and
    // 32 MiB per 100 ms. No idle GPU references or cache survives plugin shutdown.
    const UINT layoutBit = 1u << (id == 1 ? 0 : id - 4);
    if (!sampling.Allow(now, layoutBit)) { ++budgetSkips; original(); return; }
    LARGE_INTEGER begin, end, frequency; QueryPerformanceCounter(&begin); QueryPerformanceFrequency(&frequency);
    {
        Geometry::Scope scope;
        const auto result = scope.Enter(c, count, start, 32ull * 1024 * 1024 - sampling.bytes);
        ++results[static_cast<size_t>(result)];
        observedStrides[layoutIndex].store(scope.observedStride);
        observedSlotMasks[layoutIndex].store(scope.slotMask);
        observedVertexBytes[layoutIndex].store(scope.vertexBytes);
        observedIndexBytes[layoutIndex].store(scope.indexBytes);
        if (result == Geometry::Result::Ready) { ++copied; ++copiedLayouts[layoutIndex]; sampling.bytes += scope.bytes; copiedBytes += scope.bytes; }
        else if (result == Geometry::Result::Budget) ++budgetSkips;
        else if (result == Geometry::Result::Allocation) ++allocationFailures;
        else ++unsupported;
        original(); // exactly once, with owned buffers only after complete preparation
    }
    QueryPerformanceCounter(&end);
    const uint64_t elapsed = (end.QuadPart - begin.QuadPart) * 1000000ull / frequency.QuadPart;
    cpuUs += elapsed;
    auto peak = cpuPeakUs.load(); while (elapsed > peak && !cpuPeakUs.compare_exchange_weak(peak, elapsed)) {}
}
}
