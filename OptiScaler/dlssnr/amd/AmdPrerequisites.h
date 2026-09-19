#pragma once

#include <filesystem>
#include <string>

namespace DlssNr::AmdBridge
{
inline std::string MissingPrerequisite(const std::filesystem::path& directory)
{
    std::error_code ec;
    if (!std::filesystem::is_regular_file(directory / L"dlssnr_amd_pass1.dll", ec))
        return "AMD pre-SR unavailable: missing dlssnr_amd_pass1.dll";

    ec.clear();
    if (!std::filesystem::is_regular_file(directory / L"dlssnr_on_amd_weights.bin", ec))
        return "AMD pre-SR unavailable: missing dlssnr_on_amd_weights.bin";

    return {};
}
} // namespace DlssNr::AmdBridge
