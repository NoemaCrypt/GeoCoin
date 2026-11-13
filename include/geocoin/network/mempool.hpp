#pragma once
#include "geocoin/network/fee_estimator.hpp"

#include "geocoin/types.hpp"
#include "geocoin/core/transaction.hpp"
#include "geocoin/state/state.hpp"
#include "geocoin/network/mempool_policy.hpp"
#include <chrono>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <memory>
#include <optional>
#include <string>
#include <limits>

namespace geocoin::network {

struct MempoolHash256Hasher {
    size_t operator()(const Hash256& h) const noexcept {
        size_t v = 1469598103934665603ull;
        for (byte b : h) {
            v ^= static_cast<size_t>(b);
            v *= 1099511628211ull;
        }
        return v;
    }
};

/**
 * @brief Информация о транзакции в mempool
 */
struct MempoolEntry {
    core::Transaction tx;
    uint64_t fee;                               // Комиссия в angles
    uint64_t fee_per_byte;                      // Комиссия на байт
    std::chrono::steady_clock::time_point added_at;
    size_t size;                                // Размер транзакции в байтах
    // Reserved at admission; the referenced proof may disappear during a reorg.
    Amount delegation_debit = 0;
};

/**
 * @brief Статистика mempool
 */
struct MempoolStats {
    size_t tx_count;
    size_t total_size;                          // Байты
    uint64_t total_fees;                        // англы
    uint64_t min_fee_per_byte;
    uint64_t max_fee_per_byte;
    uint64_t avg_fee_per_byte;
};

/**
 * @brief Статистика отклонений mempool
 */
struct MempoolRejectStats {
    std::map<std::string, uint64_t> consensus;
    std::map<std::string, uint64_t> policy;
};

/**
 * @brief Mempool - пул неподтвержденных транзакций
 *
 * Управляет транзакциями, ожидающими включения в блок:
 * - Верификация транзакций
 * - Приоритизация по комиссиям
 * - Удаление конфликтующих транзакций
 * - Ограничение размера
 */
class Mempool {
    friend struct MempoolTestAccess;

public:
    static constexpr size_t kMaxPendingAngleCommitments = 4096;
    // Isolated zero-fee commitments (one per fake spend) are cheaper to
    // flood than a real multisig package. Cap them tighter and evict them
    // first so an in-progress package is not aged out of the 4096 pool.
    static constexpr size_t kMaxUnclaimedAngleCommitments = 1024;

    Mempool(std::shared_ptr<state::ChainState> chain_state);
    FeeEstimator& fee_estimator() { return fee_estimator_; }
    const FeeEstimator& fee_estimator() const { return fee_estimator_; }
    ~Mempool();

    /**
     * @brief Добавить транзакцию в mempool
     *
     * @param tx Транзакция
     * @return true если добавлена успешно
     */
    bool add_transaction(const core::Transaction& tx);

    /**
     * Run the same snapshot admission checks as add_transaction without
     * inserting, evicting, updating relay state, or changing reject metrics.
     * The result is advisory: a concurrent admission or block can still race
     * a later add_transaction call.
     */
    bool test_accept_transaction(const core::Transaction& tx, std::string* reject_reason = nullptr);

    /**
     * @brief Удалить транзакцию из mempool
     *
     * @param tx_hash Хеш транзакции
     * @return true если удалена
     */
    bool remove_transaction(const Hash256& tx_hash);

    /**
     * @brief Проверить наличие транзакции
     *
     * @param tx_hash Хеш транзакции
     * @return true если есть в mempool
     */
    bool has_transaction(const Hash256& tx_hash) const;

    /**
     * @brief Получить транзакцию
     *
     * @param tx_hash Хеш транзакции
     * @param tx [out] Транзакция
     * @return true если найдена
     */
    bool get_transaction(const Hash256& tx_hash, core::Transaction& tx) const;

    /**
     * @brief Получить транзакции для майнинга
     *
     * Возвращает транзакции с наибольшими комиссиями,
     * которые помещаются в блок заданного размера.
     *
     * @param max_size Максимальный размер блока в байтах
     * @param block_time Время заголовка шаблона; без него используется текущее время.
     * @return Список транзакций
     */
    std::vector<core::Transaction> get_transactions_for_mining(
        size_t max_size, std::optional<Timestamp> block_time = std::nullopt
    ) const;

    /**
     * @brief Получить все транзакции
     *
     * @return Список транзакций
     */
    std::vector<core::Transaction> get_all_transactions() const;

    /**
     * @brief Copy every mempool transaction id in deterministic order.
     *
     * Unlike get_all_transactions(), this avoids cloning large Falcon
     * witnesses when a newly authenticated peer only needs an inventory
     * backfill.
     */
    std::vector<Hash256> get_all_transaction_hashes() const;

    /**
     * @brief Получить все entry mempool с метаданными fee/size.
     *
     * Возвращает копию для read-only RPC/diagnostics.
     */
    std::vector<MempoolEntry> get_all_entries() const;

    /** Copy a deterministic txid-ordered page without cloning the whole pool. */
    std::vector<MempoolEntry> get_entries_page(
        size_t offset,
        size_t limit,
        size_t* total = nullptr
    ) const;

    /** Look up one entry without constructing a full mempool snapshot. */
    bool get_entry(const Hash256& tx_hash, MempoolEntry& entry) const;

    /**
     * @brief Удалить транзакции, включенные в блок
     *
     * @param block Блок с подтвержденными транзакциями
     */
    void remove_block_transactions(const core::Block& block);

    /**
     * Drop transactions confirmed by the connected blocks, then recheck only
     * the mempool entries those blocks can invalidate. A disconnect rechecks
     * every survivor because spent outputs and registry state can return.
     */
    void reconcile_with_chain(const std::vector<core::Block>& connected, bool had_disconnect);

    /**
     * @brief Получить счетчик обновлений mempool (как в Bitcoin)
     * @return Счетчик обновлений
     */
    uint64_t get_transactions_updated() const;

    /**
     * @brief Очистить mempool
     */
    void clear();

    /**
     * Drop stale entries so zero-fee protocol messages cannot reserve bounded
     * mempool slots forever. Returns the number of entries removed, including
     * dependent transactions.
     */
    size_t expire_old_transactions();

    /**
     * @brief Получить статистику
     *
     * @return Статистика mempool
     */
    MempoolStats get_stats() const;

    /**
     * @brief Получить статистику отклонений (consensus/policy)
     */
    MempoolRejectStats get_reject_stats() const;

    /**
     * @brief Установить максимальный размер mempool
     *
     * @param max_size Размер в байтах (по умолчанию 300 МБ)
     */
    void set_max_size(size_t max_size);

    /**
     * @brief Установить policy-конфиг mempool
     */
    void set_policy_config(const MempoolPolicyConfig& config);

    /**
     * @brief Получить максимальный размер mempool (в байтах)
     */
    size_t max_size_limit() const;

    /**
     * @brief Получить количество транзакций
     *
     * @return Количество
     */
    size_t size() const;

    /**
     * @brief Проверить, пуст ли mempool
     *
     * @return true если пуст
     */
    bool empty() const;

    /**
     * Максимум хешей, которые Node снимает с relay-очереди за один тик.
     * Совпадает с лимитом элементов INV, чтобы одно drain-сообщение
     * не пробивало окно kPolicyMaxInvMsgsPerWindow.
     */
    static constexpr size_t kRelayInventoryBatch = 2048;

    /**
     * @brief Поставить хеши в очередь INV-relay без немедленной рассылки
     *
     * Используется при handshake: большой mempool не отправляется одним
     * кадром, а батчится периодическим drain.
     */
    void queue_inventory_for_relay(const std::vector<Hash256>& hashes);

    /**
     * @brief Получить новые транзакции для relay (как в Bitcoin)
     *
     * Возвращает хеши транзакций которые нужно отправить другим нодам.
     * После вызова выбранные элементы удаляются из очереди.
     *
     * @param max_items 0 — забрать всю очередь, иначе не больше max_items
     * @return Список хешей транзакций
     */
    std::vector<Hash256> get_transactions_for_relay(size_t max_items = 0);

private:
    mutable std::mutex mutex_;
    std::shared_ptr<state::ChainState> chain_state_;

    // Новые транзакции для relay (как setInventoryToSend в Bitcoin)
    std::set<Hash256> new_transactions_;
    std::mutex relay_mutex_;

    // Транзакции: tx_hash -> entry
    std::map<Hash256, MempoolEntry> transactions_;
    FeeEstimator fee_estimator_;

    // Индекс по комиссиям (для быстрого выбора)
    std::multimap<uint64_t, Hash256, std::greater<uint64_t>> fee_index_;

    // Использованные выходы (для проверки конфликтов):
    // prev_tx_hash -> набор(vout)
    std::unordered_map<Hash256, std::unordered_set<uint32_t>, MempoolHash256Hasher> spent_outputs_;

    // Анти-replay индексы консенсуса (в mempool)
    std::set<Hash256> angle_commitment_keys_;
    std::unordered_map<Hash256, size_t, MempoolHash256Hasher>
        pending_angle_commitments_by_spend_;
    std::set<Hash256> claimed_angle_commitments_;
    std::set<Hash256> multisig_spend_keys_;
    std::set<Hash256> protocol_authorization_keys_;
    std::set<Hash256> hierarchical_spend_keys_;
    std::set<Hash256> delegation_proof_keys_;
    // Pending DELEGATION_PROOF nonces per primary multisig address. Block
    // application records each (address, nonce) once, so two pending proofs
    // that differ in content but share a nonce must not both be admitted.
    std::map<std::string, std::set<uint32_t>> delegation_nonces_;
    std::set<Hash256> revoke_delegation_keys_;
    std::set<Hash256> nft_id_keys_;

    // Учет лимитов делегирования на уровне mempool:
    // delegation_proof_txid(hex) -> сумма(amount_spent) по иерархическим тратам в мемпуле.
    // Это предотвращает принятие нескольких ожидающих трат, которые вместе превышают max_total_delegated.
    std::unordered_map<std::string, Amount> mempool_delegation_spent_;

    size_t max_size_;                           // Максимальный размер в байтах
    size_t current_size_;                       // Текущий размер в байтах
    uint64_t transactions_updated_;             // Счетчик обновлений (как в Bitcoin)
    MempoolPolicyConfig policy_config_{};

    // Метрики отказов
    std::unordered_map<std::string, uint64_t> reject_consensus_;
    std::unordered_map<std::string, uint64_t> reject_policy_;

    struct RevalidationScope {
        bool full = true;
        std::set<std::pair<Hash256, uint32_t>> touched_outpoints;
        std::set<Hash256> created_txids;
        bool delegation_changed = false;
        bool registry_changed = false;
    };

    bool remove_transaction_locked(const Hash256& tx_hash);
    std::set<Hash256> collect_descendants_locked(std::set<Hash256> roots) const;
    void note_block_effects_locked(const core::Block& block, RevalidationScope& scope) const;
    bool entry_needs_recheck_locked(const MempoolEntry& entry, const RevalidationScope& scope) const;
    void revalidate_locked(const RevalidationScope& scope);
    void record_reject(bool policy, const std::string& reason);

    static std::string hash256_to_hex(const Hash256& h);
    static bool is_null_hash256(const Hash256& h);
    Amount hierarchical_amount_spent_locked(
        const core::Transaction& tx, const core::Transaction& delegation_proof_tx, uint64_t fee) const;
    bool reserve_delegation_budget_locked(
        const MempoolEntry& entry, std::unordered_map<std::string, Amount>& pending,
        const core::Transaction* proof = nullptr) const;

    /**
     * @brief Верифицировать транзакцию
     */
    bool verify_transaction(const core::Transaction& tx, bool record_rejections = true);

    /**
     * @brief Проверить конфликты с другими транзакциями
     */
    bool has_conflicts(const core::Transaction& tx) const;

    /**
     * @brief Вычислить комиссию транзакции
     */
    uint64_t calculate_fee(const core::Transaction& tx) const;

    /**
     * @brief Удалить транзакции с наименьшими комиссиями
     */
    std::vector<Hash256> plan_eviction_locked(
        size_t bytes_needed, uint64_t maximum_fee_rate,
        const std::set<Hash256>& protected_transactions) const;
    bool evict_low_fee_transactions(
        size_t bytes_needed,
        uint64_t maximum_fee_rate = (std::numeric_limits<uint64_t>::max)(),
        const std::set<Hash256>& protected_transactions = {});

    // Unclaimed ANGLE_COMMITMENTs pay no fee, so a full pool evicts the oldest
    // isolated one no pending root claims instead of rejecting newer commitments.
    size_t unclaimed_angle_commitment_count_locked() const;
    std::optional<Hash256> oldest_unclaimed_angle_commitment_locked() const;
    bool evict_oldest_unclaimed_angle_commitment_locked();
};

namespace fee_policy {

// Fee recommendations are intentionally bounded and depend only on aggregate
// mempool occupancy. Individual high-fee transactions must not be able to
// raise every wallet's recommendation.
constexpr uint64_t MAX_CONGESTION_MULTIPLIER = 16;

uint64_t recommended_rate_per_kb(const Mempool* mempool);
Amount scale_fee_to_rate(Amount base_fee, uint64_t rate_per_kb);

} // namespace fee_policy

} // namespace geocoin::network
