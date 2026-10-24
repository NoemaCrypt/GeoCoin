#pragma once

#include "geocoin/types.hpp"
#include <stdexcept>

namespace geocoin::utils {

/**
 * @brief Кодирование CompactSize (VarInt) как в Bitcoin
 *
 * < 0xFD: 1 байт
 * <= 0xFFFF: 0xFD + 2 байта (little-endian)
 * <= 0xFFFFFFFF: 0xFE + 4 байта (little-endian)
 * > 0xFFFFFFFF: 0xFF + 8 байт (little-endian)
 */
inline bytes encode_varint(uint64_t value) {
    bytes result;

    if (value < 0xFD) {
        result.push_back(static_cast<byte>(value));
    } else if (value <= 0xFFFF) {
        result.push_back(0xFD);
        result.push_back(value & 0xFF);
        result.push_back((value >> 8) & 0xFF);
    } else if (value <= 0xFFFFFFFF) {
        result.push_back(0xFE);
        for (int i = 0; i < 4; i++) {
            result.push_back((value >> (i * 8)) & 0xFF);
        }
    } else {
        result.push_back(0xFF);
        for (int i = 0; i < 8; i++) {
            result.push_back((value >> (i * 8)) & 0xFF);
        }
    }

    return result;
}

/**
 * @brief Декодирование CompactSize (VarInt)
 *
 * @param data Данные
 * @param offset Смещение (будет обновлено)
 * @return Декодированное значение
 */
inline uint64_t decode_varint(const bytes& data, size_t& offset) {
    if (offset >= data.size()) {
        throw std::runtime_error("VarInt: unexpected end of data");
    }

    byte first = data[offset++];

    if (first < 0xFD) {
        return first;
    } else if (first == 0xFD) {
        if (offset + 2 > data.size()) {
            throw std::runtime_error("VarInt: unexpected end of data");
        }
        uint64_t value = data[offset] | (static_cast<uint64_t>(data[offset + 1]) << 8);
        offset += 2;
        if (value < 0xFD) {
            throw std::runtime_error("VarInt: non-canonical encoding");
        }
        return value;
    } else if (first == 0xFE) {
        if (offset + 4 > data.size()) {
            throw std::runtime_error("VarInt: unexpected end of data");
        }
        uint64_t value = 0;
        for (int i = 0; i < 4; i++) {
            value |= static_cast<uint64_t>(data[offset + i]) << (i * 8);
        }
        offset += 4;
        if (value <= 0xFFFF) {
            throw std::runtime_error("VarInt: non-canonical encoding");
        }
        return value;
    } else { // 0xFF
        if (offset + 8 > data.size()) {
            throw std::runtime_error("VarInt: unexpected end of data");
        }
        uint64_t value = 0;
        for (int i = 0; i < 8; i++) {
            value |= static_cast<uint64_t>(data[offset + i]) << (i * 8);
        }
        offset += 8;
        if (value <= 0xFFFFFFFFULL) {
            throw std::runtime_error("VarInt: non-canonical encoding");
        }
        return value;
    }
}

} // namespace geocoin::utils
