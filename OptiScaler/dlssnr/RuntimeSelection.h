#pragma once
#include "RuntimeCatalog.h"
#include <Config.h>
#include <Util.h>
#include <Logger.h>

namespace DlssNr::RuntimeSelection {
inline std::vector<std::filesystem::path> Roots() {
    std::vector<std::filesystem::path> roots;
    auto cfg=Config::Instance();
    if (cfg->MainDllPath.has_value()) roots.emplace_back(cfg->MainDllPath.value());
    roots.push_back(Util::DllPath().parent_path()/L"OptiScaler");
    roots.push_back(Util::ExePath().parent_path()/L"OptiScaler");
    return roots;
}
struct Selection { std::string requested; std::optional<std::filesystem::path> path; };
inline const Selection& Startup() {
    // Shared by scene, preview, Vulkan and NGX initialization. Never mix versions
    // inside one process: outstanding GPU work and runtime hooks can outlive NR off.
    static const Selection selected=[] {
        Selection s; s.requested=Config::Instance()->DlssNrRuntime.value_or_default();
        if (!s.requested.empty()) s.path=RuntimeCatalog::Resolve(Roots(),s.requested);
        else {
            s.path=RuntimeCatalog::Legacy(Util::DllPath().parent_path());
            if (!s.path) s.path=RuntimeCatalog::Legacy(Util::ExePath().parent_path());
            if (!s.path && Config::Instance()->MainDllPath.has_value()) s.path=RuntimeCatalog::Legacy(Config::Instance()->MainDllPath.value());
        }
        if (s.path) LOG_INFO("DLSS-NR runtime selected for this session: {}",s.path->string());
        else LOG_WARN("DLSS-NR runtime not found (selection '{}'); NR remains unavailable",s.requested);
        return s;
    }();
    return selected;
}
inline std::optional<std::filesystem::path> Path() { return Startup().path; }
}
