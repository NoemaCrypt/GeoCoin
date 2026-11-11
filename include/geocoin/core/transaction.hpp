#pragma once

#include "geocoin/types.hpp"
#include "geocoin/crypto/geo_area.hpp"
#include "geocoin/nft/nft_types.hpp"
#include <vector>
#include <optional>

namespace geocoin::core {

class Transaction;

// Complete protocol intent before owner/committee/UTXO signatures are attached.
bytes serialize_protocol_authorization(const Transaction& tx);
struct BatchCheckpointItem;

bytes build_sidechain_payload_for_signing(const Transaction& tx);
bytes build_sidechain_checkpoint_payload_for_signing(const Transaction& tx);
bytes build_sidechain_committee_update_payload_for_signing(const Transaction& tx);
bytes build_foundation_update_payload_for_signing(const Transaction& tx);
bytes build_sidechain_batch_checkpoint_payload_for_signing(const BatchCheckpointItem& item);

/**
 * @brief Типы транзакций
 */
enum class TransactionType : uint8_t {
    STANDARD = 0,           // Обычная транзакция
    ANGLE_COMMITMENT = 1,   // Публикация углов для мультиподписи
    MULTISIG_SPEND = 2,     // Трата с мультиподпись адреса
    DELEGATION_PROOF = 3,   // Доказательство делегирования для иерархической мультиподписи
    REVOKE_DELEGATION = 4,  // Отзыв delegation proof

    LOCK_DEPOSIT = 30,      // Блокировка депозита для сайдчейна
    SPEND_DEPOSIT = 31,     // Списание комиссии из депозита сайдчейна
    UNLOCK_DEPOSIT = 32,    // Разблокировка депозита после срока
    ISSUE_SIDECHAIN = 33,   // Регистрация сайдчейна и лимита эмиссии
    MINT_SIDECHAIN = 34,    // Увеличение выпущенных токенов сайдчейна
    REDEEM_SIDECHAIN = 35,  // Ручной выкуп/погашение токенов сайдчейна
    UPDATE_FOUNDATION_ADDRESS = 36, // Обновление адреса фонда по подписям
    SIDECHAIN_CHECKPOINT = 37, // Чекпоинт сайдчейна (авто-списание комиссии)
    UPDATE_SIDECHAIN_COMMITTEE = 38, // Обновление комитета сайдчейна
    SLASH_DEPOSIT = 39, // Конфискация части депозита при мошенничестве
    BATCH_SIDECHAIN_CHECKPOINT = 40, // Батч чекпоинтов зарегистрированных сайдчейнов
    INTEREST_DISTRIBUTION = 41, // Квартальное распределение interest pool держателям
    CARD_PROGRAM_DEFINE = 42,   // Профиль ПО/чипа карты (якорь версии/хеша applet)
    CARD_ISSUE = 43,            // Выпуск конкретной карты по профилю
    CARD_UPDATE_STATUS = 44,    // Изменение статуса карты (suspend/revoke)
    DECOMMISSION_SIDECHAIN = 45, // Начало декомиссии сайдчейна
    REGISTER_ACCOUNTING_SIDECHAIN = 46, // Регистрация учетного сайдчейна без L1-эмиссии
    TOP_UP_SIDECHAIN_DEPOSIT = 47, // Пополнение существующего депозита сайдчейна
    CARD_UPDATE_POLICY = 48,     // Смена корпоративной issuer policy карточной программы

    CADASTRAL_DOC = 20,     // Геопечать документа (кадастр)

    NFT_CREATE = 10,
    NFT_TRANSFER = 11,
    NFT_BURN = 12
};

inline bool is_sidechain_transition_type(TransactionType type) {
    switch (type) {
        case TransactionType::LOCK_DEPOSIT:
        case TransactionType::TOP_UP_SIDECHAIN_DEPOSIT:
        case TransactionType::SPEND_DEPOSIT:
        case TransactionType::UNLOCK_DEPOSIT:
        case TransactionType::ISSUE_SIDECHAIN:
        case TransactionType::REGISTER_ACCOUNTING_SIDECHAIN:
        case TransactionType::MINT_SIDECHAIN:
        case TransactionType::REDEEM_SIDECHAIN:
        case TransactionType::SIDECHAIN_CHECKPOINT:
        case TransactionType::UPDATE_SIDECHAIN_COMMITTEE:
        case TransactionType::SLASH_DEPOSIT:
        case TransactionType::BATCH_SIDECHAIN_CHECKPOINT:
        case TransactionType::DECOMMISSION_SIDECHAIN:
            return true;
        default:
            return false;
    }
}

inline bool is_card_transition_type(TransactionType type) {
    switch (type) {
        case TransactionType::CARD_PROGRAM_DEFINE:
        case TransactionType::CARD_ISSUE:
        case TransactionType::CARD_UPDATE_STATUS:
        case TransactionType::CARD_UPDATE_POLICY:
            return true;
        default:
            return false;
    }
}

inline bool is_nft_transition_type(TransactionType type) {
    switch (type) {
        case TransactionType::NFT_CREATE:
        case TransactionType::NFT_TRANSFER:
        case TransactionType::NFT_BURN:
            return true;
        default:
            return false;
    }
}

inline bool is_multisig_family_type(TransactionType type) {
    switch (type) {
        case TransactionType::ANGLE_COMMITMENT:
        case TransactionType::MULTISIG_SPEND:
        case TransactionType::DELEGATION_PROOF:
        case TransactionType::REVOKE_DELEGATION:
            return true;
        default:
            return false;
    }
}

inline bool is_known_transaction_type(TransactionType type) {
    switch (type) {
        case TransactionType::STANDARD:
        case TransactionType::ANGLE_COMMITMENT:
        case TransactionType::MULTISIG_SPEND:
        case TransactionType::DELEGATION_PROOF:
        case TransactionType::REVOKE_DELEGATION:
        case TransactionType::LOCK_DEPOSIT:
        case TransactionType::SPEND_DEPOSIT:
        case TransactionType::UNLOCK_DEPOSIT:
        case TransactionType::ISSUE_SIDECHAIN:
        case TransactionType::MINT_SIDECHAIN:
        case TransactionType::REDEEM_SIDECHAIN:
        case TransactionType::UPDATE_FOUNDATION_ADDRESS:
        case TransactionType::SIDECHAIN_CHECKPOINT:
        case TransactionType::UPDATE_SIDECHAIN_COMMITTEE:
        case TransactionType::SLASH_DEPOSIT:
        case TransactionType::BATCH_SIDECHAIN_CHECKPOINT:
        case TransactionType::INTEREST_DISTRIBUTION:
        case TransactionType::CARD_PROGRAM_DEFINE:
        case TransactionType::CARD_ISSUE:
        case TransactionType::CARD_UPDATE_STATUS:
        case TransactionType::CARD_UPDATE_POLICY:
        case TransactionType::DECOMMISSION_SIDECHAIN:
        case TransactionType::REGISTER_ACCOUNTING_SIDECHAIN:
        case TransactionType::TOP_UP_SIDECHAIN_DEPOSIT:
        case TransactionType::CADASTRAL_DOC:
        case TransactionType::NFT_CREATE:
        case TransactionType::NFT_TRANSFER:
        case TransactionType::NFT_BURN:
            return true;
        default:
            return false;
    }
}

enum class SidechainKind : uint8_t {
    FINANCIAL = 0,
    ACCOUNTING = 1,
};

inline const char* sidechain_kind_to_string(SidechainKind kind) {
    switch (kind) {
        case SidechainKind::FINANCIAL:
            return "financial";
        case SidechainKind::ACCOUNTING:
            return "accounting";
    }
    return "unknown";
}

/**
 * @brief Вход транзакции (ссылка на UTXO)
 */
struct TxInput {
    static constexpr uint32_t SEQUENCE_FINAL = 0xFFFFFFFFu;
    static constexpr uint32_t SEQUENCE_LOCKTIME = SEQUENCE_FINAL - 1;
    Hash256 prev_tx_hash{}; // Хеш предыдущей транзакции
    uint32_t vout = 0;     // Индекс выхода в предыдущей транзакции
    // Opaque signed bytes; coinbase commits height here. No script interpreter.
    bytes script_sig;
    // Any value below FINAL enables absolute lock_time. No relative delays.
    uint32_t sequence = SEQUENCE_FINAL;

    // Подпись Falcon-512 для ЭТОГО входа (используется только в ANGLE_COMMITMENT).
    Signature signature;

    // Хеш witness-подписи для остальных типов. У ANGLE_COMMITMENT всегда ноль.
    Hash256 signature_hash{};

    bytes serialize() const;
};

struct TxWitness {
    Hash256 public_key_hash{};
    bytes public_key;
    Signature signature;
};

struct CardAuthority {
    uint8_t quorum = 0;
    std::vector<Address> members;
};

struct CardCorporatePolicy {
    uint8_t version = 0;  // 0 = single card_issuer input; 1 = role quorums (adr/0001)
    CardAuthority mint;
    CardAuthority freeze;
    CardAuthority operations;
    CardAuthority recovery;
    Hash256 compliance_policy_hash{};
    uint32_t max_active_cards = 0;
};

/**
 * @brief Выход транзакции (UTXO)
 */
struct TxOutput {
    Amount value = 0;      // Количество angles
    Address address;       // Адрес получателя
    bytes script_pubkey;   // Opaque signed bytes; ownership is defined by address.

    bytes serialize() const;
};

/**
 * @brief Элемент батча чекпоинтов сайдчейна
 */
struct BatchCheckpointItem {
    Hash256 sidechain_id{};
    Hash256 checkpoint{};
    Height checkpoint_height = 0; // Высота сайдчейна
    Amount fee = 0;               // Комиссия, списывается из депозита
    uint8_t version = 2;          // 2 = financial, 1 = accounting (проверяется по registry)
    Hash256 committee_id{};
    std::vector<Signature> committee_signatures;
    bytes proof;                  // Для будущих версий (merkle proof и т.п.)
};

/**
 * @brief Транзакция GeoCoin (Falcon-512 signatures)
 */
class Transaction {
public:
    uint32_t version = 2;
    TransactionType type = TransactionType::STANDARD;  // Тип транзакции
    uint64_t timestamp = 0;        // Временная метка транзакции
    std::vector<TxInput> inputs;   // Входы (каждый со своей подписью)
    std::vector<TxOutput> outputs;
    // 0 disables the lock. Below 500000000: height; otherwise block Unix time.
    // The threshold must be strictly passed unless a nonempty input set is all FINAL.
    uint32_t lock_time = 0;
    uint8_t witness_version = 1;   // Версия witness-формата (консенсусное поле)
    std::vector<TxWitness> witnesses;

    // === Поля для ANGLE_COMMITMENT ===
    std::string commitment_network_id; // Явная сеть domain-separated подписи
    Address multisig_address;      // Мультиподпись адрес
    Hash256 spending_tx_hash{};    // Хеш транзакции, которую подписываем
    Address signer_address;        // Адрес подписанта

	    // === Поля для MULTISIG_SPEND ===
	    std::vector<Hash256> angle_commitment_hashes;  // Ссылки на angle commitments
	    uint8_t m = 0;  // Минимум подписей (для MULTISIG_SPEND)
	    uint8_t n = 0;  // Всего участников (для MULTISIG_SPEND)
	    // Участники non-hierarchical MULTISIG_SPEND (P2SH-подобный preimage конфига).
	    // Начиная с версии транзакции >= 2, для обычного (не hierarchical) MULTISIG_SPEND
	    // этот список ДОЛЖЕН присутствовать и хешироваться, чтобы адрес GEOM... проверялся trustless.
	    std::vector<Address> spend_participants;

    // === Поля для DELEGATION_PROOF ===
    Address primary_multisig_address;      // Основной мультисиг (постоянный)
    Address delegated_multisig_address;    // Делегированный мультисиг (меняется)
    uint8_t primary_m = 0;                 // M для primary multisig
    uint8_t primary_n = 0;                 // N для primary multisig
    std::vector<Address> primary_participants;  // Участники primary multisig
    uint8_t delegated_m = 0;               // M для делегированного
    uint8_t delegated_n = 0;               // N для делегированного
    std::vector<Address> delegated_participants; // Участники delegated multisig
    uint64_t valid_from = 0;               // Timestamp начала действия
    uint64_t valid_until = 0;              // Timestamp окончания (0 = бессрочно)
    uint32_t nonce = 0;                    // Для отзыва старых proofs

    // НОВЫЕ ПОЛЯ - Лимиты делегирования:
    Amount max_total_delegated = 0;        // Максимум всего делегировано (0 = без лимита)
    Amount max_per_transaction = 0;        // Максимум за одну транзакцию (0 = без лимита)
    uint8_t max_percent_of_balance = 0;    // Процент от баланса (0 = без лимита, 100 = весь баланс)

	    // === Поля для HIERARCHICAL_SPEND ===
	    Hash256 delegation_proof_txid{};       // Ссылка на delegation proof

    // === Поля для REVOKE_DELEGATION ===
    Hash256 revoked_delegation_proof_txid{}; // Какой delegation proof отзываем

    // === Поля для CADASTRAL_DOC ===
    Hash256 cadastral_doc_hash{}; // Обязательный хеш документа
    std::string cadastral_number; // Опциональный кадастровый номер
    Address cadastral_owner;      // Опциональный владелец
    Hash256 cadastral_nft_id{}; // Required cadastral NFT defining the parcel geometry

    // === Поля для CARD_* (карточные профили/выпуск/статус) ===
    // CARD_PROGRAM_DEFINE / CARD_ISSUE / CARD_UPDATE_STATUS / CARD_UPDATE_POLICY
    Hash256 card_program_id{};         // Стабильный ID карточного профиля
    Address card_issuer;               // Эмитент/владелец профиля
    std::string card_chip_vendor;      // Вендор чипа
    std::string card_chip_os;          // ОС/прошивка чипа
    std::string card_applet_aid;       // AID applet (hex/string)
    uint32_t card_applet_version = 0;  // Версия applet
    Hash256 card_applet_package_hash{}; // SHA-256 пакета applet
    uint32_t card_policy_id = 0;       // Локальный policy ID для интеграций
    Hash256 card_scope_id{};           // Scope (sidechain_id/cadastral_scope/custom)
    CardCorporatePolicy card_corporate_policy;

    // CARD_ISSUE / CARD_UPDATE_STATUS
    Hash256 card_uid_hash{};           // Хеш UID/серийника карты
    Hash256 card_pubkey_hash{};        // CARD_ISSUE: хеш публичного ключа карты/сертификата
    uint64_t card_valid_from = 0;      // CARD_ISSUE: UTC seconds
    uint64_t card_valid_to = 0;        // CARD_ISSUE: UTC seconds, 0 = без срока
    uint8_t card_status = 0;           // 0=active, 1=suspended, 2=revoked
    Hash256 card_metadata_hash{};      // Хеш off-chain метаданных/паспорта карты

    // === Поля для SIDECHAIN DEPOSIT ===
    Hash256 sidechain_id{};         // Идентификатор сайдчейна
    Address sidechain_owner;        // Владелец депозита
    Amount sidechain_amount = 0;    // Сумма депозита/списания
    Height sidechain_unlock_height = 0; // Высота разблокировки (LOCK)
    Hash256 sidechain_checkpoint{}; // Чекпоинт/хэш состояния сайдчейна
    Height sidechain_checkpoint_height = 0; // Высота (в сайдчейне) для checkpoint
    Signature sidechain_signature;  // Подпись владельца депозита
    // === SLASH_DEPOSIT fraud proof ===
    Hash256 sidechain_fraud_checkpoint_a{}; // txid checkpoint A
    Hash256 sidechain_fraud_checkpoint_b{}; // txid checkpoint B
    // Индекс элемента в BATCH_SIDECHAIN_CHECKPOINT для fraud proof.
    // 0xFFFF означает, что ссылка указывает на отдельную tx типа SIDECHAIN_CHECKPOINT.
    uint16_t sidechain_fraud_checkpoint_index_a = 0xFFFF;
    uint16_t sidechain_fraud_checkpoint_index_b = 0xFFFF;

    // === Поля комитета сайдчейна ===
    uint8_t sidechain_committee_m = 0;
    uint8_t sidechain_committee_n = 0;
    std::vector<Address> sidechain_committee_members;
    Hash256 sidechain_committee_id{};
    std::vector<Signature> sidechain_committee_signatures;

    // === Поля для SIDECHAIN ISSUE/MINT/REDEEM ===
    std::string sidechain_token_symbol; // Символ токена (ISSUE)
    uint32_t sidechain_multiplier = 0;  // Мультипликатор (ISSUE)
    uint32_t sidechain_fee_bps = 0;     // Комиссия в bps (ISSUE)
    uint32_t sidechain_max_multiplier = 0; // Максимальный мультипликатор (ISSUE)
    uint8_t sidechain_checkpoint_policy_id = 2; // Политика checkpoint для выбранного профиля
    uint8_t sidechain_checkpoint_version = 2;   // Версия checkpoint для выбранного профиля

    // === Поля для REGISTER_ACCOUNTING_SIDECHAIN ===
    std::string sidechain_namespace; // Короткое имя/namespace учетного реестра
    Hash256 sidechain_schema_hash{}; // Хеш канонической схемы состояния

    // === Поля для BATCH_SIDECHAIN_CHECKPOINT ===
    std::vector<BatchCheckpointItem> sidechain_batch_checkpoints;

    // === Поля для UPDATE_FOUNDATION_ADDRESS ===
    Address foundation_old_address;
    Address foundation_new_address;
    uint8_t foundation_m = 0;
    uint8_t foundation_n = 0;
    std::vector<Address> foundation_participants;
    uint8_t foundation_new_m = 0;
    uint8_t foundation_new_n = 0;
    std::vector<Address> foundation_new_participants;
    std::vector<Signature> foundation_signatures;

    // === NFT payloads ===
    std::optional<nft::NFTCreationTransaction> nft_create;
    std::optional<nft::NFTTransferTransaction> nft_transfer;
    std::optional<nft::NFTBurnTransaction> nft_burn;

    // Для ANGLE_COMMITMENT подпись хранится в TxInput::signature.
    // Для остальных типов подпись хранится в witness (TxWitness),
    // а в TxInput сохраняется только signature_hash (32 байта).

    /**
     * @brief Сериализация транзакции
     */
    bytes serialize() const;
    bytes serialize_without_witness() const;
    // Shared wire encoding for transactions, block commitments and pruning.
    // Omitted witness slots are encoded as empty slots up to the input count.
    void append_witness(bytes& data) const;

    // Inactive payload and input signature fields retain their defaults. Return the
    // first offending field without allocation; the throwing form is used by
    // serializers and signing helpers before they project the transaction.
    const char* unused_payload_field() const noexcept;
    void require_canonical_payload() const;

    /**
     * @brief Десериализация транзакции
     */
    static Transaction deserialize(const bytes& data, size_t& offset);
    // Decode exactly one canonical transaction, rejecting trailing bytes and
    // alternative encodings. Reuse the result instead of decoding it twice.
    static Transaction deserialize_canonical(const bytes& data);

    /**
     * @brief Вычисление хеша транзакции
     */
    Hash256 get_hash() const;
    static Hash256 canonical_hash_from_serialized(const bytes& data);

    /**
     * @brief Подписание всех входов одним ключом
     */
    void sign(const bytes& private_key);

    /**
     * @brief Подписание конкретного входа
     */
    void sign_input(size_t input_index, const bytes& private_key);

    /**
     * @brief Верификация всех входов транзакции
     */
    bool verify() const;

    /**
     * @brief Верификация конкретного входа
     */
    bool verify_input(size_t input_index) const;

    /**
     * @brief Вычисление размера транзакции в байтах
     */
    size_t get_size() const;

    /**
     * @brief Проверка, является ли транзакция coinbase
     */
    bool is_coinbase() const;

    /**
     * @brief Создать coinbase транзакцию
     */
    static Transaction create_coinbase(
        Height height,
        const Address& miner_address,
        Amount reward,
        Amount fees = 0
    );

    static Transaction create_coinbase(
        Height height,
        const Address& miner_address,
        Amount reward,
        Amount fees,
        const Address& foundation_address
    );

    /**
     * @brief Создать ANGLE_COMMITMENT транзакцию
     */
    static Transaction create_angle_commitment(
        const Address& multisig_address,
        const Hash256& spending_tx_hash,
        const bytes& private_key,
        const Address& signer_address,
        const std::string& network_id
    );

    /**
     * Canonical, domain-separated message signed by ANGLE_COMMITMENT.
     *
     * The active network id is part of the preimage. Consensus additionally
     * requires every other ANGLE_COMMITMENT field to use its canonical value,
     * so the signature and anti-replay key authorize one exact intent.
     */
    Hash256 get_angle_commitment_message_hash() const;

    /**
     * @brief Создать MULTISIG_SPEND транзакцию
     */
    static Transaction create_multisig_spend(
        const std::vector<TxInput>& inputs,
        const std::vector<TxOutput>& outputs,
        const std::vector<Hash256>& angle_commitment_hashes,
        uint8_t m,
        uint8_t n
    );

    /**
     * @brief Получить хеш транзакции без angle commitments (для подписания)
     */
    Hash256 get_hash_without_commitments() const;

    /**
     * @brief Хеш полного черновика hierarchical spend без ссылок на commitments.
     * Входы, выходы, разрешение, версия и lock_time фиксируются до подписания.
     * Замена выбранных UTXO требует новых подписей.
     */
    Hash256 get_hierarchical_spend_hash() const;

    /**
     * @brief Получить хеш delegation proof для подписания
     */
    Hash256 get_delegation_proof_hash() const;

    /** Full REVOKE_DELEGATION intent without commitment references. */
    Hash256 get_revoke_delegation_hash() const;

    // Operations without consumed UTXOs need a durable authorization identity
    // independent of the number, ordering and encoding of their signatures.
    std::optional<Hash256> get_protocol_authorization_hash() const;
};

} // namespace geocoin::core
