#pragma once
#include <Windows.h>
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

namespace FfxivCompanion::Packets
{
// Layout verified against the September 17 image and the existing NamePlate captures.
// A native batch derives vertex-buffer offsets from header+0x28. That pointer MUST
// stay in the game's allocation: substituting a heap copy would generate invalid indices.
// We own replacement command headers only. Resources/vertices remain borrowed for
// one synchronous native batch; no engine pointer enters the presentation worker.
inline constexpr size_t MaxPackets = 512, MaxEntries = 8192;
struct Queue { uintptr_t address = 0, head = 0; uint32_t count = 0; };
struct Packet
{
    uintptr_t original = 0;
    uintptr_t alignmentPadding = 0;
    alignas(16) std::array<unsigned char, 64> header {};
};
using Entry = std::array<uintptr_t, 2>;
template<class T> bool Read(uintptr_t address, T& value)
{
    SIZE_T got = 0;
    return address && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address),
        &value, sizeof(value), &got) && got == sizeof(value);
}
inline bool ReadBytes(uintptr_t address, void* out, size_t size)
{
    SIZE_T got = 0;
    return address && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address),
        out, size, &got) && got == size;
}
inline Queue Snapshot(uintptr_t queue)
{
    Queue q;
    uint32_t blocks = 0, remaining = 0;
    if (!Read(queue + 0x50, q.head) || !Read(queue + 0x68, remaining) || !Read(queue + 0x6c, blocks) ||
        blocks > 128 || remaining > 64 || (!blocks && remaining > 1) || (blocks && blocks * 64 < remaining)) return {};
    q.address = queue;
    q.count = blocks ? blocks * 64 - remaining : 0;
    return q;
}
inline bool Supported(const Packet& packet)
{
    uint32_t kind = 0, count = 0, format = 0;
    uint16_t extension = 0;
    uintptr_t vertices = 0;
    memcpy(&kind, packet.header.data(), 4);
    memcpy(&extension, packet.header.data() + 4, 2);
    memcpy(&count, packet.header.data() + 0x20, 4);
    memcpy(&vertices, packet.header.data() + 0x28, 8);
    memcpy(&format, packet.header.data() + 0x30, 4);
    const auto layout = format & 15;
    // Extended styles 1/2 contain additional material data beyond the copied header.
    // Leave those commands native until their complete layouts are supported.
    return packet.original && kind == 0x22 && extension == 0 && count && count <= 65536 && count % 4 == 0 && vertices &&
        (layout == 0 || layout == 4 || layout == 5 || layout == 6);
}
inline bool Capture(const Queue& before, const Queue& after, std::vector<Packet>& output)
{
    output.clear();
    if (!before.address || before.address != after.address || after.count < before.count ||
        after.count > MaxEntries || after.count - before.count > MaxPackets) return false;
    auto block = after.head;
    for (uint32_t index = 0; index < after.count; ++index)
    {
        if (index && index % 64 == 0)
            if (!Read(block + 0x3f0, block)) { output.clear(); return false; }
        if (index < before.count) continue;
        Entry entry;
        Packet p;
        if (!Read(block + (index % 64) * 16, entry) || !Read(entry[1], p.header))
        { output.clear(); return false; }
        p.original = entry[1];
        if (!Supported(p)) { output.clear(); return false; }
        for (const auto& previous : output)
            if (previous.original == p.original) { output.clear(); return false; }
        output.push_back(p);
    }
    return true;
}

enum class Match { None, Complete, Invalid };
// Prepare a separate pointer list. Original queue entries and command memory are never edited.
inline Match Substitute(std::vector<Entry>& entries, const std::vector<Packet>& packets)
{
    if (packets.empty() || packets.size() > MaxPackets || entries.size() > MaxEntries) return Match::Invalid;
    std::array<unsigned, MaxPackets> seen {};
    size_t found = 0;
    for (auto& entry : entries)
    {
        for (size_t i = 0; i < packets.size(); ++i)
        {
            if (entry[1] != packets[i].original) continue;
            std::array<unsigned char, 64> live;
            if (++seen[i] != 1 || !Read(packets[i].original, live) || live != packets[i].header) return Match::Invalid;
            entry[1] = reinterpret_cast<uintptr_t>(packets[i].header.data());
            ++found;
            break;
        }
    }
    if (!found) return Match::None;
    return found == packets.size() ? Match::Complete : Match::Invalid;
}

class PointerScope
{
    PVOID volatile* address = nullptr;
    PVOID original = nullptr, replacement = nullptr;
public:
    bool Enter(uintptr_t field, uintptr_t expected, uintptr_t value)
    {
        if (field % alignof(void*) || !expected || !value) return false;
        MEMORY_BASIC_INFORMATION info {};
        if (!VirtualQuery(reinterpret_cast<void*>(field), &info, sizeof(info)) || info.State != MEM_COMMIT ||
            (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
            !(info.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) ||
            field + sizeof(void*) > reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize) return false;
        auto target = reinterpret_cast<PVOID volatile*>(field);
        original = reinterpret_cast<void*>(expected); replacement = reinterpret_cast<void*>(value);
        if (InterlockedCompareExchangePointer(target, replacement, original) != original) return false;
        address = target;
        return true;
    }
    PointerScope() = default;
    PointerScope(const PointerScope&) = delete;
    PointerScope& operator=(const PointerScope&) = delete;
    ~PointerScope() { if (address) InterlockedCompareExchangePointer(address, original, replacement); }
};
}
