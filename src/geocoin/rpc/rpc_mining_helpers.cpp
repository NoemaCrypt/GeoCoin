#include "rpc_mining_helpers.hpp"

#include "geocoin/core/block.hpp"
#include "geocoin/core/blockchain.hpp"
#include "geocoin/core/merkle.hpp"
#include "geocoin/network/mempool.hpp"
#include "geocoin/state/state.hpp"
#include "geocoin/consensus/consensus.hpp"
#include "geocoin/consensus/chainparams.hpp"
#include "geocoin/consensus/geo_tag.hpp"
#include "geocoin/consensus/proof_of_work.hpp"
#include <algorithm>
#include <chrono>
#include <utility>
#include <vector>

namespace geocoin::rpc {

double bits_to_difficulty(uint32_t bits) {
    int nShift = (bits >> 24) & 0xff;
    double dDiff = static_cast<double>(0x0000ffff) / static_cast<double>(bits & 0x00ffffff);

    while (nShift < 29) {
        dDiff *= 256.0;
        nShift++;
    }
    while (nShift > 29) {
        dDiff /= 256.0;
        nShift--;
    }

    return dDiff;
}

void require_height_available_for_mining_rpc(
    const node::NodeInfoService* node_info,
    Height height
) {
    if (node_info && node_info->is_height_pruned_for_api(height)) {
        throw RPCException(RPC_MISC_ERROR, node_info->describe_pruned_api_error(height));
    }
}

namespace {

core::Block build_block_template_for_tip(
    state::ChainState* chain_state,
    network::Mempool* mempool,
    const Address& mining_address,
    const core::ActiveTipSnapshot& tip_snapshot,
    std::optional<Timestamp> requested_timestamp
) {
    core::Block block;
    const Height next_height = tip_snapshot.index ? tip_snapshot.height + 1 : 0;

    block.header.version = 1;
    uint64_t block_timestamp = requested_timestamp.value_or(
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count())
    );
    uint32_t prev_timestamp = 0;
    Hash256 prev_hash{};
    if (tip_snapshot.index) {
        block.header.prev_block_hash = tip_snapshot.hash;
        prev_timestamp = static_cast<uint32_t>(tip_snapshot.median_time_past);
        prev_hash = tip_snapshot.hash;
        const uint64_t min_timestamp = static_cast<uint64_t>(prev_timestamp) + 1;
        if (block_timestamp < min_timestamp) {
            block_timestamp = min_timestamp;
        }
    }
    block.header.timestamp = static_cast<uint32_t>(block_timestamp);
    block.header.bits = consensus::calculate_next_difficulty(tip_snapshot.index, block.header.timestamp);
    block.header.nonce = 0;

    const auto tag = consensus::geo::compute_block_geo_tag(prev_timestamp, prev_hash, next_height,
        consensus::Params().geopow_activation_height());
    block.header.geo_tag = tag.geo_tag;

    core::Transaction interest_distribution;
    const bool has_interest_distribution =
        chain_state->create_interest_distribution_transaction(next_height, interest_distribution);
    size_t mempool_limit = consensus::params::BLOCK_TEMPLATE_SOFT_LIMIT;
    if (has_interest_distribution) {
        const size_t interest_size = interest_distribution.serialize().size();
        mempool_limit = interest_size < mempool_limit ? mempool_limit - interest_size : 0;
    }

    auto mempool_txs = mempool->get_transactions_for_mining(mempool_limit, block.header.timestamp);
    const Amount reward = consensus::ProofOfWork::calculate_block_reward(next_height);
    Amount fees = 0;
    size_t accepted_count = 0;
    for (const Amount fee : chain_state->project_block_transaction_fees(mempool_txs, 0, next_height)) {
        Amount next_fees = 0;
        Amount coinbase_total = 0;
        if (fee < 0 ||
            !checked_add_amount(fees, fee, next_fees) ||
            !checked_add_amount(reward, next_fees, coinbase_total)) {
            break;
        }
        fees = next_fees;
        ++accepted_count;
    }
    mempool_txs.resize(accepted_count);

    block.transactions.push_back(core::Transaction::create_coinbase(
        next_height,
        mining_address,
        reward,
        fees,
        chain_state->get_foundation_address()
    ));
    if (has_interest_distribution) {
        block.transactions.push_back(std::move(interest_distribution));
    }
    for (const auto& tx : mempool_txs) {
        block.transactions.push_back(tx);
    }

    std::vector<Hash256> tx_hashes;
    tx_hashes.reserve(block.transactions.size());
    for (const auto& tx : block.transactions) {
        tx_hashes.push_back(tx.get_hash());
    }
    block.header.merkle_root = core::compute_merkle_root(tx_hashes);
    block.header.witness_commitment = block.calculate_witness_commitment();

    return block;
}

} // namespace

core::Block build_block_template(
    core::Blockchain* blockchain,
    state::ChainState* chain_state,
    network::Mempool* mempool,
    const Address& mining_address,
    std::optional<Timestamp> requested_timestamp
) {
    core::Block last_template;
    for (unsigned int attempt = 0; attempt < 3; ++attempt) {
        const auto tip_snapshot = blockchain->get_active_tip_snapshot();
        last_template = build_block_template_for_tip(
            chain_state,
            mempool,
            mining_address,
            tip_snapshot,
            requested_timestamp
        );
        if (blockchain->is_active_tip(tip_snapshot.hash)) {
            return last_template;
        }
    }
    return last_template;
}

} // namespace geocoin::rpc
