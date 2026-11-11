#pragma once

#include "geocoin/core/block.hpp"
#include "geocoin/types.hpp"
#include <map>
#include <string>
#include <vector>

namespace geocoin::consensus {

/**
 * @brief Тип сети (как CBaseChainParams::Network в Bitcoin)
 */
enum class Network {
    MAIN,      // degree - основная сеть
    TESTNET,   // degree-test - тестовая сеть
    LOADNET,   // degree-load - изолированная сеть нагрузочных тестов
    MAX_NETWORK_TYPES
};

/**
 * @brief Параметры блокчейна для конкретной сети (как CChainParams в Bitcoin)
 *
 * Определяет параметры для каждой сети: основная, тестовая
 */
class ChainParams {
public:
    virtual ~ChainParams() = default;

    /**
     * @brief Получить хеш Genesis блока для этой сети
     */
    const Hash256& get_genesis_hash() const { return genesis_hash_; }

    /**
     * @brief Получить Genesis блок для этой сети
     */
    const core::Block& get_genesis_block() const { return genesis_block_; }

    /**
     * @brief Получить ID сети (строка)
     */
    const std::string& network_id() const { return network_id_; }

    /**
     * @brief Получить тип сети
     */
    Network get_network() const { return network_; }

    /**
     * @brief Получить порт P2P по умолчанию
     */
    uint16_t default_port() const { return default_port_; }

    /**
     * @brief Получить порт RPC по умолчанию
     */
    uint16_t rpc_port() const { return rpc_port_; }

    /**
     * @brief Получить начальные биты сложности
     */
    uint32_t initial_bits() const { return initial_bits_; }

    /**
     * @brief Максимальный target (минимальная сложность) этой сети
     */
    uint32_t pow_limit_bits() const { return pow_limit_bits_; }

    /**
     * @brief Разрешить блоки с минимальной сложностью (для testnet)
     */
    bool allow_min_difficulty_blocks() const { return allow_min_difficulty_blocks_; }

    /**
     * @brief Keep every post-genesis block at initial_bits().
     *
     * This is only enabled by the isolated load network. Public networks keep
     * their normal difficulty adjustment rules.
     */
    bool fixed_difficulty() const { return fixed_difficulty_; }

    /**
     * @brief Required coinbase maturity for this network.
     */
    Height coinbase_maturity() const { return coinbase_maturity_; }

    /** First height using area-weighted WGS84 GeoPoW selection. */
    Height geopow_activation_height() const { return geopow_activation_height_; }

    /**
     * First height enforcing the transaction hardening soft fork: canonical
     * Falcon signature encodings, strict committee/foundation signature lists,
     * dust-checked REVOKE_DELEGATION change, full 100-block coinbase maturity
     * and active-chain SLASH_DEPOSIT evidence. Every rule only rejects what
     * honest wallets never produce; blocks below this height keep old rules.
     */
    Height transaction_hardening_activation_height() const {
        return transaction_hardening_activation_height_;
    }

    /** Initial on-chain foundation multisig configuration for this network. */
    const Address& foundation_address() const { return foundation_address_; }
    uint8_t foundation_m() const { return foundation_m_; }
    uint8_t foundation_n() const { return foundation_n_; }
    const std::vector<Address>& foundation_participants() const { return foundation_participants_; }

    /**
     * @brief Persistent identifier of the base chain ruleset. Height-activated upgrades
     * preserve this identifier and validate history using their network schedule.
     *
     * A non-empty datadir may only be opened by the ruleset that created it.
     * This prevents silent reuse of chains produced by discarded rules.
     */
    const std::string& consensus_ruleset_id() const { return consensus_ruleset_id_; }

    /**
     * @brief Набор жестких checkpoint (height -> block hash)
     */
    const std::map<Height, Hash256>& checkpoints() const { return checkpoints_; }

protected:
    ChainParams() = default;

    // Параметры сети
    Network network_;
    std::string network_id_;
    uint16_t default_port_;
    uint16_t rpc_port_;

    // Параметры консенсуса
    uint32_t initial_bits_;
    uint32_t pow_limit_bits_;
    bool allow_min_difficulty_blocks_;
    bool fixed_difficulty_ = false;
    Height coinbase_maturity_;
    Height geopow_activation_height_ = 1;
    Height transaction_hardening_activation_height_ = 1;
    std::string consensus_ruleset_id_;
    Address foundation_address_;
    uint8_t foundation_m_ = 0;
    uint8_t foundation_n_ = 0;
    std::vector<Address> foundation_participants_;

    // Жесткие сетевые checkpoints. Они задают точки доверия, но не являются
    // общей защитой от majority-hashrate атак между checkpoint-высотами.
    std::map<Height, Hash256> checkpoints_;

    // Genesis блок
    Hash256 genesis_hash_;
    core::Block genesis_block_;
};

/**
 * @brief Параметры основной сети (degree)
 */
class MainNetParams : public ChainParams {
public:
    MainNetParams();
};

/**
 * @brief Параметры тестовой сети (degree-test)
 */
class TestNetParams : public ChainParams {
public:
    TestNetParams();
};

/**
 * @brief Parameters of the isolated deterministic stress-test network.
 */
class LoadNetParams : public ChainParams {
public:
    LoadNetParams();
};

/**
 * @brief Получить параметры текущей выбранной сети
 *
 * Аналог Params() в Bitcoin
 */
const ChainParams& Params();

/**
 * @brief Получить параметры для конкретной сети
 *
 * Аналог Params(network) в Bitcoin
 */
const ChainParams& Params(Network network);

/**
 * @brief Выбрать активную сеть
 *
 * Аналог SelectParams() в Bitcoin
 */
void SelectParams(Network network);

/**
 * @brief Выбрать сеть по строковому ID
 *
 * @param network_id "degree", "degree-test" или "degree-load"
 * @return true если сеть найдена и выбрана
 */
bool SelectParamsByNetworkId(const std::string& network_id);

} // namespace geocoin::consensus
