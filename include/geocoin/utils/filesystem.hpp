#pragma once

#include <filesystem>
#include <string>

namespace geocoin::utils {

inline std::string path_to_utf8(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return {value.begin(), value.end()};
}

// Core paths are UTF-8. Windows needs an extended absolute path for long names,
// independently of the user's registry setting or the executable's manifest.
inline std::filesystem::path path_from_utf8(const std::string& value) {
    auto path = std::filesystem::u8path(value);
#ifdef _WIN32
    path.make_preferred();
    if (path.native().size() >= 248 && path.native().rfind(L"\\\\?\\", 0) != 0) {
        auto native = std::filesystem::absolute(path).lexically_normal().native();
        if (native.rfind(L"\\\\", 0) == 0) native = L"\\\\?\\UNC\\" + native.substr(2);
        else native = L"\\\\?\\" + native;
        path = std::filesystem::path(native);
    }
#endif
    return path;
}

} // namespace geocoin::utils
