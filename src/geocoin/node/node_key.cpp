#include "geocoin/node/node_key.hpp"
#include "geocoin/crypto/geo_area.hpp"

#include <openssl/crypto.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

#if defined(_WIN32)
#include "geocoin/utils/windows_secure_file.hpp"
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace geocoin::node {
namespace {

constexpr size_t kNodeKeySize = 32;

class FileDescriptor {
public:
    explicit FileDescriptor(int fd = -1) noexcept : fd_(fd) {}
    ~FileDescriptor() { close(); }

    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;

    int get() const noexcept { return fd_; }

    bool close() noexcept
    {
        if (fd_ < 0) {
            return true;
        }
#if defined(_WIN32)
        const bool ok = ::_close(fd_) == 0;
#else
        const bool ok = ::close(fd_) == 0;
#endif
        fd_ = -1;
        return ok;
    }

private:
    int fd_;
};

std::runtime_error key_error(const std::filesystem::path& path, const char* detail)
{
    return std::runtime_error(
        "node.key " + std::string(detail) + ": " + path.string()
    );
}

bool read_all(int fd, byte* data, size_t size)
{
    size_t offset = 0;
    while (offset < size) {
#if defined(_WIN32)
        const unsigned int chunk = static_cast<unsigned int>(std::min<size_t>(
            size - offset,
            static_cast<size_t>(std::numeric_limits<unsigned int>::max())
        ));
        const int count = ::_read(fd, data + offset, chunk);
#else
        const ssize_t count = ::read(fd, data + offset, size - offset);
#endif
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            return false;
        }
        offset += static_cast<size_t>(count);
    }
    return true;
}

bool write_all(int fd, const byte* data, size_t size)
{
    size_t offset = 0;
    while (offset < size) {
#if defined(_WIN32)
        const unsigned int chunk = static_cast<unsigned int>(std::min<size_t>(
            size - offset,
            static_cast<size_t>(std::numeric_limits<unsigned int>::max())
        ));
        const int count = ::_write(fd, data + offset, chunk);
#else
        const ssize_t count = ::write(fd, data + offset, size - offset);
#endif
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            return false;
        }
        offset += static_cast<size_t>(count);
    }
    return true;
}

int open_existing_key(const std::filesystem::path& path)
{
#if defined(_WIN32)
    HANDLE handle = ::CreateFileW(
        path.c_str(),
        GENERIC_READ | READ_CONTROL | WRITE_DAC,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr
    );
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD error = ::GetLastError();
        errno = (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
            ? ENOENT
            : EACCES;
        return -1;
    }
    if (!utils::is_regular_nonreparse_file(handle) ||
        !utils::restrict_file_to_current_user(handle)) {
        (void)::CloseHandle(handle);
        errno = EACCES;
        return -1;
    }
    const int fd = ::_open_osfhandle(reinterpret_cast<intptr_t>(handle), _O_RDONLY | _O_BINARY);
    if (fd < 0) {
        (void)::CloseHandle(handle);
    }
    return fd;
#else
    return ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
#endif
}

int create_new_key_file(const std::filesystem::path& path)
{
#if defined(_WIN32)
    utils::CurrentUserOnlySecurity security;
    if (!security.valid()) {
        errno = EACCES;
        return -1;
    }
    HANDLE handle = ::CreateFileW(
        path.c_str(),
        GENERIC_WRITE | READ_CONTROL | WRITE_DAC,
        0,
        security.attributes(),
        CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr
    );
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD error = ::GetLastError();
        errno = (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS) ? EEXIST : EACCES;
        return -1;
    }
    if (!utils::is_regular_nonreparse_file(handle) ||
        !utils::restrict_file_to_current_user(handle)) {
        (void)::CloseHandle(handle);
        (void)::DeleteFileW(path.c_str());
        errno = EACCES;
        return -1;
    }
    const int fd = ::_open_osfhandle(reinterpret_cast<intptr_t>(handle), _O_WRONLY | _O_BINARY);
    if (fd < 0) {
        (void)::CloseHandle(handle);
        (void)::DeleteFileW(path.c_str());
    }
    return fd;
#else
    return ::open(
        path.c_str(),
        O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
        S_IRUSR | S_IWUSR
    );
#endif
}

bool validate_existing_key_file(int fd, const std::filesystem::path& path)
{
#if defined(_WIN32)
    struct _stat64 st {};
    if (::_fstat64(fd, &st) != 0 || (st.st_mode & _S_IFREG) == 0 ||
        st.st_size != static_cast<__int64>(kNodeKeySize)) {
        return false;
    }
    (void)path;
    return true;
#else
    (void)path;
    struct stat st {};
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != ::geteuid() ||
        (st.st_mode & (S_IRWXG | S_IRWXO)) != 0 ||
        st.st_size != static_cast<off_t>(kNodeKeySize)) {
        return false;
    }
    return true;
#endif
}

bool sync_file(int fd)
{
#if defined(_WIN32)
    return ::_commit(fd) == 0;
#else
    return ::fsync(fd) == 0;
#endif
}

void sync_parent_directory(const std::filesystem::path& path)
{
#if !defined(_WIN32)
    const std::filesystem::path parent = path.has_parent_path() ? path.parent_path() : ".";
    const int raw_fd = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (raw_fd < 0) {
        throw key_error(path, "parent directory cannot be opened for sync");
    }
    FileDescriptor fd(raw_fd);
    if (::fsync(fd.get()) != 0) {
        throw key_error(path, "parent directory sync failed");
    }
#else
    (void)path;
#endif
}

bytes generate_node_key()
{
    bytes key;
    if (!crypto::generate_private_key(key) || key.size() != kNodeKeySize) {
        throw std::runtime_error("Failed to generate node private key");
    }
    return key;
}

} // namespace

bytes load_or_create_node_key(const std::filesystem::path& data_dir)
{
    std::error_code ec;
    std::filesystem::create_directories(data_dir, ec);
    if (ec) {
        throw std::runtime_error("Failed to create node data directory: " + data_dir.string());
    }

    const std::filesystem::path key_path = data_dir / "node.key";
    int raw_fd = open_existing_key(key_path);
    if (raw_fd >= 0) {
        FileDescriptor fd(raw_fd);
        if (!validate_existing_key_file(fd.get(), key_path)) {
            throw key_error(key_path, "must be an owner-controlled regular 32-byte file");
        }
        bytes key(kNodeKeySize);
        if (!read_all(fd.get(), key.data(), key.size())) {
            OPENSSL_cleanse(key.data(), key.size());
            throw key_error(key_path, "could not be read completely");
        }
        if (!fd.close()) {
            OPENSSL_cleanse(key.data(), key.size());
            throw key_error(key_path, "could not be closed cleanly");
        }
        return key;
    }
    if (errno != ENOENT) {
        throw key_error(key_path, "cannot be opened safely");
    }

    bytes key = generate_node_key();
    raw_fd = create_new_key_file(key_path);
    if (raw_fd < 0) {
        OPENSSL_cleanse(key.data(), key.size());
        throw key_error(key_path, "cannot be created atomically");
    }

    FileDescriptor fd(raw_fd);
    if (!write_all(fd.get(), key.data(), key.size()) || !sync_file(fd.get()) || !fd.close()) {
        OPENSSL_cleanse(key.data(), key.size());
        std::filesystem::remove(key_path, ec);
        throw key_error(key_path, "could not be persisted");
    }
    try {
        sync_parent_directory(key_path);
    } catch (...) {
        OPENSSL_cleanse(key.data(), key.size());
        throw;
    }
    return key;
}

} // namespace geocoin::node
