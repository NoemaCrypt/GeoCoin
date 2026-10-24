#pragma once

#include "geocoin/types.hpp"
#include "geocoin/core/transaction.hpp"
#include <vector>

namespace geocoin::core {

/**
 * @brief Заголовок блока
 */
struct BlockHeader {
    uint32_t version = 1;
    uint32_t timestamp = 0;
    Hash256 prev_block_hash = {};
    Hash256 merkle_root = {};
    uint32_t bits = 0x1d00ffff;  // Сложность (compact target)
    uint64_t geo_tag = 0;        // GeoPoW v1 WGS 84 0.1-degree cell: (lat_idx << 32) | lon_idx
    Hash256 witness_commitment = {}; // Детерминированный коммитмент witness-данных блока
    uint64_t nonce = 0;

    bytes serialize() const;
    static BlockHeader deserialize(const bytes& data);
    Hash256 get_hash() const;
};

/**
 * @brief Блок GeoCoin
 *
 * Чистый Proof-of-Work консенсус с Falcon-512 подписями
 */
class Block {
public:
    BlockHeader header;
    std::vector<Transaction> transactions;

    /**
     * @brief Вычисление корня Меркла для транзакций
     */
    Hash256 calculate_merkle_root();

    /**
     * @brief Получение хеша блока
     */
    Hash256 get_hash() const;

    /**
     * @brief Вычисление witness commitment для текущего набора транзакций
     */
    Hash256 calculate_witness_commitment() const;

    /**
     * @brief Верификация блока (implemented by geocoin_consensus).
     */
    bool verify() const;

    /**
     * @brief Сериализация блока
     */
    bytes serialize() const;

    /**
     * @brief Десериализация блока
     *
     * @param data Сериализованные данные
     * @return Блок
     */
    static Block deserialize(const bytes& data);

    /**
     * @brief Размер блока в байтах
     */
    size_t get_size() const;
};

} // namespace geocoin::core
