#pragma once

#include "geocoin/types.hpp"
#include "geocoin/core/block.hpp"
#include "geocoin/core/transaction.hpp"
#include "geocoin/chain/block_index.hpp"
#include "geocoin/consensus/validation.hpp"
#include "geocoin/nft/geometry.hpp"
#include <functional>
#include <string>

namespace geocoin::consensus {

inline constexpr uint32_t LOCK_TIME_THRESHOLD = 500000000u;

// Absolute finality only. The time is the candidate block's header timestamp;
// standalone admission uses the local clock. Signature validity is independent.
bool is_final_transaction(const core::Transaction& tx, Height block_height, Timestamp block_time);

struct ContextualTxLookup {
    // txid -> tx (для обычных ссылок)
    std::function<bool(const Hash256&, core::Transaction&)> get_transaction_by_txid;

    // Индексные проверки (НЕ txid).
    std::function<bool(const Hash256&)> has_angle_commitment_key;      // key = input.prev_tx_hash
    std::function<bool(const Hash256&)> has_multisig_spend_key;        // key = tx.get_hash_without_commitments()
    std::function<bool(const Hash256&)> has_hierarchical_spend_key;    // key = tx.get_hierarchical_spend_hash()
    std::function<bool(const Hash256&)> has_delegation_proof_key;      // key = tx.get_delegation_proof_hash()
    std::function<bool(const Hash256&)> has_revoke_delegation_key;     // key = tx.revoked_delegation_proof_txid

    // Уникальность NFT_CREATE (nft_id)
    std::function<bool(const Hash256&)> has_nft_id_key;                // key = nft_id

    // Сайдчейн-депозиты (sidechain_id)
    std::function<bool(const Hash256&)> has_sidechain_deposit_key;     // key = sidechain_id

    // Реестр сайдчейнов (sidechain_id)
    std::function<bool(const Hash256&)> has_sidechain_registry_key;    // key = sidechain_id

    // true, если подписи уже проверены на более раннем этапе (mempool/check_block).
    bool signatures_verified = false;
};

// Константы консенсуса
namespace params {
    // Жесткий лимит консенсуса: любой блок больше этого размера невалиден.
    // Предзапускный профиль для данных Falcon-512.
    constexpr size_t MAX_BLOCK_SIZE = 4 * 1000 * 1000;  // 4 MB

    // A compact malformed transaction can expand into a much larger in-memory
    // Transaction object.  Bound the count independently from byte size so a
    // single 4 MB block cannot force hundreds of megabytes of allocations.
    constexpr size_t MAX_BLOCK_TRANSACTIONS = 10000;

    // Мягкий лимит шаблона майнера: майнер по умолчанию собирает блоки меньше жесткого лимита.
    // Нужен запас под вариативный размер подписей/сериализации и для более стабильного распространения блоков.
    constexpr size_t BLOCK_TEMPLATE_SOFT_LIMIT = 2 * 1000 * 1000;  // 2 MB

    // Максимальный размер транзакции
    constexpr size_t MAX_TX_SIZE = 100000;
    constexpr size_t MAX_TOKEN_SYMBOL = 16;
    constexpr size_t MAX_SIDECHAIN_NAMESPACE = 64;
    constexpr uint32_t MIN_SIDECHAIN_MULTIPLIER = 100;
    constexpr uint32_t MAX_SIDECHAIN_MULTIPLIER = 1000000;

    // Базовые размеры для оценки транзакций (в байтах)
    constexpr size_t TX_BASE_SIZE = 40;
    // Conservative STANDARD input estimate including a Falcon-512 public key,
    // signature, witness metadata and the non-witness input fields.
    constexpr size_t TX_INPUT_SIZE = 1800;
    constexpr size_t TX_OUTPUT_SIZE = 60;

    // Сколько последних блоков храним с witness (остальные pruned).
    constexpr Height WITNESS_PRUNE_DEPTH = 1000;

    // Минимальная комиссия (angles) и ставка за килобайт
    constexpr Amount MIN_TX_FEE = 500;             // 0.000005 GC
    constexpr uint32_t MAX_SIDECHAIN_FEE_BPS = 500; // 5% max fee for sidechain ops
    constexpr uint8_t FINANCIAL_CHECKPOINT_POLICY_ID = 2;
    constexpr uint8_t FINANCIAL_CHECKPOINT_VERSION = 2;
    constexpr uint8_t ACCOUNTING_CHECKPOINT_POLICY_ID = 3;
    constexpr uint8_t ACCOUNTING_CHECKPOINT_VERSION = 1;
    constexpr size_t MAX_SIDECHAIN_COMMITTEE_MEMBERS = 64;
    constexpr size_t MAX_SIDECHAIN_COMMITTEE_SIGNATURES = 128;
    constexpr size_t MAX_BATCH_SIDECHAIN_CHECKPOINTS = 256;
    constexpr size_t MAX_BATCH_CHECKPOINT_PROOF = 4096;
    constexpr size_t MAX_BATCH_CHECKPOINT_MERKLE_DEPTH = 64;
    constexpr Height MAX_SIDECHAIN_CHECKPOINT_HEIGHT = 1000000000ULL;
    constexpr Height MIN_SIDECHAIN_CHECKPOINT_INTERVAL = 5;
    // Окно, в течение которого депозит закрываемого сайдчейна остается slashable.
    constexpr Height SIDECHAIN_DECOMMISSION_CHALLENGE_BLOCKS = 144;
    // Мягкий интервал (чем чаще, тем выше минимальная комиссия).
    constexpr Height SIDECHAIN_CHECKPOINT_SOFT_INTERVAL = 30;
    // Ограничение на множитель комиссии (защита от переполнений/крайних значений).
    constexpr uint32_t MAX_SIDECHAIN_CHECKPOINT_FEE_MULTIPLIER = 20;
    constexpr Amount FEE_RATE_PER_KB = 2000;       // 0.00002 GC за кБ

    // Dust limit - минимальный размер выхода (защита от раздувания UTXO)
    constexpr Amount DUST_LIMIT = 1000;            // 0.00001 GC

    // Максимальное количество inputs/outputs (защита от DoS)
    constexpr size_t MAX_TX_INPUTS = 1000;
    constexpr size_t MAX_TX_OUTPUTS = 1000;

    // === Лимиты сериализации/десериализации (DoS защита) ===
    constexpr size_t MAX_SCRIPT_SIG_SIZE = 10 * 1000;
    constexpr size_t MAX_SCRIPT_PUBKEY_SIZE = 10 * 1000;
    constexpr size_t MAX_SIGNATURE_SIZE = 4096;      // Falcon-512 payload cap
    constexpr size_t MAX_WITNESS_PUBKEY_SIZE = 2048;
    constexpr size_t MAX_WITNESS_SIG_SIZE = 4096;
    constexpr uint8_t WITNESS_FORMAT_VERSION_CURRENT = 1;
    // Фиксированный размер Falcon-512 публичного ключа в witness сериализации.
    // Используется вместе с флагом "есть ключ", без отдельного поля длины.
    constexpr size_t WITNESS_PUBKEY_SIZE = 897;
    constexpr size_t MAX_ADDRESS_SIZE = 128;
    constexpr size_t MAX_SIDECHAIN_TOKEN_SYMBOL_SIZE = 1024;
    // Wallet/address encoding supports at most 15 participants. Keeping the
    // wire parser at uint8_t/1000 allowed oversized multisig payloads to reach
    // expensive Falcon verification even though they could never be valid.
    constexpr size_t MAX_MULTISIG_PARTICIPANTS = 15;
    constexpr size_t MAX_ANGLE_COMMITMENTS = MAX_MULTISIG_PARTICIPANTS;

    // NFT metadata limits (defensive, non-consensus semantics)
    constexpr size_t MAX_NFT_NAME = 256;
    constexpr size_t MAX_NFT_DESCRIPTION = 4096;
    constexpr size_t MAX_NFT_IMAGE_URI = 1024;
    constexpr size_t MAX_NFT_LOCATION_NAME = 256;
    constexpr size_t MAX_NFT_TAGS = 128;
    constexpr size_t MAX_NFT_TAG_LEN = 64;
    constexpr size_t MAX_NFT_POINTS = nft::coordinates::kMaxGeometryVertices;
    // Project policy on geodesic area rounded to square centimetres.
    constexpr uint32_t NFT_CADASTRAL_AREA_TOLERANCE_PERCENT = 10;
    constexpr bool ENABLE_NFT_POL = false;
    constexpr uint32_t MAX_NFT_POL_ACCURACY_CM = 10000; // 100m

    // Cadastre metadata limits
    constexpr size_t MAX_CADASTRAL_NUMBER = 64;
    constexpr size_t MAX_CADASTRAL_OWNER_NAME = 128;
    constexpr size_t MAX_LAND_CATEGORY = 64;
    constexpr size_t MAX_PERMITTED_USE = 256;
    constexpr size_t MAX_RESTRICTIONS = 64;
    constexpr size_t MAX_RESTRICTION_LEN = 128;
    constexpr size_t MAX_PHOTO_HASHES = 32;

    // Card program / chip profile metadata limits
    constexpr size_t MAX_CARD_CHIP_VENDOR = 128;
    constexpr size_t MAX_CARD_CHIP_OS = 128;
    constexpr size_t MAX_CARD_APPLET_AID = 128;
    constexpr size_t MAX_CARD_AUTHORITY_MEMBERS = MAX_MULTISIG_PARTICIPANTS;

    // Созревание coinbase: снижает риск потери уже потраченной награды при reorg.
    // Короткое тестовое значение 2 не подходит для mainnet-поведения.
    constexpr Height COINBASE_MATURITY = 100;

    // Целевое время блока (10 минут)
    constexpr Timestamp TARGET_BLOCK_TIME = 600;

    // Per-block LWMA: окно усреднения (в блоках)
    constexpr Height DIFFICULTY_DAA_WINDOW = 60;

    // Максимальный шаг изменения target за один блок (x2 / ÷2)
    constexpr uint32_t MAX_DIFFICULTY_ADJUSTMENT = 2;

    // Максимальное время в будущем (15 минут)
    constexpr Timestamp MAX_FUTURE_BLOCK_TIME = 15 * 60;

    // Количество блоков для median time past
    constexpr int MEDIAN_TIME_SPAN = 11;

    // Локальный safety-limit глубины reorg. Это policy, а не защита от 51%:
    // при конфликте он останавливает автоматическое переключение ветки и может
    // потребовать ручного восстановления. 0 = без лимита.
    constexpr Height MAX_REORG_DEPTH = 144;

    // === Foundation Reward (Dev Fee) ===

    // Адрес фонда развития GeoCoin (MULTISIG 3-of-4).
    // This configuration is committed by the 2026-09-26 public-network
    // genesis and must change only through the on-chain foundation update.
    // Участники:
    //   1. GEO1Awz2Kenry73g56j5ia8rDNK3hGRhq1g4i
    //   2. GEO15cjEpNPC7ijYm5iJRZKv2CVkndbq9pHwn
    //   3. GEO1PsGDFBUVSQMQ4iJ517iPyS2wqZhptV2kA
    //   4. GEO1P7AfnQdxFhDGEjvP8TFSwxmRFLKCBpdNt
    // Требуется 3 подписи из 4 для расходования средств.
    constexpr const char* FOUNDATION_ADDRESS = "GEOM3PyazNssCYYzF3tW9YrsSwszLq45QXbAZv";
    constexpr uint8_t FOUNDATION_M = 3;
    constexpr uint8_t FOUNDATION_N = 4;
    constexpr const char* FOUNDATION_PARTICIPANTS[FOUNDATION_N] = {
        "GEO1Awz2Kenry73g56j5ia8rDNK3hGRhq1g4i",
        "GEO15cjEpNPC7ijYm5iJRZKv2CVkndbq9pHwn",
        "GEO1PsGDFBUVSQMQ4iJ517iPyS2wqZhptV2kA",
        "GEO1P7AfnQdxFhDGEjvP8TFSwxmRFLKCBpdNt"
    };

    // Testnet has a separate, pre-existing foundation policy.  It is kept
    // independent from the public mainnet launch configuration above.
    constexpr const char* TESTNET_FOUNDATION_ADDRESS = "GEOM3Bt4uDcaV3WU4B7ZRCHBr7d38CcmmWYuZy";
    constexpr uint8_t TESTNET_FOUNDATION_M = 4;
    constexpr uint8_t TESTNET_FOUNDATION_N = 4;
    constexpr const char* TESTNET_FOUNDATION_PARTICIPANTS[TESTNET_FOUNDATION_N] = {
        "GEO12EEJNr2jXmHXEaschbf5m9qJhPZJjuzuT",
        "GEO1vdxMNUgVjNUm6t9RFePgVKq6UMdM4Job",
        "GEO1AxuUpTawWn1cdf7NqSCTVZctVEa5ec6u3",
        "GEO1AkP5DFbesUkbzN8wNmL8AhrFV8dYP9ejE"
    };

    // Test-only 4-of-4 managers for the isolated degree-load network. Their
    // deterministic keys are public fixtures and MUST NOT be used on degree or
    // any network carrying value.
    constexpr const char* LOADNET_FOUNDATION_ADDRESS = "GEOM3EuJq9xB1xXoUjDJ6MK6DR28HhxYseseC2";
    constexpr uint8_t LOADNET_FOUNDATION_M = 4;
    constexpr uint8_t LOADNET_FOUNDATION_N = 4;
    constexpr const char* LOADNET_FOUNDATION_PARTICIPANTS[LOADNET_FOUNDATION_N] = {
        "GEO1P3rsV9e3HpE8yfR8VPtjNqwrntcNH3ZKk",
        "GEO1K9Juep1tJoUGkSdhmGiPhvgqc5WFoyAo8",
        "GEO1LiZwqv6oqA297U6CN76erHUpDJQFcBja7",
        "GEO1N4hgEstUmnPUBh7VJs9vdEebnVokALf3R"
    };

    // Процент награды для фонда (10% - правило 90/10 для Proof-of-Equity)
    constexpr uint32_t FOUNDATION_REWARD_PERCENT = 10;

    // Включить foundation reward (можно отключить для тестов)
    constexpr bool ENABLE_FOUNDATION_REWARD = true;

    // После первого halving доля фонда больше не выплачивается coinbase-output'ом.
    // Она накапливается во внутреннем interest pool (consensus/state metadata).
    constexpr Height INTEREST_POOL_ACTIVATION_HEIGHT = 210000;

    // Период распределения interest pool (≈ квартал при 10-минутных блоках).
    // 91 день * 24 часа * 6 блоков/час = 13104 блока.
    constexpr Height INTEREST_DISTRIBUTION_INTERVAL = 13104;
}

/**
 * Foundation reward активен на высоте (кроме genesis-правила).
 */
constexpr bool is_foundation_reward_active(Height height) {
    return params::ENABLE_FOUNDATION_REWARD && height > 0;
}

/**
 * После активации foundation share уходит в interest pool, а не в output coinbase.
 */
constexpr bool is_foundation_reward_redirected_to_pool(Height height) {
    return is_foundation_reward_active(height) &&
           height >= params::INTEREST_POOL_ACTIVATION_HEIGHT;
}

/**
 * Квартальная высота распределения interest pool.
 * Первое распределение происходит через INTEREST_DISTRIBUTION_INTERVAL блоков после активации.
 */
constexpr bool is_interest_distribution_due(Height height) {
    if (height <= params::INTEREST_POOL_ACTIVATION_HEIGHT) {
        return false;
    }
    const Height delta = height - params::INTEREST_POOL_ACTIVATION_HEIGHT;
    return (delta % params::INTEREST_DISTRIBUTION_INTERVAL) == 0;
}

/**
 * Размер foundation share от total_reward (reward + fees).
 */
constexpr Amount calculate_foundation_reward_cut(Amount total_reward) {
    if (!params::ENABLE_FOUNDATION_REWARD) {
        return 0;
    }
    static_assert(params::FOUNDATION_REWARD_PERCENT <= 100);
    if (total_reward <= 0) {
        return 0;
    }

    // Avoid signed overflow for consensus-valid totals near Amount::max().
    // Splitting quotient and remainder is exact for non-negative values and
    // keeps both intermediate products bounded.
    return (total_reward / 100) * params::FOUNDATION_REWARD_PERCENT +
           ((total_reward % 100) * params::FOUNDATION_REWARD_PERCENT) / 100;
}

/**
 * Разрешена ли реорганизация указанной глубины.
 */
constexpr bool is_reorg_depth_allowed(uint64_t depth) {
    return params::MAX_REORG_DEPTH == 0 || depth <= params::MAX_REORG_DEPTH;
}

/**
 * Проверка блока (контекстно-независимая)
 * Аналог CheckBlock из Bitcoin
 *
 * Проверяет:
 * - PoW (hash < target)
 * - Merkle root
 * - Timestamp (не в будущем)
 * - Размер блока
 * - Coinbase транзакцию
 * - Транзакции
 */
bool transaction_requires_basic_signature(const core::Transaction& tx);

// verified[i] is 1 when tx i does not need a basic signature or the signature
// verifies. Signature checks for different transactions run in parallel.
// State updates stay with the caller and must remain in block order.
void verify_basic_signatures(
    const std::vector<core::Transaction>& transactions,
    std::vector<uint8_t>& verified
);

bool check_block(
    const core::Block& block,
    ValidationState& state,
    ValidationMode mode = ValidationMode::FULL
);

/**
 * Контекстная проверка блока
 * Аналог ContextualCheckBlock из Bitcoin
 *
 * Проверяет:
 * - Высоту
 * - prev_block_hash
 * - Timestamp >= median(last 11 blocks)
 * - Timestamp <= median(last 11 blocks) + MAX_FUTURE_BLOCK_TIME
 * - Сложность
 */
bool contextual_check_block(
    const core::Block& block,
    ValidationState& state,
    const chain::BlockIndex* prev_index
);

/**
 * Проверка заголовка блока
 * Аналог CheckBlockHeader из Bitcoin
 */
bool check_block_header(
    const core::BlockHeader& header,
    ValidationState& state
);

/**
 * Контекстная проверка заголовка
 */
bool contextual_check_block_header(
    const core::BlockHeader& header,
    ValidationState& state,
    const chain::BlockIndex* prev_index
);

/**
 * Проверить жесткий checkpoint сети для конкретной высоты.
 * Если checkpoint на этой высоте не задан, проверка проходит.
 */
bool check_hardened_checkpoint(
    Height height,
    const Hash256& block_hash,
    ValidationState& state
);

/**
 * Проверка транзакции
 * Аналог CheckTransaction из Bitcoin
 *
 * Проверяет:
 * - Входы/выходы не пусты
 * - Размер транзакции
 * - Значения выходов валидны
 * - Нет дублирования входов
 * - Coinbase транзакция корректна
 */
bool check_transaction(
    const core::Transaction& tx,
    ValidationState& state
);

// Delegation endpoints are inclusive; valid_until == 0 means no expiry.
bool is_delegation_active_at(const core::Transaction& proof, Timestamp time);

// A delegated spend also binds its signed timestamp to the candidate block.
bool is_delegated_spend_valid_at(
    const core::Transaction& spend,
    const core::Transaction& proof,
    Timestamp block_time
);

/**
 * Контекстная проверка транзакции
 *
 * Проверяет уникальность транзакций с доступом к blockchain:
 * - ANGLE_COMMITMENT: prev_tx_hash уникален (нет дубликатов)
 * - MULTISIG_SPEND: spending_tx_hash уникален (нет replay)
 * - DELEGATION_PROOF: delegation_hash уникален
 * - REVOKE_DELEGATION: делегирование еще не отозвано
 * - HIERARCHICAL_SPEND: hierarchical_spend_hash уникален
 *
 * ВАЖНО: Эта функция требует доступ к blockchain для проверки
 * существования транзакций. Вызывается из ConnectBlock().
 *
 * @param tx Транзакция для проверки
 * @param state Состояние валидации
 * @param get_transaction Функция для получения транзакции по хешу
 * @return true если транзакция валидна в контексте blockchain
 */
bool contextual_check_transaction(
    const core::Transaction& tx,
    ValidationState& state,
    const ContextualTxLookup& lookup
);

// Политика расчета комиссии для сайдчейн-чекпоинта по нагрузке (байты).
// Используется для автозаполнения в RPC, не является консенсусной проверкой.
Amount calculate_sidechain_checkpoint_fee(uint64_t total_weight_bytes);
Amount calculate_sidechain_checkpoint_min_fee(Height current_height, Height last_checkpoint_height);
Hash256 compute_sidechain_committee_id(uint8_t m, uint8_t n, const std::vector<Address>& members);
bool is_supported_checkpoint_policy_version(uint8_t policy_id, uint8_t version);

/**
 * Проверка сложности
 */
bool check_difficulty(
    uint32_t bits,
    const chain::BlockIndex* prev_index,
    Timestamp block_timestamp,
    ValidationState& state
);

/**
 * Вычисление следующей сложности
 */
uint32_t calculate_next_difficulty(
    const chain::BlockIndex* prev_index,
    Timestamp block_timestamp
);

/**
 * Проверка timestamp
 */
bool check_timestamp(
    Timestamp timestamp,
    ValidationState& state
);

/**
 * Проверка timestamp относительно median time past
 */
bool check_timestamp_against_prev(
    Timestamp timestamp,
    const chain::BlockIndex* prev_index,
    ValidationState& state
);

/**
 * Вычисление chainwork для блока
 */
uint256 calculate_chainwork(
    const chain::BlockIndex* prev_index,
    uint32_t bits
);

/**
 * Получить median time past
 */
Timestamp get_median_time_past(const chain::BlockIndex* index);

/**
 * Проверить commitment высоты в первых 8 байтах coinbase script_sig.
 * Genesis использует отдельное текстовое сообщение и проверяется по hash.
 */
bool check_coinbase_height(
    const core::Transaction& coinbase,
    Height height,
    ValidationState& state
);

/**
 * Проверка coinbase outputs (включая foundation reward)
 *
 * Проверяет:
 * - Количество outputs (до активации pool: miner+foundation, после: только miner)
 * - Адрес фонда (до активации pool)
 * - Суммы наград (майнер + foundation_share = total reward)
 * - Что coinbase не платит больше чем положено
 *
 * @param coinbase Coinbase транзакция
 * @param expected_reward Ожидаемая награда за блок
 * @param fees Комиссии из транзакций
 * @param state Состояние валидации
 * @return true если coinbase валиден
 */
bool check_coinbase_outputs(
    const core::Transaction& coinbase,
    Height height,
    Amount expected_reward,
    Amount fees,
    const Address& foundation_address,
    ValidationState& state
);

} // namespace geocoin::consensus
