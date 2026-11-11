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

uint256 Blockchain::compute_chainwork(chain::BlockIndex* prev_index, uint32_t bits) {
    return consensus::calculate_chainwork(prev_index, bits);
}

void Blockchain::build_skip(chain::BlockIndex* index) {
    if (index) {
        index->build_skip();
    }
}

chain::BlockIndex* Blockchain::find_block_index(const Hash256& hash) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto it = block_index_.find(hash);
    return (it == block_index_.end()) ? nullptr : it->second.get();
}

const chain::BlockIndex* Blockchain::find_block_index(const Hash256& hash) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto it = block_index_.find(hash);
    return (it == block_index_.end()) ? nullptr : it->second.get();
}

bool Blockchain::has_block_index(const Hash256& hash) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return block_index_.find(hash) != block_index_.end();
}

BlockPositionSnapshot Blockchain::get_block_position(const Hash256& hash) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    BlockPositionSnapshot result;
    const auto it = block_index_.find(hash);
    if (it == block_index_.end()) {
        return result;
    }
    const auto* index = it->second.get();
    result.indexed = true;
    result.has_data = index->has_data();
    result.height = index->height;
    result.median_time_past = index->get_median_time_past();
    result.in_active_chain = chain_active_.contains(index);
    if (result.in_active_chain) {
        result.confirmations = chain_active_.height() - index->height + 1;
        if (const auto* next = chain_active_.next(index)) {
            result.next_hash = next->get_block_hash();
        }
    }
    return result;
}

std::vector<storage::BlockStorage::BlockGeographyRecord> Blockchain::get_block_geography_page(Height first, size_t limit) const {
    std::lock_guard lock(mutex_);
    std::vector<storage::BlockStorage::BlockGeographyRecord> result;
    if (!block_storage_ || limit > 1000) return result;
    for (size_t i = 0; i < limit; ++i) {
        if (first > std::numeric_limits<Height>::max() - i) break;
        storage::BlockStorage::BlockGeographyRecord record;
        if (!block_storage_->get_block_geography(first + i, record)) break;
        result.push_back(record);
    }
    return result;
}

bool Blockchain::get_block(const Hash256& hash, Block& block) const {
    if (!block_storage_) {
        return false;
    }
    return block_storage_->get_block(hash, block, true);
}

bool Blockchain::get_block_by_height(Height height, Block& block) const {
    if (!block_storage_) {
        return false;
    }

    Hash256 active_hash{};
    if (block_storage_->get_active_block_hash(height, active_hash)) {
        return block_storage_->get_block(active_hash, block, true);
    }

    return block_storage_->get_block_by_height(height, block, true);
}

bool Blockchain::get_active_block_hash_by_height(Height height, Hash256& out_hash) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    chain::BlockIndex* idx = chain_active_[height];
    if (!idx) {
        return false;
    }
    out_hash = idx->get_block_hash();
    return true;
}

bool Blockchain::prune_history(Height retention, bool prune_indexes, uint64_t* bodies, uint64_t* indexes) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (bodies) *bodies = 0;
    if (indexes) *indexes = 0;
    if (!chain_state_ || !block_storage_ || retention == 0) return false;
    const auto height = chain_active_.height();
    if (height < retention) return true;
    const auto keep_from = height - retention + 1;
    try {
        if (!chain_state_->prune_block_bodies_below_height(keep_from, bodies)) return false;
        if (!prune_indexes) return true;
        uint64_t discarded_bodies = 0;
        const bool ok = prune_block_index_below_height(keep_from, indexes, &discarded_bodies);
        if (bodies) *bodies += discarded_bodies;
        return ok;
    } catch (const storage::ReadError& error) {
        GEO_LOG_ERROR("blockchain", "History maintenance read failed: " << error.what());
        return false;
    }
}

bool Blockchain::prune_block_index_below_height(Height keep_from_height, uint64_t* pruned_count,
                                                uint64_t* discarded_body_count) {
    if (pruned_count) {
        *pruned_count = 0;
    }
    if (discarded_body_count) *discarded_body_count = 0;
    if (!block_storage_) {
        return false;
    }
    if (keep_from_height == 0) {
        return true;
    }

    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const Height anchor_height = keep_from_height - 1;
    chain::BlockIndex* anchor = chain_active_[anchor_height];
    if (!anchor) {
        GEO_LOG_WARN(
            "blockchain",
            "Cannot prune block indexes: missing active anchor at height " << anchor_height
        );
        return false;
    }

    if (!block_storage_->prune_block_indexes_below_height(
        keep_from_height,
        anchor->get_block_hash(),
        anchor->get_block_header(),
        anchor->chain_work,
        pruned_count,
        discarded_body_count
    )) return false;

    // Keep header objects alive: sync and skip links can still reference them.
    // Their bodies and candidacy must nevertheless reflect the compacted DB.
    const bool anchor_has_body = block_storage_->has_block(anchor->get_block_hash());
    for (auto& [hash, index] : block_index_) {
        const bool retained = index->height >= anchor_height && index->get_ancestor(anchor_height) == anchor;
        if (retained && (index.get() != anchor || anchor_has_body)) continue;
        block_index_candidates_.erase(index.get());
        index->status = static_cast<chain::BlockStatus>(
            static_cast<uint32_t>(index->status) & ~static_cast<uint32_t>(chain::BlockStatus::HAVE_DATA));
        index->block_pos.file = -1;
        index->block_pos.pos = 0;
    }
    if (!has_policy_checkpoint_ || policy_checkpoint_height_ < anchor_height) {
        has_policy_checkpoint_ = true;
        policy_checkpoint_height_ = anchor_height;
        policy_checkpoint_hash_ = anchor->get_block_hash();
    }
    return true;
}

std::vector<ChainTipInfo> Blockchain::get_chain_tips() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<ChainTipInfo> tips;
    if (block_index_.empty()) {
        return tips;
    }

    // tip = блок, который не является prev для другого блока.
    std::set<Hash256> non_tips;
    for (const auto& [hash, index] : block_index_) {
        (void)hash;
        if (index && index->prev) {
            non_tips.insert(index->prev->get_block_hash());
        }
    }

    const chain::BlockIndex* active_tip = chain_active_.tip();
    for (const auto& [hash, index] : block_index_) {
        if (!index) {
            continue;
        }
        if (non_tips.count(hash) != 0) {
            continue;
        }

        ChainTipInfo info;
        info.hash = hash;
        info.height = index->height;
        info.active = (active_tip == index.get());

        const chain::BlockIndex* fork = chain_active_.find_fork(index.get());
        const Height fork_height = fork ? fork->height : 0;
        info.branch_length = fork ? (index->height - fork_height) : (index->height + 1);

        if (info.active) {
            info.status = "active";
        } else if (!fork) {
            info.status = "no-common-fork";
        } else if (index->is_failed()) {
            info.status = "invalid";
        } else if (index->has_data()) {
            info.status = "valid-fork";
        } else {
            info.status = "headers-only";
        }

        tips.push_back(std::move(info));
    }

    std::sort(tips.begin(), tips.end(), [](const ChainTipInfo& a, const ChainTipInfo& b) {
        if (a.height != b.height) {
            return a.height > b.height;
        }
        if (a.active != b.active) {
            return a.active;
        }
        return a.branch_length < b.branch_length;
    });

    return tips;
}

void Blockchain::set_ibd_mode(bool enabled) {
    if (block_storage_) {
        block_storage_->set_ibd_mode(enabled);
    }
    if (chain_state_) {
        chain_state_->set_ibd_mode(enabled);
    }
}

bool Blockchain::load_block_from_disk(chain::BlockIndex* index, Block& block) {
    if (!index || !block_storage_) {
        return false;
    }
    try {
        return block_storage_->get_block(index->get_block_hash(), block, true);
    } catch (const storage::ReadError& error) {
        GEO_LOG_ERROR("blockchain", "Block read failed: " << error.what());
        return false;
    }
}

bool Blockchain::collect_active_chain_entries(
    std::vector<std::pair<Height, Hash256>>& out_entries,
    Hash256& out_tip_hash,
    Height& out_tip_height
) const {
    out_entries.clear();
    chain::BlockIndex* tip = chain_active_.tip();
    if (!tip) {
        return false;
    }

    out_tip_hash = tip->get_block_hash();
    out_tip_height = chain_active_.height();
    const bool allow_sparse_active_chain = (chain_active_[0] == nullptr);

    for (Height h = 0;; ++h) {
        chain::BlockIndex* idx = chain_active_[h];
        if (!idx) {
            if (!allow_sparse_active_chain) {
                GEO_LOG_ERROR("blockchain", "Missing active block index at height " << h);
                return false;
            }
        } else {
            out_entries.emplace_back(h, idx->get_block_hash());
        }

        if (h == out_tip_height) {
            break;
        }
    }

    return true;
}

bool Blockchain::active_chain_index_matches() const {
    if (!block_storage_) {
        return true;
    }

    chain::BlockIndex* tip = chain_active_.tip();
    if (!tip) {
        return false;
    }

    const Hash256 expected_tip_hash = tip->get_block_hash();
    const Height expected_tip_height = chain_active_.height();

    Hash256 stored_tip_hash{};
    Height stored_tip_height = 0;
    if (!block_storage_->get_active_tip(stored_tip_hash, stored_tip_height) ||
        stored_tip_hash != expected_tip_hash ||
        stored_tip_height != expected_tip_height) {
        return false;
    }

    const bool allow_sparse_active_chain = (chain_active_[0] == nullptr);
    for (Height h = 0;; ++h) {
        chain::BlockIndex* idx = chain_active_[h];
        Hash256 stored_hash{};
        const bool has_stored_hash = block_storage_->get_active_block_hash(h, stored_hash);
        if (!idx) {
            if (!allow_sparse_active_chain || has_stored_hash) {
                return false;
            }
        } else if (!has_stored_hash || stored_hash != idx->get_block_hash()) {
            return false;
        }

        if (h == expected_tip_height) {
            break;
        }
    }

    return true;
}

bool Blockchain::persist_active_chain_index() {
    if (!block_storage_) {
        return true;
    }

    std::vector<std::pair<Height, Hash256>> entries;
    Hash256 tip_hash{};
    Height tip_height = 0;
    if (!collect_active_chain_entries(entries, tip_hash, tip_height)) {
        return false;
    }

    return block_storage_->replace_active_chain(entries, tip_hash, tip_height);
}

bool Blockchain::recover_interrupted_active_chain_transition(bool has_pruned_anchor) {
    if (!block_storage_ || !chain_state_) {
        return true;
    }

    storage::ActiveChainTransitionRecord transition;
    const auto read_result = block_storage_->get_active_chain_transition(transition);
    if (read_result == storage::ActiveChainTransitionReadResult::NOT_FOUND) {
        return true;
    }
    if (read_result != storage::ActiveChainTransitionReadResult::VALID) {
        GEO_LOG_ERROR("blockchain", "Corrupted or unreadable active-chain transition journal");
        return false;
    }

    struct Position {
        bool exists = false;
        Height height = 0;
        Hash256 hash{};
    };

    Position state_position;
    state_position.height = chain_state_->get_height();
    state_position.hash = chain_state_->get_tip_hash();
    state_position.exists = state_position.hash != Hash256{};

    Position active_position;
    active_position.exists = block_storage_->get_active_tip(
        active_position.hash,
        active_position.height
    );

    const Position old_position{
        transition.old_tip_exists,
        transition.old_height,
        transition.old_tip_hash
    };
    const Position new_position{
        true,
        transition.new_height,
        transition.new_tip_hash
    };

    const auto matches = [](const Position& lhs, const Position& rhs) {
        if (lhs.exists != rhs.exists) {
            return false;
        }
        return !lhs.exists || (lhs.height == rhs.height && lhs.hash == rhs.hash);
    };
    const auto valid_index_position = [this](const Position& position) {
        if (!position.exists) {
            return true;
        }
        const chain::BlockIndex* index = find_block_index(position.hash);
        return index && index->height == position.height;
    };
    const auto persist_new = [&]() {
        if (!valid_index_position(new_position)) {
            return false;
        }
        if (transition.kind == storage::ActiveChainTransitionKind::CONNECT) {
            return block_storage_->set_active_block_and_tip(
                new_position.height,
                new_position.hash
            );
        }
        return block_storage_->truncate_active_chain_and_set_tip(
            old_position.height,
            new_position.hash,
            new_position.height
        );
    };
    const auto persist_old = [&]() {
        if (!old_position.exists) {
            return block_storage_->clear_active_chain();
        }
        if (!valid_index_position(old_position)) {
            return false;
        }
        if (transition.kind == storage::ActiveChainTransitionKind::CONNECT) {
            return block_storage_->truncate_active_chain_and_set_tip(
                new_position.height,
                old_position.hash,
                old_position.height
            );
        }
        return block_storage_->set_active_block_and_tip(
            old_position.height,
            old_position.hash
        );
    };

    bool repaired = false;
    bool resolved = false;
    if (matches(state_position, new_position) && matches(active_position, old_position)) {
        GEO_LOG_WARN(
            "blockchain",
            "Rolling active index forward to committed ChainState at height "
                << new_position.height
        );
        if (!persist_new()) {
            return false;
        }
        repaired = true;
        resolved = true;
    } else if (matches(state_position, old_position) && matches(active_position, new_position)) {
        GEO_LOG_WARN(
            "blockchain",
            "Rolling active index back to ChainState at height " << old_position.height
        );
        if (!persist_old()) {
            return false;
        }
        repaired = true;
        resolved = true;
    } else if ((matches(state_position, new_position) && matches(active_position, new_position)) ||
               (matches(state_position, old_position) && matches(active_position, old_position)) ||
               matches(state_position, active_position)) {
        resolved = true;
    }

    if (!resolved) {
        if (has_pruned_anchor) {
            GEO_LOG_ERROR(
                "blockchain",
                "Ambiguous active-chain transition cannot be rebuilt from compacted history"
            );
            return false;
        }
        GEO_LOG_WARN(
            "blockchain",
            "Ambiguous active-chain transition; full-history startup will rebuild ChainState"
        );
    }

    if (!block_storage_->clear_active_chain_transition()) {
        GEO_LOG_ERROR("blockchain", "Failed to clear active-chain transition journal");
        return false;
    }
    if (repaired) {
        GEO_LOG_INFO("blockchain", "Recovered interrupted active-chain transition");
    }
    return true;
}

} // namespace geocoin::core
