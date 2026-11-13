#include "geocoin/wallet/wallet_persist_internal.hpp"
#include "geocoin/crypto/encryption.hpp"
#include "geocoin/crypto/addresses.hpp"
#include "geocoin/crypto/geo_area.hpp"
#include "geocoin/utils/bytes.hpp"
#include "geocoin/utils/secure_memory.hpp"
#include "nlohmann_json.hpp"

#include <filesystem>
#include <stdexcept>
#include <openssl/rand.h>

namespace geocoin::wallet {
namespace {
using json = nlohmann::json;

uint64_t unix_time() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

bytes decode(const json& value) {
    bytes result;
    if (!value.is_string() || !utils::hex_to_bytes_checked(value.get_ref<const std::string&>(), result))
        throw std::runtime_error("Invalid wallet hexadecimal field");
    return result;
}

Hash256 hash(const json& value) {
    Hash256 result{};
    if (!value.is_string() || !utils::hex_to_hash256_checked(value.get_ref<const std::string&>(), result))
        throw std::runtime_error("Invalid wallet hash");
    return result;
}

// The outer document contains public wallet metadata and ciphertext only.
// The checksum detects corruption; private keys are authenticated with AES-GCM.
bool commit_document(const std::string& filename, const bytes& body, bool backup, bool replace) {
    std::string temporary;
    const auto fd = open_secure_temp_file(filename, temporary);
    if (fd < 0) return false;
    const bool written = write_all_to_fd(fd, body.data(), body.size());
    const bool synced = written ? sync_and_close_fd(fd) : (close_fd_without_sync(fd), false);
    std::error_code ec;
    if (!synced) { std::filesystem::remove(temporary, ec); return false; }
    if (std::filesystem::exists(filename, ec)) {
        if (!is_regular_private_file(filename)) { std::filesystem::remove(temporary, ec); return false; }
        if (backup) {
            const std::string previous = filename + ".bak";
            std::filesystem::remove(previous, ec);
            if (ec || !rename_file_durably(filename, previous, false, ec)) {
                std::filesystem::remove(temporary, ec); return false;
            }
        }
    }
    if (!rename_file_durably(temporary, filename, replace, ec)) {
        if (backup && !std::filesystem::exists(filename)) {
            std::error_code restore;
            (void)rename_file_durably(filename + ".bak", filename, false, restore);
        }
        std::filesystem::remove(temporary, ec);
        return false;
    }
    return true;
}
} // namespace

bool Wallet::vault_locked_locked() const {
    return vault_mode_ && (vault_master_key_.empty() ||
        std::chrono::steady_clock::now() >= unlock_deadline_);
}

bool Wallet::is_vault() const { std::lock_guard<std::mutex> guard(mutex_); return vault_mode_; }
bool Wallet::is_locked() const { std::lock_guard<std::mutex> guard(mutex_); return vault_locked_locked(); }
uint64_t Wallet::unlocked_until() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return vault_locked_locked() ? 0 : unlock_until_;
}

bool Wallet::verify_vault_password_locked(const std::string& password) const {
    bytes master;
    SensitiveBytesGuard guard{master};
    return !password.empty() && crypto::decrypt_aes256(wrapped_master_key_, password, master) && master.size() == 32;
}

bool Wallet::refresh_vault_keys_locked() {
    if (!vault_mode_ || !vault_keys_dirty_) return true;
    if (vault_master_key_.empty()) return false;
    bytes material;
    SensitiveBytesGuard guard{material};
    append_u32_le(material, static_cast<uint32_t>(keys_.size()));
    for (const auto& [address, info] : keys_) {
        if (info.private_key.size() != 32) return false;
        append_u32_le(material, static_cast<uint32_t>(address.str().size()));
        material.insert(material.end(), address.str().begin(), address.str().end());
        material.insert(material.end(), info.private_key.begin(), info.private_key.end());
    }
    std::string secret(vault_master_key_.begin(), vault_master_key_.end());
    utils::ScopedContainerCleanser secret_guard(secret);
    bytes replacement;
    // One iteration is appropriate here: the secret is a random 256-bit master
    // key. The user password wrapper uses the standard 600,000-iteration KDF.
    if (!crypto::encrypt_aes256(material, secret, replacement, 1)) return false;
    encrypted_keys_.swap(replacement);
    vault_keys_dirty_ = false;
    return true;
}

void Wallet::lock_keys_locked() {
    if (vault_mode_ && !refresh_vault_keys_locked()) throw std::runtime_error("Cannot seal wallet keys");
    for (auto& [address, info] : keys_) {
        (void)address;
        if (!info.private_key.empty()) OPENSSL_cleanse(info.private_key.data(), info.private_key.size());
        info.private_key.clear();
    }
    if (!vault_master_key_.empty()) OPENSSL_cleanse(vault_master_key_.data(), vault_master_key_.size());
    vault_master_key_.clear();
    unlock_until_ = 0;
    unlock_deadline_ = {};
}

void Wallet::lock() {
    {
        std::lock_guard<std::mutex> guard(mutex_);
        if (vault_mode_) lock_keys_locked();
        vault_cv_.notify_all();
    }
    NotifyStatusChanged(this);
}

bool Wallet::unlock(const std::string& password, uint32_t timeout_seconds) {
    if (timeout_seconds == 0 || timeout_seconds > 86400) return false;
    std::unique_lock<std::mutex> guard(mutex_);
    if (!vault_mode_) return false;
    bytes master, material;
    SensitiveBytesGuard master_guard{master}, material_guard{material};
    if (!crypto::decrypt_aes256(wrapped_master_key_, password, master) || master.size() != 32) return false;
    std::string secret(master.begin(), master.end());
    utils::ScopedContainerCleanser secret_guard(secret);
    if (!crypto::decrypt_aes256(encrypted_keys_, secret, material, 1)) return false;
    size_t offset = 0;
    auto u32 = [&]() {
        if (material.size() - offset < 4) throw std::runtime_error("Truncated key vault");
        uint32_t value = 0;
        for (unsigned i = 0; i < 4; ++i) value |= uint32_t(material[offset++]) << (8 * i);
        return value;
    };
    std::map<Address, secure_bytes> restored;
    try {
        const auto count = u32();
        if (count != keys_.size()) return false;
        for (uint32_t i = 0; i < count; ++i) {
            const auto length = u32();
            if (length > material.size() - offset || material.size() - offset - length < 32) return false;
            Address address(std::string(material.begin() + offset, material.begin() + offset + length));
            offset += length;
            bytes key(material.begin() + offset, material.begin() + offset + 32);
            SensitiveBytesGuard key_guard{key};
            offset += 32;
            // AES-GCM authenticates the address/key bindings sealed at insertion.
            // Avoid regenerating every Falcon public key on each unlock.
            if (!keys_.contains(address) ||
                !restored.emplace(address, secure_bytes(key.begin(), key.end())).second) return false;
        }
        if (offset != material.size()) return false;
    } catch (...) { return false; }
    for (auto& [address, key] : restored) keys_.at(address).private_key = std::move(key);
    vault_keys_dirty_ = false;
    vault_master_key_.assign(master.begin(), master.end());
    unlock_deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
    unlock_until_ = unix_time() + timeout_seconds;
    if (!vault_timer_.joinable()) {
        try {
            vault_timer_ = std::thread([this] {
                std::unique_lock<std::mutex> timer_guard(mutex_);
                while (!vault_timer_stop_) {
                    if (vault_master_key_.empty()) { vault_cv_.wait(timer_guard); continue; }
                    const auto deadline = unlock_deadline_;
                    if (vault_cv_.wait_until(timer_guard, deadline) == std::cv_status::timeout &&
                        vault_locked_locked()) {
                        try { lock_keys_locked(); } catch (...) {
                            // Accessors enforce the expired deadline even if sealing fails.
                            vault_cv_.wait_for(timer_guard, std::chrono::seconds(1));
                        }
                    }
                }
            });
        } catch (...) { lock_keys_locked(); throw; }
    }
    vault_cv_.notify_all();
    guard.unlock();
    NotifyStatusChanged(this);
    return true;
}

bool Wallet::enable_vault(const std::string& password) {
    std::lock_guard<std::mutex> guard(mutex_);
    if (vault_mode_ || password.empty() || keys_.empty() ||
        (has_password_hash_ && !verify_password_material(password, has_password_hash_, password_hash_, password_salt_, password_kdf_iterations_)))
        return false;
    bytes master(32);
    SensitiveBytesGuard master_guard{master};
    if (RAND_bytes(master.data(), master.size()) != 1 || !crypto::encrypt_aes256(master, password, wrapped_master_key_)) return false;
    vault_master_key_.assign(master.begin(), master.end());
    vault_mode_ = true;
    vault_keys_dirty_ = true;
    ++wallet_revision_;
    unlock_deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    if (!refresh_vault_keys_locked() || !save_vault_locked(file_path_.empty() ? get_default_wallet_path() : file_path_, false)) {
        if (last_save_status_ == SaveStatus::FAILED) {
            vault_mode_ = false; wrapped_master_key_.clear(); encrypted_keys_.clear(); vault_master_key_.clear();
        } else { has_password_hash_ = true; lock_keys_locked(); }
        return false;
    }
    has_password_hash_ = true;
    OPENSSL_cleanse(password_hash_.data(), password_hash_.size());
    utils::cleanse_container(password_salt_); password_kdf_iterations_ = 0;
    lock_keys_locked();
    std::error_code backup_error;
    std::filesystem::remove((file_path_.empty() ? get_default_wallet_path() : file_path_) + ".bak", backup_error);
    if (backup_error || !sync_parent_directory(file_path_)) { last_save_status_ = SaveStatus::COMMITTED_NOT_DURABLE; return false; }
    return true;
}

bool Wallet::save_vault_locked(const std::string& filename, bool preserve_backup) const {
    auto* self = const_cast<Wallet*>(this);
    last_save_status_ = SaveStatus::FAILED;
    const bool live = file_path_.empty() || std::filesystem::absolute(filename).lexically_normal() == std::filesystem::path(file_path_);
    if (live && vault_saved_revision_ == wallet_revision_ && is_regular_private_file(filename)) {
        last_save_status_ = SaveStatus::DURABLE; return true;
    }
    if (!self->refresh_vault_keys_locked()) return false;
    json document{{"format", 1}, {"primary", primary_address_.str()},
        {"wrapped_master", utils::bytes_to_hex(wrapped_master_key_)}, {"key_vault", utils::bytes_to_hex(encrypted_keys_)},
        {"scanned_height", last_scanned_height_}, {"scanned_hash", utils::hash256_to_hex(last_scanned_block_hash_)},
        {"keys", json::array()}, {"transactions", json::array()}, {"multisig", json::array()}};
    for (const auto& [address, info] : keys_) document["keys"].push_back({{"address", address.str()},
        {"label", info.label}, {"created", info.created_at}, {"used", info.used}});
    for (const auto& tx : transactions_) {
        json removed = json::array();
        json conflicts = json::array();
        for (const auto& [other, location] : tx.conflicts) conflicts.push_back({{"txid", utils::hash256_to_hex(other)},
            {"hash", utils::hash256_to_hex(location.hash)}, {"height", location.height}, {"index", location.index}, {"time", location.time}});
        for (const auto& location : tx.removed_blocks) removed.push_back({{"hash", utils::hash256_to_hex(location.hash)},
            {"height", location.height}, {"index", location.index}, {"time", location.time}});
        document["transactions"].push_back({{"hex", utils::bytes_to_hex(tx.tx.serialize())}, {"height", tx.block_height},
            {"time", tx.timestamp}, {"amount", tx.amount}, {"fee", tx.fee}, {"memo", tx.memo},
            {"blockhash", utils::hash256_to_hex(tx.block_hash)}, {"blockindex", tx.block_index},
            {"blocktime", tx.block_time}, {"removed", std::move(removed)}, {"change", tx.change_outputs}, {"broadcast", tx.broadcast},
            {"conflicts", std::move(conflicts)}, {"received", tx.received_at}});
    }
    for (const auto& [address, config] : multisig_addresses_) {
        json participants = json::array();
        for (const auto& participant : config.addresses) participants.push_back(participant.str());
        if (participants.empty()) for (const auto& participant : config.participants) participants.push_back(participant.address.str());
        document["multisig"].push_back({{"address", address}, {"m", config.m}, {"n", config.n}, {"participants", participants}});
    }
    const auto serialized = document.dump();
    if (serialized.size() > kMaxWalletPayloadSize) return false;
    bytes file{'G', 'E', 'O', 'V'};
    append_u32_le(file, static_cast<uint32_t>(serialized.size()));
    file.insert(file.end(), serialized.begin(), serialized.end());
    const auto checksum = crypto::sha256(file);
    file.insert(file.end(), checksum.begin(), checksum.end());
    if (live && checksum == vault_saved_checksum_ && is_regular_private_file(filename)) {
        vault_saved_revision_ = wallet_revision_;
        last_save_status_ = SaveStatus::DURABLE; return true;
    }
    if (!commit_document(filename, file, preserve_backup, live)) return false;
    const bool durable = sync_parent_directory(filename);
    if (file_path_.empty()) file_path_ = std::filesystem::absolute(filename).lexically_normal().string();
    last_save_status_ = durable ? SaveStatus::DURABLE : SaveStatus::COMMITTED_NOT_DURABLE;
    if (durable && live) { vault_saved_checksum_ = checksum; vault_saved_revision_ = wallet_revision_; }
    return durable;
}

bool Wallet::load_vault_locked(const std::string& filename, const std::string& password) {
    try {
        uint64_t size = 0;
        ScopedReadFd fd(open_secure_input_file(filename, size));
        if (fd.get() < 0 || size < 40 || size > kMaxWalletPayloadSize + 40) return false;
        bytes file(size);
        if (!read_all_from_fd(fd.get(), file.data(), file.size())) return false;
        uint32_t length = 0;
        for (unsigned i = 0; i < 4; ++i) length |= uint32_t(file[4 + i]) << (8 * i);
        if (uint64_t(length) + 40 != size) return false;
        bytes body(file.begin(), file.end() - 32);
        const auto checksum = crypto::sha256(body);
        if (CRYPTO_memcmp(checksum.data(), file.data() + size - 32, 32) != 0) return false;
        const auto document = json::parse(file.begin() + 8, file.end() - 32,
            [](int depth, json::parse_event_t, json&) {
                if (depth > 64) throw std::runtime_error("Wallet metadata nesting limit");
                return true;
            });
        if (document.at("format") != 1) return false;
        WalletLoadScratch loaded;
        for (const auto& key : document.at("keys")) {
            KeyInfo info;
            info.address = Address(key.at("address").get<std::string>());
            if (!crypto::verify_address(info.address)) return false;
            info.label = key.at("label").get<std::string>();
            info.created_at = key.at("created").get<uint64_t>(); info.used = key.at("used").get<bool>();
            if (!loaded.keys.emplace(info.address, std::move(info)).second) return false;
        }
        loaded.primary_address = Address(document.at("primary").get<std::string>());
        if (!loaded.keys.contains(loaded.primary_address)) return false;
        for (const auto& entry : document.at("transactions")) {
            WalletTransaction tx;
            tx.tx = core::Transaction::deserialize_canonical(decode(entry.at("hex")));
            tx.tx_hash = tx.tx.get_hash(); tx.is_coinbase = tx.tx.is_coinbase();
            tx.block_height = entry.at("height").get<Height>(); tx.timestamp = entry.at("time").get<uint64_t>();
            tx.amount = entry.at("amount").get<Amount>(); tx.fee = entry.at("fee").get<uint64_t>();
            tx.memo = entry.at("memo").get<std::string>(); tx.block_hash = hash(entry.at("blockhash"));
            tx.block_index = entry.at("blockindex").get<uint32_t>(); tx.block_time = entry.at("blocktime").get<Timestamp>();
            tx.change_outputs = entry.at("change").get<std::vector<uint32_t>>();
            tx.broadcast = entry.at("broadcast").get<bool>();
            tx.received_at = entry.at("received").get<uint64_t>();
            for (const auto& conflict : entry.at("conflicts")) tx.note_conflict(hash(conflict.at("txid")),
                {hash(conflict.at("hash")), conflict.at("height").get<Height>(), conflict.at("index").get<uint32_t>(), conflict.at("time").get<Timestamp>()});
            for (auto index : tx.change_outputs) if (index >= tx.tx.outputs.size()) return false;
            for (const auto& location : entry.at("removed")) tx.removed_blocks.push_back({hash(location.at("hash")),
                location.at("height").get<Height>(), location.at("index").get<uint32_t>(), location.at("time").get<Timestamp>()});
            if (!loaded.tx_index.emplace(tx.tx_hash, loaded.transactions.size()).second) return false;
            loaded.transactions.push_back(std::move(tx));
        }
        for (const auto& entry : document.at("multisig")) {
            MultisigConfig config;
            config.address = Address(entry.at("address").get<std::string>());
            config.m = entry.at("m").get<uint8_t>(); config.n = entry.at("n").get<uint8_t>();
            for (const auto& participant : entry.at("participants")) {
                Address address(participant.get<std::string>());
                config.addresses.push_back(address); config.participants.emplace_back(address);
            }
            if (!validate_multisig_config(config.address, config) ||
                !loaded.multisig_addresses.emplace(config.address.str(), config).second) return false;
        }
        const auto wrapped = decode(document.at("wrapped_master"));
        const auto encrypted = decode(document.at("key_vault"));
        if (wrapped.size() != 76 || encrypted.size() < 48) return false;
        if (!password.empty()) {
            bytes master;
            SensitiveBytesGuard guard{master};
            if (!crypto::decrypt_aes256(wrapped, password, master) || master.size() != 32) return false;
        }
        const auto scanned_height = document.at("scanned_height").get<Height>();
        const auto scanned_hash = hash(document.at("scanned_hash"));
        keys_.swap(loaded.keys); transactions_.swap(loaded.transactions); tx_index_.swap(loaded.tx_index);
        multisig_addresses_.swap(loaded.multisig_addresses); primary_address_ = loaded.primary_address;
        wrapped_master_key_ = wrapped; encrypted_keys_ = encrypted;
        if (!vault_master_key_.empty()) OPENSSL_cleanse(vault_master_key_.data(), vault_master_key_.size());
        vault_master_key_.clear();
        vault_mode_ = true; vault_keys_dirty_ = false; vault_saved_checksum_ = checksum;
        ++wallet_revision_; vault_saved_revision_ = wallet_revision_;
        has_password_hash_ = true; unlock_until_ = 0; unlock_deadline_ = {};
        OPENSSL_cleanse(password_hash_.data(), password_hash_.size());
        utils::cleanse_container(password_salt_); password_kdf_iterations_ = 0;
        last_scanned_height_ = scanned_height; last_scanned_block_hash_ = scanned_hash;
        last_scan_result_ = {};
        file_path_ = std::filesystem::absolute(filename).lexically_normal().string();
        vault_cv_.notify_all();
        return true;
    } catch (...) { return false; }
}

bool Wallet::backup(const std::string& destination) const {
    std::lock_guard<std::mutex> guard(mutex_);
    std::error_code error;
    const bool exists = std::filesystem::exists(destination, error);
    if (!vault_mode_ || exists || error ||
        std::filesystem::absolute(destination).lexically_normal() == std::filesystem::absolute(file_path_).lexically_normal()) return false;
    // Serialize the current public history and sealed key vault, including when locked.
    return save_vault_locked(destination, false);
}
} // namespace geocoin::wallet
