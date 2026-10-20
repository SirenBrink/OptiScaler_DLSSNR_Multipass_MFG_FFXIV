#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <vector>
#include <algorithm>
#include <queue>

namespace DlssNr::RuntimeCatalog {
// A version is a folder name, never an arbitrary DLL path.
inline bool Valid(const std::string& name) {
    if (name.empty()) return true;
    if (!name.starts_with("NR.") || name.size() <= 3) return false;
    bool digit = false;
    for (size_t i=3; i<name.size(); ++i) {
        const char c=name[i];
        if (c=='.') { if (!digit) return false; digit=false; }
        else if (c>='0' && c<='9') digit=true;
        else return false;
    }
    return digit;
}
inline std::vector<std::string> Scan(const std::vector<std::filesystem::path>& roots) {
    std::vector<std::string> names;
    for (const auto& root:roots) {
        std::error_code ec;
        std::filesystem::directory_iterator it(root, ec), end;
        while (!ec && it!=end) {
            auto name=it->path().filename().string();
            if (Valid(name) && !name.empty() && std::filesystem::is_regular_file(it->path()/"nvngx_dlssnr.dll",ec))
                names.push_back(name);
            it.increment(ec);
        }
    }
    std::sort(names.begin(),names.end());
    names.erase(std::unique(names.begin(),names.end()),names.end());
    return names;
}
inline std::optional<std::filesystem::path> Legacy(const std::filesystem::path& root) {
    std::queue<std::filesystem::path> pending; pending.push(root);
    while (!pending.empty()) {
        auto dir=pending.front(); pending.pop();
        std::error_code ec;
        const auto dll=dir/"nvngx_dlssnr.dll";
        if (std::filesystem::is_regular_file(dll,ec)) return dll;
        ec.clear();
        std::filesystem::directory_iterator it(dir,ec), end;
        while (!ec && it!=end) {
            const auto name=it->path().filename().string();
            if (it->is_directory(ec) && !it->is_symlink(ec) && !(Valid(name) && !name.empty()))
                pending.push(it->path());
            it.increment(ec);
        }
    }
    return std::nullopt;
}
inline std::optional<std::filesystem::path> Resolve(const std::vector<std::filesystem::path>& roots,
        const std::string& name) {
    if (name.empty() || !Valid(name)) return std::nullopt;
    for (const auto& root:roots) {
        auto dll=root/name/"nvngx_dlssnr.dll";
        std::error_code ec;
        if (std::filesystem::is_regular_file(dll,ec)) return dll;
    }
    // An explicit missing choice must never quietly run a different model.
    return std::nullopt;
}
}
