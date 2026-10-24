#include "geocoin/wallet/wallet.hpp"
#include "geocoin/utils/logger.hpp"
#if defined(_WIN32)
#include <direct.h>
#else
#include <unistd.h>
#endif
#include <exception>
#include <openssl/crypto.h>

namespace geocoin::wallet {

namespace {
std::string g_default_wallet_path;
}

Wallet::Wallet()
{
}

Wallet::~Wallet() {
    try {
        clear();
    } catch (const std::exception& e) {
        // Подавляем исключения в деструкторе
        GEO_LOG_ERROR("wallet", "Error in Wallet destructor: " << e.what());
    } catch (...) {
        // Подавляем все исключения в деструкторе
        GEO_LOG_ERROR("wallet", "Unknown error in Wallet destructor");
    }
}

void Wallet::clear() {
    std::lock_guard<std::recursive_mutex> scan_lock(scan_mutex_);
    std::lock_guard<std::mutex> lock(mutex_);

    for (auto& [address, info] : keys_) {
        if (!info.private_key.empty()) {
            OPENSSL_cleanse(info.private_key.data(), info.private_key.size());
        }
    }

    keys_.clear();
    primary_address_ = Address();
    transactions_.clear();
    tx_index_.clear();
    last_scanned_height_ = 0;
    last_scanned_block_hash_ = {};
    last_scan_result_ = {};
    has_password_hash_ = false;
    OPENSSL_cleanse(password_hash_.data(), password_hash_.size());
    if (!password_salt_.empty()) {
        OPENSSL_cleanse(password_salt_.data(), password_salt_.size());
    }
    password_salt_.clear();
    password_kdf_iterations_ = 0;
}

std::string Wallet::get_default_wallet_path() {
    if (!g_default_wallet_path.empty()) {
        return g_default_wallet_path;
    }
    char cwd[1024];
    if (
#if defined(_WIN32)
        _getcwd(cwd, static_cast<int>(sizeof(cwd))) != nullptr
#else
        getcwd(cwd, sizeof(cwd)) != nullptr
#endif
    ) {
        return std::string(cwd) + "/wallet.dat";
    }
    return "./wallet.dat";
}

void Wallet::set_default_wallet_path(const std::string& path) {
    g_default_wallet_path = path;
}

} // namespace geocoin::wallet
