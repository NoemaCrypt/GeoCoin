#include "geocoin/crypto/addresses.hpp"
#include "geocoin/crypto/geo_area.hpp"
#include <cstring>
#include <vector>

namespace geocoin::crypto {

static const char* BASE58_ALPHABET = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

// Полная реализация Base58 encoding
std::string base58_encode(const bytes& data) {
    if (data.empty()) {
        return "";
    }

    // Подсчитываем ведущие нули
    size_t leading_zeros = 0;
    for (size_t i = 0; i < data.size() && data[i] == 0; i++) {
        leading_zeros++;
    }

    // Выделяем буфер (максимальный размер: input_size * 138 / 100 + 1)
    size_t size = data.size() * 138 / 100 + 1;
    std::vector<uint8_t> b58(size);

    // Обрабатываем байты
    for (size_t i = 0; i < data.size(); i++) {
        uint32_t carry = data[i];

        // Применяем "b58 = b58 * 256 + carry"
        for (int j = size - 1; j >= 0; j--) {
            carry += 256 * b58[j];
            b58[j] = carry % 58;
            carry /= 58;
        }
    }

    // Пропускаем ведущие нули в результате
    size_t start = 0;
    while (start < size && b58[start] == 0) {
        start++;
    }

    // Конвертируем в строку
    std::string result;
    result.reserve(leading_zeros + (size - start));

    // Добавляем '1' для каждого ведущего нуля
    result.append(leading_zeros, '1');

    // Добавляем остальные символы
    for (size_t i = start; i < size; i++) {
        result.push_back(BASE58_ALPHABET[b58[i]]);
    }

    return result;
}

// Полная реализация Base58 decoding
bytes base58_decode(const std::string& encoded) {
    if (encoded.empty()) {
        return bytes();
    }

    // Подсчитываем ведущие '1'
    size_t leading_ones = 0;
    for (size_t i = 0; i < encoded.size() && encoded[i] == '1'; i++) {
        leading_ones++;
    }

    // Выделяем буфер
    size_t size = encoded.size() * 733 / 1000 + 1;
    std::vector<uint8_t> b256(size);

    // Обрабатываем символы
    for (size_t i = 0; i < encoded.size(); i++) {
        // Находим значение символа
        const char* p = strchr(BASE58_ALPHABET, encoded[i]);
        if (!p) {
            return bytes(); // Невалидный символ
        }

        uint32_t carry = p - BASE58_ALPHABET;

        // Применяем "b256 = b256 * 58 + carry"
        for (int j = size - 1; j >= 0; j--) {
            carry += 58 * b256[j];
            b256[j] = carry % 256;
            carry /= 256;
        }
    }

    // Пропускаем ведущие нули
    size_t start = 0;
    while (start < size && b256[start] == 0) {
        start++;
    }

    // Создаем результат
    bytes result;
    result.reserve(leading_ones + (size - start));

    // Добавляем нули для ведущих '1'
    result.insert(result.end(), leading_ones, 0);

    // Добавляем остальные байты
    result.insert(result.end(), b256.begin() + start, b256.end());

    return result;
}

bool verify_address(const Address& address) {
    // Проверяем префикс (обычный GEO или мультиподпись GEOM)
    if (address.address.size() < 4) {
        return false;
    }

    std::string prefix;
    std::string base58_part;

    // Проверяем мультиподпись адрес (GEOM)
    if (address.address.substr(0, 4) == "GEOM") {
        prefix = "GEOM";
        base58_part = address.address.substr(4);
    }
    // Проверяем обычный адрес (GEO)
    else if (address.address.substr(0, 3) == "GEO") {
        prefix = "GEO";
        base58_part = address.address.substr(3);
    }
    else {
        return false;
    }

    // Декодируем Base58
    bytes decoded = base58_decode(base58_part);

    // Адрес имеет единственное каноническое представление:
    // version(1) + HASH160(20) + checksum(4). Проверка только минимальной
    // длины позволяла подобрать корректный checksum для произвольного более
    // длинного payload и получить адрес, который никогда не создаёт кошелёк.
    if (decoded.size() != 25) {
        return false;
    }

    constexpr byte NORMAL_VERSION = 0x00;
    constexpr byte MULTISIG_VERSION = 0x05;
    const byte expected_version = prefix == "GEOM" ? MULTISIG_VERSION : NORMAL_VERSION;
    if (decoded[0] != expected_version) {
        return false;
    }

    // Извлекаем checksum (последние 4 байта)
    bytes payload(decoded.begin(), decoded.end() - 4);
    bytes checksum(decoded.end() - 4, decoded.end());

    // Вычисляем ожидаемый checksum
    auto expected_checksum_hash = double_sha256(payload);
    bytes expected_checksum(expected_checksum_hash.begin(), expected_checksum_hash.begin() + 4);

    // Сравниваем checksums
    return checksum == expected_checksum;
}

} // namespace geocoin::crypto
