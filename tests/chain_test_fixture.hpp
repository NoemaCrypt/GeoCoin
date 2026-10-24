#pragma once

#include "test_utils.hpp"
#include "geocoin/consensus/chainparams.hpp"
#include "geocoin/consensus/consensus.hpp"
#include "geocoin/consensus/geo_tag.hpp"
#include "geocoin/consensus/proof_of_work.hpp"
#include "geocoin/core/blockchain.hpp"
#include "geocoin/state/state.hpp"
#include "geocoin/storage/block_storage.hpp"

#include <memory>

namespace geocoin::test {

// Real PoW and the production validation path, on the isolated easy network.
// Each executable using this fixture selects LOADNET before constructing it.
class TestChain {
public:
    explicit TestChain(std::unique_ptr<state::Storage> backend =
                           std::make_unique<state::MemoryStorage>())
        : directory(make_temp_dir("geocoin-chain-test")),
          blocks(std::make_shared<storage::BlockStorage>((directory / "blocks").string())),
          state(std::make_shared<state::ChainState>(std::move(backend))),
          chain(std::make_shared<core::Blockchain>(state, blocks)) {
        expect(consensus::Params().fixed_difficulty(), "fixture requires loadnet");
        state->set_block_storage(blocks);
        expect(chain->initialize(), "fixture initializes genesis");
    }

    ~TestChain() {
        chain.reset();
        state.reset();
        blocks.reset();
        std::error_code error;
        std::filesystem::remove_all(directory, error);
    }

    core::Block make_block(const chain::BlockIndex* previous = nullptr,
                           Amount excess_reward = 0) const {
        if (!previous) previous = chain->get_tip();
        core::Block block;
        block.header.version = 1;
        block.header.prev_block_hash = previous->get_block_hash();
        block.header.timestamp = previous->get_block_time() + consensus::params::TARGET_BLOCK_TIME;
        block.header.bits = consensus::calculate_next_difficulty(previous, block.header.timestamp);
        block.transactions.push_back(core::Transaction::create_coinbase(
            previous->height + 1, address_from_seed(42),
            consensus::ProofOfWork::calculate_block_reward(previous->height + 1) + excess_reward,
            0, state->get_foundation_address()));
        block.transactions.front().timestamp = block.header.timestamp;
        seal(block, previous);
        return block;
    }

    static void seal(core::Block& block, const chain::BlockIndex* previous) {
        block.header.merkle_root = block.calculate_merkle_root();
        block.header.witness_commitment = block.calculate_witness_commitment();
        const auto time = static_cast<uint32_t>(previous->get_median_time_past());
        block.header.geo_tag = consensus::geo::compute_block_geo_tag(time, previous->get_block_hash(), previous->height + 1,
            consensus::Params().geopow_activation_height()).geo_tag;
        block.header.nonce = 0;
        while (!consensus::ProofOfWork::verify_proof_of_work(
                   block.header, block.header.bits, time, previous->get_block_hash(), previous->height + 1)) {
            expect(++block.header.nonce < 100000, "loadnet PoW unexpectedly difficult");
        }
    }

    chain::BlockIndex* accept(const core::Block& block) const {
        consensus::ValidationState validation;
        expect(consensus::check_block(block, validation), "fixture block passes body checks");
        chain::BlockIndex* index = nullptr;
        expect(chain->accept_block(block, validation, &index), "fixture block accepted into index");
        return index;
    }

    void append(const core::Block& block) const {
        consensus::ValidationState validation;
        expect(chain->process_new_block(block, validation), validation.get_reject_reason());
        expect(state->get_tip_hash() == block.get_hash(), "state advances to appended block");
    }

    std::filesystem::path directory;
    std::shared_ptr<storage::BlockStorage> blocks;
    std::shared_ptr<state::ChainState> state;
    std::shared_ptr<core::Blockchain> chain;
};

} // namespace geocoin::test
