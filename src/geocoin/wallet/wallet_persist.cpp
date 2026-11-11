#include "geocoin/wallet/wallet_persist_internal.hpp"
#include "geocoin/crypto/encryption.hpp"
#include "geocoin/utils/logger.hpp"

#include <filesystem>
#include <limits>
#include <openssl/crypto.h>
#include <utility>

namespace geocoin::wallet {

std::string Wallet::get_file_path() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return file_path_.empty() ? get_default_wallet_path() : file_path_;
}

bool Wallet::save(const std::string& password) const {
    return save_to_file(get_file_path(), password);
}

Wallet::SaveStatus Wallet::last_save_status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_save_status_;
}

bool Wallet::save_to_file(const std::string& filename, const std::string& password) const {
    return save_to_file(filename, password, true);
}

bool Wallet::save_to_file(
    const std::string& filename,
    const std::string& password,
    bool preserve_backup
) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return save_to_file_locked(filename, password, preserve_backup, false);
}

bool Wallet::change_password(
    const std::string& filename,
    const std::string& old_password,
    const std::string& new_password
) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (new_password.empty() || !has_password_hash_ ||
        !verify_password_material(
            old_password,
            has_password_hash_,
            password_hash_,
            password_salt_,
            password_kdf_iterations_)) {
        return false;
    }
    return save_to_file_locked(filename, new_password, false, true);
}

bool Wallet::save_to_file_locked(
    const std::string& filename,
    const std::string& password,
    bool preserve_backup,
    bool replace_password_verifier
) const {
    last_save_status_ = SaveStatus::FAILED;
    const std::string selected_path = std::filesystem::absolute(filename).lexically_normal().string();

    // БЕЗОПАСНОСТЬ: не позволяем тихо пересохранить зашифрованный кошелек без пароля
    // (иначе файл станет plaintext и приватные ключи утекут на диск).
    if (has_password_hash_ && password.empty()) {
        GEO_LOG_ERROR("wallet", "Refusing to save password-protected wallet without password");
        return false;
    }

    // Build the verifier state off to the side. It becomes visible in memory
    // only after the corresponding wallet file has been committed.
    bool effective_has_password_hash = has_password_hash_;
    Hash256 effective_password_hash = password_hash_;
    SensitiveHashGuard effective_hash_guard{effective_password_hash};
    bytes effective_password_salt = password_salt_;
    SensitiveBytesGuard effective_salt_guard{effective_password_salt};
    uint32_t effective_password_kdf_iterations = password_kdf_iterations_;

    if (replace_password_verifier) {
        bytes salt;
        SensitiveBytesGuard salt_guard{salt};
        uint32_t iters = 0;
        Hash256 verifier{};
        SensitiveHashGuard verifier_guard{verifier};
        if (password.empty() ||
            !init_password_verifier_from_password(password, salt, iters, verifier)) {
            return false;
        }
        effective_password_hash = verifier;
        effective_password_salt = std::move(salt);
        effective_password_kdf_iterations = iters;
        effective_has_password_hash = true;
    }
    // Инициализируем/проверяем verifier пароля, если пароль задан.
    else if (!password.empty() && has_password_hash_) {
        if (!verify_password_material(
                password,
                has_password_hash_,
                password_hash_,
                password_salt_,
                password_kdf_iterations_)) {
            GEO_LOG_ERROR("wallet", "Refusing to save wallet: incorrect password");
            return false;
        }

    }

    if (!replace_password_verifier && !password.empty() && !has_password_hash_) {
        bytes salt;
        SensitiveBytesGuard salt_guard{salt};
        uint32_t iters = 0;
        Hash256 verifier{};
        SensitiveHashGuard verifier_guard{verifier};
        if (!init_password_verifier_from_password(password, salt, iters, verifier)) {
            return false;
        }
        effective_password_hash = verifier;
        effective_password_salt = std::move(salt);
        effective_password_kdf_iterations = iters;
        effective_has_password_hash = true;
    }

    bytes buffer;
    SensitiveBytesGuard buffer_guard{buffer};

    // Verifier пароля:
    // 0 = no verifier, 2 = the current PBKDF2 verifier (hash + salt + iterations).
    uint8_t password_mode = kPasswordModeNone;
    if (effective_has_password_hash) {
        if (effective_password_salt.size() != kPasswordVerifierSaltSize ||
            effective_password_kdf_iterations != kPasswordVerifierIterations) return false;
        password_mode = kPasswordModePbkdf2;
    }

    buffer.push_back(password_mode);
    if (password_mode == kPasswordModePbkdf2) {
        buffer.insert(buffer.end(), effective_password_hash.begin(), effective_password_hash.end());
    }
    if (password_mode == kPasswordModePbkdf2) {
        buffer.insert(buffer.end(), effective_password_salt.begin(), effective_password_salt.end());
        append_u32_le(buffer, effective_password_kdf_iterations);
    }

    // Количество ключей
    uint32_t key_count = keys_.size();
    append_u32_le(buffer, key_count);

    // Все ключи
    for (const auto& [address, info] : keys_) {
        // Приватный ключ (32 байта)
        buffer.insert(buffer.end(), info.private_key.begin(), info.private_key.end());

        // Метка (длина + данные)
        uint32_t label_len = info.label.size();
        append_u32_le(buffer, label_len);
        buffer.insert(buffer.end(), info.label.begin(), info.label.end());

        // Время создания (8 байт)
        append_u64_le(buffer, info.created_at);
        buffer.push_back(info.used ? 1 : 0);
    }

    // Количество транзакций
    uint32_t tx_count = transactions_.size();
    append_u32_le(buffer, tx_count);

    // Все транзакции
    for (const auto& wtx : transactions_) {
        if (wtx.tx_hash != wtx.tx.get_hash() || wtx.is_coinbase != wtx.tx.is_coinbase()) {
            GEO_LOG_ERROR("wallet", "Cannot save inconsistent transaction history");
            return false;
        }
        // Хеш транзакции (32 байта)
        buffer.insert(buffer.end(), wtx.tx_hash.begin(), wtx.tx_hash.end());

        // Высота блока (8 байт)
        append_u64_le(buffer, wtx.block_height);

        // Метка времени (8 байт)
        append_u64_le(buffer, wtx.timestamp);

        // Сумма (8 байт)
        append_i64_le(buffer, wtx.amount);

        // Комиссия (8 байт)
        append_u64_le(buffer, wtx.fee);

        // Флаг коинбейса (1 байт)
        uint8_t is_coinbase = wtx.is_coinbase ? 1 : 0;
        buffer.push_back(is_coinbase);

        // Заметка (длина + данные)
        uint32_t memo_len = wtx.memo.size();
        append_u32_le(buffer, memo_len);
        buffer.insert(buffer.end(), wtx.memo.begin(), wtx.memo.end());

        // Сериализованная транзакция
        bytes tx_data = wtx.tx.serialize();
        uint32_t tx_data_len = tx_data.size();
        append_u32_le(buffer, tx_data_len);
        buffer.insert(buffer.end(), tx_data.begin(), tx_data.end());
    }

    // Последняя просканированная высота (8 байт)
    append_u64_le(buffer, last_scanned_height_);
    // Хеш активного блока на этой высоте. Высота без хеша не
    // позволяет заметить офлайн-реорганизацию той же длины.
    buffer.insert(
        buffer.end(),
        last_scanned_block_hash_.begin(),
        last_scanned_block_hash_.end()
    );

    // Мультиподпись адреса: полная конфигурация
    uint32_t multisig_count = multisig_addresses_.size();
    append_u32_le(buffer, multisig_count);

    for (const auto& [address, config] : multisig_addresses_) {
        // Адрес (длина + данные)
        uint32_t addr_len = address.size();
        append_u32_le(buffer, addr_len);
        buffer.insert(buffer.end(), address.begin(), address.end());

        // M и N (по 1 байту)
        buffer.push_back(config.m);
        buffer.push_back(config.n);

        // Количество участников: приоритет новой структуры participants,
        // запасной вариант: список адресов.
        uint32_t participant_count = !config.participants.empty()
            ? static_cast<uint32_t>(config.participants.size())
            : static_cast<uint32_t>(config.addresses.size());
        append_u32_le(buffer, participant_count);

        // Участники (только адрес).
        if (!config.participants.empty()) {
            for (const auto& participant : config.participants) {
                std::string addr_str = participant.address.str();
                uint32_t participant_addr_len = addr_str.size();
                append_u32_le(buffer, participant_addr_len);
                buffer.insert(buffer.end(), addr_str.begin(), addr_str.end());
            }
        } else {
            for (const auto& addr : config.addresses) {
                std::string addr_str = addr.str();
                uint32_t participant_addr_len = addr_str.size();
                append_u32_le(buffer, participant_addr_len);
                buffer.insert(buffer.end(), addr_str.begin(), addr_str.end());
            }
        }
    }

    const auto primary = resolve_primary_address(keys_, primary_address_).str();
    append_u32_le(buffer, static_cast<uint32_t>(primary.size()));
    buffer.insert(buffer.end(), primary.begin(), primary.end());

    // Шифруем только если есть пароль
    if (buffer.size() > kMaxWalletPayloadSize) {
        GEO_LOG_ERROR("wallet", "Wallet payload exceeds the maximum supported size");
        return false;
    }

    bytes encrypted;
    SensitiveBytesGuard encrypted_guard{encrypted};
    if (!password.empty()) {
        if (!crypto::encrypt_aes256(buffer, password, encrypted, crypto::kPbkdf2Iterations)) {
            return false;
        }
    } else {
        encrypted = buffer;  // Без шифрования
    }

    if (encrypted.size() > kMaxWalletPayloadSize || encrypted.size() > std::numeric_limits<uint32_t>::max()) {
        GEO_LOG_ERROR("wallet", "Encrypted wallet payload exceeds the maximum supported size");
        return false;
    }

    // Сохраняем атомарно: сначала уникальный private temp, затем переименование (+ .bak).
    const std::string bak_filename = filename + ".bak";

    uint32_t magic = 0x47454F43;  // "GEOC"
    uint32_t version = kWalletVersion;
    uint8_t encrypted_flag = password.empty() ? 0 : 1;
    uint32_t data_size = static_cast<uint32_t>(encrypted.size());

    bytes file_body;
    SensitiveBytesGuard file_body_guard{file_body};
    file_body.reserve(sizeof(magic) + sizeof(version) + sizeof(encrypted_flag) + sizeof(data_size) + encrypted.size());
    append_u32_le(file_body, magic);
    append_u32_le(file_body, version);
    file_body.push_back(encrypted_flag);
    append_u32_le(file_body, data_size);
    file_body.insert(file_body.end(), encrypted.begin(), encrypted.end());

    bytes hmac;
    SensitiveBytesGuard hmac_guard{hmac};
    const bool need_hmac = (encrypted_flag != 0);
    if (need_hmac) {
        if (!compute_wallet_hmac(password, encrypted, crypto::kPbkdf2Iterations, file_body, hmac)) {
            GEO_LOG_ERROR("wallet", "Failed to compute wallet HMAC");
            return false;
        }
        if (hmac.size() != kWalletHmacSize) {
            GEO_LOG_ERROR("wallet", "Invalid wallet HMAC size");
            return false;
        }
    }

    std::string tmp_filename;
    const int temp_fd = open_secure_temp_file(filename, tmp_filename);
    if (temp_fd < 0) {
        GEO_LOG_ERROR("wallet", "Failed to securely create a temporary wallet file");
        return false;
    }

    const bool body_written = write_all_to_fd(temp_fd, file_body.data(), file_body.size());
    const bool hmac_written = !need_hmac || write_all_to_fd(temp_fd, hmac.data(), hmac.size());
    if (!body_written || !hmac_written) {
        GEO_LOG_ERROR("wallet", "Failed to write the temporary wallet file");
        close_fd_without_sync(temp_fd);
        std::error_code remove_ec;
        std::filesystem::remove(tmp_filename, remove_ec);
        return false;
    }
    if (!sync_and_close_fd(temp_fd)) {
        GEO_LOG_ERROR("wallet", "Failed to durably flush the temporary wallet file");
        std::error_code remove_ec;
        std::filesystem::remove(tmp_filename, remove_ec);
        return false;
    }
    // Rename preserves this file's owner-only mode/DACL. Secure the temporary
    // inode before it can become the live wallet path so a post-commit chmod
    // failure cannot leave verifier state and on-disk state out of sync.
    if (!set_private_file_permissions(tmp_filename)) {
        GEO_LOG_ERROR("wallet", "Failed to secure the temporary wallet file");
        std::error_code remove_ec;
        std::filesystem::remove(tmp_filename, remove_ec);
        return false;
    }

    std::error_code ec;
    bool previous_wallet_was_plaintext = false;
    bool previous_encryption_flag_known = false;
    bool keep_previous_backup = false;
    const bool had_previous_wallet = std::filesystem::exists(filename, ec);
    if (ec) {
        std::filesystem::remove(tmp_filename, ec);
        return false;
    }
    if (had_previous_wallet) {
        if (!is_regular_private_file(filename) || !set_private_file_permissions(filename)) {
            GEO_LOG_ERROR("wallet", "Refusing to replace a wallet that is not a private, owner-controlled regular file");
            std::filesystem::remove(tmp_filename, ec);
            return false;
        }

        bool previous_encrypted = false;
        previous_encryption_flag_known = read_wallet_encryption_flag(filename, previous_encrypted);
        previous_wallet_was_plaintext = previous_encryption_flag_known && !previous_encrypted;

        // Never retain material that could be opened with an old/empty
        // password. If the old file's encryption state cannot be proven, fail
        // closed and replace it without creating another copy.
        const bool backup_is_safe = !need_hmac ||
            (previous_encryption_flag_known && !previous_wallet_was_plaintext);
        keep_previous_backup = preserve_backup && !replace_password_verifier && backup_is_safe;

        std::filesystem::remove(bak_filename, ec);
        if (ec) {
            GEO_LOG_ERROR("wallet", "Failed to remove the previous wallet backup: " << ec.message());
            std::filesystem::remove(tmp_filename, ec);
            return false;
        }
        if (keep_previous_backup) {
            if (!rename_file_durably(filename, bak_filename, false, ec)) {
                GEO_LOG_ERROR("wallet", "Failed to move wallet to backup: " << ec.message());
                std::filesystem::remove(tmp_filename, ec);
                return false;
            }
            if (!set_private_file_permissions(bak_filename)) {
                GEO_LOG_ERROR("wallet", "Failed to secure wallet backup permissions");
                std::error_code restore_ec;
                (void)rename_file_durably(bak_filename, filename, false, restore_ec);
                std::filesystem::remove(tmp_filename, ec);
                return false;
            }
        }
    }

    const bool replace_live_wallet = had_previous_wallet && !keep_previous_backup;
    if (!rename_file_durably(tmp_filename, filename, replace_live_wallet, ec)) {
        GEO_LOG_ERROR("wallet", "Failed to finalize wallet save: " << ec.message());
        // Пытаемся восстановить backup, если он есть.
        if (keep_previous_backup) {
            std::error_code exists_ec;
            if (std::filesystem::exists(bak_filename, exists_ec) && !exists_ec) {
                std::error_code restore_ec;
                (void)rename_file_durably(bak_filename, filename, false, restore_ec);
            }
        }
        std::error_code remove_ec;
        std::filesystem::remove(tmp_filename, remove_ec);
        return false;
    }

    const bool durable = sync_parent_directory(filename);
    if (!durable) {
        GEO_LOG_ERROR("wallet", "Wallet was committed but the directory fsync failed");
    }

    auto* self = const_cast<Wallet*>(this);
    OPENSSL_cleanse(self->password_hash_.data(), self->password_hash_.size());
    if (!self->password_salt_.empty()) {
        OPENSSL_cleanse(self->password_salt_.data(), self->password_salt_.size());
    }
    self->has_password_hash_ = effective_has_password_hash;
    self->password_hash_ = effective_password_hash;
    self->password_salt_ = std::move(effective_password_salt);
    self->password_kdf_iterations_ = effective_password_kdf_iterations;
    // Exporting a copy must not redirect subsequent saves of the open wallet.
    if (file_path_.empty()) file_path_ = selected_path;
    last_save_status_ = durable ? SaveStatus::DURABLE : SaveStatus::COMMITTED_NOT_DURABLE;

    return durable;
}

} // namespace geocoin::wallet
