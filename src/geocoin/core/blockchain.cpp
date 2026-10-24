#include "geocoin/core/blockchain.hpp"
#include "geocoin/core/blockchain_internal.hpp"
#include "geocoin/consensus/consensus.hpp"
#include "geocoin/consensus/proof_of_work.hpp"
#include "geocoin/consensus/chainparams.hpp"
#include "geocoin/utils/logger.hpp"
#include "geocoin/utils/metrics.hpp"

#include <algorithm>
#include <functional>
#include <iostream>
#include <vector>

namespace geocoin::core {

Blockchain::Blockchain(
    std::shared_ptr<state::ChainState> chain_state,
    std::shared_ptr<storage::BlockStorage> block_storage
)
    : chain_state_(chain_state)
    , block_storage_(block_storage)
{
}

Blockchain::~Blockchain() {
}

Blockchain::BlockRejectStats Blockchain::get_block_reject_stats() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return block_reject_stats_;
}

void Blockchain::record_block_reject_(const consensus::ValidationState& state, bool policy) {
    std::string reason = state.get_reject_reason();
    if (reason.empty()) {
        reason = state.get_debug_message();
    }
    if (reason.empty()) {
        reason = "unknown";
    }

    auto& target = policy ? block_reject_stats_.policy : block_reject_stats_.consensus;
    target[reason]++;
}

PolicyCheckpointInfo Blockchain::get_policy_checkpoint_info() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    PolicyCheckpointInfo info;
    info.has_checkpoint = has_policy_checkpoint_;
    info.height = policy_checkpoint_height_;
    info.hash = policy_checkpoint_hash_;
    info.interval = policy_checkpoint_interval_;
    info.confirmations = policy_checkpoint_confirmations_;
    info.allow_deep_reorg = allow_deep_reorg_;
    return info;
}

void Blockchain::set_policy_checkpoint_options(
    Height interval,
    Height confirmations,
    bool allow_deep_reorg
) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    policy_checkpoint_interval_ = interval;
    policy_checkpoint_confirmations_ = confirmations;
    allow_deep_reorg_ = allow_deep_reorg;
    if (chain_state_) {
        chain_state_->set_undo_retention_depth(
            allow_deep_reorg ? 0 : consensus::params::MAX_REORG_DEPTH
        );
    }
}

bool Blockchain::load_policy_checkpoint() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    has_policy_checkpoint_ = false;
    policy_checkpoint_height_ = 0;
    policy_checkpoint_hash_ = {};

    if (!block_storage_ || chain_active_.tip() == nullptr) {
        return true;
    }

    Hash256 stored_hash;
    Height stored_height = 0;
    if (!block_storage_->get_policy_checkpoint(stored_hash, stored_height)) {
        return true;
    }

    if (stored_height > chain_active_.height()) {
        GEO_LOG_WARN(
            "blockchain",
            "Rejecting stale policy checkpoint above active tip: cp_height=" << stored_height
                << ", tip_height=" << chain_active_.height()
        );
        return false;
    }

    chain::BlockIndex* idx = chain_active_[stored_height];
    if (!idx || idx->get_block_hash() != stored_hash) {
        GEO_LOG_WARN(
            "blockchain",
            "Rejecting invalid policy checkpoint (hash mismatch at height " << stored_height << ")"
        );
        return false;
    }

    has_policy_checkpoint_ = true;
    policy_checkpoint_height_ = stored_height;
    policy_checkpoint_hash_ = stored_hash;
    return true;
}

void Blockchain::maybe_update_policy_checkpoint() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    if (!block_storage_ || chain_active_.tip() == nullptr) {
        return;
    }

    if (policy_checkpoint_interval_ == 0 || policy_checkpoint_confirmations_ == 0) {
        return;
    }

    const Height active_height = chain_active_.height();
    if (active_height < policy_checkpoint_confirmations_) {
        return;
    }

    const Height finalized_height = active_height - policy_checkpoint_confirmations_;
    if (finalized_height == 0 || finalized_height % policy_checkpoint_interval_ != 0) {
        return;
    }

    if (has_policy_checkpoint_ && finalized_height <= policy_checkpoint_height_) {
        return;
    }

    chain::BlockIndex* finalized = chain_active_[finalized_height];
    if (!finalized) {
        return;
    }

    const Hash256 finalized_hash = finalized->get_block_hash();
    if (!block_storage_->set_policy_checkpoint(finalized_hash, finalized_height)) {
        GEO_LOG_WARN("blockchain", "Failed to persist policy checkpoint at height " << finalized_height);
        return;
    }

    has_policy_checkpoint_ = true;
    policy_checkpoint_height_ = finalized_height;
    policy_checkpoint_hash_ = finalized_hash;

    GEO_LOG_INFO(
        "blockchain",
        "Policy checkpoint updated: height=" << finalized_height
            << ", hash=" << HashToHex(finalized_hash)
    );
}

} // namespace geocoin::core
