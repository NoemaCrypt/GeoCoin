#include "geocoin/consensus/chainparams.hpp"
#include "geocoin/core/genesis.hpp"
#include "geocoin/consensus/proof_of_work.hpp"
#include "geocoin/consensus/consensus.hpp"
#include <cassert>
#include <stdexcept>

namespace geocoin::consensus {
namespace {

constexpr bool cstr_equal(const char* a, const char* b) {
    while (*a && *a == *b) {
        ++a;
        ++b;
    }
    return *a == *b;
}

constexpr bool loadnet_foundation_overlaps_mainnet() {
    if (cstr_equal(params::LOADNET_FOUNDATION_ADDRESS, params::FOUNDATION_ADDRESS)) {
        return true;
    }
    for (uint8_t i = 0; i < params::LOADNET_FOUNDATION_N; ++i) {
        for (uint8_t j = 0; j < params::FOUNDATION_N; ++j) {
            if (cstr_equal(params::LOADNET_FOUNDATION_PARTICIPANTS[i], params::FOUNDATION_PARTICIPANTS[j])) {
                return true;
            }
        }
    }
    return false;
}

static_assert(
    !loadnet_foundation_overlaps_mainnet(),
    "loadnet foundation managers must not reuse mainnet/testnet keys"
);

} // namespace

// ============================================================================
// Параметры основной сети (degree)
// ============================================================================

MainNetParams::MainNetParams() {
    network_ = Network::MAIN;
    network_id_ = "degree";
    default_port_ = 3030;
    rpc_port_ = 3033;

    // Параметры консенсуса
    initial_bits_ = core::genesis_parameters(core::GenesisNetwork::MAINNET).bits;
    pow_limit_bits_ = 0x1d00ffff;
    allow_min_difficulty_blocks_ = false;
    fixed_difficulty_ = false;
    coinbase_maturity_ = params::COINBASE_MATURITY;
    // Preserve the established chain; every mainnet participant activates the
    // same WGS84 rule at this height, independently of local configuration.
    geopow_activation_height_ = 2500;
    // Soft fork: must stay above the live tip with enough margin for every
    // node and miner to upgrade first. Older blocks keep the original rules.
    transaction_hardening_activation_height_ = 6000;
    consensus_ruleset_id_ = "degree-lwma-maturity100-sidechain-v4-genesis20260926";
    foundation_address_ = Address(params::FOUNDATION_ADDRESS);
    foundation_m_ = params::FOUNDATION_M;
    foundation_n_ = params::FOUNDATION_N;
    for (const char* participant : params::FOUNDATION_PARTICIPANTS) {
        foundation_participants_.emplace_back(participant);
    }

    // Genesis блок для основной сети
    genesis_block_ = core::create_genesis_block(core::GenesisNetwork::MAINNET);
    genesis_hash_ = genesis_block_.get_hash();

    // Проверяем, что Genesis хеш совпадает с ожидаемым.
    // Критично: это должно проверяться и в Release-сборке (не только assert).
    Hash256 expected_hash = core::get_genesis_hash(core::GenesisNetwork::MAINNET);
    if (genesis_hash_ != expected_hash) {
        throw std::runtime_error("MainNet genesis hash mismatch! Check genesis.cpp");
    }

    // Genesis is the only hardcoded checkpoint before the public launch.
    // Additional checkpoints require a separate, explicit release decision.
    checkpoints_[0] = genesis_hash_;
}

// ============================================================================
// Параметры тестовой сети (degree-test)
// ============================================================================

TestNetParams::TestNetParams() {
    network_ = Network::TESTNET;
    network_id_ = "degree-test";
    default_port_ = 13030;
    rpc_port_ = 13033;

    // Параметры консенсуса
    initial_bits_ = core::genesis_parameters(core::GenesisNetwork::TESTNET).bits;
    // Keep the testnet limit canonical and representable in 256 bits.  Using
    // 0x2100ffff overflows the internal 256-bit target representation and makes
    // compact-target validation ambiguous.
    pow_limit_bits_ = 0x207fffff;
    allow_min_difficulty_blocks_ = true;
    fixed_difficulty_ = false;
    coinbase_maturity_ = params::COINBASE_MATURITY;
    // Soft fork activation for the public testnet; keep above its live tip.
    transaction_hardening_activation_height_ = 6000;
    consensus_ruleset_id_ = "degree-test-lwma-maturity100-sidechain-v3";
    foundation_address_ = Address(params::TESTNET_FOUNDATION_ADDRESS);
    foundation_m_ = params::TESTNET_FOUNDATION_M;
    foundation_n_ = params::TESTNET_FOUNDATION_N;
    for (const char* participant : params::TESTNET_FOUNDATION_PARTICIPANTS) {
        foundation_participants_.emplace_back(participant);
    }

    // Testnet has an independently mined genesis and cannot join mainnet by accident.
    genesis_block_ = core::create_genesis_block(core::GenesisNetwork::TESTNET);
    genesis_hash_ = genesis_block_.get_hash();

    const Hash256 expected_hash = core::get_genesis_hash(core::GenesisNetwork::TESTNET);
    if (genesis_hash_ != expected_hash) {
        throw std::runtime_error("TestNet genesis hash mismatch! Check genesis.cpp");
    }

    // Жесткие checkpoint (минимум: genesis).
    checkpoints_[0] = genesis_hash_;
}

// ============================================================================
// Parameters of the isolated stress-test network (degree-load)
// ============================================================================

LoadNetParams::LoadNetParams() {
    network_ = Network::LOADNET;
    network_id_ = "degree-load";
    default_port_ = 23030;
    rpc_port_ = 23033;

    initial_bits_ = core::genesis_parameters(core::GenesisNetwork::LOADNET).bits;
    pow_limit_bits_ = 0x207fffff;
    allow_min_difficulty_blocks_ = true;
    fixed_difficulty_ = true;
    coinbase_maturity_ = params::COINBASE_MATURITY;
    consensus_ruleset_id_ =
        "degree-load-fixed207fffff-maturity100-sidechain-v3-foundation4of4-v1";

    foundation_address_ = Address(params::LOADNET_FOUNDATION_ADDRESS);
    foundation_m_ = params::LOADNET_FOUNDATION_M;
    foundation_n_ = params::LOADNET_FOUNDATION_N;
    for (const char* participant : params::LOADNET_FOUNDATION_PARTICIPANTS) {
        foundation_participants_.emplace_back(participant);
    }

    genesis_block_ = core::create_genesis_block(core::GenesisNetwork::LOADNET);
    genesis_hash_ = genesis_block_.get_hash();
    const Hash256 expected_hash = core::get_genesis_hash(core::GenesisNetwork::LOADNET);
    if (genesis_hash_ != expected_hash) {
        throw std::runtime_error("LoadNet genesis hash mismatch! Check genesis.cpp");
    }
    checkpoints_[0] = genesis_hash_;
}

// ============================================================================
// Глобальное управление параметрами
// ============================================================================

static Network current_network = Network::MAIN;

const ChainParams& Params() {
    return Params(current_network);
}

const ChainParams& Params(Network network) {
    static const MainNetParams mainnet_params;
    static const TestNetParams testnet_params;
    static const LoadNetParams loadnet_params;
    switch (network) {
        case Network::MAIN:
            return mainnet_params;
        case Network::TESTNET:
            return testnet_params;
        case Network::LOADNET:
            return loadnet_params;
        default:
            assert(false && "Invalid network type");
            return mainnet_params;
    }
}

void SelectParams(Network network) {
    (void)Params(network);
    current_network = network;
}

bool SelectParamsByNetworkId(const std::string& network_id) {
    if (network_id == "degree") {
        SelectParams(Network::MAIN);
        return true;
    } else if (network_id == "degree-test") {
        SelectParams(Network::TESTNET);
        return true;
    } else if (network_id == "degree-load") {
        SelectParams(Network::LOADNET);
        return true;
    }
    return false;
}

} // namespace geocoin::consensus
