#pragma once
#include "RuntimeCatalog.h"
#include <Config.h>
#include <Util.h>
#include <Logger.h>
#include <memory>
#include <mutex>
#include <cstdint>

namespace DlssNr::RuntimeSelection {
inline std::vector<std::filesystem::path> Roots() {
    std::vector<std::filesystem::path> roots;
    auto cfg=Config::Instance();
    if (cfg->MainDllPath.has_value()) roots.emplace_back(cfg->MainDllPath.value());
    roots.push_back(Util::DllPath().parent_path()/L"OptiScaler");
    roots.push_back(Util::ExePath().parent_path()/L"OptiScaler");
    return roots;
}
struct Selection { std::string requested; std::optional<std::filesystem::path> path; uint64_t revision=0; };
inline Selection Resolve(const std::string& requested) {
    Selection s;s.requested=requested;
    if (!s.requested.empty()) s.path=RuntimeCatalog::Resolve(Roots(),s.requested);
    else {
        s.path=RuntimeCatalog::Legacy(Util::DllPath().parent_path());
        if (!s.path) s.path=RuntimeCatalog::Legacy(Util::ExePath().parent_path());
        if (!s.path && Config::Instance()->MainDllPath.has_value()) s.path=RuntimeCatalog::Legacy(Config::Instance()->MainDllPath.value());
    }
    return s;
}
inline std::shared_ptr<const Selection> Current() {
    static std::mutex mutex;
    static std::shared_ptr<const Selection> selected;
    std::lock_guard lock(mutex);
    const auto requested=Config::Instance()->DlssNrRuntime.value_or_default();
    if (!selected || selected->requested!=requested) {
        auto next=Resolve(requested);next.revision=selected?selected->revision+1:1;
        selected=std::make_shared<const Selection>(std::move(next));
        if(selected->path) LOG_INFO("DLSS-NR runtime request {}: {}",selected->revision,selected->path->string());
        else LOG_WARN("DLSS-NR runtime not found (selection '{}'); NR remains unavailable",requested);
    }
    return selected;
}
inline const std::shared_ptr<const Selection>& Session() {
    // Core search paths and native Vulkan remain pinned. D3D12 direct features
    // use Current(), retire their own GPU work and retain loaded model modules.
    static const auto selected=Current();return selected;
}
inline const Selection& Startup() { return *Session(); }
inline std::shared_ptr<const Selection> D3D12() {
    // The proxy discovers its DLL through the core's startup search paths.
    // Hybrid FP4 contraction caches weights per device, not per model module.
    const auto cfg=Config::Instance();
    return cfg->DlssNrUseProxy.value_or_default() || cfg->DlssNrPrecision.value_or_default()==4
        ? Session() : Current();
}
inline std::optional<std::filesystem::path> Path() { return D3D12()->path; }
}
