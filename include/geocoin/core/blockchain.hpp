#pragma once

#include "geocoin/types.hpp"
#include "geocoin/core/block.hpp"
#include "geocoin/core/transaction.hpp"
#include "geocoin/chain/block_index.hpp"
#include "geocoin/chain/chain.hpp"
#include "geocoin/consensus/validation.hpp"
#include "geocoin/state/state.hpp"
#include "geocoin/storage/block_storage.hpp"
#include <memory>
#include <map>
#include <set>
#include <mutex>
#include <vector>
#include <string>
#include <utility>
#include <unordered_set>

namespace geocoin::core {

struct ChainTipInfo {
    Hash256 hash{};
    Height height = 0;
    Height branch_length = 0;
    bool active = false;
    std::string status;
};

struct PolicyCheckpointInfo {
    bool has_checkpoint = false;
    Height height = 0;
    Hash256 hash{};
    Height interval = 0;
    Height confirmations = 0;
    bool allow_deep_reorg = false;
};

/**
 * Atomically captured active-tip context for block-template construction.
 * BlockIndex objects remain stable for the lifetime of Blockchain, so the
 * pointer can safely be used for read-only difficulty/MTP calculations after
 * the chain mutex is released. Call is_active_tip() before publishing work.
 */
struct ActiveTipSnapshot {
    chain::BlockIndex* index = nullptr;
    Hash256 hash{};
    Height height = 0;
    Timestamp median_time_past = 0;
};

struct BlockPositionSnapshot {
    bool indexed = false;
    bool has_data = false;
    bool in_active_chain = false;
    Height height = 0;
    Height confirmations = 0;
    Timestamp median_time_past = 0;
    Hash256 next_hash{};
};

/**
 * Главный класс управления блокчейном
 * Аналог функций из main.cpp в Bitcoin
 *
 * Отвечает за:
 * - Индексацию блоков (BlockMap)
 * - Активную цепь (Chain)
 * - Валидацию и прием блоков
 * - Реорганизацию цепи
 * - Применение блоков к UTXO
 */
class Blockchain {
#ifdef GEOCOIN_ENABLE_BLOCKCHAIN_TEST_ACCESS
    friend struct BlockchainTestAccess;
#endif

public:
    Blockchain(
        std::shared_ptr<state::ChainState> chain_state,
        std::shared_ptr<storage::BlockStorage> block_storage
    );

    ~Blockchain();

    std::shared_ptr<state::ChainState> get_chain_state() const { return chain_state_; }

    // ========== Прием и валидация блоков ==========

    /**
     * Обработать новый блок
     * Аналог ProcessNewBlock из Bitcoin
     *
     * Этапы:
     * 1. CheckBlock() - быстрая проверка
     * 2. AcceptBlock() - добавление в индекс
     * 3. ActivateBestChain() - активация лучшей цепи
     *
     * @param block Новый блок
     * @param state Состояние валидации
     * @return true если блок принят
     */
    bool process_new_block(
        const Block& block,
        consensus::ValidationState& state
    );

    /**
     * Принять блок в индекс
     * Аналог AcceptBlock из Bitcoin
     *
     * - Проверяет блок (CheckBlock + ContextualCheckBlock)
     * - Добавляет в BlockIndex
     * - Сохраняет на диск
     * - НЕ применяет к UTXO (это делает ActivateBestChain)
     *
     * @param block Блок для приема
     * @param state Состояние валидации
     * @param index Выходной параметр - созданный BlockIndex
     * @return true если блок принят
     */
    bool accept_block(
        const Block& block,
        consensus::ValidationState& state,
        chain::BlockIndex** index
    );

    /**
     * Активировать лучшую цепь
     * Аналог ActivateBestChain из Bitcoin
     *
     * - Находит цепь с максимальным chainwork
     * - Выполняет реорганизацию если нужно
     * - Применяет блоки к UTXO
     *
     * @param state Состояние валидации
     * @param block Опциональный блок для активации
     * @return true если успешно
     */
    bool activate_best_chain(
        consensus::ValidationState& state,
        const Block* block = nullptr
    );

    /**
     * Переключиться на конкретную ветку по tip-хешу (принудительный reorg).
     */
    bool switch_to_tip(
        const Hash256& tip_hash,
        consensus::ValidationState& state
    );

    // ========== Применение блоков к UTXO ==========

    /**
     * Применить блок к UTXO (ConnectTip)
     * Аналог ConnectTip из Bitcoin
     *
     * - Загружает блок с диска
     * - Вызывает ConnectBlock()
     * - Обновляет chain_active
     *
     * @param state Состояние валидации
     * @param index BlockIndex блока
     * @return true если успешно
     */
    bool connect_tip(
        consensus::ValidationState& state,
        chain::BlockIndex* index
    );

    /**
     * Откатить последний блок (DisconnectTip)
     * Аналог DisconnectTip из Bitcoin
     *
     * - Загружает блок с диска
     * - Вызывает DisconnectBlock()
     * - Обновляет chain_active
     *
     * @param state Состояние валидации
     * @return true если успешно
     */
    bool disconnect_tip(
        consensus::ValidationState& state
    );

    /**
     * Применить блок к UTXO set
     * Аналог ConnectBlock из Bitcoin
     *
     * Полная валидация:
     * - Проверка всех входов (UTXO существуют)
     * - Проверка coinbase maturity
     * - Проверка двойной траты
     * - Проверка подписей Falcon-512
     * - Проверка награды coinbase (с комиссиями!)
     * - Применение изменений к UTXO
     * - Создание undo данных
     *
     * @param block Блок для применения
     * @param state Состояние валидации
     * @param index BlockIndex блока
     * @return true если успешно
     */
    bool connect_block(
        const Block& block,
        consensus::ValidationState& state,
        chain::BlockIndex* index
    );

    /**
     * Откатить блок из UTXO set
     * Аналог DisconnectBlock из Bitcoin
     *
     * - Загружает undo данные
     * - Восстанавливает потраченные UTXO
     * - Удаляет созданные UTXO
     *
     * @param block Блок для отката
     * @param state Состояние валидации
     * @param index BlockIndex блока
     * @return true если успешно
     */
    bool disconnect_block(
        const Block& block,
        consensus::ValidationState& state,
        chain::BlockIndex* index
    );

    // ========== Доступ к индексу ==========

    /**
     * Найти BlockIndex по хешу
     */
    chain::BlockIndex* find_block_index(const Hash256& hash);
    const chain::BlockIndex* find_block_index(const Hash256& hash) const;

    /**
     * Получить активную цепь
     */
    chain::Chain get_active_chain() const {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        return chain_active_;
    }

    /**
     * Получить tip активной цепи
     */
    chain::BlockIndex* get_tip() const {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        return chain_active_.tip();
    }

    ActiveTipSnapshot get_active_tip_snapshot() const {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        ActiveTipSnapshot snapshot;
        snapshot.index = chain_active_.tip();
        if (snapshot.index) {
            snapshot.hash = snapshot.index->get_block_hash();
            snapshot.height = snapshot.index->height;
            snapshot.median_time_past = snapshot.index->get_median_time_past();
        }
        return snapshot;
    }

    bool is_active_tip(const Hash256& hash) const {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        const auto* tip = chain_active_.tip();
        return tip ? tip->get_block_hash() == hash : hash == Hash256{};
    }

    /**
     * Получить высоту активной цепи
     */
    Height get_height() const {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        return chain_active_.height();
    }

    /**
     * Потокобезопасно получить hash активного блока по высоте.
     */
    bool get_active_block_hash_by_height(Height height, Hash256& out_hash) const;

    // Capture branch membership and confirmations under the same chain lock.
    BlockPositionSnapshot get_block_position(const Hash256& hash) const;
    std::vector<storage::BlockStorage::BlockGeographyRecord> get_block_geography_page(Height first, size_t limit) const;

    /**
     * Удалить старые index-записи для PrunedNode, сохранив boundary anchor.
     */
    bool prune_block_index_below_height(Height keep_from_height, uint64_t* pruned_count = nullptr,
                                       uint64_t* discarded_body_count = nullptr);
    // Serialize history maintenance with chain transitions and state batches.
    bool prune_history(Height retention, bool prune_indexes, uint64_t* bodies = nullptr, uint64_t* indexes = nullptr);

    /**
     * Проверить, есть ли блок в индексе
     */
    bool has_block_index(const Hash256& hash) const;

    /**
     * Получить блок по хешу
     */
    bool get_block(const Hash256& hash, Block& block) const;

    /**
     * Получить блок по высоте (из активной цепи)
     */
    bool get_block_by_height(Height height, Block& block) const;

    /**
     * Получить снимок всех "tip" веток (активной и форков).
     */
    std::vector<ChainTipInfo> get_chain_tips() const;

    /**
     * Получить состояние локального policy-checkpoint.
     */
    PolicyCheckpointInfo get_policy_checkpoint_info() const;

    /**
     * Настроить параметры локального policy-checkpoint.
     * interval/confirmations == 0 отключают автосоздание чекпоинтов.
     */
    void set_policy_checkpoint_options(
        Height interval,
        Height confirmations,
        bool allow_deep_reorg
    );

    /**
     * Получить доступ к дисковому хранилищу блоков (инженерные инструменты/диагностика).
     */
    std::shared_ptr<storage::BlockStorage> get_block_storage() const { return block_storage_; }

    struct BlockRejectStats {
        std::map<std::string, uint64_t> consensus;
        std::map<std::string, uint64_t> policy;
    };

    /**
     * Статистика отклонений блоков (consensus/policy).
     */
    BlockRejectStats get_block_reject_stats() const;

    /**
     * Включить/выключить IBD режим хранения блоков.
     */
    void set_ibd_mode(bool enabled);

    // ========== Инициализация ==========

    /**
     * Инициализировать блокчейн
     * - Загружает индекс блоков из хранилища
     * - Восстанавливает активную цепь
     * - Проверяет консистентность
     */
    bool initialize();

    /**
     * Применить Genesis блок
     */
    bool apply_genesis();

    /**
     * @brief Обновить локальный policy-checkpoint при достижении интервала.
     */
    void maybe_update_policy_checkpoint();

    /**
     * @brief Загрузить локальный policy-checkpoint из BlockStorage.
     */
    bool load_policy_checkpoint();

private:
    void record_block_reject_(const consensus::ValidationState& state, bool policy);

    /**
     * Единая policy-проверка перехода от активной цепи к candidate tip.
     * Вызывающий обязан удерживать mutex_.
     */
    bool check_chain_transition(
        const chain::BlockIndex* target_tip,
        consensus::ValidationState& state
    ) const;

    // ========== Внутренние методы ==========

    /**
     * Добавить блок в индекс
     */
    chain::BlockIndex* add_to_block_index(const Block& block);

    /**
     * Найти лучшую цепь (с максимальным chainwork)
     */
    chain::BlockIndex* find_most_work_chain();
    chain::BlockIndex* find_most_work_chain_allowed(
        consensus::ValidationState* first_rejection = nullptr
    );

    /**
     * Проверить, является ли цепь валидной
     */
    bool is_chain_valid(chain::BlockIndex* tip);

    // Shared transition/compensation path for automatic and explicit switches.
    bool transition_to_tip(chain::BlockIndex* target_tip, consensus::ValidationState& state);

    /**
     * Пометить всех потомков failed-блока как FAILED_CHILD.
     */
    void mark_failed_descendants(chain::BlockIndex* failed_root);
    void record_permanent_failure(chain::BlockIndex* index);

    /**
     * Загрузить блок с диска
     */
    bool load_block_from_disk(chain::BlockIndex* index, Block& block);

    /**
     * Сохранить блок на диск
     */
    bool save_block_to_disk(const Block& block, chain::BlockIndex* index, Height height);

    /**
     * Собрать активную цепь в формат persisted active-index.
     */
    bool collect_active_chain_entries(
        std::vector<std::pair<Height, Hash256>>& out_entries,
        Hash256& out_tip_hash,
        Height& out_tip_height
    ) const;

    /**
     * Проверить persisted active-index на полноту и соответствие chain_active_.
     */
    bool active_chain_index_matches() const;

    /**
     * Атомарно записать active-index и tip в BlockStorage.
     */
    bool persist_active_chain_index();

    /** Repair or resolve a crash-interrupted ChainState/active-index update. */
    bool recover_interrupted_active_chain_transition(bool has_pruned_anchor);

    /**
     * Вычислить chainwork для нового блока
     */
    uint256 compute_chainwork(chain::BlockIndex* prev_index, uint32_t bits);

    /**
     * Построить skip указатель
     */
    void build_skip(chain::BlockIndex* index);

    // ========== Данные ==========

    // Индекс всех блоков (hash → BlockIndex*)
    // Аналог mapBlockIndex из Bitcoin
    std::map<Hash256, std::unique_ptr<chain::BlockIndex>> block_index_;

    // Proven ancestry is reused until any indexed branch is marked failed.
    std::unordered_set<const chain::BlockIndex*> valid_ancestry_;

    // Активная цепь (Genesis → Tip)
    // Аналог chainActive из Bitcoin
    chain::Chain chain_active_;

    // Кандидаты на лучшую цепь (отсортированы по chainwork)
    // Аналог setBlockIndexCandidates из Bitcoin
    std::set<chain::BlockIndex*, chain::BlockIndexWorkComparator> block_index_candidates_;

    // Состояние UTXO
    std::shared_ptr<state::ChainState> chain_state_;

    // Хранилище блоков
    std::shared_ptr<storage::BlockStorage> block_storage_;

    // Идентификатор последовательности для блоков (порядок получения)
    uint32_t next_sequence_id_ = 1;

    // Включается только на пути ProcessNewBlock после успешного check_block().
    bool skip_expensive_block_verify_ = false;

    // Локальный policy-checkpoint (не консенсус).
    // Используется для ограничения глубоких reorg на этой ноде.
    bool has_policy_checkpoint_{false};
    Height policy_checkpoint_height_{0};
    Hash256 policy_checkpoint_hash_{};
    Height policy_checkpoint_interval_{100};
    Height policy_checkpoint_confirmations_{144};
    bool allow_deep_reorg_{false};

    // Статистика причин отклонения блоков (для RPC/мониторинга).
    BlockRejectStats block_reject_stats_{};

    // Мьютекс для потокобезопасности
    // Public chain operations call one another (for example initialize ->
    // apply_genesis -> accept_block).  A recursive mutex lets every public
    // entry point enforce the same synchronization contract without exposing
    // unsafe "caller already holds the lock" variants.
    mutable std::recursive_mutex mutex_;
};

} // namespace geocoin::core
