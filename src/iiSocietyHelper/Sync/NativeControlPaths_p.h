#pragma once
#include <filesystem>
#include <string>
namespace iiSocietyHelper::Sync::detail {
// Shared by native watch discovery and the replica index. Portable user-edited
// .iisacc.json annotations remain eligible; derived/state files never do.
inline bool providerIndexControlName(const std::filesystem::path &path) {
    const auto utf8 = path.filename().u8string(); std::string name(utf8.begin(), utf8.end());
    for (auto &c : name) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return name.starts_with(".society-") || name.starts_with(".iiwebserver-")
        || name == ".iisacc-temporary.lock" || name.ends_with(".iisacc-preview")
        || name.starts_with(".iifile-stage-") || name == ".iisacc-file-provider"
        || name.ends_with(".iisacc-lock");
}
}
