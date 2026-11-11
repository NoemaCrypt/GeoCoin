#pragma once

#include "geocoin/types.hpp"
#include <string>
#include <stdexcept>

namespace geocoin::crypto {

inline constexpr int kPbkdf2Iterations = 600000;       // OWASP 2023
inline constexpr int kPbkdf2LegacyIterations = 200000; // Legacy wallets (v10 and older)

/**
 * @brief PBKDF2-HMAC-SHA256 derivation (32 bytes key)
 */
bool derive_key_pbkdf2(
    const std::string& password,
    const bytes& salt,
    int iterations,
    bytes& key
);

/**
 * @brief Шифрование данных с помощью AES-256-GCM
 *
 * @param data Данные для шифрования
 * @param password Пароль
 * @param encrypted [out] Зашифрованные данные
 * @return true при успехе
 */
bool encrypt_aes256(
    const bytes& data,
    const std::string& password,
    bytes& encrypted
);

bool encrypt_aes256(
    const bytes& data,
    const std::string& password,
    bytes& encrypted,
    int iterations
);

/**
 * @brief Шифрование данных (для совместимости с тестами)
 *
 * @param data Данные для шифрования
 * @param password Пароль
 * @return Зашифрованные данные
 */
inline bytes encrypt_data(const bytes& data, const std::string& password) {
    bytes encrypted;
    if (!encrypt_aes256(data, password, encrypted)) {
        throw std::runtime_error("Encryption failed");
    }
    return encrypted;
}

/**
 * @brief Расшифровка данных с помощью AES-256-GCM
 *
 * @param encrypted Зашифрованные данные
 * @param password Пароль
 * @param data [out] Расшифрованные данные
 * @return true при успехе
 */
bool decrypt_aes256(
    const bytes& encrypted,
    const std::string& password,
    bytes& data
);

bool decrypt_aes256(
    const bytes& encrypted,
    const std::string& password,
    bytes& data,
    int iterations
);

/**
 * @brief Расшифровка данных (для совместимости с тестами)
 *
 * @param encrypted Зашифрованные данные
 * @param password Пароль
 * @return Расшифрованные данные
 */
inline bytes decrypt_data(const bytes& encrypted, const std::string& password) {
    bytes data;
    if (!decrypt_aes256(encrypted, password, data)) {
        throw std::runtime_error("Decryption failed");
    }
    return data;
}

} // namespace geocoin::crypto
