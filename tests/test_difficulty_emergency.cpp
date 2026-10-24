#include "geocoin/chain/block_index.hpp"
#include "geocoin/consensus/chainparams.hpp"
#include "geocoin/consensus/consensus.hpp"
#include "geocoin/consensus/proof_of_work.hpp"
#include "geocoin/core/genesis.hpp"

#include <iostream>
#include <cmath>
#include <memory>
#include <string_view>
#include <vector>

using namespace geocoin;

namespace {

int failures = 0;
constexpr uint32_t kStableTestBits = 0x1c03be28;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

std::vector<std::unique_ptr<chain::BlockIndex>> make_regular_chain(Height tip_height) {
    std::vector<std::unique_ptr<chain::BlockIndex>> chain;
    chain.reserve(static_cast<size_t>(tip_height + 1));

    const Timestamp base_time = 1'700'000'000;
    for (Height height = 0; height <= tip_height; ++height) {
        core::BlockHeader header;
        header.timestamp = static_cast<uint32_t>(base_time + height * consensus::params::TARGET_BLOCK_TIME);
        header.bits = kStableTestBits;
        if (!chain.empty()) {
            header.prev_block_hash = chain.back()->get_block_hash();
        }

        auto index = std::make_unique<chain::BlockIndex>(header);
        index->height = height;
        if (!chain.empty()) {
            index->prev = chain.back().get();
        }
        chain.push_back(std::move(index));
    }

    return chain;
}

void test_mainnet_stall_does_not_bypass_daa() {
    consensus::SelectParams(consensus::Network::MAIN);
    auto chain = make_regular_chain(consensus::params::DIFFICULTY_DAA_WINDOW + 20);
    const chain::BlockIndex* tip = chain.back().get();

    const Timestamp regular_next_time = tip->get_block_time() + consensus::params::TARGET_BLOCK_TIME;
    const Timestamp stalled_next_time = tip->get_block_time() + consensus::params::TARGET_BLOCK_TIME * 20;

    const uint32_t regular_bits = consensus::calculate_next_difficulty(tip, regular_next_time);
    const uint32_t stalled_bits = consensus::calculate_next_difficulty(tip, stalled_next_time);

    check(regular_bits == stalled_bits,
          "mainnet timestamp gap must not trigger an emergency difficulty bypass");
}

void test_daa_uses_partial_window_from_genesis() {
    auto make_two_block_chain = [](Timestamp solve_time) {
        std::vector<std::unique_ptr<chain::BlockIndex>> chain;
        chain.reserve(2);
        for (Height height = 0; height < 2; ++height) {
            core::BlockHeader header;
            header.timestamp = 1'700'000'000 + (height == 0 ? 0 : solve_time);
            header.bits = kStableTestBits;
            if (!chain.empty()) {
                header.prev_block_hash = chain.back()->get_block_hash();
            }
            auto index = std::make_unique<chain::BlockIndex>(header);
            index->height = height;
            index->prev = chain.empty() ? nullptr : chain.back().get();
            chain.push_back(std::move(index));
        }
        return chain;
    };

    const auto check_network = [&](consensus::Network network) {
        consensus::SelectParams(network);

        auto regular = make_two_block_chain(consensus::params::TARGET_BLOCK_TIME);
        check(consensus::calculate_next_difficulty(
                  regular.back().get(),
                  regular.back()->get_block_time() + consensus::params::TARGET_BLOCK_TIME) ==
                  kStableTestBits,
              "DAA keeps target stable at regular cadence from genesis");

        auto slow = make_two_block_chain(consensus::params::TARGET_BLOCK_TIME * 6);
        const uint256 old_target = consensus::ProofOfWork::bits_to_target(kStableTestBits);
        const uint256 slow_target = consensus::ProofOfWork::bits_to_target(
            consensus::calculate_next_difficulty(
                slow.back().get(),
                slow.back()->get_block_time() + consensus::params::TARGET_BLOCK_TIME)
        );
        check(slow_target > old_target && slow_target <= old_target * 2,
              "DAA eases difficulty after a slow first solve with bounded step");

        auto fast = make_two_block_chain(1);
        const uint256 fast_target = consensus::ProofOfWork::bits_to_target(
            consensus::calculate_next_difficulty(
                fast.back().get(),
                fast.back()->get_block_time() + consensus::params::TARGET_BLOCK_TIME)
        );
        check(fast_target < old_target && fast_target >= old_target / 2,
              "DAA raises difficulty after a fast first solve with bounded step");
    };

    check_network(consensus::Network::MAIN);
    check_network(consensus::Network::TESTNET);

    consensus::SelectParams(consensus::Network::MAIN);
}

void test_consensus_rules_are_not_height_gated() {
    consensus::SelectParams(consensus::Network::MAIN);
    const auto& params = consensus::Params();

    check(params.coinbase_maturity() == consensus::params::COINBASE_MATURITY,
          "mainnet must use hardened coinbase maturity from genesis");
    check(consensus::Params(consensus::Network::TESTNET).coinbase_maturity() ==
              consensus::params::COINBASE_MATURITY,
          "testnet must use hardened coinbase maturity from genesis");
    check(params.checkpoints().size() == 1 && params.checkpoints().count(0) == 1,
          "pre-launch mainnet must hardcode only the genesis checkpoint");

    consensus::ValidationState state;
    check(consensus::check_hardened_checkpoint(1, Hash256{}, state),
          "non-genesis blocks must not depend on a temporary launch checkpoint");
}

void test_network_specific_genesis_blocks_are_mined_and_pinned() {
    const auto main_genesis = core::create_genesis_block(core::GenesisNetwork::MAINNET);
    const auto test_genesis = core::create_genesis_block(core::GenesisNetwork::TESTNET);
    const auto load_genesis = core::create_genesis_block(core::GenesisNetwork::LOADNET);
    const auto& main_parameters = core::genesis_parameters(core::GenesisNetwork::MAINNET);
    const auto& test_parameters = core::genesis_parameters(core::GenesisNetwork::TESTNET);
    const auto& load_parameters = core::genesis_parameters(core::GenesisNetwork::LOADNET);

    check(std::string_view(main_parameters.network_id) == "degree",
          "mainnet genesis commits to degree network id");
    check(std::string_view(test_parameters.network_id) == "degree-test",
          "testnet genesis commits to degree-test network id");
    check(std::string_view(load_parameters.network_id) == "degree-load",
          "loadnet genesis commits to degree-load network id");
    check(core::genesis_message(core::GenesisNetwork::MAINNET) !=
              core::genesis_message(core::GenesisNetwork::TESTNET),
          "mainnet and testnet genesis messages must be domain separated");
    check(main_genesis.get_hash() == core::get_genesis_hash(core::GenesisNetwork::MAINNET),
          "mainnet genesis hash must match the independently mined constant");
    check(test_genesis.get_hash() == core::get_genesis_hash(core::GenesisNetwork::TESTNET),
          "testnet genesis hash must match the independently mined constant");
    check(load_genesis.get_hash() == core::get_genesis_hash(core::GenesisNetwork::LOADNET),
          "loadnet genesis hash must match the independently mined constant");
    check(main_genesis.get_hash() != test_genesis.get_hash(),
          "mainnet and testnet must never share a genesis hash");
    check(core::is_genesis_block(main_genesis, core::GenesisNetwork::MAINNET) &&
              !core::is_genesis_block(main_genesis, core::GenesisNetwork::TESTNET),
          "network-specific genesis recognition must reject cross-network blocks");

    const Hash256 null_hash{};
    check(consensus::ProofOfWork::verify_proof_of_work(
              main_genesis.header,
              main_parameters.bits,
              0,
              null_hash,
              0, consensus::Params(consensus::Network::MAIN)),
          "mainnet genesis must satisfy its canonical proof-of-work target");
    check(consensus::ProofOfWork::verify_proof_of_work(
              test_genesis.header,
              test_parameters.bits,
              0,
              null_hash,
              0, consensus::Params(consensus::Network::TESTNET)),
          "testnet genesis must satisfy its canonical proof-of-work target");
    check(consensus::ProofOfWork::verify_proof_of_work(
              load_genesis.header,
              load_parameters.bits,
              0,
              null_hash,
              0, consensus::Params(consensus::Network::LOADNET)),
          "loadnet genesis must satisfy its canonical proof-of-work target");

    const auto& main_chain = consensus::Params(consensus::Network::MAIN);
    const auto& test_chain = consensus::Params(consensus::Network::TESTNET);
    const auto& load_chain = consensus::Params(consensus::Network::LOADNET);
    check(main_chain.get_genesis_hash() == main_genesis.get_hash(),
          "mainnet ChainParams must pin the mined mainnet genesis");
    check(test_chain.get_genesis_hash() == test_genesis.get_hash(),
          "testnet ChainParams must pin the mined testnet genesis");
    check(test_chain.pow_limit_bits() == test_parameters.bits,
          "testnet genesis target must equal the canonical testnet PoW limit");
    check(load_chain.get_genesis_hash() == load_genesis.get_hash(),
          "loadnet ChainParams must pin the mined loadnet genesis");
    check(load_chain.fixed_difficulty() && load_chain.pow_limit_bits() == load_parameters.bits,
          "loadnet must use its fixed minimum difficulty");
    check(main_chain.foundation_address().str() == "GEOM3PyazNssCYYzF3tW9YrsSwszLq45QXbAZv" &&
              main_chain.foundation_m() == 3 && main_chain.foundation_n() == 4 &&
              main_chain.foundation_participants().size() == 4,
          "mainnet genesis must pin the public 3-of-4 foundation policy");
    check(load_chain.foundation_address().str() ==
              "GEOM3EuJq9xB1xXoUjDJ6MK6DR28HhxYseseC2" &&
              load_chain.foundation_m() == 4 && load_chain.foundation_n() == 4 &&
              load_chain.foundation_participants().size() == 4,
          "loadnet must pin its isolated 4-of-4 foundation managers");
    check(load_chain.foundation_address().str() != main_chain.foundation_address().str(),
          "loadnet foundation address must not reuse mainnet/testnet");
    check(load_chain.foundation_participants() != main_chain.foundation_participants(),
          "loadnet foundation participants must not reuse mainnet/testnet");
}

void test_loadnet_keeps_trivial_difficulty_fixed() {
    consensus::SelectParams(consensus::Network::LOADNET);
    auto chain = make_regular_chain(consensus::params::DIFFICULTY_DAA_WINDOW + 20);
    for (auto& index : chain) {
        index->header.bits = consensus::Params().initial_bits();
    }
    const auto* tip = chain.back().get();
    check(consensus::calculate_next_difficulty(
              tip, tip->get_block_time() + 1) == consensus::Params().initial_bits(),
          "fast loadnet blocks must keep fixed minimum difficulty");
    check(consensus::calculate_next_difficulty(
              tip, tip->get_block_time() + consensus::params::TARGET_BLOCK_TIME * 20) ==
              consensus::Params().initial_bits(),
          "slow loadnet blocks must keep fixed minimum difficulty");
    check(consensus::SelectParamsByNetworkId("degree-load") &&
              consensus::Params().get_network() == consensus::Network::LOADNET,
          "degree-load network id must select loadnet params");
    consensus::SelectParams(consensus::Network::MAIN);
}

void test_testnet_allows_min_difficulty_after_stall() {
    consensus::SelectParams(consensus::Network::TESTNET);
    auto chain = make_regular_chain(consensus::params::DIFFICULTY_DAA_WINDOW + 20);
    const chain::BlockIndex* tip = chain.back().get();

    const Timestamp regular_next_time = tip->get_block_time() + consensus::params::TARGET_BLOCK_TIME;
    const Timestamp stalled_next_time = tip->get_block_time() + consensus::params::TARGET_BLOCK_TIME * 2 + 1;

    const uint32_t regular_bits = consensus::calculate_next_difficulty(tip, regular_next_time);
    const uint32_t minimum_bits = consensus::calculate_next_difficulty(tip, stalled_next_time);

    const uint256 regular_target = consensus::ProofOfWork::bits_to_target(regular_bits);
    const uint256 minimum_target = consensus::ProofOfWork::bits_to_target(minimum_bits);
    const uint256 pow_limit_target = consensus::ProofOfWork::bits_to_target(
        consensus::Params(consensus::Network::TESTNET).pow_limit_bits()
    );

    check(regular_bits == kStableTestBits,
          "regular testnet cadence should use normal LWMA");
    check(minimum_target > regular_target,
          "stalled testnet cadence should allow minimum difficulty");
    check(minimum_target == pow_limit_target,
          "testnet minimum difficulty must equal pow limit");

    consensus::SelectParams(consensus::Network::MAIN);
}

void test_lwma_step_change_does_not_compound_window_error() {
    consensus::SelectParams(consensus::Network::MAIN);
    auto chain = make_regular_chain(consensus::params::DIFFICULTY_DAA_WINDOW + 20);
    const uint256 stable_target = consensus::ProofOfWork::bits_to_target(kStableTestBits);
    const uint256 minimum_reasonable_target = stable_target / 8;
    auto target_as_long_double = [](const uint256& target) {
        long double value = 0.0L;
        for (int word = 3; word >= 0; --word) {
            value = std::ldexp(value, 64) + static_cast<long double>(target.data[word]);
        }
        return value;
    };
    const long double stable_target_value = target_as_long_double(stable_target);

    for (Height height = chain.back()->height + 1; height <= 260; ++height) {
        core::BlockHeader header;
        header.bits = consensus::calculate_next_difficulty(
            chain.back().get(),
            chain.back()->header.timestamp + consensus::params::TARGET_BLOCK_TIME / 2
        );
        const uint256 target = consensus::ProofOfWork::bits_to_target(header.bits);
        const long double relative_difficulty = stable_target_value / target_as_long_double(target);
        const auto expected_solvetime = static_cast<Timestamp>(std::max<long double>(
            1.0L,
            std::round(
                static_cast<long double>(consensus::params::TARGET_BLOCK_TIME) *
                relative_difficulty / 2.0L
            )
        ));
        header.timestamp = chain.back()->header.timestamp + expected_solvetime;
        header.prev_block_hash = chain.back()->get_block_hash();

        auto index = std::make_unique<chain::BlockIndex>(header);
        index->height = height;
        index->prev = chain.back().get();
        check(target >= minimum_reasonable_target,
              "2x hashrate step must not create exponential LWMA overshoot");
        chain.push_back(std::move(index));
    }

    const uint256 final_target = consensus::ProofOfWork::bits_to_target(chain.back()->header.bits);
    check(final_target < stable_target,
          "LWMA should raise difficulty after a sustained 2x hashrate step");
    check(final_target > stable_target / 4,
          "LWMA response should remain close to the sustained hashrate change");
}

void test_lwma_wide_intermediate_clamps_instead_of_wrapping() {
    consensus::SelectParams(consensus::Network::TESTNET);
    const uint32_t pow_limit_bits = consensus::Params().pow_limit_bits();

    std::vector<std::unique_ptr<chain::BlockIndex>> chain;
    const Timestamp base_time = 1'700'000'000;
    for (Height height = 0; height <= consensus::params::DIFFICULTY_DAA_WINDOW + 10; ++height) {
        core::BlockHeader header;
        header.timestamp = static_cast<uint32_t>(
            base_time + height * consensus::params::TARGET_BLOCK_TIME * 6
        );
        header.bits = pow_limit_bits;
        if (!chain.empty()) {
            header.prev_block_hash = chain.back()->get_block_hash();
        }
        auto index = std::make_unique<chain::BlockIndex>(header);
        index->height = height;
        index->prev = chain.empty() ? nullptr : chain.back().get();
        chain.push_back(std::move(index));
    }

    const Timestamp next_time =
        chain.back()->get_block_time() + consensus::params::TARGET_BLOCK_TIME;
    check(consensus::calculate_next_difficulty(chain.back().get(), next_time) == pow_limit_bits,
          "LWMA wide multiply must clamp an overflowing easy target to pow limit");

    consensus::SelectParams(consensus::Network::MAIN);
}

} // namespace

int main() {
    test_mainnet_stall_does_not_bypass_daa();
    test_daa_uses_partial_window_from_genesis();
    test_consensus_rules_are_not_height_gated();
    test_network_specific_genesis_blocks_are_mined_and_pinned();
    test_loadnet_keeps_trivial_difficulty_fixed();
    test_testnet_allows_min_difficulty_after_stall();
    test_lwma_step_change_does_not_compound_window_error();
    test_lwma_wide_intermediate_clamps_instead_of_wrapping();

    if (failures != 0) {
        std::cerr << failures << " difficulty emergency tests failed\n";
        return 1;
    }

    return 0;
}
