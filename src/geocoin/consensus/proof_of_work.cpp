#include "geocoin/consensus/proof_of_work.hpp"
#include "geocoin/consensus/chainparams.hpp"
#include "geocoin/consensus/geo_tag.hpp"
#include <cstring>
#include <algorithm>
#include <chrono>

namespace geocoin::consensus {

ProofOfWork::ProofOfWork() {
}

Hash256 ProofOfWork::compute_pow_hash(
    const core::BlockHeader& header,
    uint32_t prev_timestamp,
    const Hash256& prev_hash,
    Height height
) {
    const auto tag = geo::compute_block_geo_tag(prev_timestamp, prev_hash, height, Params().geopow_activation_height());

    core::BlockHeader header_with_tag = header;
    header_with_tag.geo_tag = tag.geo_tag;
    return header_with_tag.get_hash();
}

bool ProofOfWork::verify_proof_of_work(
    const core::BlockHeader& header,
    uint32_t target_bits,
    uint32_t prev_timestamp,
    const Hash256& prev_hash,
    Height height
) {
    return verify_proof_of_work(
        header,
        target_bits,
        prev_timestamp,
        prev_hash,
        height,
        Params()
    );
}

bool ProofOfWork::verify_proof_of_work(
    const core::BlockHeader& header,
    uint32_t target_bits,
    uint32_t prev_timestamp,
    const Hash256& prev_hash,
    Height height,
    const ChainParams& params
) {
    // Проверяем, что bits в заголовке совпадает
    if (header.bits != target_bits) {
        return false;
    }

    if (!is_valid_target_bits(target_bits, params.pow_limit_bits())) {
        return false;
    }

    const auto tag = geo::compute_block_geo_tag(prev_timestamp, prev_hash, height, params.geopow_activation_height());
    if (header.geo_tag != tag.geo_tag) {
        return false;
    }

    auto target = bits_to_target(target_bits);
    const auto hash = header.get_hash();

    return hash_meets_target(hash, target);
}

uint256 ProofOfWork::bits_to_target(uint32_t bits) {
    uint8_t exponent = bits >> 24;
    uint32_t mantissa = bits & 0x007FFFFF;
    bool negative = (bits & 0x00800000) != 0;

    uint256 target;
    target.data[0] = 0;
    target.data[1] = 0;
    target.data[2] = 0;
    target.data[3] = 0;

    if (exponent <= 3) {
        mantissa >>= 8 * (3 - exponent);
        target.data[0] = mantissa;
    } else {
        int shift = 8 * (exponent - 3);
        int word_index = shift / 64;
        int bit_shift = shift % 64;

        if (word_index < 4) {
            target.data[word_index] |= static_cast<uint64_t>(mantissa) << bit_shift;
        }
        if (bit_shift > 64 - 24 && word_index + 1 < 4) {
            target.data[word_index + 1] |= static_cast<uint64_t>(mantissa) >> (64 - bit_shift);
        }
    }

    if (negative) {
        target = uint256();  // Негативные target не допускаются, возвращаем 0
    }

    return target;
}

bool ProofOfWork::is_valid_target_bits(uint32_t bits) {
    return is_valid_target_bits(bits, Params().pow_limit_bits());
}

bool ProofOfWork::is_valid_target_bits(uint32_t bits, uint32_t pow_limit_bits) {
    const uint256 target = bits_to_target(bits);
    if (target.is_zero()) {
        return false;
    }

    const uint256 pow_limit = bits_to_target(pow_limit_bits);
    if (target > pow_limit) {
        return false;
    }

    // Одна и та же цель не должна иметь несколько consensus-представлений.
    return target_to_bits(target) == bits;
}

uint32_t ProofOfWork::target_to_bits(const uint256& target) {
    // Находим старший ненулевой байт (в big-endian представлении)
    int highest_byte = -1;
    uint8_t bytes[32];
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 8; j++) {
            bytes[i * 8 + j] = (target.data[3 - i] >> (56 - j * 8)) & 0xFF;
        }
    }

    for (int i = 0; i < 32; i++) {
        if (bytes[i] != 0) {
            highest_byte = i;
            break;
        }
    }

    if (highest_byte == -1) {
        return 0;
    }

    uint8_t exponent = 32 - highest_byte;
    uint32_t mantissa = 0;

    if (highest_byte <= 29) {
        mantissa = (bytes[highest_byte] << 16) |
                   (bytes[highest_byte + 1] << 8) |
                   (bytes[highest_byte + 2]);
    } else if (highest_byte == 30) {
        mantissa = (bytes[highest_byte] << 16) |
                   (bytes[highest_byte + 1] << 8);
    } else {
        mantissa = (bytes[highest_byte] << 16);
    }

    // Если mantissa >= 0x800000, нормализуем (как в Bitcoin Core)
    if (mantissa & 0x00800000) {
        mantissa >>= 8;
        exponent += 1;
    }

    mantissa &= 0x007FFFFF;
    return (static_cast<uint32_t>(exponent) << 24) | mantissa;
}

bool ProofOfWork::hash_meets_target(const Hash256& hash, const uint256& target) {
    // SHA-256 возвращает 32 байта в big-endian (hash[0] = старший байт числа)
    // uint256.data[] хранит число как 4 x uint64_t, где data[3] = старшие 64 бита
    //
    // Для сравнения нужно интерпретировать hash как big-endian 256-битное число
    // и сравнить с target (который уже в правильном формате)
    //
    // hash[0..7] = старшие 64 бита → сравниваем с target.data[3]
    // hash[8..15] = следующие 64 бита → сравниваем с target.data[2]
    // hash[16..23] = следующие 64 бита → сравниваем с target.data[1]
    // hash[24..31] = младшие 64 бита → сравниваем с target.data[0]

    for (int i = 3; i >= 0; i--) {
        // Берём 8 байт из hash и интерпретируем как big-endian uint64
        int hash_offset = (3 - i) * 8;  // data[3] -> hash[0..7], data[0] -> hash[24..31]
        uint64_t hash_part = 0;

        // Читаем 8 байт как big-endian число (старший байт имеет больший вес)
        for (int j = 0; j < 8; j++) {
            hash_part = (hash_part << 8) | static_cast<uint64_t>(hash[hash_offset + j]);
        }

        if (hash_part < target.data[i]) return true;
        if (hash_part > target.data[i]) return false;
    }
    return true;  // Равны - проходит (hash == target допустимо)
}

} // namespace geocoin::consensus
