#include "geocoin/wallet/wallet.hpp"
#include "geocoin/wallet/wallet_internal.hpp"
#include "geocoin/crypto/addresses.hpp"
#include "geocoin/crypto/signatures.hpp"
#include "geocoin/utils/secure_memory.hpp"
#include "geocoin/utils/u64_math.hpp"
#include <openssl/crypto.h>
#include <chrono>

namespace geocoin::wallet {

bool Wallet::initialize(const bytes& private_key) {
    if (private_key.size() != 32) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (vault_locked_locked()) return false;
    // Создаем адрес напрямую из приватного ключа
    KeyInfo info;
    info.private_key.assign(private_key.begin(), private_key.end());

    // Вычисляем адрес из приватного ключа
    if (!crypto::compute_address_from_private_key(private_key, info.address)) {
        return false;
    }

    info.label = "default";
    info.created_at = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    info.used = false;

    keys_[info.address] = info;
    vault_keys_dirty_ = true;
    ++wallet_revision_;
    primary_address_ = info.address;
    return true;
}


Address Wallet::create_new_key(const std::string& label) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (vault_locked_locked()) return "";

    // Генерируем случайный приватный ключ
    bytes private_key;
    utils::ScopedContainerCleanser<bytes> private_key_cleanser(private_key);
    if (!crypto::generate_private_key(private_key)) {
        return "";
    }

    KeyInfo info;
    info.private_key.assign(private_key.begin(), private_key.end());

    // Вычисляем адрес
    if (!crypto::compute_address_from_private_key(private_key, info.address)) {
        return "";
    }

    info.label = label.empty() ? "key_" + std::to_string(keys_.size()) : label;
    info.created_at = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    info.used = false;

    keys_[info.address] = info;
    vault_keys_dirty_ = true;
    ++wallet_revision_;

    return info.address;
}

bool Wallet::import_private_key(const bytes& private_key, const std::string& label) {
    if (private_key.size() != 32) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (vault_locked_locked()) return false;

    Address address;
    if (!crypto::compute_address_from_private_key(private_key, address)) {
        return false;
    }

    if (keys_.find(address) != keys_.end()) {
        return false;
    }

    KeyInfo info;
    info.private_key.assign(private_key.begin(), private_key.end());
    info.address = address;
    info.label = label;
    info.created_at = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    info.used = false;

    keys_[address] = info;
    vault_keys_dirty_ = true;
    ++wallet_revision_;
    return true;
}

std::vector<Address> Wallet::get_all_addresses() const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<Address> addresses;

    for (const auto& [address, info] : keys_) {
        addresses.push_back(address);
    }

    // Только для просмотра: мультисиг-адреса тоже считаются "адресами кошелька" для целей
    // отображения входящих/баланса/истории. Приватников у них нет, но UTXO надо видеть.
    for (const auto& [addr, _] : multisig_addresses_) {
        try {
            addresses.emplace_back(addr);
        } catch (...) {
            // Пропускаем невалидные записи (на случай поврежденного wallet.dat)
        }
    }

    return addresses;
}

std::vector<Address> Wallet::get_spendable_addresses() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Address> addresses;
    for (const auto& [address, _] : keys_) {
        addresses.push_back(address);
    }
    return addresses;
}

std::vector<Address> Wallet::get_watch_only_addresses() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Address> addresses;
    for (const auto& [addr, _] : multisig_addresses_) {
        try {
            addresses.emplace_back(addr);
        } catch (...) {
        }
    }
    return addresses;
}

bool Wallet::get_key_info(const Address& address, KeyInfo& info) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = keys_.find(address);
    if (it == keys_.end()) {
        return false;
    }

    info = it->second;
    if (vault_locked_locked()) utils::cleanse_container(info.private_key);
    return true;
}

bool Wallet::get_private_key(const Address& address, bytes& private_key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    utils::cleanse_container(private_key);
    if (vault_locked_locked()) return false;

    auto it = keys_.find(address);
    if (it == keys_.end()) {
        return false;
    }

    private_key.assign(it->second.private_key.begin(), it->second.private_key.end());
    return true;
}

bool Wallet::has_key(const Address& address) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return keys_.find(address) != keys_.end();
}

Amount Wallet::get_balance(const Address& address, std::shared_ptr<state::ChainState> chain_state) const {
    if (!chain_state) {
        return 0;
    }

    return chain_state->get_balance(address);
}

Amount Wallet::get_total_balance(std::shared_ptr<state::ChainState> chain_state) const {
    if (!chain_state) {
        return 0;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    Amount total = 0;
    for (const auto& [address, info] : keys_) {
        for (const auto& utxo : chain_state->get_utxos_for_address(address)) {
            if (!checked_accumulate_nonnegative(total, utxo.value)) {
                return 0;
            }
        }
    }

    return total;
}

size_t Wallet::get_key_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return keys_.size();
}

void Wallet::mark_address_used(const Address& address) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = keys_.find(address);
    if (it != keys_.end()) {
        if (!it->second.used) ++wallet_revision_;
        it->second.used = true;
    }
}

Address Wallet::get_address() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return resolve_primary_address(keys_, primary_address_);
}

bytes Wallet::export_private_key(const std::string& password) const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (vault_mode_) {
        if (vault_locked_locked() || (!password.empty() && !verify_vault_password_locked(password))) return {};
        const auto it = keys_.find(resolve_primary_address(keys_, primary_address_));
        return it == keys_.end() ? bytes{} : bytes(it->second.private_key.begin(), it->second.private_key.end());
    }

    if (!verify_password_material(
            password,
            has_password_hash_,
            password_hash_,
            password_salt_,
            password_kdf_iterations_)) {
        return bytes();
    }

    const Address primary = resolve_primary_address(keys_, primary_address_);
    if (primary.empty()) {
        return bytes();
    }
    const auto it = keys_.find(primary);
    if (it == keys_.end()) {
        return bytes();
    }
    return bytes(it->second.private_key.begin(), it->second.private_key.end());
}

void Wallet::remember_password(const std::string& password) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (vault_mode_) return;

    bytes salt;
    uint32_t iters = 0;
    Hash256 verifier{};
    if (!init_password_verifier_from_password(password, salt, iters, verifier)) {
        has_password_hash_ = false;
        OPENSSL_cleanse(password_hash_.data(), password_hash_.size());
        if (!password_salt_.empty()) {
            OPENSSL_cleanse(password_salt_.data(), password_salt_.size());
        }
        password_salt_.clear();
        password_kdf_iterations_ = 0;
        return;
    }

    password_hash_ = verifier;
    if (!password_salt_.empty()) {
        OPENSSL_cleanse(password_salt_.data(), password_salt_.size());
    }
    password_salt_ = std::move(salt);
    password_kdf_iterations_ = iters;
    has_password_hash_ = true;
}

bool Wallet::verify_password(const std::string& password) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (vault_mode_) return verify_vault_password_locked(password);

    return verify_password_material(
        password,
        has_password_hash_,
        password_hash_,
        password_salt_,
        password_kdf_iterations_
    );
}

bool Wallet::has_password_protection() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return has_password_hash_;
}

void Wallet::clear_password_cache() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (vault_mode_) { lock_keys_locked(); vault_cv_.notify_all(); return; }
    has_password_hash_ = false;
    OPENSSL_cleanse(password_hash_.data(), password_hash_.size());
    if (!password_salt_.empty()) {
        OPENSSL_cleanse(password_salt_.data(), password_salt_.size());
    }
    password_salt_.clear();
    password_kdf_iterations_ = 0;
}

bool Wallet::add_multisig_address(const wallet::MultisigConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::string address = config.address.str();

    if (!wallet::validate_multisig_config(config.address, config)) {
        return false;
    }

    multisig_addresses_[address] = config;
    ++wallet_revision_;
    return true;
}

std::vector<wallet::MultisigConfig> Wallet::get_multisig_addresses() const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<wallet::MultisigConfig> result;
    for (const auto& [addr, config] : multisig_addresses_) {
        result.push_back(config);
    }
    return result;
}

bool Wallet::remove_multisig_address(const std::string& address) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = multisig_addresses_.find(address);
    if (it == multisig_addresses_.end()) {
        return false;
    }

    multisig_addresses_.erase(it);
    ++wallet_revision_;
    return true;
}

} // namespace geocoin::wallet
