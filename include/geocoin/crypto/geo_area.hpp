#pragma once

#include "geocoin/types.hpp"
namespace geocoin::crypto {

/**
 * @brief SHA-256 хеширование
 */
Hash256 sha256(const bytes& data);
Hash256 sha256(const uint8_t* data, size_t size);

/**
 * @brief RIPEMD-160 хеширование
 */
Hash160 ripemd160(const bytes& data);

/**
 * @brief Двойное хеширование SHA-256
 */
Hash256 double_sha256(const bytes& data);
Hash256 double_sha256(const uint8_t* data, size_t size);

/**
 * @brief Конвертация bytes в hex строку
 */
std::string bytes_to_hex(const bytes& data);

/**
 * @brief Конвертация hex строки в bytes
 */
bytes hex_to_bytes(const std::string& hex);

/**
 * @brief Generate a 32-byte private Falcon identity seed using OpenSSL's
 * private DRBG. The output is cleared on failure.
 */
bool generate_private_key(bytes& private_key);

/**
 * @brief Derive a GEO address from a 32-byte Falcon identity seed.
 *
 * @param private_key Falcon identity seed (32 bytes)
 * @param address [out] Адрес (GEO...)
 * @return true при успехе
 */
bool compute_address_from_private_key(
    const bytes& private_key,
    Address& address
);

} // namespace geocoin::crypto
