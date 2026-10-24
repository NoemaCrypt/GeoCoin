#pragma once

#include "geocoin/wallet/wallet.hpp"
#include "geocoin/wallet/wallet_internal.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>
#include <openssl/crypto.h>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace geocoin::wallet {

inline bool SyncDebugEnabled() {
    static const bool enabled = (std::getenv("GEOCOIN_SYNC_DEBUG") != nullptr);
    return enabled;
}

constexpr size_t kWalletHmacSize = 32;
constexpr size_t kWalletSaltSize = 16;
constexpr size_t kPasswordVerifierSaltSize = 16;
constexpr uint32_t kPasswordVerifierIterations = 600000;
constexpr uint8_t kPasswordModeNone = 0;
constexpr uint8_t kPasswordModePbkdf2 = 2;
constexpr uint64_t kMaxWalletPayloadSize = 256ULL * 1024ULL * 1024ULL;
constexpr size_t kWalletHeaderSize = sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint8_t) + sizeof(uint32_t);
constexpr uint32_t kWalletVersion = 13;

struct SensitiveBytesGuard {
    bytes& value;

    ~SensitiveBytesGuard() {
        if (!value.empty()) {
            OPENSSL_cleanse(value.data(), value.size());
        }
    }
};

struct SensitiveHashGuard {
    Hash256& value;

    ~SensitiveHashGuard() {
        OPENSSL_cleanse(value.data(), value.size());
    }
};

struct WalletLoadScratch {
    bool has_password_hash = false;
    Hash256 password_hash{};
    bytes password_salt;
    uint32_t password_kdf_iterations = 0;
    std::map<Address, KeyInfo> keys;
    std::vector<WalletTransaction> transactions;
    std::unordered_map<Hash256, size_t, Hash256Hasher> tx_index;
    std::map<std::string, wallet::MultisigConfig> multisig_addresses;
    Height last_scanned_height = 0;
    Hash256 last_scanned_block_hash{};
    Address primary_address;

    ~WalletLoadScratch() {
        OPENSSL_cleanse(password_hash.data(), password_hash.size());
        if (!password_salt.empty()) {
            OPENSSL_cleanse(password_salt.data(), password_salt.size());
        }
        for (auto& [address, info] : keys) {
            (void)address;
            if (!info.private_key.empty()) {
                OPENSSL_cleanse(info.private_key.data(), info.private_key.size());
            }
        }
    }
};

class ScopedReadFd {
public:
    explicit ScopedReadFd(int fd = -1) noexcept : fd_(fd) {}
    ~ScopedReadFd() {
        if (fd_ >= 0) {
#if defined(_WIN32)
            (void)::_close(fd_);
#else
            (void)::close(fd_);
#endif
        }
    }

    ScopedReadFd(const ScopedReadFd&) = delete;
    ScopedReadFd& operator=(const ScopedReadFd&) = delete;

    int get() const noexcept { return fd_; }

private:
    int fd_;
};

inline bool constant_time_compare(const uint8_t* a, const uint8_t* b, size_t n) {
    uint8_t diff = 0;
    for (size_t i = 0; i < n; ++i) {
        diff |= (a[i] ^ b[i]);
    }
    return diff == 0;
}

inline void append_u32_le(bytes& out, uint32_t value) {
    out.push_back(static_cast<byte>(value & 0xFFu));
    out.push_back(static_cast<byte>((value >> 8) & 0xFFu));
    out.push_back(static_cast<byte>((value >> 16) & 0xFFu));
    out.push_back(static_cast<byte>((value >> 24) & 0xFFu));
}

inline void append_u64_le(bytes& out, uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<byte>((value >> (i * 8)) & 0xFFu));
    }
}

inline void append_i64_le(bytes& out, int64_t value) {
    append_u64_le(out, static_cast<uint64_t>(value));
}

bool set_private_file_permissions(const std::string& path);
bool is_regular_private_file(const std::string& path);
int open_secure_input_file(const std::string& path, uint64_t& file_size);
bool read_all_from_fd(int fd, byte* data, size_t size);
int open_secure_temp_file(const std::string& filename, std::string& tmp_path);
bool write_all_to_fd(int fd, const byte* data, size_t size);
bool sync_and_close_fd(int fd);
void close_fd_without_sync(int fd);
bool rename_file_durably(
    const std::string& source,
    const std::string& destination,
    bool replace_existing,
    std::error_code& ec
);
bool sync_parent_directory(const std::string& filename);
bool read_wallet_encryption_flag(const std::string& path, bool& encrypted);
bool derive_hmac_key_from_encrypted(
    const std::string& password,
    const bytes& encrypted,
    int iterations,
    bytes& key
);
bool compute_wallet_hmac(
    const std::string& password,
    const bytes& encrypted,
    int iterations,
    const bytes& file_body,
    bytes& hmac_out
);
bool derive_password_verifier_pbkdf2(
    const std::string& password,
    const bytes& salt,
    uint32_t iterations,
    Hash256& out
);

} // namespace geocoin::wallet
