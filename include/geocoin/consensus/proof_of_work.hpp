#pragma once

#include "geocoin/types.hpp"
#include "geocoin/core/block.hpp"
#include <cstring>

namespace geocoin::consensus {

class ChainParams;

/**
 * @brief Proof-of-Work консенсус (GeoPoW)
 */
class ProofOfWork {
public:
    ProofOfWork();

    /**
     * @brief Вычислить PoW hash для заголовка блока
     *
     * GeoPoW:
     * - geo_tag детерминированно вычисляется из (prev_timestamp, prev_hash)
     * - Финальный hash = double_sha256(header_bytes),
     *   где header_bytes включает geo_tag
     *
     * @param header Заголовок блока
     * @return PoW hash
     */
    static Hash256 compute_pow_hash(
        const core::BlockHeader& header,
        uint32_t prev_timestamp,
        const Hash256& prev_hash,
        Height height
    );

    /**
     * @brief Проверить Proof-of-Work
     *
     * @param header Заголовок блока
     * @param target_bits Ожидаемая сложность
     * @return true если PoW валиден
     */
    static bool verify_proof_of_work(
        const core::BlockHeader& header,
        uint32_t target_bits,
        uint32_t prev_timestamp,
        const Hash256& prev_hash,
        Height height
    );

    /** Canonical verifier with an explicit network and activation schedule. */
    static bool verify_proof_of_work(
        const core::BlockHeader& header,
        uint32_t target_bits,
        uint32_t prev_timestamp,
        const Hash256& prev_hash,
        Height height,
        const ChainParams& params
    );

    /**
     * @brief Конвертировать bits (compact) в target
     *
     * @param bits Compact представление (4 байта)
     * @return 256-битный target
     */
    static uint256 bits_to_target(uint32_t bits);

    /**
     * @brief Проверить compact target на знак, диапазон и каноничность
     */
    static bool is_valid_target_bits(uint32_t bits);
    static bool is_valid_target_bits(uint32_t bits, uint32_t pow_limit_bits);

    /**
     * @brief Конвертировать target в bits (compact)
     *
     * @param target 256-битный target
     * @return Compact представление
     */
    static uint32_t target_to_bits(const uint256& target);

    /**
     * @brief Вычислить награду за блок
     *
     * @param height Высота блока
     * @return Награда в angles
     */
    static constexpr Amount calculate_block_reward(Height height) {
        const Height halvings = height / get_halving_interval();
        if (halvings >= 64) {
            return 0;
        }
        return get_initial_reward() >> halvings;
    }

    /**
     * @brief Получить интервал халвинга
     */
    static constexpr Height get_halving_interval() {
        return 210000;
    }

    /**
     * @brief Получить начальную награду
     */
    static constexpr Amount get_initial_reward() {
        return 50 * COIN;
    }

    /**
     * @brief Получить начальную сложность
     * Используется для Genesis блока и первых блоков
     */
    static constexpr uint32_t get_initial_bits() {
        return 0x1d00ffff;  // Bitcoin difficulty=1
    }

private:
    /**
     * @brief Проверить, что hash < target
     */
    static bool hash_meets_target(const Hash256& hash, const uint256& target);
};

} // namespace geocoin::consensus
