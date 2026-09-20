// Adapted from y4my4my4m/OptiScaler_DLSSNR_Multipass_MFG, tag v4 (7b7220bb), GPL-3.0.
#include "pch.h"

#include "MfgUnlock.h"
#include "AdaTemporal.h"
#include <mutex>
#include <atomic>

#include <Config.h>
#include <State.h>
#include <Util.h>
#include <scanner/scanner.h>
#include <misc/IdentifyGpu.h>


namespace
{
// mov ebx,1 / mov r8d,3 / cmp edi,0x1b0 / cmovl r8d,ebx. The two counts and the architecture
// constant together are unique in the module; the wildcards cover nothing, they are here only to
// keep the shape readable.
constexpr std::string_view kAdvertisePattern = "BB 01 00 00 00 41 B8 03 00 00 00 81 FF B0 01 00 00 44 0F 4C C3";

// cmp eax,0x1b0 / jl / cmp ebx,3 / jbe. The only comparison against the architecture constant that
// is followed by a signed branch and a count test.
constexpr std::string_view kValidatePattern = "3D B0 01 00 00 7C ? 83 FB 03 76";

// Five generated frames, the count both patched sites carry.
constexpr uint8_t kMaxGeneratedFrames = 5;

// 310.9 restructured both gates. The count is no longer an immediate next to the comparison: the
// Blackwell branch starts at five and reads a configured value, and anything below Blackwell is sent
// to a branch that publishes one.
//     cmp ebp, 0x1b0
//     jl  ada          <- neutralised, so every card takes the Blackwell branch
//     mov edi, 0x5
constexpr std::string_view kAdvertisePattern309 = "81 FD B0 01 00 00 0F 8C ? ? ? ? BF 05 00 00 00";

// The capability flag in the same build is a setae rather than a branch.
//     cmp   eax, 0x1b0
//     setae al
constexpr std::string_view kValidatePattern309 = "3D B0 01 00 00 0F 93 C0";



MfgUnlock::Status g_status {};
std::mutex g_patchMutex;
std::mutex g_serviceMutex;
std::atomic<unsigned> g_unlockedMax { 0 };
std::atomic<bool> g_settled { false };
std::atomic<bool> g_rescanRequested { true };
std::atomic<ULONGLONG> g_discoveryStarted { 0 };
std::atomic<ULONGLONG> g_nextService { 0 };
// Each deferred slot owns a loader reference, so an address cannot disappear or be reused.
std::array<std::atomic<HMODULE>, 32> g_deferred {};
struct ProviderState
{
    MfgUnlock::Status status;
    std::vector<mfgunlock::midpoint::Patch> temporalPatches;
    void* temporalAllocation = nullptr;
    HMODULE retainedModule = nullptr;
    std::wstring path;
    bool metadataPending = true;
};
std::unordered_map<std::string, ProviderState> g_providers;
// A patched provider and its replacement allocation live together until process exit.
// Holding a loader reference prevents same-address reloads and stale successful capability state.
// Rejected providers are not retained. Loader notifications revalidate rejected mappings.
struct ModuleReference
{
    HMODULE module = nullptr;
    ~ModuleReference() { if (module) FreeLibrary(module); }
    HMODULE Release() { auto value = module; module = nullptr; return value; }
};

bool Eligible()
{
    if (!Config::Instance()->FGDLSSGAdaMfgUnlock.value_or_default() || State::Instance().externalFrameGeneration)
        return false;
    const auto& gpu = IdentifyGpu::getPrimaryGpu();
    return gpu.vendorId == VendorId::Nvidia && gpu.nvidiaArchInfo.architecture_id == NV_GPU_ARCHITECTURE_AD100;
}

void Defer(ModuleReference& reference)
{
    for (auto& slot : g_deferred)
    {
        HMODULE empty = nullptr;
        if (slot.compare_exchange_strong(empty, reference.module))
        {
            reference.Release();
            g_rescanRequested.store(true);
            return;
        }
    }
    // Fixed-size queue: never grow allocations indefinitely under contention.
    g_rescanRequested.store(true);
}
uintptr_t UniqueAddress(HMODULE module, std::string_view pattern)
{
    const auto first = scanner::GetAddress(module, pattern);
    return first && !scanner::GetAddress(module, pattern, 0, first + 1) ? first : 0;
}

// The module's own file version, for the report. A signature that does not match is expected on a
// version nobody has looked at, and the version is the one thing that makes such a report actionable.
std::string ModuleVersion(const std::wstring& path)
{
    version_t file {};
    version_t product {};
    if (!Util::GetFileVersion(path.c_str(), &file, &product)) return {};
    return std::format("{}.{}.{}", file.major, file.minor, file.patch);
}
bool WriteBytes(uintptr_t address, const uint8_t* bytes, size_t count)
{
    DWORD oldProtect = 0;

    if (!VirtualProtect((LPVOID) address, count, PAGE_EXECUTE_READWRITE, &oldProtect))
    {
        LOG_WARN("VirtualProtect failed at {:X}", address);
        return false;
    }

    std::memcpy((void*) address, bytes, count);

    DWORD ignored = 0;
    VirtualProtect((LPVOID) address, count, oldProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), (LPCVOID) address, count);

    return true;
}

std::string Hex(const uint8_t* bytes, size_t count)
{
    std::string out;

    for (size_t i = 0; i < count; ++i)
        out += std::format("{}{:02X}", i == 0 ? "" : " ", bytes[i]);

    return out;
}

// Diagnostic scans retain the existing scanner and exact uniqueness rules.
void ReportPattern(HMODULE module, std::string_view label, std::string_view pattern)
{
    size_t hits = 0;
    uintptr_t next = 0;
    while (hits < 16)
    {
        auto at = scanner::GetAddress(module, pattern, 0, next);
        if (!at) break;
        ++hits;
        LOG_INFO("MFG diagnostic: {} match {} RVA {:X}", label, hits,
                 at - reinterpret_cast<uintptr_t>(module));
        next = at + 1;
    }
    LOG_INFO("MFG diagnostic: {} matches={}{}", label, hits, hits == 16 ? "+ (capped)" : "");
}

void ReportGateBytes(HMODULE module)
{
    const auto base = reinterpret_cast<const uint8_t*>(module);
    const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const auto sections = IMAGE_FIRST_SECTION(nt);
    size_t reports = 0;
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections && reports < 16; ++i)
    {
        const auto& section = sections[i];
        if (!(section.Characteristics & IMAGE_SCN_MEM_EXECUTE) ||
            section.VirtualAddress >= nt->OptionalHeader.SizeOfImage ||
            section.Misc.VirtualSize > nt->OptionalHeader.SizeOfImage - section.VirtualAddress) continue;
        const auto start = base + section.VirtualAddress;
        for (size_t offset = 0; offset + 24 <= section.Misc.VirtualSize && reports < 16; ++offset)
        {
            const auto p = start + offset;
            // Include both stock Blackwell and commonly patched Ada comparisons. Diagnostic only.
            const bool eax = p[0] == 0x3D && (p[1] == 0xB0 || p[1] == 0x90) &&
                             p[2] == 1 && p[3] == 0 && p[4] == 0;
            const bool reg = p[0] == 0x81 && p[1] >= 0xF8 && p[1] <= 0xFF &&
                             (p[2] == 0xB0 || p[2] == 0x90) && p[3] == 1 && p[4] == 0 && p[5] == 0;
            if (!eax && !reg) continue;
            ++reports;
            LOG_INFO("MFG diagnostic: gate candidate RVA {:X}: {}", p - base, Hex(p, 24));
        }
    }
    LOG_INFO("MFG diagnostic: gate byte reports={}{}", reports, reports == 16 ? "+ (capped)" : "");
}
// Rewrites count and neutralises the architecture clamp, so MultiFrameCountMax is published as five.
bool PatchAdvertise(HMODULE module)
{
    if (const auto at309 = UniqueAddress(module, kAdvertisePattern309); at309 != 0)
    {
        // The jl is a rel32, six bytes.
        const auto branchAt = at309 + 6;
        const uint8_t nop[] = { 0x0F, 0x1F, 0x44, 0x00, 0x00, 0x90 };

        LOG_INFO("MFG unlock: advertise (310.9) at {:X}, jl {} -> {}", at309,
                 Hex((const uint8_t*) branchAt, sizeof(nop)), Hex(nop, sizeof(nop)));

        return WriteBytes(branchAt, nop, sizeof(nop));
    }

    const auto address = UniqueAddress(module, kAdvertisePattern);

    if (address == 0)
    {
        LOG_WARN("MFG unlock: the advertise signature did not match, nvngx_dlssg.dll left alone");
        return false;
    }

    // Offsets within the matched sequence: the r8d immediate, and the cmovl.
    const auto countAt = address + 7;
    const auto cmovAt = address + 17;

    const uint8_t count[] = { kMaxGeneratedFrames };
    const uint8_t nop[] = { 0x0F, 0x1F, 0x40, 0x00 };

    LOG_INFO("MFG unlock: advertise at {:X}, count {} -> {}, cmovl {} -> {}", address,
             *(const uint8_t*) countAt, kMaxGeneratedFrames, Hex((const uint8_t*) cmovAt, sizeof(nop)),
             Hex(nop, sizeof(nop)));

    return WriteBytes(countAt, count, sizeof(count)) && WriteBytes(cmovAt, nop, sizeof(nop));
}

// Drops the Ada branch and raises the accepted count, so a request for five is not rejected.
bool PatchValidate(HMODULE module)
{
    if (const auto at309 = UniqueAddress(module, kValidatePattern309); at309 != 0)
    {
        // setae al -> mov al, 1, so the flag is set whatever the architecture reports.
        const auto setAt = at309 + 5;
        const uint8_t always[] = { 0xB0, 0x01, 0x90 };

        LOG_INFO("MFG unlock: validate (310.9) at {:X}, setae {} -> {}", at309,
                 Hex((const uint8_t*) setAt, sizeof(always)), Hex(always, sizeof(always)));

        return WriteBytes(setAt, always, sizeof(always));
    }

    const auto address = UniqueAddress(module, kValidatePattern);

    if (address == 0)
    {
        LOG_WARN("MFG unlock: the validate signature did not match, nvngx_dlssg.dll left alone");
        return false;
    }

    // Offsets within the matched sequence: the jl, and the immediate of the count test behind it.
    const auto branchAt = address + 5;
    const auto countAt = address + 9;

    const uint8_t nop[] = { 0x90, 0x90 };
    const uint8_t count[] = { kMaxGeneratedFrames };

    LOG_INFO("MFG unlock: validate at {:X}, jl {} -> {}, count {} -> {}", address,
             Hex((const uint8_t*) branchAt, sizeof(nop)), Hex(nop, sizeof(nop)), *(const uint8_t*) countAt,
             kMaxGeneratedFrames);

    return WriteBytes(branchAt, nop, sizeof(nop)) && WriteBytes(countAt, count, sizeof(count));
}


// Takes ownership of a reference acquired BEFORE taking g_patchMutex. Releasing it happens
// AFTER unlocking, so a loader callback can never wait on our mutex while we wait on the loader.
void ProcessProvider(HMODULE ownedModule, bool loaderNotification)
{
    ModuleReference reference { ownedModule };
    auto module = reference.module;
    wchar_t path[32768] {};
    if (!GetModuleFileNameW(module, path, static_cast<DWORD>(std::size(path)))) return;
    std::unique_lock lock(g_patchMutex, std::try_to_lock);
    if (!lock.owns_lock()) { Defer(reference); return; }
    const auto base = reinterpret_cast<const uint8_t*>(module);
    const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;
    const auto pathText = std::filesystem::path(path).string();
    const auto identity = std::format("{}|{:X}|{:X}|{:X}", pathText, reinterpret_cast<uintptr_t>(module),
                                      nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage);
    auto [entry, inserted] = g_providers.try_emplace(identity);
    if (!inserted)
    {
        if (entry->second.retainedModule || !loaderNotification) return;
        // A rejected address can be reused after unloading. Re-check on each load notification.
        entry->second = ProviderState {};
    }
    auto& provider = entry->second;
    provider.path = path;
    auto& status = provider.status;
    status.ModuleFound = true;
    // Metadata is read later by Service, never by a loader callback or while holding the patch mutex.
    status.SnippetVersion = "metadata pending";
    struct SummaryOnExit
    {
        ProviderState& provider;
        ModuleReference& reference;
        ~SummaryOnExit()
        {
            auto& result = provider.status;
            if (result.AdvertiseMatched && result.ValidateMatched && result.KernelsRewritten)
            {
                provider.retainedModule = reference.Release();
                g_unlockedMax.store(kMaxGeneratedFrames, std::memory_order_release);
                g_settled.store(true, std::memory_order_release);
            }
            else if (provider.temporalAllocation)
            {
                // A failed rollback may still leave descriptors pointing at this allocation.
                provider.retainedModule = reference.Release();
            }
            if (result.AdvertiseMatched || !g_status.AdvertiseMatched) g_status = result;
        }
    } summary { provider, reference };
    auto& g_temporalPatches = provider.temporalPatches;
    auto& g_temporalAllocation = provider.temporalAllocation;
    LOG_INFO("MFG lifecycle v3: provider={} module={:X} PE timestamp={:X} imageSize={:X}",
             pathText, reinterpret_cast<uintptr_t>(module), nt->FileHeader.TimeDateStamp,
             nt->OptionalHeader.SizeOfImage);
    ReportPattern(module, "advertise 310.9", kAdvertisePattern309);
    ReportPattern(module, "validate 310.9", kValidatePattern309);
    ReportPattern(module, "advertise legacy", kAdvertisePattern);
    ReportPattern(module, "validate legacy", kValidatePattern);
    ReportGateBytes(module);
    // Validate both gates before touching either. Ambiguous/unknown versions remain unmodified.
    const bool knownGates =
        (UniqueAddress(module, kAdvertisePattern309) && UniqueAddress(module, kValidatePattern309)) ||
        (UniqueAddress(module, kAdvertisePattern) && UniqueAddress(module, kValidatePattern));
    if (!knownGates)
    {
        LOG_WARN("MFG unlock: unsupported or ambiguous DLSSG {} signatures; left unchanged",
                 status.SnippetVersion);
        return;
    }

    // Replace only the validated Ada temporal program. The former broad Blackwell
    // retarget and AdaBlackwellKernels configuration are intentionally no longer used.
    std::string detail;
    if (!mfgunlock::midpoint::Apply(module, g_temporalPatches, g_temporalAllocation, detail))
    {
        LOG_WARN("MFG unlock: targeted Ada temporal correction rejected: {}; gates left unchanged", detail);
        return;
    }
    status.KernelsRewritten = 1;
    LOG_INFO("MFG unlock: targeted Ada temporal correction applied: {}; provider={}", detail, pathText);

    // Save both gate regions so any partial capability patch can be rolled back.
    auto advertiseAt = UniqueAddress(module, kAdvertisePattern309);
    auto validateAt = UniqueAddress(module, kValidatePattern309);
    if (!advertiseAt) advertiseAt = UniqueAddress(module, kAdvertisePattern);
    if (!validateAt) validateAt = UniqueAddress(module, kValidatePattern);
    std::array<uint8_t, 21> advertiseOriginal;
    std::array<uint8_t, 11> validateOriginal;
    std::memcpy(advertiseOriginal.data(), (void*)advertiseAt, advertiseOriginal.size());
    std::memcpy(validateOriginal.data(), (void*)validateAt, validateOriginal.size());
    const bool advertise = PatchAdvertise(module);
    const bool validate = PatchValidate(module);
    status.AdvertiseMatched = advertise;
    status.ValidateMatched = validate;

    if (advertise && validate)
        LOG_INFO("MFG unlock: nvngx_dlssg.dll patched for {} generated frames", kMaxGeneratedFrames);
    else
    {
        const bool restoredAdvertise = WriteBytes(advertiseAt, advertiseOriginal.data(), advertiseOriginal.size());
        const bool restoredValidate = WriteBytes(validateAt, validateOriginal.data(), validateOriginal.size());
        if (restoredAdvertise && restoredValidate)
            mfgunlock::midpoint::Restore(g_temporalPatches, g_temporalAllocation);
        status.AdvertiseMatched = false;
        status.ValidateMatched = false;
        status.KernelsRewritten = 0;
        LOG_ERROR("MFG unlock: incomplete gate patch; rollback advertise {}, validate {}. Unlock unavailable",
                  restoredAdvertise, restoredValidate);
    }

}

void Service()
{
    const auto now = GetTickCount64();
    ULONGLONG zero = 0;
    g_discoveryStarted.compare_exchange_strong(zero, now);
    if (now < g_nextService.load() && !g_rescanRequested.load()) return;
    std::unique_lock serviceLock(g_serviceMutex, std::try_to_lock);
    if (!serviceLock.owns_lock()) return;
    g_nextService.store(now + 1000);
    g_rescanRequested.store(false);
    for (auto& slot : g_deferred)
        if (auto module = slot.exchange(nullptr)) ProcessProvider(module, true);

    // Local discovery is bounded to once per second; load callbacks still patch synchronously.
    const auto configured = Util::DllPath() / L"nvngx_dlssg.dll";
    const auto game = Util::ExePath().parent_path() / L"nvngx_dlssg.dll";
    const std::wstring paths[] = { configured.wstring(), game.wstring(), L"nvngx_dlssg.dll" };
    for (const auto& path : paths)
    {
        HMODULE module = nullptr;
        if (GetModuleHandleExW(0, path.c_str(), &module)) ProcessProvider(module, false);
    }

    std::vector<std::pair<std::string, std::wstring>> reports;
    {
        std::unique_lock lock(g_patchMutex, std::try_to_lock);
        if (!lock.owns_lock()) { g_rescanRequested.store(true); return; }
        for (auto& [identity, provider] : g_providers)
            if (provider.metadataPending)
            {
                provider.metadataPending = false;
                reports.emplace_back(identity, provider.path);
            }
    }
    for (const auto& [identity, path] : reports)
    {
        const auto version = ModuleVersion(path);
        LOG_INFO("MFG lifecycle v3: provider={} fileVersion={}", std::filesystem::path(path).string(), version);
        std::lock_guard lock(g_patchMutex);
        if (auto entry = g_providers.find(identity); entry != g_providers.end())
        {
            entry->second.status.SnippetVersion = version;
            if (entry->second.status.AdvertiseMatched || !g_status.AdvertiseMatched)
                g_status = entry->second.status;
        }
    }
    // Stop postponing the Streamline ceiling cache if no compatible provider appears.
    // Future loader notifications remain enabled and a later success can still raise the ceiling.
    if (!g_settled.load() && now - g_discoveryStarted.load() >= 10000)
    {
        g_settled.store(true);
        LOG_INFO("MFG lifecycle v3: capability discovery settled without an unlocked provider");
    }
}
} // namespace

void MfgUnlock::TryApply(HMODULE module)
{
    if (!Eligible()) return;
    if (!module) { Service(); return; }
    HMODULE reference = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                          reinterpret_cast<LPCWSTR>(module), &reference))
        ProcessProvider(reference, true);
}

unsigned int MfgUnlock::UnlockedMax() { return g_unlockedMax.load(std::memory_order_acquire); }
bool MfgUnlock::Pending() { return Eligible() && !g_settled.load(std::memory_order_acquire); }
bool MfgUnlock::Watching() { return Eligible(); }
MfgUnlock::Status MfgUnlock::LastStatus()
{
    std::lock_guard lock(g_patchMutex);
    return g_status;
}
