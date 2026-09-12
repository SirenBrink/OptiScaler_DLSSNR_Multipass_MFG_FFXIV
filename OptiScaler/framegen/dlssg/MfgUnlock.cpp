// Adapted from y4my4my4m/OptiScaler_DLSSNR_Multipass_MFG, tag v4 (7b7220bb), GPL-3.0.
#include "pch.h"

#include "MfgUnlock.h"
#include "AdaTemporal.h"
#include <mutex>

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
struct ProviderState
{
    MfgUnlock::Status status;
    std::vector<mfgunlock::midpoint::Patch> temporalPatches;
    void* temporalAllocation = nullptr;
};
// Never dereference cached module addresses: a rejected provider may already be unloaded.
// The identity includes its mapped base, full path, timestamp and image size.
std::unordered_map<std::string, ProviderState> g_providers;
// The provider may retain this address until process exit. Do not free it during DLL teardown.


uintptr_t UniqueAddress(HMODULE module, std::string_view pattern)
{
    const auto first = scanner::GetAddress(module, pattern);
    return first && !scanner::GetAddress(module, pattern, 0, first + 1) ? first : 0;
}

// The module's own file version, for the report. A signature that does not match is expected on a
// version nobody has looked at, and the version is the one thing that makes such a report actionable.
std::string ModuleVersion(HMODULE module)
{
    wchar_t path[MAX_PATH] {};

    if (GetModuleFileNameW(module, path, MAX_PATH) == 0)
        return {};

    version_t file {};
    version_t product {};

    if (!Util::GetFileVersion(path, &file, &product))
        return {};

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


} // namespace

void MfgUnlock::TryApply(HMODULE requestedModule)
{
    if (!Config::Instance()->FGDLSSGAdaMfgUnlock.value_or_default() || State::Instance().externalFrameGeneration)
        return;
    const auto& gpu = IdentifyGpu::getPrimaryGpu();
    // The kernel retarget is Ada-specific. Do not patch Ampere/Turing or change Blackwell's working path.
    if (gpu.vendorId != VendorId::Nvidia || gpu.nvidiaArchInfo.architecture_id != NV_GPU_ARCHITECTURE_AD100)
        return;

    if (!requestedModule)
    {
        // GetModuleHandle with a full path distinguishes identically named local providers.
        // No module is loaded here, and no continuous process-wide enumeration is needed.
        const auto configured = Util::DllPath() / L"nvngx_dlssg.dll";
        const auto game = Util::ExePath().parent_path() / L"nvngx_dlssg.dll";
        const HMODULE modules[] = { GetModuleHandleW(configured.c_str()), GetModuleHandleW(game.c_str()),
                                    GetModuleHandleW(L"nvngx_dlssg.dll") };
        for (auto module : modules)
            if (module) TryApply(module);
        return;
    }

    // Loader callbacks must not wait on a thread that may itself need the loader lock.
    std::unique_lock lock(g_patchMutex, std::try_to_lock);
    if (!lock.owns_lock()) return;
    auto module = requestedModule;
    wchar_t path[32768] {};
    if (!GetModuleFileNameW(module, path, static_cast<DWORD>(std::size(path)))) return;
    const auto base = reinterpret_cast<const uint8_t*>(module);
    const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;
    const auto pathText = std::filesystem::path(path).string();
    const auto identity = std::format("{}|{:X}|{:X}|{:X}", pathText, reinterpret_cast<uintptr_t>(module),
                                      nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage);
    auto [entry, inserted] = g_providers.try_emplace(identity);
    if (!inserted) return;
    auto& provider = entry->second;
    // Keep the existing UI status as a summary of the most recent attempt; logs identify each provider.
    auto& status = provider.status;
    status.ModuleFound = true;
    status.SnippetVersion = ModuleVersion(module);
    struct SummaryOnExit {
        Status& result;
        ~SummaryOnExit() {
            if ((result.AdvertiseMatched && result.ValidateMatched) || !g_status.AdvertiseMatched)
                g_status = result;
        }
    } summary { status };
    auto& g_temporalPatches = provider.temporalPatches;
    auto& g_temporalAllocation = provider.temporalAllocation;
    LOG_INFO("MFG diagnostic v2: provider={} module={:X} version={} PE timestamp={:X} imageSize={:X}",
             pathText, reinterpret_cast<uintptr_t>(module), status.SnippetVersion,
             nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage);
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

unsigned int MfgUnlock::UnlockedMax()
{
    const auto& status = LastStatus();

    return status.AdvertiseMatched && status.ValidateMatched && status.KernelsRewritten > 0
               ? kMaxGeneratedFrames : 0;
}

bool MfgUnlock::Pending()
{
    if (!Config::Instance()->FGDLSSGAdaMfgUnlock.value_or_default() ||
        State::Instance().externalFrameGeneration)
        return false;
    const auto& gpu = IdentifyGpu::getPrimaryGpu();
    return gpu.vendorId == VendorId::Nvidia && gpu.nvidiaArchInfo.architecture_id == NV_GPU_ARCHITECTURE_AD100;
}

const MfgUnlock::Status& MfgUnlock::LastStatus() { return g_status; }
