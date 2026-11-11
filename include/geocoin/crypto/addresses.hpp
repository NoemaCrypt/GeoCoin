#pragma once

#include "geocoin/types.hpp"

namespace geocoin::crypto {

/**
 * @brief Проверка валидности адреса
 *
 * @param address Адрес для проверки
 * @return true если адрес валиден
 */
bool verify_address(const Address& address);

/**
 * @brief Base58 кодирование
 */
std::string base58_encode(const bytes& data);

/**
 * @brief Base58 декодирование
 */
bytes base58_decode(const std::string& encoded);

} // namespace geocoin::crypto
