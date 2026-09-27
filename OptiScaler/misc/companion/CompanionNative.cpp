#include "pch.h"
#include "CompanionNative.h"
#include "CompanionPackets.h"
#include "CompanionRuntime.h"
#include "CompanionGpu.h"
#include "CompanionLayer.h"
#include <detours/detours.h>
#include <filesystem>

namespace FfxivCompanion::Native
{
using Batch = void(__fastcall*)(uintptr_t, char);
static Batch batch16 = nullptr, batch32 = nullptr;
using Enqueue=int(__fastcall*)(uintptr_t,uintptr_t);
using BindState=void(__fastcall*)(uintptr_t,uintptr_t,UINT);
static Enqueue enqueue=nullptr;
static BindState bindState=nullptr;
static int __fastcall OnEnqueue(uintptr_t context,uintptr_t command)
{Layer::EmitVisibilityCommand(command);return enqueue(context,command);}
static void __fastcall OnBindState(uintptr_t context,uintptr_t state,UINT instances)
{bindState(context,state,instances);Layer::BindVisibilityCommand(context,state);}
static uintptr_t base = 0;
static std::atomic<bool> installed {false}, requested {false};
static std::atomic<uint64_t> captures {0}, replacements {0}, packetCount {0}, fallbacks {0};
struct Pending
{
    Packets::Queue before;
    std::vector<Packets::Packet> packets;
    std::vector<Packets::Entry> entries;
    ULONGLONG capturedAt = 0;
    bool begun = false, inside = false;
};
static thread_local Pending pending;

static Packets::Queue Queue()
{
    uintptr_t module = 0, renderer = 0, queue = 0;
    if (!Packets::Read(base + 0x2aa0590, module) || !Packets::Read(module + 8, renderer) ||
        !Packets::Read(renderer + 8, queue)) return {};
    return Packets::Snapshot(queue);
}
static void Begin() noexcept
{
    if (pending.inside) return;
    pending.begun = false;
    if (!pending.packets.empty()) { ++fallbacks; pending.packets.clear(); }
    if (!requested.load() || !installed.load() || pending.inside) return;
    Layer::BeginSnapshot();
    pending.before = Queue();
    pending.begun = pending.before.address != 0;
}
static void End() noexcept
{
    if (pending.inside) return;
    const bool begun = pending.begun;
    pending.begun = false;
    if (!begun || !requested.load() || !installed.load()) return;
    try
    {
        if (!Packets::Capture(pending.before, Queue(), pending.packets)) { ++fallbacks; Gpu::Clear(); Layer::InvalidateFrame(); return; }
        Gpu::Publish(pending.packets);
        Layer::Capture(pending.before);
        if (pending.packets.empty()) return;
        pending.capturedAt = GetTickCount64();
        ++captures;
    }
    catch (...) { pending.packets.clear(); ++fallbacks; Gpu::Clear(); Layer::InvalidateFrame(); }
}
static void Invoke(Batch original, uintptr_t renderer, char mode)
{
    if (requested.load() && !pending.inside && Layer::ReplaceBatch(original,renderer,mode))
    { pending.packets.clear(); return; }
    if (!requested.load() || pending.inside || pending.packets.empty())
    { original(renderer, mode); return; }
    if (GetTickCount64() - pending.capturedAt > 250)
    { pending.packets.clear(); ++fallbacks; original(renderer, mode); return; }
    uintptr_t list = 0;
    uint32_t count = 0;
    // Retain bounded capacity between frames instead of allocating a command list
    // on every native UI submission. No additional queue or frame delay is introduced.
    auto& entries = pending.entries;
    auto match = Packets::Match::Invalid;
    try
    {
        if (Packets::Read(renderer + 0x580, list) && Packets::Read(renderer + 0x588, count) &&
            count && count <= Packets::MaxEntries)
        {
            entries.resize(count);
            if (Packets::ReadBytes(list, entries.data(), entries.size() * sizeof(entries[0])))
                match = Packets::Substitute(entries, pending.packets);
        }
    }
    catch (...) { match = Packets::Match::Invalid; }
    // A different batch can be consumed first. Leave it intact and await the owning batch.
    if (match == Packets::Match::None) { original(renderer, mode); return; }
    if (match != Packets::Match::Complete)
    { pending.packets.clear(); ++fallbacks; original(renderer, mode); return; }
    {
        Packets::PointerScope scope;
        if (!scope.Enter(renderer + 0x580, list, reinterpret_cast<uintptr_t>(entries.data())))
        { pending.packets.clear(); ++fallbacks; original(renderer, mode); return; }
        struct Guard { Guard() { pending.inside = true; } ~Guard() { pending.inside = false; } } guard;
        // The verified batch reads headers synchronously and derives indices into the
        // game's existing vertex allocation. Only our header/list addresses differ.
        // Original is called exactly once, at its native position among other UI draws.
        original(renderer, mode);
        packetCount += pending.packets.size();
        const auto completed = ++replacements;
        if (completed == 1 || completed % 600 == 0)
            LOG_INFO("Companion native submissions: batches {}, packets {}, fallback {}, age {} ms",
                completed, packetCount.load(), fallbacks.load(), GetTickCount64() - pending.capturedAt);
    } // Restore the engine list before releasing any of our command headers.
    pending.packets.clear();
}
struct VisibilityScope
{
    bool active=false;
    VisibilityScope(uintptr_t renderer,UINT bytes)
    {if(requested.load() && !pending.inside)active=Layer::BeginVisibilityNative(base,renderer,bytes,pending.packets,pending.capturedAt);}
    ~VisibilityScope(){if(active)Layer::EndVisibilityNative();}
};
static void __fastcall OnBatch16(uintptr_t renderer, char mode) { VisibilityScope scope(renderer,2);Invoke(batch16, renderer, mode); }
static void __fastcall OnBatch32(uintptr_t renderer, char mode) { VisibilityScope scope(renderer,4);Invoke(batch32, renderer, mode); }

void Install()
{
    if (installed.load() || batch16) return;
    wchar_t executable[MAX_PATH] {};
    GetModuleFileNameW(nullptr, executable, MAX_PATH);
    if (_wcsicmp(std::filesystem::path(executable).filename().c_str(), L"ffxiv_dx11.exe")) return;
    base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    IMAGE_DOS_HEADER dos {};
    IMAGE_NT_HEADERS64 nt {};
    if (!Packets::Read(base, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0 ||
        dos.e_lfanew > 4096 || !Packets::Read(base + dos.e_lfanew, nt) || nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.FileHeader.TimeDateStamp != 0x6aa84feb || nt.OptionalHeader.SizeOfImage != 0x3807000)
    { LOG_WARN("Companion native submissions: unsupported executable; no batch hooks installed"); return; }
    // Current disk SHA256 was verified as 5BBC501D...CC44. In-process guards also reject
    // patches/hook detours at either entry point instead of assuming disk identity suffices.
    constexpr std::array<unsigned char, 16> expected {0x88,0x54,0x24,0x10,0x48,0x89,0x4c,0x24,0x08,0x55,0x53,0x56,0x41,0x54,0x41,0x55};
    std::array<unsigned char, 16> a, b;
    if (!Packets::Read(base + 0x6e5480, a) || !Packets::Read(base + 0x6e6020, b) || a != expected || b != expected)
    { LOG_WARN("Companion native submissions: batch byte guards rejected; native drawing unchanged"); return; }
    constexpr std::array<unsigned char,16> enqueueBytes{0x48,0x63,0x41,0x0c,0x4c,0x8b,0xda,0x4c,0x8d,0x04,0x40,0x8b,0x41,0x08,0x42,0x83};
    constexpr std::array<unsigned char,16> bindBytes{0x40,0x56,0x41,0x56,0x48,0x83,0xec,0x48,0x80,0x7a,0x49,0x00,0x4c,0x8b,0xf2,0x48};
    if(!Packets::Read(base+0x240a80,a) || !Packets::Read(base+0x22d6b0,b) || a!=enqueueBytes || b!=bindBytes)
    {LOG_WARN("Companion native submissions: queue hook byte guards rejected");return;}
    enqueue=reinterpret_cast<Enqueue>(base+0x240a80);bindState=reinterpret_cast<BindState>(base+0x22d6b0);
    batch16 = reinterpret_cast<Batch>(base + 0x6e5480);
    batch32 = reinterpret_cast<Batch>(base + 0x6e6020);
    if (DetourTransactionBegin() != NO_ERROR) { batch16 = batch32 = nullptr; return; }
    bool ok = DetourUpdateThread(GetCurrentThread()) == NO_ERROR;
    ok &= DetourAttach(reinterpret_cast<PVOID*>(&batch16), OnBatch16) == NO_ERROR;
    ok &= DetourAttach(reinterpret_cast<PVOID*>(&batch32), OnBatch32) == NO_ERROR;
    ok &= DetourAttach(reinterpret_cast<PVOID*>(&enqueue), OnEnqueue) == NO_ERROR;
    ok &= DetourAttach(reinterpret_cast<PVOID*>(&bindState), OnBindState) == NO_ERROR;
    LONG result = ERROR_INVALID_FUNCTION;
    if (ok) result = DetourTransactionCommit(); else DetourTransactionAbort();
    if (result != NO_ERROR) { batch16 = batch32 = nullptr; return; }
    installed.store(true);
    beginNativeDraw.store(Begin);
    endNativeDraw.store(End);
    LOG_INFO("Companion native submissions: guarded batch hooks ready; substitution off until requested");
}
void Detach()
{
    Layer::Stop();
    Gpu::requested.store(false); Gpu::Clear();
    requested.store(false);
    beginNativeDraw.store(nullptr); endNativeDraw.store(nullptr);
    if (!installed.load() || DetourTransactionBegin() != NO_ERROR) return;
    bool ok = DetourUpdateThread(GetCurrentThread()) == NO_ERROR;
    ok &= DetourDetach(reinterpret_cast<PVOID*>(&batch16), OnBatch16) == NO_ERROR;
    ok &= DetourDetach(reinterpret_cast<PVOID*>(&batch32), OnBatch32) == NO_ERROR;
    ok &= DetourDetach(reinterpret_cast<PVOID*>(&enqueue), OnEnqueue) == NO_ERROR;
    ok &= DetourDetach(reinterpret_cast<PVOID*>(&bindState), OnBindState) == NO_ERROR;
    if (ok)
    {
        if (DetourTransactionCommit() == NO_ERROR) { installed.store(false); batch16 = batch32 = nullptr; }
    }
    else DetourTransactionAbort();
}
void SetRequested(bool value)
{
    requested.store(value && installed.load());
    if (!requested.load()) { Gpu::requested.store(false); Gpu::Clear(); Layer::Stop(); }
}
Statistics GetStatistics()
{
    return {installed.load(), requested.load(), captures.load(), replacements.load(), packetCount.load(), fallbacks.load()};
}
}
