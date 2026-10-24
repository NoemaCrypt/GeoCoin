#include "geocoin/wallet/wallet_persist_internal.hpp"
#include "geocoin/crypto/encryption.hpp"
#include "geocoin/utils/logger.hpp"

#include <array>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include "geocoin/crypto/ioke_sha256.h"
#include <openssl/crypto.h>

namespace geocoin::wallet {

bool Wallet::load_from_file(const std::string& filename, const std::string& password) {
    std::lock_guard<std::recursive_mutex> scan_lock(scan_mutex_);
    std::lock_guard<std::mutex> lock(mutex_);

    auto try_load = [&](const std::string& path) -> bool {
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

        std::array<byte, kWalletHeaderSize> header{};
        if (actual_file_size < header.size() ||
            !read_all_from_fd(file.get(), header.data(), header.size())) {
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
            GEO_LOG_ERROR("wallet", "Invalid wallet magic: 0x" << std::hex << magic << std::dec);
            return false;
        }

        if (SyncDebugEnabled()) {
            GEO_LOG_DEBUG("wallet", "Wallet version: " << version);
        }
        if (version != kWalletVersion) {
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
            if (password.empty()) {
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
                GEO_LOG_ERROR("wallet", "Wallet HMAC verification failed");
                return false;
            }
        }

        bytes buffer;
        SensitiveBytesGuard buffer_guard{buffer};
        if (encrypted_flag) {
            if (!crypto::decrypt_aes256(encrypted, password, buffer, pbkdf2_iters)) {
                GEO_LOG_ERROR("wallet", "Wallet decryption failed");
                return false;
            }
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
        if (password_mode == kPasswordModeNone) {
            loaded.has_password_hash = false;
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

        uint32_t key_count = 0;
        if (!read_u32(key_count)) {
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
            if (!require(1) || buffer[offset] > 1) return false;
            info.used = buffer[offset++] == 1;
            const auto address = info.address;
            if (!loaded.keys.emplace(address, std::move(info)).second) return false;
        }

        uint32_t tx_count = 0;
        if (!read_u32(tx_count)) {
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
        {
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
        if (!read_u32(multisig_count)) {
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
            if (!read_u32(participant_count)) return false;

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

        uint32_t primary_length = 0;
        if (!read_u32(primary_length) || !require(primary_length)) return false;
        loaded.primary_address = Address(std::string(buffer.begin() + offset, buffer.begin() + offset + primary_length));
        offset += primary_length;
        if (loaded.keys.empty() ? !loaded.primary_address.empty() : !loaded.keys.contains(loaded.primary_address)) return false;
        if (offset != buffer.size()) {
            GEO_LOG_ERROR("wallet", "Wallet payload size mismatch: unread bytes remain");
            return false;
        }

        has_password_hash_ = loaded.has_password_hash;
        std::swap(password_hash_, loaded.password_hash);
        password_salt_.swap(loaded.password_salt);
        password_kdf_iterations_ = loaded.password_kdf_iterations;
        keys_.swap(loaded.keys);
        transactions_.swap(loaded.transactions);
        tx_index_.swap(loaded.tx_index);
        multisig_addresses_.swap(loaded.multisig_addresses);
        last_scanned_height_ = loaded.last_scanned_height;
        last_scanned_block_hash_ = loaded.last_scanned_block_hash;
        last_scan_result_ = {};
        primary_address_ = loaded.primary_address;

        return true;
    };

    auto safe_try_load = [&](const std::string& path) -> bool {
        try {
            return try_load(path);
        } catch (const std::bad_alloc&) {
            GEO_LOG_ERROR("wallet", "Wallet load aborted due to an allocation failure: " << path);
            return false;
        } catch (const std::length_error&) {
            GEO_LOG_ERROR("wallet", "Wallet load rejected an oversized field: " << path);
            return false;
        } catch (const std::exception& error) {
            GEO_LOG_ERROR("wallet", "Wallet load rejected malformed data: " << path << ": " << error.what());
            return false;
        } catch (...) {
            GEO_LOG_ERROR("wallet", "Wallet load rejected malformed data: " << path);
            return false;
        }
    };

    if (safe_try_load(filename)) {
        file_path_ = std::filesystem::absolute(filename).lexically_normal().string();
        return true;
    }

    const std::string bak_filename = filename + ".bak";
    std::error_code ec;
    if (std::filesystem::exists(bak_filename, ec)) {
        GEO_LOG_ERROR("wallet", "Primary wallet load failed, trying backup: " << bak_filename);
        if (safe_try_load(bak_filename)) {
            file_path_ = std::filesystem::absolute(filename).lexically_normal().string();
            return true;
        }
    }

    return false;
}

} // namespace geocoin::wallet
