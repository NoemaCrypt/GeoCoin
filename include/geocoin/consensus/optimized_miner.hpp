#pragma once

#include "geocoin/types.hpp"
#include "geocoin/core/block.hpp"
#include "geocoin/consensus/proof_of_work.hpp"
#include <atomic>
#include <chrono>
#include <string>

namespace geocoin::consensus {

enum class MiningSha256Backend {
    Auto,
    Native,
    Legacy,
    OpenSSL
};

bool set_mining_sha256_backend(const std::string& backend);
std::string get_mining_sha256_backend();

/**
 * @brief Оптимизированный майнер с SHA-256 как у Сатоши
 *
 * Оптимизации:
 * 1. Midstate optimization - предвычисление первых 64 байт
 * 2. Inline SHA-256 без лишних копирований
 * 3. Batch hashing - проверка нескольких nonce за раз
 * 4. Early termination - быстрая проверка target
 */
class OptimizedMiner {
    friend struct OptimizedMinerTestAccess;

public:
    OptimizedMiner();

    /**
     * @brief Майнинг блока с оптимизациями
     *
     * @param block Блок для майнинга
     * @param target_bits Целевая сложность
     * @param max_iterations Максимум итераций (0 = бесконечно)
     * @param stop_flag Флаг остановки
     * @param thread_id ID потока для вывода (опционально)
     * @return true если блок найден
     */
    bool mine_block_optimized(
        core::Block& block,
        uint32_t target_bits,
        uint32_t prev_timestamp,
        const Hash256& prev_hash,
        Height height,
        uint64_t max_iterations = 0,
        std::atomic<bool>* stop_flag = nullptr,
        int thread_id = -1,
        std::atomic<uint64_t>* hash_counter = nullptr,
        uint64_t nonce_step = 1
    );

    /**
     * @brief Получить количество хешей в секунду
     */
    double get_hashrate() const { return hashrate_; }

private:
    double hashrate_;
    bool stats_initialized_;
    uint64_t total_iterations_;
    std::chrono::steady_clock::time_point start_time_;
    std::chrono::steady_clock::time_point last_report_time_;
    Height current_height_;
    bool height_initialized_;

    /**
     * @brief Быстрая проверка: hash < target?
     * Проверяет только старшие байты для ранней оптимизации
     */
    bool quick_check_target(const Hash256& hash, const uint256& target);

    /**
     * @brief Оптимизированный SHA-256 для майнинга
     * Использует midstate optimization
     */
    Hash256 optimized_sha256(const bytes& data, const bytes& midstate);

    /**
     * @brief Вычислить midstate для заголовка блока
     * Предвычисляет первые 64 байта SHA-256
     */
    bytes compute_midstate(const core::BlockHeader& header);
};

/**
 * @brief Inline SHA-256 для критического пути
 * Оптимизирован для скорости, без проверок
 */
void inline_sha256_transform(uint32_t state[8], const uint8_t block[64]);

/**
 * @brief Быстрое сравнение hash с target
 * Использует SIMD если доступно
 */
bool fast_compare_hash_target(const uint8_t hash[32], const uint256& target);

} // namespace geocoin::consensus
