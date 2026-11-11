#pragma once

#include "geocoin/types.hpp"
#include "geocoin/core/transaction.hpp"
#include "geocoin/core/block.hpp"
#include "geocoin/storage/block_storage.hpp"
#include "geocoin/storage/state_records.hpp"
#include "geocoin/state/storage.hpp"
#include "geocoin/multisig/angle_store.hpp"
#include "geocoin/multisig/verifier.hpp"
#include "geocoin/nft/nft_types.hpp"
#include "geocoin/nft/nft_validator.hpp"
#include "geocoin/utils/activation_manifest.hpp"
#include "geocoin/consensus/validation.hpp"
#include <map>
#include <set>
#include <string>
#include <memory>
#include <mutex>
#include <deque>
#include <list>
#include <unordered_map>
#include <optional>
#include <span>

namespace geocoin::state {

struct UtxoKey {
    Hash256 tx_hash{};
    uint32_t vout = 0;

    bool operator==(const UtxoKey& other) const noexcept {
        return vout == other.vout && tx_hash == other.tx_hash;
    }
};

inline UtxoKey utxo_memory_key(const Hash256& tx_hash, uint32_t vout) noexcept {
    return UtxoKey{tx_hash, vout};
}

struct UtxoKeyHash {
    size_t operator()(const UtxoKey& key) const noexcept {
        size_t value = 1469598103934665603ull;
        for (byte b : key.tx_hash) {
            value ^= static_cast<size_t>(b);
            value *= 1099511628211ull;
        }
        value ^= static_cast<size_t>(key.vout) + 0x9e3779b9u;
        return value;
    }
};

struct Hash256Hash {
    size_t operator()(const Hash256& hash) const noexcept {
        size_t value = 1469598103934665603ull;
        for (byte b : hash) {
            value ^= static_cast<size_t>(b);
            value *= 1099511628211ull;
        }
        return value;
    }
};

/**
 * @brief UTXO (Unspent Transaction Output)
 */
struct UTXO {
    Hash256 tx_hash{};      // Хеш транзакции
    uint32_t vout = 0;      // Индекс выхода
    Amount value = 0;       // Количество angles
    Address address{};      // Адрес владельца
    Height height = 0;      // Высота блока
    bool is_coinbase = false; // Coinbase-транзакция?

    // Ключ для хранения: tx_hash:vout
    std::string get_key() const;

    UtxoKey memory_key() const noexcept { return utxo_memory_key(tx_hash, vout); }
    std::string get_address_index_key() const;
    static std::string address_index_prefix(const Address& address);

    // Сериализация для хранения
    bytes serialize() const;
    static UTXO deserialize(const bytes& data);
};

/**
 * @brief Депозит сайдчейна (consensus state)
 */
struct SidechainDeposit {
    Address owner;
    Amount amount = 0;
    Height unlock_height = 0;
    Hash256 checkpoint{};
    Height last_checkpoint_height = 0;
    Height last_sidechain_checkpoint_height = 0;
};

enum class SidechainStatus : uint8_t {
    ACTIVE = 0,
    DECOMMISSIONING = 1,
};

inline const char* sidechain_status_to_string(SidechainStatus status) {
    switch (status) {
        case SidechainStatus::ACTIVE:
            return "active";
        case SidechainStatus::DECOMMISSIONING:
            return "decommissioning";
    }
    return "unknown";
}

struct SidechainRegistryEntry {
    core::SidechainKind kind = core::SidechainKind::FINANCIAL;
    SidechainStatus status = SidechainStatus::ACTIVE;
    Height decommission_height = 0;
    Address owner;
    Amount issuance_limit = 0;
    Amount total_issued = 0;
    uint32_t multiplier = 0;
    uint32_t fee_bps = 0;
    uint32_t max_multiplier = 0;
    std::string token_symbol;
    std::string accounting_namespace;
    Hash256 accounting_schema_hash{};
    uint8_t committee_m = 0;
    uint8_t committee_n = 0;
    std::vector<Address> committee_members;
    Hash256 committee_id{};
    uint8_t checkpoint_policy_id = 2;   // 2 for financial, 3 for accounting
    uint8_t checkpoint_version = 2;     // 2 for financial, 1 for accounting
};

struct FoundationConfig {
    Address address;
    uint8_t m = 0;
    uint8_t n = 0;
    std::vector<Address> participants;
};

struct DelegationState {
    Address primary_address;
    uint32_t nonce = 0;
    Amount total_spent = 0;
    Hash256 proof_key{};
};

struct CardProgramEntry {
    Address issuer;
    std::string chip_vendor;
    std::string chip_os;
    std::string applet_aid;
    uint32_t applet_version = 0;
    Hash256 applet_package_hash{};
    uint32_t policy_id = 0;
    Hash256 scope_id{};
    core::CardCorporatePolicy corporate_policy;
    Height created_height = 0;
};

struct CardInstanceEntry {
    Hash256 program_id{};
    Address issuer;
    Hash256 uid_hash{};
    Hash256 pubkey_hash{};
    uint64_t valid_from = 0;
    uint64_t valid_to = 0;
    uint8_t status = 0; // 0=active, 1=suspended, 2=revoked
    Hash256 metadata_hash{};
    Height issued_height = 0;
    Height updated_height = 0;
};

/**
 * @brief Undo данные для одной транзакции
 */
struct TxUndo {
    std::vector<UTXO> spent_utxos;  // Потраченные UTXO
    // Уникальные pubkey_hash из witness этой tx, для которых был увеличен refcount.
    // При откате refcount каждого такого ключа уменьшается на 1.
    std::vector<Hash256> added_witness_pubkeys;

    // Runtime-only ownership marker. A failed candidate block must not remove
    // an ANGLE_COMMITMENT that was already present on behalf of the mempool.
    // This flag is deliberately cleared before durable undo is cached: a later
    // active-chain disconnect must remove the chain-owned commitment normally.
    bool angle_store_preexisted = false;

    struct NFTUndo {
        uint8_t kind = 0; // 0=none, 1=create, 2=transfer, 3=burn
        Hash256 nft_id{};
        std::string prev_owner;
        bytes prev_metadata;
    };

    NFTUndo nft_undo;

    struct DepositUndo {
        uint8_t kind = 0; // 0=none, 1=create->delete, 2=update->restore, 3=delete->restore
        Hash256 sidechain_id{};
        std::string prev_owner;
        Amount prev_amount = 0;
        Height prev_unlock_height = 0;
        Hash256 prev_checkpoint{};
        Height prev_last_checkpoint_height = 0;
        Height prev_last_sidechain_checkpoint_height = 0;
    };

    DepositUndo deposit_undo;
    // Для батч-чекпоинтов: несколько изменённых депозитов в одной транзакции
    std::vector<DepositUndo> deposit_undo_batch;

    struct SidechainUndo {
        uint8_t kind = 0; // 0=none, 1=create->delete, 2=update->restore
        Hash256 sidechain_id{};
        std::string prev_owner;
        Amount prev_limit = 0;
        Amount prev_issued = 0;
        core::SidechainKind prev_kind = core::SidechainKind::FINANCIAL;
        SidechainStatus prev_status = SidechainStatus::ACTIVE;
        Height prev_decommission_height = 0;
        uint32_t prev_multiplier = 0;
        uint32_t prev_fee_bps = 0;
        uint32_t prev_max_multiplier = 0;
        std::string prev_symbol;
        std::string prev_accounting_namespace;
        Hash256 prev_accounting_schema_hash{};
        uint8_t prev_committee_m = 0;
        uint8_t prev_committee_n = 0;
        std::vector<std::string> prev_committee_members;
        Hash256 prev_committee_id{};
        uint8_t prev_checkpoint_policy_id = 2;
        uint8_t prev_checkpoint_version = 2;
    };

    SidechainUndo sidechain_undo;

    struct FoundationUndo {
        uint8_t kind = 0; // 0=none, 1=update->restore
        std::string prev_address;
        uint8_t prev_m = 0;
        uint8_t prev_n = 0;
        std::vector<std::string> prev_participants;
    };

    FoundationUndo foundation_undo;

    struct CardUndo {
        uint8_t kind = 0; // 0=none, 1=program_create->delete, 2=card_issue->delete, 3=card_update->restore, 4=policy_update->restore
        Hash256 program_id{};
        Hash256 card_uid_hash{};
        Hash256 card_pubkey_hash{};
        std::string prev_issuer;
        uint64_t prev_valid_from = 0;
        uint64_t prev_valid_to = 0;
        uint8_t prev_status = 0;
        Hash256 prev_metadata_hash{};
        Height prev_issued_height = 0;
        Height prev_updated_height = 0;
        CardProgramEntry prev_program;
    };

    CardUndo card_undo;

    bytes serialize() const;
    static TxUndo deserialize(const bytes& data);
};

/**
 * @brief Undo данные для блока
 */
struct BlockUndo {
    Height height;                      // Высота блока
    Hash256 block_hash;                 // Хеш блока
    std::vector<TxUndo> tx_undos;       // Undo-данные для каждой транзакции
    Amount interest_pool_delta = 0;     // Сколько было добавлено в interest pool этим блоком

    bytes serialize() const;
    static BlockUndo deserialize(const bytes& data);
};

struct WitnessPubkeyStats {
    uint64_t total_keys = 0;
    uint64_t cache_hits = 0;
    uint64_t cache_misses = 0;
    uint64_t bytes_saved = 0;
};

struct IndexedTxRef {
    Hash256 txid{};
    Height height = 0;
};

/**
 * @brief Состояние блокчейна (UTXO set + балансы)
 */
class ChainState {
    friend class CardReadView;
    friend class NFTReadView;
#ifdef GEOCOIN_ENABLE_CHAINSTATE_TEST_ACCESS
    friend struct ChainStateTestAccess;
#endif
public:
    ChainState();
    explicit ChainState(std::unique_ptr<Storage> storage);

    /**
     * @brief Инициализация с хранилищем блоков
     *
     * @param storage Хранилище блоков
     */
    void set_block_storage(std::shared_ptr<storage::BlockStorage> storage);

    /**
     * @brief Заменить хранилище и загрузить его состояние заново.
     *
     * nullptr начинает пустое состояние в памяти. Политика pruning сохраняется.
     * Незавершённый batch отклоняется до замены. Ошибка загрузки делает новое
     * представление непригодным для применения транзакций до восстановления.
     * @param storage Хранилище состояния
     */
    void set_storage(std::unique_ptr<Storage> storage);

    /**
     * @brief Получить хранилище состояния
     *
     * @return Указатель на хранилище
     */
    Storage* get_storage() const { return storage_.get(); }

    /**
     * @brief Включить/выключить IBD режим записи состояния.
     */
    void set_ibd_mode(bool enabled);

    /**
     * @brief Задать интервал принудительного sync в IBD (в блоках).
     */
    void set_ibd_flush_interval(size_t blocks);

    /**
     * @brief Включить архивный режим (без pruning witness)
     */
    void set_archive_mode(bool enabled) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        archive_mode_ = enabled;
    }

    /**
     * @brief Задать глубину хранения witness (0 = default consensus)
     */
    void set_prune_depth(Height depth);

    /**
     * @brief Получить текущую глубину pruning witness
     */
    Height get_prune_depth() const {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        return prune_depth_;
    }

    /**
     * @brief Принудительный pruning witness до указанной высоты (включительно)
     */
    bool prune_witness_up_to(Height target_height, size_t max_blocks = 0);
    bool prune_block_bodies_below_height(Height keep_from_height, uint64_t* pruned_count = nullptr);

    /**
     * @brief Получить статистику pruning
     */
    bool get_pruning_stats(storage::BlockStorage::PruningStats& out) const;

    /**
     * @brief Получить witness payload для блока (если есть)
     */
    bool get_witness_payload(const Hash256& block_hash, bytes& out) const;

    /**
     * @brief Восстановить witness в блоке из payload
     */
    bool restore_witness_from_payload(const Hash256& block_hash, const bytes& payload);

    /**
     * @brief Получить высоту, до которой witness уже pruned
     */
    bool get_witness_pruned_height(Height& out) const;

    /**
     * @brief Проверить архивный режим
     */
    bool is_archive_mode() const {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        return archive_mode_;
    }

    /**
     * @brief Применить блок к состоянию
     *
     * @param block Блок для применения
     * @return true при успехе
     */
    bool apply_block(const core::Block& block, bool already_verified = false);
    // Preserve consensus rejection versus local persistence failure for fork choice.
    bool apply_block(const core::Block& block, consensus::ValidationState& validation,
                     bool already_verified = false);

    // Есть ли восстановимый derived state (для decision-логики при старте node/blockchain).
    bool has_derived_state_snapshot() const;
    bool prepare_rebuild();
    bool finish_rebuild();
    bool has_utxo_count_snapshot() const;

    /**
     * @brief Откатить блок (для реорганизации)
     *
     * @param block Блок для отката
     * @return true при успехе
     */
    bool revert_block(const core::Block& block);

    /**
     * @brief Применить транзакцию к состоянию
     *
     * @param tx Транзакция
     * @param height Высота блока
     * @param is_coinbase Coinbase-транзакция?
     * @return true при успехе
     */
    bool apply_transaction(
        const core::Transaction& tx,
        Height height,
        bool is_coinbase = false,
        consensus::ValidationState* validation = nullptr,
        const Hash256* known_txid = nullptr,
        bool prevalidated = false
    );

    /**
     * @brief Проверить, что транзакция валидна
     *
     * Проверяет:
     * - Все входы существуют в UTXO set
     * - Подписи корректны
     * - Сумма входов >= сумма выходов
     *
     * @param tx Транзакция
     * @return true если валидна
     */
    bool validate_transaction(const core::Transaction& tx) const;

    /**
     * @brief Получить баланс адреса
     *
     * @param address Адрес
     * @return Баланс в angles
     */
    Amount get_balance(const Address& address) const;

    /**
     * @brief Получить все UTXO для адреса
     *
     * @param address Адрес
     * @return Список UTXO
     */
    std::vector<UTXO> get_utxos_for_address(const Address& address) const;
    std::vector<UTXO> get_utxos_for_address_page(
        const Address& address,
        size_t offset,
        size_t limit,
        size_t* total = nullptr
    ) const;

    /**
     * @brief Получить общее количество UTXO
     *
     * @return Количество UTXO
     */
    size_t get_utxo_count() const {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        return utxo_set_.size();
    }

    /**
     * @brief Получить текущую высоту блокчейна
     *
     * @return Высота
     */
    Height get_height() const {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        return current_height_;
    }

    /**
     * @brief Получить сколько уже потрачено по delegation proof (для UI/лимитов)
     */
    Amount get_delegation_total_spent(const Hash256& delegation_proof_txid) const;

    /**
     * @brief Получить хеш tip-блока, к которому сейчас привязан UTXO set
     *
     * Важно:
     * - Высота сама по себе недостаточна (на одной высоте могут быть разные ветки).
     * - Этот хеш используется при старте ноды для проверки, что ChainState соответствует
     *   активной цепи (chain_active.tip()).
     */
    Hash256 get_tip_hash() const {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        return current_tip_hash_;
    }

    /**
     * Thread-safe value snapshot used when validation happens outside the
     * mempool lock and must be tied to one ChainState tip.
     */
    Hash256 get_tip_hash_snapshot() const {
        return get_tip_hash();
    }

    /**
     * @brief Установить высоту блокчейна
     *
     * @param height Высота
     */
    bool set_height(Height height);

    /**
     * @brief Установить tip hash (и сохранить в storage при наличии)
     */
    bool set_tip_hash(const Hash256& tip_hash);

    /**
     * @brief Проверить наличие блока
     *
     * @param block_hash Хеш блока
     * @return true если блок есть
     */
    bool has_block(const Hash256& block_hash) const;

    /**
     * @brief Получить блок по хешу
     *
     * @param block_hash Хеш блока
     * @param block [out] Блок
     * @return true если найден
     */
    bool get_block(const Hash256& block_hash, core::Block& block) const;

    /**
     * @brief Получить блок по высоте
     *
     * @param height Высота
     * @param block [out] Блок
     * @return true если найден
     */
    bool get_block_by_height(Height height, core::Block& block) const;

    bool get_active_block_hash(Height height, Hash256& hash) const;
    bool is_block_body_pruned(Height height) const;

    /**
     * @brief Получить транзакцию по хешу
     *
     * Ищет транзакцию в блоках через block_storage.
     *
     * @param tx_hash Хеш транзакции
     * @return Указатель на транзакцию или nullptr если не найдена
     */
    std::unique_ptr<core::Transaction> get_transaction(const Hash256& tx_hash) const;
    bool has_active_transaction(const Hash256& tx_hash) const;

    /**
     * @brief Получить ранее подтвержденную checkpoint-транзакцию для fraud proof.
     *
     * В отличие от get_transaction(), этот внутренний consensus lookup может
     * читать raw transaction store после reorg. Публичные RPC по-прежнему
     * используют только active-chain индекс и не раскрывают orphan history.
     */
    std::unique_ptr<core::Transaction> get_transaction_for_fraud_proof(
        const Hash256& tx_hash
    ) const;

    /**
     * @brief Получить активные tx refs для адреса.
     */
    std::vector<IndexedTxRef> get_address_txids(const Address& address) const;

    /** Copy one bounded page of active address history. */
    std::vector<IndexedTxRef> get_address_txids_page(
        const Address& address,
        size_t offset,
        size_t limit,
        size_t* total = nullptr
    ) const;

    /**
     * @brief Получить активные tx refs для NFT id.
     */
    std::vector<IndexedTxRef> get_nft_txids(const Hash256& nft_id) const;

    /** Copy one bounded page of active NFT history. */
    std::vector<IndexedTxRef> get_nft_txids_page(
        const Hash256& nft_id,
        size_t offset,
        size_t limit,
        size_t* total = nullptr
    ) const;

    /**
     * @brief Получить UTXO (публичный метод)
     *
     * @param tx_hash Хеш транзакции
     * @param vout Индекс выхода
     * @param utxo [out] UTXO
     * @return true если найден
     */
    bool get_utxo(const Hash256& tx_hash, uint32_t vout, UTXO& utxo) const;

    /** Thread-safe existence check which never exposes map-owned storage. */
    bool has_utxo(const Hash256& tx_hash, uint32_t vout) const;

    /**
     * @brief Вычислить комиссию транзакции
     *
     * Вычисляет комиссию как разницу между суммой входов и суммой выходов.
     * Требует доступа к UTXO set для получения сумм входов.
     *
     * @param tx Транзакция
     * @return Комиссия (сумма входов - сумма выходов), или 0 если невалидна
     */
    Amount calculate_transaction_fee(const core::Transaction& tx) const;

    /**
     * Fees of transactions[first_index..] as they will be charged when applied
     * in order at applying_height. A sidechain registry created or removed by an
     * earlier transaction in the same block changes the fee policy of every
     * later MINT_SIDECHAIN/REDEEM_SIDECHAIN, so the committed registry alone is
     * not enough to price the coinbase.
     */
    std::vector<Amount> project_block_transaction_fees(
        const std::vector<core::Transaction>& transactions,
        size_t first_index,
        Height applying_height) const;

    /**
     * @brief Получить общее количество монет в обращении
     *
     * @return Количество angles
     */
    Amount get_total_supply() const;

    /**
     * @brief Текущий баланс протокольного interest pool (в angles)
     */
    Amount get_interest_pool_balance() const;

    /**
     * @brief Собрать каноническую INTEREST_DISTRIBUTION транзакцию для next_height.
     *
     * Возвращает false, если распределение сейчас не требуется или не может быть построено.
     */
    bool create_interest_distribution_transaction(Height next_height, core::Transaction& out_tx) const;

    /**
     * @brief Очистить состояние
     */
    void clear();

    /**
     * @brief Сохранить undo данные для блока
     *
     * @param undo Undo данные
     * @return true при успехе
     */
    bool save_block_undo(const BlockUndo& undo);

    /**
     * @brief Загрузить undo данные для блока
     *
     * @param block_hash Хеш блока
     * @param undo [out] Undo данные
     * @return true если найдены
     */
    bool load_block_undo(const Hash256& block_hash, BlockUndo& undo) const;

    /**
     * Bound persistent undo history to the permitted reorganization window.
     * A depth of zero retains all undo records for explicitly enabled deep reorgs.
     */
    void set_undo_retention_depth(Height depth);

    /**
     * @brief Получить хранилище angle commitments
     */
    std::shared_ptr<multisig::AngleCommitmentStore> get_angle_store() const {
        return angle_store_;
    }

    /**
     * @brief Добавить ANGLE_COMMITMENT в хранилище (для mempool)
     *
     * @param tx ANGLE_COMMITMENT транзакция
     */
    void add_angle_commitment_to_store(const core::Transaction& tx);

    /**
     * @brief Удалить неподтвержденный ANGLE_COMMITMENT из хранилища mempool
     *
     * Если commitment уже подтвержден в активной цепочке, он остается в store.
     *
     * @param tx_hash Хеш ANGLE_COMMITMENT транзакции
     */
    void remove_mempool_angle_commitment_from_store(const Hash256& tx_hash);

    /**
     * @brief Проверить, отозван ли delegation proof
     *
     * Ищет в блокчейне транзакцию REVOKE_DELEGATION для данного delegation proof.
     * Не требует хранилища в памяти - просто сканирует блокчейн.
     *
     * @param delegation_proof_txid Хеш delegation proof транзакции
     * @return true если отозван
     */
    bool is_delegation_revoked(const Hash256& delegation_proof_txid) const;

    // === Анти-replay индексы консенсуса ===
    bool has_angle_commitment_key(const Hash256& key) const;
    bool has_multisig_spend_key(const Hash256& key) const;
    bool has_protocol_authorization_key(const Hash256& key) const;
    bool has_hierarchical_spend_key(const Hash256& key) const;
    bool has_delegation_proof_key(const Hash256& key) const;
    bool has_revoke_delegation_key(const Hash256& delegation_proof_txid) const;
    bool has_nft_id_key(const Hash256& nft_id) const;

    bool has_sidechain_deposit_key(const Hash256& sidechain_id) const;
    bool get_sidechain_deposit(const Hash256& sidechain_id, SidechainDeposit& out) const;

    bool has_sidechain_registry_key(const Hash256& sidechain_id) const;
    bool has_accounting_namespace(const std::string& accounting_namespace) const;
    bool get_sidechain_registry(const Hash256& sidechain_id, SidechainRegistryEntry& out) const;
    bool has_witness_pubkey(const Hash256& pubkey_hash) const;
    bool get_witness_pubkey(const Hash256& pubkey_hash, bytes& out_pubkey) const;
    WitnessPubkeyStats get_witness_pubkey_stats() const;
    bool get_sidechain_registry_by_id_hex(const std::string& sidechain_id_hex, SidechainRegistryEntry& out) const;

    Address get_foundation_address() const;
    FoundationConfig get_foundation_config() const;

    std::vector<Hash256> get_owned_nfts(const Address& owner) const;
    std::vector<Hash256> get_owned_nfts_page(
        const Address& owner,
        size_t offset,
        size_t limit,
        size_t* total_out = nullptr
    ) const;
    Address get_nft_owner(const Hash256& nft_id) const;
    nft::NFTMetadata get_nft_metadata(const Hash256& nft_id) const;
    bool nft_exists(const Hash256& nft_id) const;

    // Сбрасывает epoch для anti-replay индексов. Используется при rebuild ChainState.
    void reset_index_epoch();

    // Local transaction-admission policy. It must never affect block validity,
    // otherwise two nodes with different local files can split consensus.
    void set_activation_manifest(const utils::ActivationManifest& manifest);

private:
    Timestamp transaction_validation_time_() const;
    bool validate_transaction_impl(
        const core::Transaction& tx,
        bool enforce_local_feature_policy,
        std::optional<Height> applying_height = std::nullopt,
        const Hash256* known_txid = nullptr,
        bool syntax_already_checked = false
    ) const;

    struct JournalEntry {
        uint8_t version = 1;
        uint8_t phase = 0;
        Height height = 0;
        Hash256 block_hash{};
    };

    bool write_state_journal(Height height, const Hash256& block_hash, uint8_t phase);
    bool clear_state_journal();
    bool load_state_journal(JournalEntry& out) const;
    bool recover_from_journal_if_needed();
    bool recover_from_storage_mismatch();
    void load_chain_position_from_storage();
    bool purge_state_metadata_for_rebuild();
    void load_utxo_set_from_storage();
    bool persist_utxo_record_(const UTXO& utxo, bool erase);

    mutable std::recursive_mutex mutex_;

    // Набор UTXO: "tx_hash:vout" -> UTXO
    std::unordered_map<UtxoKey, UTXO, UtxoKeyHash> utxo_set_;
    struct HeightAddressBalances {
        std::unordered_map<std::string, Amount> by_address;
        Amount total = 0;
    };
    std::map<Height, HeightAddressBalances> utxo_balance_by_height_;
    bool utxo_balance_index_valid_ = true;

    // Текущая высота блокчейна
    Height current_height_;
    Hash256 current_tip_hash_{};

    // Хранилище блоков
    std::shared_ptr<storage::BlockStorage> block_storage_;

    // Хранилище состояния (chainstate storage или Memory)
    std::shared_ptr<Storage> storage_;

    // Журнал undo: block_hash -> BlockUndo (в памяти, позже в chainstate storage)
    // Used only without Storage; otherwise its batch view is the undo authority.
    std::map<std::string, BlockUndo> undo_log_;

    // Хранилище angle commitments для мультиподписи
    std::shared_ptr<multisig::AngleCommitmentStore> angle_store_;

    // Верификатор мультиподписи
    std::shared_ptr<multisig::MultisigVerifier> multisig_verifier_;

    // БЕЗОПАСНОСТЬ: Отслеживание использованных nonce для delegation proof
    // primary_address -> набор использованных nonce
    mutable std::map<std::string, std::set<uint32_t>> used_delegation_nonces_;

    // БЕЗОПАСНОСТЬ: Отслеживание потраченных средств по delegation proof
    // delegation_proof_txid -> суммарно потрачено
    std::map<std::string, DelegationState> delegations_;

    // БЕЗОПАСНОСТЬ: Кеш отозванных delegation proof (для быстрой проверки O(1))
    mutable std::set<std::string> revoked_delegations_cache_;

    // Анти-replay индексы консенсуса (hex-ключи)
    mutable std::set<std::string> angle_commitment_keys_;
    mutable std::set<std::string> multisig_spend_keys_;
    mutable std::set<std::string> protocol_authorization_keys_;
    mutable std::set<std::string> hierarchical_spend_keys_;
    mutable std::set<std::string> delegation_proof_keys_;
    mutable std::set<std::string> nft_id_keys_;

    // Состояние NFT для консенсуса (в памяти). Персистится как metadata при наличии storage_.
    nft::NFTRegistryImpl nft_registry_;

    // Состояние сайдчейн-депозитов (в памяти). Персистится как metadata при наличии storage_.
    std::map<std::string, SidechainDeposit> sidechain_deposits_;
    std::map<std::string, SidechainRegistryEntry> sidechain_registry_;
    std::map<std::string, CardProgramEntry> card_programs_;      // program_id(hex) -> card program
    std::map<std::string, CardInstanceEntry> card_instances_;    // card_uid_hash(hex) -> card instance
    std::map<std::string, std::string> card_pubkey_to_uid_;      // card_pubkey_hash(hex) -> card_uid_hash(hex)
    // Derived in memory from card records; suspended cards also occupy a slot.
    std::map<std::string, size_t> card_active_counts_;
    // Без Storage храним ту же привязку транзакции к блоку, что и в atx:.
    mutable std::unordered_map<Hash256, storage::ActiveTransactionRecord, Hash256Hash> active_tx_index_;
    mutable std::unordered_map<std::string, std::vector<IndexedTxRef>> active_address_tx_index_;
    mutable std::unordered_map<std::string, std::vector<IndexedTxRef>> active_nft_tx_index_;
    mutable std::map<std::string, bytes> witness_pubkey_cache_;
    mutable std::unordered_map<std::string, uint64_t> witness_pubkey_refcounts_;
    mutable std::list<std::string> witness_pubkey_lru_;
    mutable std::unordered_map<std::string, std::list<std::string>::iterator> witness_pubkey_lru_index_;
    mutable uint64_t witness_pubkey_cache_hits_{0};
    mutable uint64_t witness_pubkey_cache_misses_{0};
    mutable uint64_t witness_pubkey_bytes_saved_{0};
    mutable uint64_t witness_pubkey_stats_dirty_events_{0};
    static constexpr uint64_t WITNESS_STATS_FLUSH_INTERVAL = 1024;
    static constexpr size_t MAX_WITNESS_CACHE_SIZE = 100000;

    FoundationConfig foundation_config_;
    Amount interest_pool_balance_{0};

    // Epoch для персистентных anti-replay индексов. Ключи пишутся как idx:<epoch>:*
    std::string idx_epoch_;

    // Контекстная timestamp "текущего применяемого блока" для консенсусной валидации транзакций.
    // Заполняется в apply_block() и используется в validate_transaction() (например, для delegation time windows).
    std::optional<Timestamp> validation_block_time_;

    // Архивный режим: хранить witness всегда
    bool archive_mode_{false};

    // Глубина хранения witness (в блоках)
    Height prune_depth_{0};

    // Кеш witness для недавних блоков
    mutable std::mutex witness_cache_mutex_;
    size_t witness_cache_max_{256};
    std::deque<std::string> witness_cache_order_;
    std::unordered_map<std::string, bytes> witness_cache_;

    bool journal_recovery_required_{false};
    bool chain_position_loaded_from_storage_{false};
    // False when a persisted consensus-derived record was malformed, missing
    // its pair, or internally inconsistent. Blockchain startup must rebuild
    // instead of treating a partial snapshot as authoritative.
    bool derived_state_storage_valid_{true};
    // Runtime apply/revert failures may leave the in-memory view only partly
    // changed. Unlike a malformed startup snapshot, no direct mutation or
    // validation is safe until clear()+active-chain rebuild completes.
    bool state_transition_poisoned_{false};
    // Every persistence write performed by one state transition contributes to
    // this result. Helpers which update memory and storage must not silently
    // turn a failed backend operation into a successful block application.
    bool transition_persistence_ok_{true};
    bool transition_in_progress_{false};
    Height undo_retention_depth_{0};
    // Operator-controlled, local-only policy used by validate_transaction().
    // apply_block()/apply_transaction() deliberately bypass this gate while
    // retaining every deterministic transaction and state validation rule.
    utils::ActivationManifest activation_manifest_{};

    static std::string hash256_to_hex(const Hash256& h);

    std::string idx_meta_key(const std::string& kind, const std::string& hex) const;
    void load_index_epoch_from_storage();
    void load_delegations_from_storage_();
    bool persist_delegation_(const std::string& txid);
    bool resolve_angle_commitments_(const std::vector<Hash256>& hashes) const;
    static std::string generate_index_epoch();

    void load_nft_registry_from_storage();
    bool persist_nft_owner_to_storage(const Hash256& nft_id, const Address& owner);
    bool persist_nft_metadata_to_storage(const Hash256& nft_id, const nft::NFTMetadata& metadata);
    bool delete_nft_from_storage(const Hash256& nft_id);

    void load_sidechain_deposits_from_storage();
    void persist_sidechain_deposit_to_storage(const Hash256& sidechain_id, const SidechainDeposit& deposit);
    void delete_sidechain_deposit_from_storage(const Hash256& sidechain_id);

    void load_sidechain_registry_from_storage();
    void persist_sidechain_registry_to_storage(const Hash256& sidechain_id, const SidechainRegistryEntry& entry);
    void delete_sidechain_registry_from_storage(const Hash256& sidechain_id);

    void load_card_registry_from_storage();
    void persist_card_program_to_storage(const Hash256& card_program_id, const CardProgramEntry& entry);
    void delete_card_program_from_storage(const Hash256& card_program_id);
    void persist_card_instance_to_storage(const Hash256& card_uid_hash, const CardInstanceEntry& entry);
    void delete_card_instance_from_storage(const Hash256& card_uid_hash);

    void reset_foundation_config_to_defaults_();
    void load_foundation_config_from_storage();
    void persist_foundation_config_to_storage();
    void load_interest_pool_balance_from_storage();
    void persist_interest_pool_balance_to_storage();
    bool persist_utxo_count_to_storage();
    void load_witness_pubkeys_from_storage();
    void persist_witness_pubkey_to_storage(const Hash256& pubkey_hash, const bytes& pubkey);
    void delete_witness_pubkey_from_storage(const Hash256& pubkey_hash);
    void register_witness_pubkeys_from_tx(const core::Transaction& tx);
    void rebuild_witness_pubkey_cache_from_active_chain();
    // Caller holds mutex_; the returned cache entry remains valid under that lock.
    const bytes* find_witness_pubkey_(const Hash256& pubkey_hash, bool& cache_hit) const;
    uint64_t witness_refcount_get_(const std::string& hex) const;
    void witness_refcount_set_(const std::string& hex, uint64_t count);
    bool witness_refcount_decrement_(const std::string& hex);
    void witness_cache_touch_lru_(const std::string& hex) const;
    void witness_cache_put_(const std::string& hex, bytes pubkey) const;
    void witness_cache_evict_if_needed_() const;
    void load_witness_pubkey_stats_from_storage();
    void persist_witness_pubkey_stats_to_storage(bool force) const;

    bool build_interest_distribution_outputs_(
        Height distribution_height,
        std::vector<core::TxOutput>& outputs,
        Amount& distributed_total
    ) const;

    // Internal transition helpers. The caller must hold mutex_.
    bool process_standard_utxo_effects_(
        const core::Transaction& tx,
        Height height,
        bool mutate,
        bool require_witness
    );
    bool apply_standard_utxo_effects_(
        const core::Transaction& tx,
        Height height,
        bool is_coinbase,
        consensus::ValidationState* validation = nullptr,
        Amount* spent_input_total = nullptr
    );
    bool apply_nft_transaction_(const core::Transaction& tx, Height height);
    bool validate_nft_transaction_(const core::Transaction& tx) const;
    bool apply_sidechain_transaction_(const core::Transaction& tx, Height height);
    // relay_hardening enables the hardening soft-fork rules before their
    // activation height; relay validation always enforces them.
    bool validate_sidechain_transaction_(const core::Transaction& tx, bool relay_hardening = false) const;
    bool apply_card_transaction_(const core::Transaction& tx, Height height);
    bool validate_card_transaction_(const core::Transaction& tx) const;
    void cache_card_instance_(const CardInstanceEntry& card);
    void erase_card_instance_(const Hash256& uid);
    void prepare_nft_undo_(const core::Transaction& tx, TxUndo& tx_undo) const;
    bool revert_nft_undo_(const TxUndo& tx_undo);
    void prepare_sidechain_undo_(const core::Transaction& tx, TxUndo& tx_undo) const;
    bool revert_sidechain_undo_(const TxUndo& tx_undo);
    void prepare_card_undo_(const core::Transaction& tx, TxUndo& tx_undo) const;
    bool revert_card_undo_(const TxUndo& tx_undo);
    void prepare_foundation_undo_(const core::Transaction& tx, TxUndo& tx_undo) const;
    bool validate_foundation_undo_(const core::Block& block, const BlockUndo& undo) const;
    bool revert_foundation_undo_(const TxUndo& tx_undo);
    bool apply_foundation_transaction_(const core::Transaction& tx, Height height);
    bool validate_foundation_transaction_(
        const core::Transaction& tx,
        Height evaluation_height,
        bool relay_hardening
    ) const;
    bool apply_interest_distribution_(const core::Transaction& tx, Height height);
    bool interest_distribution_matches_(const core::Transaction& tx, Height height) const;
    bool validate_interest_distribution_(const core::Transaction& tx) const;
    bool revert_interest_distribution_(const core::Transaction& tx);
    bool apply_angle_commitment_(const core::Transaction& tx, Height height);
    bool apply_delegation_proof_(const core::Transaction& tx, Height height);
    bool apply_revoke_delegation_(const core::Transaction& tx, Height height);
    bool apply_multisig_spend_effects_(const core::Transaction& tx, Amount total_input);
    bool validate_angle_commitment_(const core::Transaction& tx) const;
    bool validate_delegation_proof_(const core::Transaction& tx) const;
    bool validate_revoke_delegation_(const core::Transaction& tx) const;
    bool validate_multisig_spend_(const core::Transaction& tx) const;
    bool revert_delegation_nonce_(const core::Transaction& tx);
    bool revert_multisig_indexes_(const core::Transaction& tx, const TxUndo& tx_undo);
    bool revert_multisig_hierarchical_spent_(const core::Transaction& tx, const TxUndo& undo);
    bool verify_multisig_control_fee_(
        const core::Transaction& tx,
        const Address& owner,
        Height evaluation_height
    ) const;
    bool verify_owned_utxo_inputs_(
        const core::Transaction& tx,
        Amount* actual_fee = nullptr
    ) const;
    bool collect_spendable_inputs_(
        const core::Transaction& tx,
        Height height,
        Amount& total_input,
        std::vector<UTXO>& inputs_to_spend
    ) const;
    // Caller must hold mutex_. This pointer never crosses the public API.
    const UTXO* find_utxo_unlocked_(const Hash256& tx_hash, uint32_t vout) const;
    bool spend_collected_inputs_(const std::vector<UTXO>& inputs_to_spend);
    bool add_tx_outputs_as_utxos_(
        const core::Transaction& tx,
        const Hash256& tx_hash,
        Height height
    );
    bool store_applied_transaction_(const Hash256& tx_hash, const core::Transaction& tx);
    bool record_persistence_result_(bool success, const char* operation);
    bool prune_expired_undo_(Height applying_height);
    bool prune_expired_undo_history_();
    // Caller holds mutex_; earlier transactions provide same-block input values.
    Amount calculate_transaction_fee_(const core::Transaction& tx,
        const std::map<Hash256, const core::Transaction*>* earlier_transactions = nullptr) const;

    void cache_witness_payload(const Hash256& block_hash, const bytes& payload);
    bool active_tx_record_(const Hash256& txid, storage::ActiveTransactionRecord& record) const;
    void rebuild_active_tx_index_from_active_chain();
    void persist_active_tx_index_for_block(
        const core::Block& block,
        Height height,
        std::span<const TxUndo> tx_undos,
        std::span<const Hash256> txids = {});
    void adopt_utxo_memory_(const UTXO& utxo);
    void track_utxo_inserted_(const UTXO& utxo);
    void track_utxo_erased_(const UTXO& utxo);
    void erase_active_tx_index_for_block(const core::Block& block);
    void index_query_refs_for_tx_(
        const core::Transaction& tx,
        Height height,
        std::span<const UTXO> spent_utxos,
        const Hash256& txid);
    void erase_query_refs_for_tx_(const Hash256& txid);

    /**
     * @brief Добавить UTXO в set
     */
    void add_utxo(const UTXO& utxo);

    /**
     * @brief Удалить UTXO из set
     */
    bool remove_utxo(const Hash256& tx_hash, uint32_t vout);

    /**
     * @brief Откатить транзакцию используя undo данные
     *
     * @param tx Транзакция
     * @param tx_undo Undo данные
     * @return true при успехе
     */
    bool revert_transaction(const core::Transaction& tx, const TxUndo& tx_undo);
};

} // namespace geocoin::state
