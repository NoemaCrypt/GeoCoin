#pragma once

#include "geocoin/types.hpp"
#include "geocoin/core/block.hpp"
#include "geocoin/storage/db_options.hpp"
#include <memory>
#include <string>
#include <vector>
#include <mutex>
#include <chrono>
#include <cstdint>
#include <limits>
#include <utility>
#include <functional>

namespace geocoin::storage {

class RocksDBStore;
struct KVWrite;

enum class ActiveChainTransitionKind : uint8_t {
    CONNECT = 1,
    DISCONNECT = 2
};

struct ActiveChainTransitionRecord {
    ActiveChainTransitionKind kind = ActiveChainTransitionKind::CONNECT;
    bool old_tip_exists = false;
    Height old_height = 0;
    Hash256 old_tip_hash{};
    Height new_height = 0;
    Hash256 new_tip_hash{};
};

enum class ActiveChainTransitionReadResult {
    NOT_FOUND,
    VALID,
    CORRUPT,
    IO_ERROR
};

/**
 * @brief Хранилище блоков на диске
 *
 * Сохраняет блоки на диск для персистентности.
 * Индексирует блоки по хешу и высоте.
 */
class BlockStorage {
public:
    struct BlockIndexRecord {
        Hash256 hash{};
        core::BlockHeader header;
        Height height = std::numeric_limits<Height>::max();
        bool has_body = false;
        bool has_chain_work = false;
        bool failed = false;
        uint256 chain_work{};
    };
    struct BlockGeographyRecord {
        Height height = 0;
        Hash256 hash{};
        uint64_t geo_tag = 0;
        bool index_pruned = false;
    };
    bool get_block_geography(Height height, BlockGeographyRecord& record) const;

    struct PrunedIndexAnchor {
        Hash256 hash{};
        core::BlockHeader header;
        Height height = 0;
        uint256 chain_work{};
    };

    struct PruningStats {
        uint64_t total_blocks = 0;
        uint64_t pruned_blocks = 0;
        uint64_t body_bytes_reduced = 0;
        uint64_t bytes_total = 0;
    };
    BlockStorage();
    explicit BlockStorage(
        const std::string& path,
        StorageBackend backend = StorageBackend::ROCKSDB,
        StorageOptions options = {}
    );
    ~BlockStorage();

    /**
     * @brief Открыть хранилище
     *
     * @param path Путь к директории с данными
     * @return true при успехе
     */
    bool open(
        const std::string& path,
        StorageBackend backend = StorageBackend::ROCKSDB,
        StorageOptions options = {}
    );

    /**
     * @brief Закрыть хранилище
     */
    void close();

    /**
     * @brief Проверить, открыто ли хранилище
     *
     * @return true если открыто
     */
    bool is_open() const;

    /**
     * @brief Index/chaindata dual-write is incomplete or had an uncertain result.
     *
     * Fail-closed: Blockchain::initialize refuses to treat the datadir as consistent.
     */
    bool has_incomplete_split_write() const;

    /**
     * @brief Сохранить блок
     *
     * @param block Блок
     * @param height Высота блока (в активной/проверяемой цепи)
     * @param enforce_validation Проверять целостность блока перед записью
     * @return true при успехе
     */
    bool save_block(const core::Block& block, Height height, bool enforce_validation = true);

    /**
     * @brief Сохранить блок с получением позиции на диске
     *
     * @param block Блок
     * @param height Высота блока (в активной/проверяемой цепи)
     * @param file [out] Номер файла
     * @param pos [out] Позиция в файле
     * @param enforce_validation Проверять целостность блока перед записью
     * @return true при успехе
     */
    bool save_block_with_pos(
        const core::Block& block,
        Height height,
        int32_t& file,
        uint32_t& pos,
        bool enforce_validation = true
    );

    /**
     * @brief Включить/выключить IBD режим записи.
     *
     * В IBD режиме записи выполняются с sync=false и периодическим sync раз в N записей.
     */
    void set_ibd_mode(bool enabled);

    /**
     * @brief Настроить период sync для IBD (по умолчанию 1000 записей).
     */
    void set_ibd_flush_interval(size_t blocks);

    /**
     * @brief Получить блок по хешу
     *
     * @param block_hash Хеш блока
     * @param block [out] Блок
     * @param verify_integrity Проверять хеш заголовка, Merkle root и witness commitment
     * @return true если найден
     */
    bool get_block(const Hash256& block_hash, core::Block& block, bool verify_integrity = true) const;

    /**
     * @brief Получить блок по высоте
     *
     * @param height Высота
     * @param block [out] Блок
     * @param verify_integrity Проверять хеш заголовка, Merkle root и witness commitment
     * @return true если найден
     */
    bool get_block_by_height(Height height, core::Block& block, bool verify_integrity = true) const;

    /**
     * @brief Получить блоки, известные на заданной высоте (включая форки)
     *
     * Это форк-совместимый индекс: один height может соответствовать нескольким блокам.
     * Используется для диагностики/будущей логики синхронизации.
     */
    std::vector<Hash256> get_block_hashes_by_height(Height height) const;

    /**
     * @brief Получить дочерние блоки для заданного родительского хеша (форк-индекс).
     */
    std::vector<Hash256> get_children(const Hash256& parent_hash) const;

    /**
     * @brief Установить блок активной цепи для заданной высоты
     *
     * Индекс активной цепи хранится отдельно от общего набора блоков и не должен
     * перетираться при получении форков.
     */
    bool set_active_block(Height height, const Hash256& block_hash);

    /**
     * @brief Получить хеш блока активной цепи для заданной высоты
     * @return true если запись существует
     */
    bool get_active_block_hash(Height height, Hash256& block_hash) const;

    /**
     * @brief Получить tip активной цепи, сохранённый в BlockStorage
     * @return true если tip установлен
     */
    bool get_active_tip(Hash256& block_hash, Height& height) const;

    /**
     * @brief Persist the consensus ruleset that owns this block database.
     */
    bool set_consensus_ruleset_id(const std::string& ruleset_id);

    /**
     * @brief Read the consensus ruleset that owns this block database.
     */
    bool get_consensus_ruleset_id(std::string& ruleset_id) const;

    /**
     * @brief Установить tip активной цепи
     */
    bool set_active_tip(const Hash256& block_hash, Height height);

    /**
     * @brief Атомарно записать active height->hash и tip.
     */
    bool set_active_block_and_tip(Height height, const Hash256& block_hash);

    /** Persist an intent before mutating derived chain state. */
    bool begin_active_chain_transition(const ActiveChainTransitionRecord& transition);

    /** Read a pending cross-store active-chain transition intent. */
    ActiveChainTransitionReadResult get_active_chain_transition(
        ActiveChainTransitionRecord& transition
    ) const;

    /** Clear a transition after derived state and the active index agree. */
    bool clear_active_chain_transition();

    /** Remove every active height entry, active tip, and pending transition. */
    bool clear_active_chain();

    /**
     * @brief Атомарно удалить active-записи начиная с высоты и записать новый tip.
     */
    bool truncate_active_chain_and_set_tip(
        Height first_removed_height,
        const Hash256& tip_hash,
        Height tip_height
    );

    /**
     * @brief Атомарно заменить persisted active-chain индекс и tip.
     */
    bool replace_active_chain(
        const std::vector<std::pair<Height, Hash256>>& active_blocks,
        const Hash256& tip_hash,
        Height tip_height
    );

    /**
     * @brief Сохранить локальный policy-checkpoint (не консенсус)
     */
    bool set_policy_checkpoint(const Hash256& block_hash, Height height);

    /**
     * @brief Прочитать локальный policy-checkpoint
     * @return true если checkpoint найден
     */
    bool get_policy_checkpoint(Hash256& block_hash, Height& height) const;

    /**
     * @brief Очистить индекс активной цепи начиная с указанной высоты (включительно)
     *
     * Используется при реорганизации: записи выше форка больше не принадлежат активной цепи.
     */
    bool clear_active_from_height(Height start_height);

    /**
     * @brief Проверить наличие блока
     *
     * @param block_hash Хеш блока
     * @return true если есть
     */
    bool has_block(const Hash256& block_hash) const;
    bool is_block_body_pruned(Height height) const;

    /**
     * @brief Получить лучший блок (с максимальной высотой)
     *
     * @param block [out] Блок
     * @param verify_integrity Проверять хеш заголовка, Merkle root и witness commitment
     * @return true если найден
     */
    bool get_best_block(core::Block& block, bool verify_integrity = true) const;

    /**
     * @brief Получить высоту лучшего блока
     *
     * @return Высота
     */
    Height get_best_height() const;

    /**
     * @brief Установить лучший блок
     *
     * @param block_hash Хеш блока
     * @param height Высота
     * @return true при успехе
     */
    bool set_best_block(const Hash256& block_hash, Height height);

    /**
     * @brief Удалить блок
     *
     * @param block_hash Хеш блока
     * @return true при успехе
     */
    bool delete_block(const Hash256& block_hash);

    /**
     * @brief Удалить только тело блока активной цепи, сохранив header/index.
     */
    bool prune_block_body_at_height(Height height, Hash256* pruned_hash = nullptr);
    // Invoked before removing transaction material. Failure leaves the source body intact.
    void set_transaction_archiver(std::function<bool(const core::Block&, Height)> archiver) {
        transaction_archiver_ = std::move(archiver);
    }

    /**
     * @brief Удалить тела блоков активной цепи ниже keep_from_height.
     *
     * Header/index остаются на диске, чтобы pruned-нода могла восстановить
     * активную цепь после рестарта.
     */
    bool prune_block_bodies_below_height(
        Height keep_from_height,
        size_t max_blocks = 0,
        uint64_t* pruned_count = nullptr
    );

    /**
     * @brief Удалить исторические secondary/core index-записи ниже окна PrunedNode.
     *
     * Сохраняет один anchor на высоте keep_from_height - 1, чтобы цепь могла
     * восстановиться после рестарта без genesis/header-истории ниже окна.
     * Удаляет также тела и witness веток, не происходящих от anchor.
     * Тела активных предков должны быть заранее архивированы и удалены.
     */
    bool prune_block_indexes_below_height(
        Height keep_from_height,
        const Hash256& anchor_hash,
        const core::BlockHeader& anchor_header,
        const uint256& anchor_chain_work,
        uint64_t* pruned_count = nullptr,
        uint64_t* discarded_body_count = nullptr
    );

    /**
     * @brief Прочитать anchor compacted pruned-index.
     */
    bool get_pruned_index_anchor(PrunedIndexAnchor& out) const;

    /**
     * @brief Удалить witness у блока активной цепи на заданной высоте
     */
    bool prune_witness_at_height(Height height);

    /** Process a contiguous witness range, persisting progress with each result. */
    bool prune_witness_up_to(Height target_height, size_t max_blocks = 0);

    /**
     * @brief Получить статистику pruning
     */
    bool get_pruning_stats(PruningStats& out) const;

    /**
     * @brief Сохранить статистику pruning
     */
    bool set_pruning_stats(const PruningStats& stats);

    /**
     * @brief Получить высоту, до которой witness уже pruned
     */
    bool get_witness_pruned_height(Height& out) const;

    /**
     * @brief Установить высоту, до которой witness уже pruned
     */
    bool set_witness_pruned_height(Height height);

    /**
     * @brief Получить witness payload для блока (если сохранён)
     */
    bool get_witness_payload(const Hash256& block_hash, bytes& out) const;

    /**
     * @brief Сохранить witness payload для блока (для unpruning)
     */
    bool set_witness_payload(const Hash256& block_hash, const bytes& payload);

    /**
     * @brief Сформировать witness payload из текущего блока (если witness присутствует)
     */
    bool build_witness_payload_from_block(const Hash256& block_hash, bytes& out) const;

    /**
     * @brief Восстановить witness в блоке из payload и сохранить блок
     */
    bool restore_witness_from_payload(const Hash256& block_hash, const bytes& payload);

    /**
     * @brief Сохранить chainwork для блока (по хешу)
     */
    bool set_chainwork(const Hash256& block_hash, const uint256& chain_work);

    /**
     * Remember that a stored block failed consensus validation.
     * The marker survives restart; absence means the block may be tried again.
     */
    bool mark_block_failed(const Hash256& block_hash);

    /**
     * @brief Получить chainwork для блока (по хешу)
     */
    bool get_chainwork(const Hash256& block_hash, uint256& chain_work) const;

    /**
     * @brief Получить размер хранилища
     *
     * @return Размер в байтах
     */
    uint64_t get_size() const;

    /**
     * @brief Получить все хеши блоков
     *
     * @return Вектор хешей
     */
    std::vector<Hash256> get_all_block_hashes() const;

    /**
     * @brief Получить header/index записи всех известных блоков.
     */
    std::vector<BlockIndexRecord> get_all_block_index_records() const;

    /**
     * @brief Получить количество блоков
     *
     * @return Количество блоков
     */
    size_t get_block_count() const;

    const std::string& get_base_path() const { return path_; }
    const std::string& get_chaindata_path() const { return chaindata_path_; }
    const std::string& get_index_path() const { return index_path_; }
    StorageBackend get_backend() const { return backend_; }
    StorageOptions get_options() const { return options_; }

private:
    std::function<bool(const core::Block&, Height)> transaction_archiver_;
#ifdef GEOCOIN_ENABLE_BLOCK_STORAGE_TEST_ACCESS
    friend struct BlockStorageTestAccess;
#endif

    std::unique_ptr<RocksDBStore> chain_db_;
    std::unique_ptr<RocksDBStore> index_db_;
    std::string path_;
    std::string chaindata_path_;
    std::string index_path_;
    StorageBackend backend_{StorageBackend::ROCKSDB};
    StorageOptions options_{};
    mutable std::recursive_mutex db_write_mutex_;
    bool split_write_failed_{false};
    mutable std::mutex write_mode_mutex_;
    bool ibd_mode_{false};
    bool force_sync_next_write_{false};
    size_t ibd_pending_writes_{0};
    size_t ibd_flush_interval_{1000};

    mutable std::mutex size_cache_mutex_;
    mutable uint64_t size_cache_{0};
    mutable std::chrono::steady_clock::time_point size_cache_time_{};

    bool next_ibd_sync_write();
    bool disable_wal_for_write() const;
    RocksDBStore* db_for_key(const std::string& key) const;
    bool is_chaindata_key(const std::string& key) const;
    bool db_get(const std::string& key, std::string& value) const;
    bool db_put(const std::string& key, const std::string& value, bool sync);
    bool db_delete(const std::string& key, bool sync);
    bool db_write_batch(std::vector<KVWrite> writes, bool sync);
    Height block_body_prune_next_height() const;
    bool prune_witness_at_height_impl(Height height, bool advance_progress);

    /**
     * @brief Ключ для блока по хешу
     */
    std::string block_key(const Hash256& hash) const;

    /**
     * @brief Ключ для persistent block index record
     */
    std::string block_index_key(const Hash256& hash) const;
    std::string failed_block_key(const Hash256& hash) const;
    bool read_failed_marker(const Hash256& hash, bool& failed) const;

    /**
     * @brief Ключ для индекса высота -> хеш
     */

    /**
     * @brief Ключ для форк-совместимого индекса "все блоки на высоте"
     *
     * Формат: hh:<height(10 digits)>:<hashhex>
     */
    std::string height_hash_key(Height height, const Hash256& hash) const;

    /**
     * @brief Ключ для индекса активной цепи "height -> active_hash"
     *
     * Формат: a:<height(10 digits)>
     */
    std::string active_height_key(Height height) const;

    /**
     * @brief Ключ для tip активной цепи
     *
     * Формат значения: height(4 bytes) + hash(32 bytes)
     */
    std::string active_tip_key() const;

    /** Key for the cross-store active-chain transition intent. */
    std::string active_transition_key() const;

    /**
     * @brief Key for the persistent consensus ruleset identifier.
     */
    std::string consensus_ruleset_key() const;

    /**
     * @brief Ключ для локального policy-checkpoint
     */
    std::string policy_checkpoint_key() const;

    /**
     * @brief Ключ anchor compacted pruned-index
     */
    std::string pruned_index_anchor_key() const;

    /**
     * @brief Ключ для лучшего блока
     */
    std::string best_block_key() const;

    /**
     * @brief Ключ для parent->child индекса
     *
     * Формат: pc:<parenthashhex>:<childhashhex>
     */
    std::string parent_child_key(const Hash256& parent_hash, const Hash256& child_hash) const;

    /**
     * @brief Ключ для chainwork блока
     *
     * Формат: bw:<hashhex>
     */
    std::string chainwork_key(const Hash256& hash) const;
};

} // namespace geocoin::storage
