#pragma once
#include <array>
#include <cstdint>
#include <optional>
namespace DlssModelHints {
// This historical UI value is not an NGX model preset. Request the runtime's
// default explicitly instead of sending an invalid enum and relying on failure.
inline uint32_t NgxPreset(uint32_t value) { return value == 0x00FFFFFFu ? 0u : value; }
struct Settings {
    uint32_t quality=0;
    std::array<uint32_t,6> presets{};
    bool operator==(const Settings&) const = default;
};
inline Settings Resolve(uint32_t quality,std::array<uint32_t,6> source,bool enabled,
    std::optional<uint32_t> all,const std::array<std::optional<uint32_t>,6>& perQuality) {
    for(size_t i=0;i<source.size();++i)
        source[i]=NgxPreset(enabled ? all.value_or(perQuality[i].value_or(source[i])) : source[i]);
    return {quality,source};
}
}
