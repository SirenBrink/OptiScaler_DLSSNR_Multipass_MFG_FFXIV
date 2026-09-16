#pragma once
#include <json.hpp>
#include <filesystem>
#include <fstream>
#include <optional>
#include <cmath>
#include <type_traits>
#include <stdexcept>
#include <Windows.h>

namespace DlssNr::UserPresets
{
using Json = nlohmann::json;

// Explicit allowlist: never restore GPU/provider selection, shortcuts, frozen frames,
// comparison/debug views, capture state, or game-specific exposure calibration.
template<class C, class F> void Fields(C& c, F&& f)
{
#define NR_FIELD(name, lo, hi) f(#name, c.DlssNr##name, lo, hi)
    NR_FIELD(Enabled, 0, 1); NR_FIELD(RunBeforeSr, 0, 1);
    NR_FIELD(DeferredDlss, 0, 1); NR_FIELD(ResidualFg, 0, 1);
    NR_FIELD(ResidualFgApproxCamera, 0, 1); NR_FIELD(Precision, 0, 4);
    NR_FIELD(Passes, 1, 30); NR_FIELD(UnlockPasses, 0, 1);
    NR_FIELD(Preset, 0, 3); NR_FIELD(Style, 0, 2);
    NR_FIELD(Intensity, 0, 2); NR_FIELD(LocalStructure, 0, 2);
    NR_FIELD(LocalTone, 0, 2); NR_FIELD(SkinStructure, -1, 2); NR_FIELD(AutoMask, 0, 1);
    NR_FIELD(Pass2Preset, 0, 3); NR_FIELD(Pass2Style, 0, 2);
    NR_FIELD(Pass2Intensity, 0, 2); NR_FIELD(Pass2LocalStructure, 0, 2);
    NR_FIELD(Pass2LocalTone, 0, 2); NR_FIELD(Pass2SkinStructure, -1, 2); NR_FIELD(Pass2AutoMask, 0, 1);
    NR_FIELD(Pass3Preset, 0, 3); NR_FIELD(Pass3Style, 0, 2);
    NR_FIELD(Pass3Intensity, 0, 2); NR_FIELD(Pass3LocalStructure, 0, 2);
    NR_FIELD(Pass3LocalTone, 0, 2); NR_FIELD(Pass3SkinStructure, -1, 2); NR_FIELD(Pass3AutoMask, 0, 1);
    NR_FIELD(WorkingScale, 0.25, 2); NR_FIELD(ScalingDownscaler, 0, 7);
    NR_FIELD(TransferStrength, 0, 2); NR_FIELD(ColourStrength, 0, 4);
    NR_FIELD(Transfer, 0, 1); NR_FIELD(ReversibleMode, 0, 4); NR_FIELD(ApplyModel, 0, 1);
    NR_FIELD(MaxRatio, 1, 30); NR_FIELD(SkinProtection, 0, 1);
    NR_FIELD(SkinToneEnabled, 0, 1); NR_FIELD(SkinDetail, 0, 1); NR_FIELD(SkinColour, 0, 1);
    NR_FIELD(EnvironmentDetail, 0, 1); NR_FIELD(EnvironmentColour, 0, 1);
    NR_FIELD(WhitePointSource, 0, 2); NR_FIELD(WhitePointScale, 0.25, 2000);
    NR_FIELD(WhitePointTrim, 0.25, 4); NR_FIELD(ScanTrim, 0.25, 4);
#undef NR_FIELD
    for (unsigned i = 0; i < 27; ++i)
    {
        auto& p = c.DlssNrExtraPasses[i];
        const auto prefix = "Pass" + std::to_string(i + 4);
        f(prefix + "Style", p.style, 0, 2);
        f(prefix + "Intensity", p.intensity, 0, 2);
        f(prefix + "LocalStructure", p.structure, 0, 2);
        f(prefix + "LocalTone", p.tone, 0, 2);
        f(prefix + "SkinStructure", p.skin, -1, 2);
        f(prefix + "AutoMask", p.autoMask, 0, 1);
    }
}

template<class Option> void ValidateValue(const Json& j, const Option&, double lo, double hi)
{
    using T = typename Option::value_type;
    if (j.is_null()) return; // Preserve later-pass inheritance and optional defaults.
    if constexpr (std::is_same_v<T, bool>)
    {
        if (!j.is_boolean()) throw std::runtime_error("Expected a true/false setting");
    }
    else
    {
        if (!j.is_number() || j.is_boolean()) throw std::runtime_error("Expected a numeric setting");
        const double v = j.get<double>();
        if (!std::isfinite(v) || v < lo || v > hi ||
            ((!std::is_floating_point_v<T>) && std::floor(v) != v))
            throw std::runtime_error("Preset setting is outside the supported range");
    }
}

template<class C> Json Capture(C& c)
{
    Json result = Json::object();
    Fields(c, [&](const std::string& key, auto& opt, double, double) {
        using T = typename std::decay_t<decltype(opt)>::value_type;
        auto encode = [](T value) -> Json {
            if constexpr (std::is_enum_v<T>) return static_cast<unsigned>(value);
            else return value;
        };
        if constexpr (requires { opt.value_or_default(); }) result[key] = encode(opt.value_or_default());
        else result[key] = opt.has_value() ? encode(opt.value()) : Json(nullptr);
    });
    return result;
}

template<class C> void Validate(const Json& j, C& c)
{
    if (!j.is_object()) throw std::runtime_error("Invalid preset settings");
    Fields(c, [&](const std::string& key, auto& opt, double lo, double hi) {
        if (!j.contains(key)) throw std::runtime_error("Preset is missing " + key);
        ValidateValue(j.at(key), opt, lo, hi);
    });
    if (!j.at("Precision").is_null() && j.at("Precision") != 0 && j.at("Precision") != 4)
        throw std::runtime_error("Unsupported model precision");
}

template<class C> void Apply(const Json& j, C& c)
{
    Validate(j, c); // Validate every pass before changing any live setting.
    Fields(c, [&](const std::string& key, auto& opt, double, double) {
        using T = typename std::decay_t<decltype(opt)>::value_type;
        const auto& value = j.at(key);
        if (value.is_null()) opt = std::optional<T>{};
        else if constexpr (std::is_enum_v<T>) opt = static_cast<T>(value.get<unsigned>());
        else opt = value.get<T>();
    });
}

inline Json Read(const std::filesystem::path& path)
{
    if (!std::filesystem::exists(path)) return Json{{"version", 1}, {"presets", Json::object()}};
    if (std::filesystem::file_size(path) > 1024 * 1024) throw std::runtime_error("Preset file exceeds 1 MB");
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot read NR presets");
    auto j = Json::parse(input);
    if (!j.is_object() || !j.contains("version") || j.at("version") != 1 ||
        !j.contains("presets") || !j.at("presets").is_object())
        throw std::runtime_error("Unsupported NR preset file");
    return j;
}

inline std::string Name(std::string text)
{
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) throw std::runtime_error("Enter a preset name");
    text = text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
    if (text.size() > 120) throw std::runtime_error("Preset name is too long");
    for (unsigned char ch : text) if (ch < 32) throw std::runtime_error("Preset name contains control characters");
    return text; // A JSON key, never a filesystem path.
}

inline void Write(const std::filesystem::path& path, const Json& document)
{
    const auto bytes = document.dump(2);
    if (bytes.size() > 1024 * 1024) throw std::runtime_error("Too many saved presets (1 MB limit)");
    auto temporary = path;
    temporary += L".tmp-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot create preset file; check folder permissions");
    DWORD written = 0;
    const bool ok = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
                    written == bytes.size() && FlushFileBuffers(file);
    CloseHandle(file);
    if (!ok || !MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        DeleteFileW(temporary.c_str());
        throw std::runtime_error("Could not save presets; the previous file was retained");
    }
}
}
