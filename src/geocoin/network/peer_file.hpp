#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace geocoin::network::detail {

// Peer caches are replaceable, but a failed refresh must leave the last
// complete file available. Each writer owns a distinct temporary file.
template<class Writer>
bool write_peer_file(const std::string& path, Writer write) {
    static std::atomic<uint64_t> sequence{0};
#ifdef _WIN32
    const auto process = GetCurrentProcessId();
#else
    const auto process = getpid();
#endif
    const std::filesystem::path target(path);
    const std::filesystem::path temporary = path + ".tmp." + std::to_string(process) + "." +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "." +
        std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
    struct Cleanup {
        const std::filesystem::path& path;
        ~Cleanup() { std::error_code ignored; std::filesystem::remove(path, ignored); }
    } cleanup{temporary};
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    if (!file) return false;
    write(file);
    file.close();
    if (!file) return false;
#ifdef _WIN32
    return MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
#else
    std::error_code error;
    std::filesystem::rename(temporary, target, error);
    return !error;
#endif
}

} // namespace geocoin::network::detail
