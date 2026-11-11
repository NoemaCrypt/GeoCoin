#include "geocoin/wallet/wallet_persist_internal.hpp"
#include "geocoin/crypto/encryption.hpp"
#include "geocoin/crypto/geo_area.hpp"
#include "geocoin/utils/logger.hpp"

#include <cerrno>
#include <algorithm>
#include <array>
#include <filesystem>
#include <limits>
#include <openssl/rand.h>
#include "geocoin/crypto/ioke_sha256.h"
#include <openssl/hmac.h>
#include <openssl/evp.h>

#if defined(_WIN32)
#include "geocoin/utils/windows_secure_file.hpp"
#include <fcntl.h>
#include <io.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace geocoin::wallet {

bool set_private_file_permissions(const std::string& path) {
#if defined(_WIN32)
    const std::filesystem::path native_path(path);
    HANDLE handle = ::CreateFileW(
        native_path.c_str(),
        READ_CONTROL | WRITE_DAC | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr
    );
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    const bool ok = utils::is_regular_nonreparse_file(handle) &&
        utils::restrict_file_to_current_user(handle);
    (void)::CloseHandle(handle);
    return ok;
#else
    struct stat st {};
    if (::lstat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != ::geteuid()) {
        return false;
    }
    return ::chmod(path.c_str(), S_IRUSR | S_IWUSR) == 0;
#endif
}

bool is_regular_private_file(const std::string& path) {
#if defined(_WIN32)
    const std::filesystem::path native_path(path);
    HANDLE handle = ::CreateFileW(
        native_path.c_str(),
        FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr
    );
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    const bool ok = utils::is_regular_nonreparse_file(handle);
    (void)::CloseHandle(handle);
    return ok;
#else
    struct stat st {};
    return ::lstat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == ::geteuid();
#endif
}

int open_secure_input_file(const std::string& path, uint64_t& file_size) {
#if defined(_WIN32)
    const std::filesystem::path native_path(path);
    HANDLE handle = ::CreateFileW(
        native_path.c_str(),
        GENERIC_READ | READ_CONTROL | WRITE_DAC,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr
    );
    if (handle == INVALID_HANDLE_VALUE) {
        return -1;
    }

    uint64_t size = 0;
    if (!utils::is_regular_nonreparse_file(handle, &size) ||
        !utils::restrict_file_to_current_user(handle)) {
        (void)::CloseHandle(handle);
        return -1;
    }

    const int fd = ::_open_osfhandle(reinterpret_cast<intptr_t>(handle), _O_RDONLY | _O_BINARY);
    if (fd < 0) {
        (void)::CloseHandle(handle);
        return -1;
    }
    file_size = size;
    return fd;
#else
    const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }

    struct stat st {};
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != ::geteuid() || st.st_size < 0 ||
        ::fchmod(fd, S_IRUSR | S_IWUSR) != 0) {
        (void)::close(fd);
        return -1;
    }
    file_size = static_cast<uint64_t>(st.st_size);
    return fd;
#endif
}

bool read_all_from_fd(int fd, byte* data, size_t size) {
    size_t read_total = 0;
    while (read_total < size) {
#if defined(_WIN32)
        const unsigned int chunk = static_cast<unsigned int>(std::min<size_t>(
            size - read_total,
            static_cast<size_t>(std::numeric_limits<unsigned int>::max())
        ));
        const int result = ::_read(fd, data + read_total, chunk);
#else
        const ssize_t result = ::read(fd, data + read_total, size - read_total);
        if (result < 0 && errno == EINTR) {
            continue;
        }
#endif
        if (result <= 0) {
            return false;
        }
        read_total += static_cast<size_t>(result);
    }
    return true;
}

int open_secure_temp_file(const std::string& filename, std::string& tmp_path) {
    static constexpr char kHex[] = "0123456789abcdef";
    for (int attempt = 0; attempt < 16; ++attempt) {
        std::array<unsigned char, 12> random_suffix{};
        if (RAND_bytes(random_suffix.data(), static_cast<int>(random_suffix.size())) != 1) {
            return -1;
        }

        tmp_path = filename + ".tmp.";
        tmp_path.reserve(tmp_path.size() + random_suffix.size() * 2);
        for (const unsigned char value : random_suffix) {
            tmp_path.push_back(kHex[value >> 4]);
            tmp_path.push_back(kHex[value & 0x0f]);
        }

#if defined(_WIN32)
        utils::CurrentUserOnlySecurity security;
        if (!security.valid()) {
            return -1;
        }
        const std::filesystem::path native_path(tmp_path);
        HANDLE handle = ::CreateFileW(
            native_path.c_str(),
            GENERIC_WRITE | READ_CONTROL | WRITE_DAC,
            0,
            security.attributes(),
            CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr
        );
        if (handle != INVALID_HANDLE_VALUE) {
            if (!utils::is_regular_nonreparse_file(handle) ||
                !utils::restrict_file_to_current_user(handle)) {
                (void)::CloseHandle(handle);
                (void)::DeleteFileW(native_path.c_str());
                return -1;
            }
            const int fd = ::_open_osfhandle(
                reinterpret_cast<intptr_t>(handle),
                _O_WRONLY | _O_BINARY
            );
            if (fd < 0) {
                (void)::CloseHandle(handle);
                (void)::DeleteFileW(native_path.c_str());
                return -1;
            }
            return fd;
        }
        const DWORD error = ::GetLastError();
        if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) {
            return -1;
        }
#else
        const int fd = ::open(
            tmp_path.c_str(),
            O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
            S_IRUSR | S_IWUSR
        );
        if (fd >= 0) {
            return fd;
        }
        if (errno != EEXIST) {
            return -1;
        }
#endif
    }
    return -1;
}

bool write_all_to_fd(int fd, const byte* data, size_t size) {
    size_t written = 0;
    while (written < size) {
#if defined(_WIN32)
        const unsigned int chunk = static_cast<unsigned int>(std::min<size_t>(
            size - written,
            static_cast<size_t>(std::numeric_limits<unsigned int>::max())
        ));
        const int result = ::_write(fd, data + written, chunk);
#else
        const ssize_t result = ::write(fd, data + written, size - written);
#endif
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result <= 0) {
            return false;
        }
        written += static_cast<size_t>(result);
    }
    return true;
}

bool sync_and_close_fd(int fd) {
#if defined(_WIN32)
    const bool synced = ::_commit(fd) == 0;
    const bool closed = ::_close(fd) == 0;
#else
    int sync_result = -1;
    do {
        sync_result = ::fsync(fd);
    } while (sync_result != 0 && errno == EINTR);
    const bool synced = sync_result == 0;
    const bool closed = ::close(fd) == 0;
#endif
    return synced && closed;
}

void close_fd_without_sync(int fd) {
#if defined(_WIN32)
    (void)::_close(fd);
#else
    (void)::close(fd);
#endif
}

bool rename_file_durably(
    const std::string& source,
    const std::string& destination,
    bool replace_existing,
    std::error_code& ec
) {
#if defined(_WIN32)
    const std::filesystem::path native_source(source);
    const std::filesystem::path native_destination(destination);
    DWORD flags = MOVEFILE_WRITE_THROUGH;
    if (replace_existing) {
        flags |= MOVEFILE_REPLACE_EXISTING;
    }
    if (::MoveFileExW(native_source.c_str(), native_destination.c_str(), flags) != FALSE) {
        ec.clear();
        return true;
    }
    ec = std::error_code(static_cast<int>(::GetLastError()), std::system_category());
    return false;
#else
    (void)replace_existing;
    std::filesystem::rename(source, destination, ec);
    return !ec;
#endif
}

bool sync_parent_directory(const std::string& filename) {
#if defined(_WIN32)
    // Every metadata transition is performed through MoveFileExW with
    // MOVEFILE_WRITE_THROUGH. Windows does not provide a portable directory
    // fsync equivalent for ordinary applications.
    (void)filename;
    return true;
#else
    std::filesystem::path parent = std::filesystem::path(filename).parent_path();
    if (parent.empty()) {
        parent = ".";
    }
    const int fd = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    int sync_result = -1;
    do {
        sync_result = ::fsync(fd);
    } while (sync_result != 0 && errno == EINTR);
    const bool ok = sync_result == 0;
    (void)::close(fd);
    return ok;
#endif
}

bool read_wallet_encryption_flag(const std::string& path, bool& encrypted) {
    uint64_t file_size = 0;
    ScopedReadFd file(open_secure_input_file(path, file_size));
    std::array<byte, 9> prefix{};
    if (file.get() < 0 || file_size < prefix.size() ||
        !read_all_from_fd(file.get(), prefix.data(), prefix.size())) {
        return false;
    }
    const uint32_t magic = static_cast<uint32_t>(prefix[0]) |
        (static_cast<uint32_t>(prefix[1]) << 8) |
        (static_cast<uint32_t>(prefix[2]) << 16) |
        (static_cast<uint32_t>(prefix[3]) << 24);
    if (magic != 0x47454F43 || prefix[8] > 1) {
        return false;
    }
    encrypted = prefix[8] != 0;
    return true;
}

bool derive_hmac_key_from_encrypted(
    const std::string& password,
    const bytes& encrypted,
    int iterations,
    bytes& key
) {
    if (password.empty() || encrypted.size() < kWalletSaltSize) {
        return false;
    }
    bytes salt(encrypted.begin(), encrypted.begin() + kWalletSaltSize);
    return crypto::derive_key_pbkdf2(password, salt, iterations, key);
}

bool compute_wallet_hmac(
    const std::string& password,
    const bytes& encrypted,
    int iterations,
    const bytes& file_body,
    bytes& hmac_out
) {
    bytes key;
    if (!derive_hmac_key_from_encrypted(password, encrypted, iterations, key)) {
        return false;
    }

    unsigned int len = 0;
    hmac_out.resize(kWalletHmacSize);
    if (!HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
              file_body.data(), file_body.size(), hmac_out.data(), &len)) {
        OPENSSL_cleanse(key.data(), key.size());
        return false;
    }
    hmac_out.resize(len);
    OPENSSL_cleanse(key.data(), key.size());
    return true;
}

bool derive_password_verifier_pbkdf2(
    const std::string& password,
    const bytes& salt,
    uint32_t iterations,
    Hash256& out
) {
    if (password.empty() || salt.size() != kPasswordVerifierSaltSize ||
        iterations != kPasswordVerifierIterations) {
        return false;
    }

    bytes derived;
    if (!crypto::derive_key_pbkdf2(password, salt, static_cast<int>(iterations), derived)) {
        return false;
    }
    if (derived.size() < out.size()) {
        if (!derived.empty()) {
            OPENSSL_cleanse(derived.data(), derived.size());
        }
        return false;
    }

    std::copy(derived.begin(), derived.begin() + out.size(), out.begin());
    OPENSSL_cleanse(derived.data(), derived.size());
    return true;
}

bool init_password_verifier_from_password(
    const std::string& password,
    bytes& salt_out,
    uint32_t& iterations_out,
    Hash256& verifier_out
) {
    salt_out.assign(kPasswordVerifierSaltSize, 0);
    if (RAND_bytes(reinterpret_cast<unsigned char*>(salt_out.data()), static_cast<int>(salt_out.size())) != 1) {
        return false;
    }
    iterations_out = kPasswordVerifierIterations;
    if (!derive_password_verifier_pbkdf2(password, salt_out, iterations_out, verifier_out)) {
        OPENSSL_cleanse(salt_out.data(), salt_out.size());
        salt_out.clear();
        iterations_out = 0;
        return false;
    }
    return true;
}

bool verify_password_material(
    const std::string& password,
    bool has_password_hash,
    const Hash256& password_hash,
    const bytes& password_salt,
    uint32_t password_kdf_iterations
) {
    if (!has_password_hash) {
        return true;
    }

    if (!password_salt.empty()) {
        if (password_salt.size() != kPasswordVerifierSaltSize ||
            password_kdf_iterations != kPasswordVerifierIterations) {
            return false;
        }
        Hash256 input_hash{};
        SensitiveHashGuard input_hash_guard{input_hash};
        return derive_password_verifier_pbkdf2(
                   password,
                   password_salt,
                   password_kdf_iterations,
                   input_hash
               ) &&
               constant_time_compare(password_hash.data(), input_hash.data(), input_hash.size());
    }

    return false;
}

} // namespace geocoin::wallet
