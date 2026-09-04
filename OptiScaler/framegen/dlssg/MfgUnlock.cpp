#include "pch.h"

#include "MfgUnlock.h"

#include <Config.h>
#include <scanner/scanner.h>

#include "MfgTemporalBlob.h"

namespace
{
// mov ebx,1 / mov r8d,3 / cmp edi,0x1b0 / cmovl r8d,ebx. The two counts and the architecture
// constant together are unique in the module; the wildcards cover nothing, they are here only to
// keep the shape readable.
constexpr std::string_view kAdvertisePattern = "BB 01 00 00 00 41 B8 03 00 00 00 81 FF B0 01 00 00 44 0F 4C C3";

// cmp eax,0x1b0 / jl / cmp ebx,3 / jbe. The only comparison against the architecture constant that
// is followed by a signed branch and a count test.
constexpr std::string_view kValidatePattern = "3D B0 01 00 00 7C ? 83 FB 03 76";

// sl.dlss_g.dll, where the count nvngx published is taken as min(published, 3):
//     mov   r8d, 0x3
//     cmp   ecx, r8d
//     cmovb r8d, ecx
// The wrapper carries its own ceiling, so raising nvngx alone gets three back.
constexpr std::string_view kWrapperClampPattern = "41 B8 03 00 00 00 41 3B C8 44 0F 42 C1";

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

// The fatbin holding main_kernel: container magic, version, header size, then the exact payload
// length and the first image header. Unique in the module.
constexpr uint8_t kBlendFatbinSignature[] = { 0x50, 0xED, 0x55, 0xBA, 0x01, 0x00, 0x10, 0x00,
                                              0x90, 0x57, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                              0x01, 0x00, 0x01, 0x01, 0x68, 0x00, 0x00, 0x00,
                                              0x08, 0x17, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

// Header size plus payload of the stock container, which is what the replacement has to fit inside.
constexpr size_t kBlendFatbinSize = 22432;

// scanner::GetAddress only walks sections marked executable. The fatbin is data, so it needs its own
// search. Returns 0 unless exactly one section contains the sequence, once.
uintptr_t FindDataBytes(HMODULE module, const uint8_t* needle, size_t length)
{
    auto base = reinterpret_cast<uint8_t*>(module);
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    auto section = IMAGE_FIRST_SECTION(nt);

    uintptr_t found = 0;
    size_t hits = 0;

    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i)
    {
        const auto& s = section[i];

        if (s.Characteristics & IMAGE_SCN_MEM_EXECUTE)
            continue;

        uint8_t* start = base + s.VirtualAddress;
        uint8_t* end = start + s.Misc.VirtualSize;

        for (uint8_t* p = std::search(start, end, needle, needle + length); p != end;
             p = std::search(p + 1, end, needle, needle + length))
        {
            found = reinterpret_cast<uintptr_t>(p);

            if (++hits > 1)
                return 0;
        }
    }

    return hits == 1 ? found : 0;
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

// Rewrites count and neutralises the architecture clamp, so MultiFrameCountMax is published as five.
bool PatchAdvertise(HMODULE module)
{
    if (const auto at309 = scanner::GetAddress(module, kAdvertisePattern309); at309 != 0)
    {
        // The jl is a rel32, six bytes.
        const auto branchAt = at309 + 6;
        const uint8_t nop[] = { 0x0F, 0x1F, 0x44, 0x00, 0x00, 0x90 };

        LOG_INFO("MFG unlock: advertise (310.9) at {:X}, jl {} -> {}", at309,
                 Hex((const uint8_t*) branchAt, sizeof(nop)), Hex(nop, sizeof(nop)));

        return WriteBytes(branchAt, nop, sizeof(nop));
    }

    const auto address = scanner::GetAddress(module, kAdvertisePattern);

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
    if (const auto at309 = scanner::GetAddress(module, kValidatePattern309); at309 != 0)
    {
        // setae al -> mov al, 1, so the flag is set whatever the architecture reports.
        const auto setAt = at309 + 5;
        const uint8_t always[] = { 0xB0, 0x01, 0x90 };

        LOG_INFO("MFG unlock: validate (310.9) at {:X}, setae {} -> {}", at309,
                 Hex((const uint8_t*) setAt, sizeof(always)), Hex(always, sizeof(always)));

        return WriteBytes(setAt, always, sizeof(always));
    }

    const auto address = scanner::GetAddress(module, kValidatePattern);

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

// Replaces main_kernel's fatbin with one whose sm_89 image blends by the temporal parameter.
//
// Generated frames are otherwise all composed at the midpoint between the two source frames, which
// is right for 2X and wrong for everything above it: five frames carrying one picture. The
// replacement is PTX only, so the driver JITs it instead of running the sm_89 SASS the stock
// container also carries. Costs a compile on first use.
bool PatchTemporalBlend(HMODULE module)
{
    const auto address = FindDataBytes(module, kBlendFatbinSignature, sizeof(kBlendFatbinSignature));

    if (address == 0)
    {
        LOG_WARN("MFG unlock: the blend fatbin was not found once, temporal fix not applied");
        return false;
    }

    static_assert(sizeof(kMfgTemporalFatbin) <= kBlendFatbinSize, "replacement fatbin does not fit");

    LOG_INFO("MFG unlock: blend fatbin at {:X}, {} bytes replaced by {}", address, kBlendFatbinSize,
             sizeof(kMfgTemporalFatbin));

    if (!WriteBytes(address, kMfgTemporalFatbin, sizeof(kMfgTemporalFatbin)))
        return false;

    // The container reports its own length, so the remainder of the old payload is never read. Zero
    // it rather than leaving the tail of a fatbin the header no longer describes.
    const std::vector<uint8_t> pad(kBlendFatbinSize - sizeof(kMfgTemporalFatbin), 0);

    return WriteBytes(address + sizeof(kMfgTemporalFatbin), pad.data(), pad.size());
}

// Raises the wrapper's own ceiling to match, so the min() keeps what nvngx published.
bool PatchWrapperClamp(HMODULE module)
{
    const auto address = scanner::GetAddress(module, kWrapperClampPattern);

    if (address == 0)
    {
        LOG_WARN("MFG unlock: the wrapper clamp signature did not match, sl.dlss_g.dll left alone");
        return false;
    }

    // The r8d immediate of the ceiling this clamps against.
    const auto countAt = address + 2;
    const uint8_t count[] = { kMaxGeneratedFrames };

    LOG_INFO("MFG unlock: wrapper clamp at {:X}, count {} -> {}", address, *(const uint8_t*) countAt,
             kMaxGeneratedFrames);

    return WriteBytes(countAt, count, sizeof(count));
}
} // namespace

void MfgUnlock::TryApply()
{
    if (!Config::Instance()->FGDLSSGAdaMfgUnlock.value_or_default())
        return;

    // The two modules arrive at different times and each is latched on its own, so whichever is
    // present first is patched then rather than waiting for the other.
    static bool snippetDone = false;
    static bool wrapperDone = false;

    if (!snippetDone)
    {
        if (auto module = GetModuleHandleW(L"nvngx_dlssg.dll"); module != nullptr)
        {
            snippetDone = true;

            const bool advertise = PatchAdvertise(module);
            const bool validate = PatchValidate(module);

            if (Config::Instance()->FGDLSSGAdaTemporalFix.value_or_default())
                PatchTemporalBlend(module);

            if (advertise && validate)
                LOG_INFO("MFG unlock: nvngx_dlssg.dll patched for {} generated frames", kMaxGeneratedFrames);
            else
                LOG_WARN("MFG unlock: nvngx_dlssg.dll incomplete, advertise {}, validate {}", advertise, validate);
        }
    }

    if (!wrapperDone)
    {
        if (auto module = GetModuleHandleW(L"sl.dlss_g.dll"); module != nullptr)
        {
            wrapperDone = true;

            if (PatchWrapperClamp(module))
                LOG_INFO("MFG unlock: sl.dlss_g.dll ceiling raised to {}", kMaxGeneratedFrames);
        }
    }
}
