#pragma once

#include "geocoin/utils/secure_memory.hpp"

#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string>

#if defined(_WIN32)
#include "geocoin/utils/windows_secure_file.hpp"
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace geocoin::utils {

// Read a single secret from an owner-controlled regular file. Descriptor
// checks happen after open to avoid path-check/open races and symlink swaps.
inline std::string read_secret_file(
    const std::filesystem::path& path,
    const char* label,
    size_t max_secret_size = 4096
) {
    if (path.empty() || max_secret_size == 0 ||
        max_secret_size > (std::numeric_limits<size_t>::max)() - 2) {
        throw std::runtime_error(std::string("Invalid ") + label + " file parameters");
    }

    std::string raw;
#if defined(_WIN32)
    ScopedWindowsHandle handle(::CreateFileW(
        path.c_str(),
        GENERIC_READ | READ_CONTROL | WRITE_DAC,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr
    ));
    if (handle.get() == INVALID_HANDLE_VALUE) {
        throw std::runtime_error(std::string("Failed to open ") + label +
            " file safely: " + path.string());
    }

    uint64_t file_size = 0;
    if (!is_regular_nonreparse_file(handle.get(), &file_size) ||
        !restrict_file_to_current_user(handle.get()) ||
        file_size == 0 || file_size > max_secret_size + 2) {
        throw std::runtime_error(std::string(label) +
            " file must be a private regular file of bounded size: " + path.string());
    }

    raw.resize(static_cast<size_t>(file_size));
    DWORD bytes_read = 0;
    if (!::ReadFile(
            handle.get(),
            raw.data(),
            static_cast<DWORD>(raw.size()),
            &bytes_read,
            nullptr) ||
        static_cast<size_t>(bytes_read) != raw.size()) {
        cleanse_container(raw);
        throw std::runtime_error(std::string("Failed to read ") + label +
            " file: " + path.string());
    }
#else
    int open_flags = O_RDONLY;
#ifdef O_NOFOLLOW
    open_flags |= O_NOFOLLOW;
#endif
#ifdef O_CLOEXEC
    open_flags |= O_CLOEXEC;
#endif
    const int fd = ::open(path.c_str(), open_flags);
    if (fd < 0) {
        throw std::runtime_error(std::string("Failed to open ") + label +
            " file safely: " + path.string());
    }

    struct DescriptorGuard {
        int fd;
        ~DescriptorGuard() { if (fd >= 0) (void)::close(fd); }
    } guard{fd};

    struct stat st {};
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
        st.st_uid != ::geteuid() || (st.st_mode & (S_IRWXG | S_IRWXO)) != 0 ||
        st.st_size <= 0 || static_cast<uint64_t>(st.st_size) > max_secret_size + 2) {
        throw std::runtime_error(std::string(label) +
            " file must be owner-controlled, mode 0600/0400, regular and bounded: " +
            path.string());
    }

    raw.resize(static_cast<size_t>(st.st_size));
    size_t offset = 0;
    while (offset < raw.size()) {
        const ssize_t count = ::read(fd, raw.data() + offset, raw.size() - offset);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            cleanse_container(raw);
            throw std::runtime_error(std::string("Failed to read ") + label +
                " file: " + path.string());
        }
        offset += static_cast<size_t>(count);
    }
#endif

    const size_t line_end = raw.find_first_of("\r\n");
    const size_t secret_size = line_end == std::string::npos ? raw.size() : line_end;
    if (secret_size == 0 || secret_size > max_secret_size ||
        raw.find('\0') < secret_size) {
        cleanse_container(raw);
        throw std::runtime_error(std::string(label) +
            " file contains an invalid secret: " + path.string());
    }

    if (line_end != std::string::npos) {
        for (size_t i = line_end; i < raw.size(); ++i) {
            if (raw[i] != '\r' && raw[i] != '\n') {
                cleanse_container(raw);
                throw std::runtime_error(std::string(label) +
                    " file must contain exactly one line: " + path.string());
            }
        }
    }

    std::string secret(raw.data(), secret_size);
    cleanse_container(raw);
    return secret;
}

} // namespace geocoin::utils
