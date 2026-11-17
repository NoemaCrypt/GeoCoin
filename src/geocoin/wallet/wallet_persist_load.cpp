#include "geocoin/wallet/wallet_persist_internal.hpp"
#include "geocoin/crypto/encryption.hpp"
#include "geocoin/utils/logger.hpp"

#include <array>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <tuple>
#include "geocoin/crypto/ioke_sha256.h"
#include <openssl/crypto.h>

namespace geocoin::wallet {

const char* wallet_load_error_name(WalletLoadError error) noexcept {
    switch (error) {
#define GEO_LOAD_ERROR(name) case WalletLoadError::name: return #name
    GEO_LOAD_ERROR(NONE); GEO_LOAD_ERROR(IO_ERROR); GEO_LOAD_ERROR(INVALID_FORMAT);
    GEO_LOAD_ERROR(UNSUPPORTED_VERSION); GEO_LOAD_ERROR(PASSWORD_REQUIRED);
    GEO_LOAD_ERROR(AUTHENTICATION_FAILED); GEO_LOAD_ERROR(CORRUPT_DATA); GEO_LOAD_ERROR(RESOURCE_LIMIT);
#undef GEO_LOAD_ERROR
    }
    return "UNKNOWN";
}

const char* wallet_migration_error_name(WalletMigrationError error) noexcept {
    switch (error) {
#define GEO_MIGRATION_ERROR(name) case WalletMigrationError::name: return #name
    GEO_MIGRATION_ERROR(NONE); GEO_MIGRATION_ERROR(CONFIRMATION_REQUIRED); GEO_MIGRATION_ERROR(LOAD_FAILED);
    GEO_MIGRATION_ERROR(NOT_NEEDED); GEO_MIGRATION_ERROR(INVALID_PATH); GEO_MIGRATION_ERROR(DESTINATION_EXISTS);
    GEO_MIGRATION_ERROR(BACKUP_FAILED); GEO_MIGRATION_ERROR(WRITE_FAILED); GEO_MIGRATION_ERROR(VALIDATION_FAILED);
    GEO_MIGRATION_ERROR(SOURCE_CHANGED); GEO_MIGRATION_ERROR(COMMIT_FAILED);
#undef GEO_MIGRATION_ERROR
    }
    return "UNKNOWN";
}

bool Wallet::load_from_file(const std::string& filename, const std::string& password) {
    return load_from_file_result(filename, password, true).ok();
}

WalletLoadResult Wallet::last_load_result() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_load_result_;
}

WalletLoadResult Wallet::load_from_file_result(const std::string& filename,
    const std::string& password, bool allow_backup) {
    std::lock_guard<std::recursive_mutex> scan_lock(scan_mutex_);
    std::lock_guard<std::mutex> lock(mutex_);
    WalletLoadResult result;
    std::string selected_path;
    try {
        selected_path = absolute_wallet_path(filename);
    } catch (...) {
        result.error = WalletLoadError::IO_ERROR;
        last_load_result_ = result;
        return result;
    }

    auto try_load = [&](const std::string& path) -> bool {
        result = {};
        result.error = WalletLoadError::IO_ERROR;
        result.source_path = path;
        if (SyncDebugEnabled()) {
            GEO_LOG_DEBUG("wallet", "load_from_file START: filename=" << path);
        }

        uint64_t actual_file_size = 0;
        ScopedReadFd file(open_secure_input_file(path, actual_file_size));
        if (file.get() < 0) {
            GEO_LOG_ERROR(
                "wallet",
                "Refusing to load a wallet that is not an owner-controlled regular file: " << path
            );
            return false;
        }

        result.error = WalletLoadError::CORRUPT_DATA;
        std::array<byte, 4> prefix{};
        if (actual_file_size < prefix.size() || !read_all_from_fd(file.get(), prefix.data(), prefix.size()))
            return false;
        if (prefix == std::array<byte, 4>{'G', 'E', 'O', 'V'}) {
            file.close();
            if (!load_vault_locked(path, password, result)) return false;
            file_path_.swap(selected_path);
            return true;
        }

        std::array<byte, kWalletHeaderSize> header{};
        std::copy(prefix.begin(), prefix.end(), header.begin());
        if (actual_file_size < header.size() ||
            !read_all_from_fd(file.get(), header.data() + prefix.size(), header.size() - prefix.size())) {
            GEO_LOG_ERROR("wallet", "Failed to read wallet header: " << path);
            return false;
        }

        auto read_header_u32 = [&](size_t offset) -> uint32_t {
            return static_cast<uint32_t>(header[offset]) |
                   (static_cast<uint32_t>(header[offset + 1]) << 8) |
                   (static_cast<uint32_t>(header[offset + 2]) << 16) |
                   (static_cast<uint32_t>(header[offset + 3]) << 24);
        };

        const uint32_t magic = read_header_u32(0);
        const uint32_t version = read_header_u32(sizeof(uint32_t));
        const uint8_t encrypted_flag = header[sizeof(uint32_t) * 2];
        const uint32_t data_size = read_header_u32(sizeof(uint32_t) * 2 + sizeof(uint8_t));

        constexpr uint32_t kWalletMagic = 0x47454F43; // "GEOC"

        if (magic != kWalletMagic) {
            result.error = WalletLoadError::INVALID_FORMAT;
            GEO_LOG_ERROR("wallet", "Invalid wallet magic: 0x" << std::hex << magic << std::dec);
            return false;
        }

        if (SyncDebugEnabled()) {
            GEO_LOG_DEBUG("wallet", "Wallet version: " << version);
        }
        result.version = version;
        switch (version) {
        case 11: result.format = WalletFileFormat::LEGACY_V11; break;
        case 12: result.format = WalletFileFormat::LEGACY_V12; break;
        case 13: result.format = WalletFileFormat::NATIVE_V13; break;
        default:
            result.error = WalletLoadError::UNSUPPORTED_VERSION;
            GEO_LOG_ERROR(
                "wallet",
                "Unsupported wallet version: " << version << " (supported "
                    << kWalletVersion << ")"
            );
            return false;
        }

        if (encrypted_flag > 1) {
            GEO_LOG_ERROR("wallet", "Invalid encrypted flag: " << static_cast<int>(encrypted_flag));
            return false;
        }

        if (data_size > kMaxWalletPayloadSize) {
            result.error = WalletLoadError::RESOURCE_LIMIT;
            GEO_LOG_ERROR("wallet", "Wallet payload exceeds the maximum supported size");
            return false;
        }

        const uint64_t expected_file_size = kWalletHeaderSize + static_cast<uint64_t>(data_size) +
            (encrypted_flag ? kWalletHmacSize : 0);
        if (actual_file_size != expected_file_size) {
            GEO_LOG_ERROR("wallet", "Wallet file size does not match its header");
            return false;
        }

        if (SyncDebugEnabled()) {
            GEO_LOG_DEBUG(
                "wallet",
                "encrypted_flag=" << (int)encrypted_flag << ", data_size=" << data_size
            );
        }

        bytes encrypted;
        SensitiveBytesGuard encrypted_guard{encrypted};
        try {
            encrypted.resize(data_size);
        } catch (const std::bad_alloc&) {
            GEO_LOG_ERROR("wallet", "Insufficient memory for wallet payload");
            return false;
        }
        if (!encrypted.empty() && !read_all_from_fd(file.get(), encrypted.data(), encrypted.size())) {
            GEO_LOG_ERROR("wallet", "Failed to read wallet payload");
            return false;
        }

        bytes hmac_file;
        SensitiveBytesGuard hmac_file_guard{hmac_file};
        if (encrypted_flag) {
            hmac_file.resize(kWalletHmacSize);
            if (!read_all_from_fd(file.get(), hmac_file.data(), hmac_file.size())) {
                GEO_LOG_ERROR("wallet", "Failed to read wallet HMAC");
                return false;
            }
        }

        const int pbkdf2_iters = crypto::kPbkdf2Iterations;

        bytes file_body;
        SensitiveBytesGuard file_body_guard{file_body};
        file_body.reserve(sizeof(magic) + sizeof(version) + sizeof(encrypted_flag) + sizeof(data_size) + encrypted.size());
        append_u32_le(file_body, magic);
        append_u32_le(file_body, version);
        file_body.push_back(encrypted_flag);
        append_u32_le(file_body, data_size);
        file_body.insert(file_body.end(), encrypted.begin(), encrypted.end());

        if (encrypted_flag) {
            result.encrypted = true;
            if (password.empty()) {
                result.error = WalletLoadError::PASSWORD_REQUIRED;
                GEO_LOG_ERROR("wallet", "Wallet HMAC requires password");
                return false;
            }

            bytes hmac_calc;
            SensitiveBytesGuard hmac_calc_guard{hmac_calc};
            if (!compute_wallet_hmac(password, encrypted, pbkdf2_iters, file_body, hmac_calc) ||
                hmac_calc.size() != kWalletHmacSize) {
                GEO_LOG_ERROR("wallet", "Failed to compute wallet HMAC");
                return false;
            }
            if (!constant_time_compare(hmac_file.data(), hmac_calc.data(), kWalletHmacSize)) {
                result.error = WalletLoadError::AUTHENTICATION_FAILED;
                GEO_LOG_ERROR("wallet", "Wallet HMAC verification failed");
                return false;
            }
        }

        bytes buffer;
        SensitiveBytesGuard buffer_guard{buffer};
        if (encrypted_flag) {
            if (!crypto::decrypt_aes256(encrypted, password, buffer, pbkdf2_iters)) {
                result.error = WalletLoadError::AUTHENTICATION_FAILED;
                GEO_LOG_ERROR("wallet", "Wallet decryption failed");
                return false;
            }
            result.authenticated = true;
        } else {
            buffer = encrypted;
        }

        size_t offset = 0;
        auto require = [&](size_t len) -> bool {
            return offset <= buffer.size() && len <= buffer.size() - offset;
        };
        auto read_u32 = [&](uint32_t& out) -> bool {
            if (!require(sizeof(uint32_t))) return false;
            out = static_cast<uint32_t>(buffer[offset]) |
                  (static_cast<uint32_t>(buffer[offset + 1]) << 8) |
                  (static_cast<uint32_t>(buffer[offset + 2]) << 16) |
                  (static_cast<uint32_t>(buffer[offset + 3]) << 24);
            offset += sizeof(uint32_t);
            return true;
        };
        auto read_u64 = [&](uint64_t& out) -> bool {
            if (!require(sizeof(uint64_t))) return false;
            out = 0;
            for (int i = 0; i < 8; ++i) {
                out |= static_cast<uint64_t>(buffer[offset + i]) << (i * 8);
            }
            offset += sizeof(uint64_t);
            return true;
        };
        auto read_i64 = [&](int64_t& out) -> bool {
            uint64_t tmp = 0;
            if (!read_u64(tmp)) return false;
            out = static_cast<int64_t>(tmp);
            return true;
        };

        WalletLoadScratch loaded;

        if (!require(1)) {
            return false;
        }
        const uint8_t password_mode = buffer[offset++];
        bool upgrade_legacy_verifier = false;
        if (password_mode == kPasswordModeNone) {
            loaded.has_password_hash = false;
        } else if (password_mode == kPasswordModeLegacySha256 && (version == 11 || version == 12) &&
            encrypted_flag == 1 && result.authenticated) {
            // Historical mode 1 is exactly SHA256(password), with no salt/cost.
            // Never allow it without both outer HMAC and AES-GCM authentication.
            if (!require(32)) return false;
            auto digest = crypto::sha256(reinterpret_cast<const byte*>(password.data()), password.size());
            SensitiveHashGuard digest_guard{digest};
            if (!constant_time_compare(digest.data(), buffer.data() + offset, digest.size())) {
                result.error = WalletLoadError::AUTHENTICATION_FAILED;
                return false;
            }
            offset += digest.size();
            loaded.has_password_hash = true;
            upgrade_legacy_verifier = true;
        } else if (password_mode == kPasswordModePbkdf2) {
            if (!require(32 + kPasswordVerifierSaltSize + sizeof(uint32_t))) {
                return false;
            }
            std::memcpy(loaded.password_hash.data(), buffer.data() + offset, 32);
            offset += 32;
            loaded.password_salt.assign(
                buffer.begin() + offset,
                buffer.begin() + offset + kPasswordVerifierSaltSize
            );
            offset += kPasswordVerifierSaltSize;
            if (!read_u32(loaded.password_kdf_iterations)) {
                return false;
            }
            if (loaded.password_kdf_iterations != kPasswordVerifierIterations) {
                return false;
            }
            loaded.has_password_hash = true;
        } else {
            return false;
        }
        if (loaded.has_password_hash && !upgrade_legacy_verifier && !verify_password_material(password,
            loaded.has_password_hash, loaded.password_hash, loaded.password_salt,
            loaded.password_kdf_iterations)) {
            result.error = password.empty() ? WalletLoadError::PASSWORD_REQUIRED : WalletLoadError::AUTHENTICATION_FAILED;
            return false;
        }

        uint32_t key_count = 0;
        if (!read_u32(key_count) || key_count > (buffer.size() - offset) / 44) {
            return false;
        }

        for (uint32_t i = 0; i < key_count; ++i) {
            if (!require(32)) return false;
            bytes private_key(32);
            SensitiveBytesGuard private_key_guard{private_key};
            std::memcpy(private_key.data(), buffer.data() + offset, 32);
            offset += 32;

            uint32_t label_len = 0;
            if (!read_u32(label_len) || !require(label_len)) {
                return false;
            }
            std::string label(buffer.begin() + offset, buffer.begin() + offset + label_len);
            offset += label_len;

            uint64_t created_at = 0;
            if (!read_u64(created_at)) {
                return false;
            }

            KeyInfo info;
            info.private_key.assign(private_key.begin(), private_key.end());
            if (!crypto::compute_address_from_private_key(private_key, info.address)) return false;
            info.label = label;
            info.created_at = created_at;
            // v11 omits usage and the scan hash; v12 adds them. Only v13 stores
            // an explicit primary address and optional WEX1 history extension.
            info.used = true;
            if (version >= 12) {
                if (!require(1) || buffer[offset] > 1) return false;
                info.used = buffer[offset++] == 1;
            }
            const auto address = info.address;
            if (!loaded.keys.emplace(address, std::move(info)).second) return false;
        }

        uint32_t tx_count = 0;
        if (!read_u32(tx_count) || tx_count > (buffer.size() - offset) / 73) {
            return false;
        }
        for (uint32_t i = 0; i < tx_count; ++i) {
            WalletTransaction wtx;
            if (!require(32)) return false;
            std::memcpy(wtx.tx_hash.data(), buffer.data() + offset, 32);
            offset += 32;

            if (!read_u64(wtx.block_height)) return false;
            if (!read_u64(wtx.timestamp)) return false;
            if (!read_i64(wtx.amount)) return false;
            if (!read_u64(wtx.fee)) return false;
            if (!require(1)) return false;
            if (buffer[offset] > 1) return false;
            wtx.is_coinbase = buffer[offset++] == 1;

            uint32_t memo_len = 0;
            if (!read_u32(memo_len) || !require(memo_len)) return false;
            wtx.memo = std::string(buffer.begin() + offset, buffer.begin() + offset + memo_len);
            offset += memo_len;

            uint32_t tx_data_len = 0;
            if (!read_u32(tx_data_len) || !require(tx_data_len)) return false;
            bytes tx_data(buffer.begin() + offset, buffer.begin() + offset + tx_data_len);
            offset += tx_data_len;

            try {
                wtx.tx = core::Transaction::deserialize_canonical(tx_data);
            } catch (...) {
                return false;
            }
            if (wtx.tx_hash != wtx.tx.get_hash() || wtx.is_coinbase != wtx.tx.is_coinbase() ||
                !loaded.tx_index.emplace(wtx.tx_hash, loaded.transactions.size()).second) return false;
            loaded.transactions.push_back(std::move(wtx));
        }

        uint64_t scanned_height = 0;
        if (!read_u64(scanned_height)) {
            return false;
        }
        loaded.last_scanned_height = scanned_height;
        if (version >= 12) {
            if (!require(loaded.last_scanned_block_hash.size())) {
                return false;
            }
            std::memcpy(
                loaded.last_scanned_block_hash.data(),
                buffer.data() + offset,
                loaded.last_scanned_block_hash.size()
            );
            offset += loaded.last_scanned_block_hash.size();
        }

        uint32_t multisig_count = 0;
        if (!read_u32(multisig_count) || multisig_count > (buffer.size() - offset) / 10) {
            return false;
        }
        for (uint32_t i = 0; i < multisig_count; ++i) {
            uint32_t addr_len = 0;
            if (!read_u32(addr_len) || !require(addr_len)) return false;
            std::string address(buffer.begin() + offset, buffer.begin() + offset + addr_len);
            offset += addr_len;

            wallet::MultisigConfig config;
            config.address = Address(address);

            if (!require(2)) return false;
            config.m = buffer[offset++];
            config.n = buffer[offset++];

            uint32_t participant_count = 0;
            if (!read_u32(participant_count) || participant_count != config.n ||
                participant_count > (buffer.size() - offset) / 4) return false;

            for (uint32_t j = 0; j < participant_count; ++j) {
                uint32_t participant_addr_len = 0;
                if (!read_u32(participant_addr_len) || !require(participant_addr_len)) return false;
                std::string participant_addr_str(
                    buffer.begin() + offset,
                    buffer.begin() + offset + participant_addr_len
                );
                offset += participant_addr_len;

                Address participant_addr(participant_addr_str);
                config.addresses.push_back(participant_addr);
                config.participants.emplace_back(participant_addr);
            }

            if (!wallet::validate_multisig_config(config.address, config)) {
                GEO_LOG_ERROR("wallet", "Wallet contains an invalid multisig configuration");
                return false;
            }

            if (!loaded.multisig_addresses.emplace(address, std::move(config)).second) return false;
        }

        if (version >= 13) {
            uint32_t primary_length = 0;
            if (!read_u32(primary_length) || !require(primary_length)) return false;
            loaded.primary_address = Address(std::string(buffer.begin() + offset, buffer.begin() + offset + primary_length));
            offset += primary_length;
        } else {
            loaded.primary_address = resolve_primary_address(loaded.keys, {});
        }
        if (loaded.keys.empty() ? !loaded.primary_address.empty() : !loaded.keys.contains(loaded.primary_address)) return false;
        if (version >= 13 && offset != buffer.size()) {
            uint32_t marker = 0, count = 0;
            if (!read_u32(marker) || marker != 0x31584557 || !read_u32(count) ||
                count != loaded.transactions.size()) return false;
            for (auto& tx : loaded.transactions) {
                if (!require(32)) return false;
                std::copy_n(buffer.begin() + offset, 32, tx.block_hash.begin());
                offset += 32;
                uint32_t removed = 0;
                if (!read_u32(tx.block_index) || !read_u64(tx.block_time) || !read_u32(removed) ||
                    removed > (buffer.size() - offset) / 52) return false;
                for (uint32_t i = 0; i < removed; ++i) {
                    WalletBlockLocation location;
                    if (!require(32)) return false;
                    std::copy_n(buffer.begin() + offset, 32, location.hash.begin());
                    offset += 32;
                    if (!read_u64(location.height) || !read_u32(location.index) || !read_u64(location.time) ||
                        location.hash == Hash256{}) return false;
                    tx.removed_blocks.push_back(location);
                }
                uint32_t changes = 0;
                if (!read_u32(changes) || changes > tx.tx.outputs.size()) return false;
                for (uint32_t i = 0; i < changes; ++i) {
                    uint32_t index = 0;
                    if (!read_u32(index) || index >= tx.tx.outputs.size() ||
                        std::find(tx.change_outputs.begin(), tx.change_outputs.end(), index) != tx.change_outputs.end()) return false;
                    tx.change_outputs.push_back(index);
                }
                if (!require(1) || buffer[offset] > 1) return false;
                tx.broadcast = buffer[offset++] == 1;
                if (!read_u64(tx.received_at)) return false;
                uint32_t conflicts = 0;
                if (!read_u32(conflicts) || conflicts > (buffer.size() - offset) / 84) return false;
                for (uint32_t i = 0; i < conflicts; ++i) {
                    Hash256 other{};
                    WalletBlockLocation location;
                    if (!require(64)) return false;
                    std::copy_n(buffer.begin() + offset, 32, other.begin()); offset += 32;
                    std::copy_n(buffer.begin() + offset, 32, location.hash.begin()); offset += 32;
                    if (!read_u64(location.height) || !read_u32(location.index) || !read_u64(location.time)) return false;
                    tx.note_conflict(other, location);
                }
            }
        }
        if (offset != buffer.size()) {
            GEO_LOG_ERROR("wallet", "Wallet payload size mismatch: unread bytes remain");
            return false;
        }

        // Publish only current PBKDF2 material in memory. The source stays byte-
        // exact until explicit migration; a native save can never emit mode 1.
        if (upgrade_legacy_verifier && !init_password_verifier_from_password(password,
            loaded.password_salt, loaded.password_kdf_iterations, loaded.password_hash)) {
            result.error = WalletLoadError::IO_ERROR;
            return false;
        }

        // Finish every potentially allocating operation before publishing state.
        result.migration_needed = version < 13;
        result.error = WalletLoadError::NONE;
        std::swap(primary_address_, loaded.primary_address);
        file_path_.swap(selected_path);
        loaded_format_ = result.format;
        loaded_encrypted_ = result.encrypted;
        has_password_hash_ = loaded.has_password_hash;
        std::swap(password_hash_, loaded.password_hash);
        password_salt_.swap(loaded.password_salt);
        password_kdf_iterations_ = loaded.password_kdf_iterations;
        keys_.swap(loaded.keys);
        ++wallet_revision_;
        vault_mode_ = false;
        wrapped_master_key_.clear();
        encrypted_keys_.clear();
        if (!vault_master_key_.empty()) OPENSSL_cleanse(vault_master_key_.data(), vault_master_key_.size());
        vault_master_key_.clear();
        unlock_until_ = 0;
        unlock_deadline_ = {};
        vault_cv_.notify_all();
        transactions_.swap(loaded.transactions);
        tx_index_.swap(loaded.tx_index);
        multisig_addresses_.swap(loaded.multisig_addresses);
        last_scanned_height_ = loaded.last_scanned_height;
        last_scanned_block_hash_ = loaded.last_scanned_block_hash;
        last_scan_result_ = {};

        return true;
    };

    auto safe_try_load = [&](const std::string& path) -> bool {
        try {
            return try_load(path);
        } catch (const std::bad_alloc&) {
            result.error = WalletLoadError::RESOURCE_LIMIT;
            GEO_LOG_ERROR("wallet", "Wallet load aborted due to an allocation failure: " << path);
            return false;
        } catch (const std::length_error&) {
            result.error = WalletLoadError::RESOURCE_LIMIT;
            GEO_LOG_ERROR("wallet", "Wallet load rejected an oversized field: " << path);
            return false;
        } catch (const std::exception& error) {
            result.error = WalletLoadError::CORRUPT_DATA;
            GEO_LOG_ERROR("wallet", "Wallet load rejected malformed data: " << path << ": " << error.what());
            return false;
        } catch (...) {
            result.error = WalletLoadError::CORRUPT_DATA;
            GEO_LOG_ERROR("wallet", "Wallet load rejected malformed data: " << path);
            return false;
        }
    };

    if (safe_try_load(selected_path)) {
        last_load_result_ = std::move(result);
        return last_load_result_;
    }

    const auto primary_result = result;
    const std::string bak_filename = selected_path + ".bak";
    std::error_code ec;
    if (allow_backup && (result.error == WalletLoadError::IO_ERROR ||
        result.error == WalletLoadError::CORRUPT_DATA) && std::filesystem::exists(wallet_path(bak_filename), ec)) {
        GEO_LOG_ERROR("wallet", "Primary wallet load failed, trying backup: " << bak_filename);
        if (safe_try_load(bak_filename)) {
            result.recovered_from_backup = true;
            last_load_result_ = std::move(result);
            return last_load_result_;
        }
    }

    last_load_result_ = primary_result;
    return last_load_result_;
}

namespace {
bool read_wallet_bytes(const std::string& path, bytes& value) {
    uint64_t size = 0;
    ScopedReadFd file(open_secure_input_file(path, size));
    if (file.get() < 0 || size > kMaxWalletPayloadSize + kWalletHeaderSize + kWalletHmacSize) return false;
    value.resize(static_cast<size_t>(size));
    return read_all_from_fd(file.get(), value.data(), value.size());
}

struct MigrationTemp {
    std::string path;
    ~MigrationTemp() {
        if (!path.empty()) {
            std::error_code error;
            std::filesystem::remove(wallet_path(path), error);
        }
    }
};

bool path_present(const std::string& path) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(wallet_path(path), error);
    if (error == std::errc::no_such_file_or_directory) return false;
    return error || status.type() != std::filesystem::file_type::not_found;
}
} // namespace

WalletMigrationResult Wallet::migrate_file(const std::string& filename,
    const std::string& password, const WalletMigrationOptions& options) {
    WalletMigrationResult result;
    if (!options.confirmed) {
        result.error = WalletMigrationError::CONFIRMATION_REQUIRED;
        return result;
    }
    result.error = WalletMigrationError::INVALID_PATH;
    try {
        const auto source_path = absolute_wallet_path(filename);
        result.output_path = options.output_path.empty() ? source_path :
            absolute_wallet_path(options.output_path);
        std::error_code error;
        const bool in_place = source_path == result.output_path ||
            std::filesystem::equivalent(wallet_path(source_path), wallet_path(result.output_path), error);
        result.backup_path = options.backup_path.empty() ?
            (in_place ? source_path : result.output_path) + ".pre-v13.bak" : absolute_wallet_path(options.backup_path);
        // Reject aliases rather than writing through a hardlink or symlink path.
        if ((in_place && source_path != result.output_path) || source_path == result.backup_path ||
            result.output_path == result.backup_path) return result;
        if (path_present(result.backup_path) || (!in_place && path_present(result.output_path))) {
            result.error = WalletMigrationError::DESTINATION_EXISTS;
            return result;
        }

        bytes original, check;
        SensitiveBytesGuard original_guard{original}, check_guard{check};
        result.error = WalletMigrationError::LOAD_FAILED;
        if (!read_wallet_bytes(source_path, original)) { result.source.error = WalletLoadError::IO_ERROR; return result; }
        Wallet candidate;
        result.source = candidate.load_from_file_result(source_path, password);
        if (!result.source.ok()) return result;
        if (!result.source.migration_needed) { result.error = WalletMigrationError::NOT_NEEDED; return result; }
        // A caller-supplied password must not silently encrypt an originally plaintext wallet.
        if (!result.source.encrypted && !password.empty()) { result.error = WalletMigrationError::VALIDATION_FAILED; return result; }
        if (!read_wallet_bytes(source_path, check) || check != original) {
            result.error = WalletMigrationError::SOURCE_CHANGED; return result;
        }

        result.error = WalletMigrationError::BACKUP_FAILED;
        MigrationTemp backup;
        const int backup_fd = open_secure_temp_file(result.backup_path, backup.path);
        if (backup_fd < 0) return result;
        const bool written = write_all_to_fd(backup_fd, original.data(), original.size());
        const bool synced = sync_and_close_fd(backup_fd);
        if (!written || !synced || !read_wallet_bytes(backup.path, check) || check != original ||
            !publish_wallet_file_exclusive(backup.path, result.backup_path)) return result;
        if (!read_wallet_bytes(result.backup_path, check) || check != original ||
            !sync_parent_directory(result.backup_path)) return result;

        result.error = WalletMigrationError::WRITE_FAILED;
        MigrationTemp temporary;
        const int fd = open_secure_temp_file(result.output_path, temporary.path);
        if (fd < 0) return result;
        if (!sync_and_close_fd(fd)) return result;
        // Only this confirmed migration may serialize a legacy snapshot as v13.
        if (!candidate.save_to_file_locked(temporary.path, password, false, false, true)) return result;

        result.error = WalletMigrationError::VALIDATION_FAILED;
        Wallet verified;
        const auto reloaded = verified.load_from_file_result(temporary.path, password);
        if (!reloaded.ok() || reloaded.format != WalletFileFormat::NATIVE_V13 ||
            reloaded.encrypted != result.source.encrypted) return result;
        // Compare every persisted field, not just the primary address or key count.
        const auto same_location = [](const auto& a, const auto& b) {
            return std::tie(a.hash, a.height, a.index, a.time) == std::tie(b.hash, b.height, b.index, b.time);
        };
        if (candidate.primary_address_ != verified.primary_address_ ||
            candidate.has_password_hash_ != verified.has_password_hash_ ||
            candidate.password_hash_ != verified.password_hash_ || candidate.password_salt_ != verified.password_salt_ ||
            candidate.password_kdf_iterations_ != verified.password_kdf_iterations_ ||
            candidate.last_scanned_height_ != verified.last_scanned_height_ ||
            candidate.last_scanned_block_hash_ != verified.last_scanned_block_hash_ ||
            candidate.keys_.size() != verified.keys_.size() ||
            candidate.transactions_.size() != verified.transactions_.size() ||
            candidate.multisig_addresses_.size() != verified.multisig_addresses_.size()) return result;
        for (const auto& [address, a] : candidate.keys_) {
            const auto found = verified.keys_.find(address);
            if (found == verified.keys_.end()) return result;
            const auto& b = found->second;
            if (std::tie(a.private_key, a.address, a.label, a.created_at, a.used) !=
                std::tie(b.private_key, b.address, b.label, b.created_at, b.used)) return result;
        }
        for (size_t i = 0; i < candidate.transactions_.size(); ++i) {
            const auto& a = candidate.transactions_[i];
            const auto& b = verified.transactions_[i];
            if (a.tx.serialize() != b.tx.serialize() ||
                std::tie(a.tx_hash, a.block_height, a.timestamp, a.amount, a.fee, a.is_coinbase, a.memo,
                    a.block_hash, a.block_index, a.block_time, a.change_outputs, a.broadcast, a.received_at) !=
                std::tie(b.tx_hash, b.block_height, b.timestamp, b.amount, b.fee, b.is_coinbase, b.memo,
                    b.block_hash, b.block_index, b.block_time, b.change_outputs, b.broadcast, b.received_at) ||
                a.removed_blocks.size() != b.removed_blocks.size() || a.conflicts.size() != b.conflicts.size()) return result;
            for (size_t j = 0; j < a.removed_blocks.size(); ++j)
                if (!same_location(a.removed_blocks[j], b.removed_blocks[j])) return result;
            for (size_t j = 0; j < a.conflicts.size(); ++j)
                if (a.conflicts[j].first != b.conflicts[j].first ||
                    !same_location(a.conflicts[j].second, b.conflicts[j].second)) return result;
        }
        for (const auto& [address, a] : candidate.multisig_addresses_) {
            const auto found = verified.multisig_addresses_.find(address);
            if (found == verified.multisig_addresses_.end()) return result;
            const auto& b = found->second;
            if (a.address != b.address || a.m != b.m || a.n != b.n || a.addresses != b.addresses ||
                a.participants.size() != b.participants.size()) return result;
            for (size_t i = 0; i < a.participants.size(); ++i)
                if (a.participants[i].address != b.participants[i].address) return result;
        }

        if (!read_wallet_bytes(source_path, check) || check != original) {
            result.error = WalletMigrationError::SOURCE_CHANGED; return result;
        }
        // Recheck the published backup immediately before commit, including its bytes.
        if (!read_wallet_bytes(result.backup_path, check) || check != original) {
            result.error = WalletMigrationError::BACKUP_FAILED; return result;
        }
        result.error = WalletMigrationError::COMMIT_FAILED;
        const bool committed = in_place ? rename_file_durably(temporary.path, source_path, true, error) :
            publish_wallet_file_exclusive(temporary.path, result.output_path);
        if (!committed) return result;
        // Record publication before attempting directory durability.
        result.committed = true;
        result.error = WalletMigrationError::NONE;
        result.durable = sync_parent_directory(result.output_path);
        return result;
    } catch (...) {
        // The stage-specific error identifies the last attempted operation.
        return result;
    }
}

} // namespace geocoin::wallet
