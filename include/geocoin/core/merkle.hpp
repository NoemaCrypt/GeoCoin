#pragma once

#include "geocoin/types.hpp"
#include <vector>

namespace geocoin::core {

/**
 * @brief Вычисление корня дерева Меркла
 *
 * @param hashes Список хешей транзакций
 * @return Корень дерева Меркла
 */
Hash256 compute_merkle_root(const std::vector<Hash256>& hashes);

/**
 * @brief Построение пути Меркла для доказательства включения
 *
 * @param hashes Список хешей транзакций
 * @param index Индекс транзакции
 * @return Путь Меркла (список хешей)
 */
std::vector<Hash256> compute_merkle_path(const std::vector<Hash256>& hashes, size_t index);

/**
 * @brief Верификация пути Меркла
 *
 * @param tx_hash Хеш транзакции
 * @param merkle_path Путь Меркла
 * @param merkle_root Корень дерева
 * @param index Индекс транзакции
 * @return true если путь валиден
 */
bool verify_merkle_path(const Hash256& tx_hash,
                       const std::vector<Hash256>& merkle_path,
                       const Hash256& merkle_root,
                       size_t index);

/**
 * Empty path (single-leaf tree) is valid only for leaf_index 0.
 * Depth >= 32 always fits a uint32 leaf index (2^32 leaves or more).
 */
inline bool merkle_leaf_index_in_range(size_t path_length, uint32_t leaf_index) noexcept {
    if (path_length >= 32) {
        return true;
    }
    return static_cast<uint64_t>(leaf_index) < (uint64_t{1} << path_length);
}

} // namespace geocoin::core
