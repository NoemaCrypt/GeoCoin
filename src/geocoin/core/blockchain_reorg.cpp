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
#include <chrono>
#include <string>

namespace geocoin::core {

bool Blockchain::is_chain_valid(chain::BlockIndex* tip) {
    if (!tip) {
        return false;
    }
    if (!tip->has_data()) {
        return false;
    }
    std::vector<const chain::BlockIndex*> checked;
    for (auto* index = tip; index; index = index->prev) {
        GEO_METRIC_INC("blockchain.ancestry_entries_checked_total", 1);
        if (index->is_failed()) return false;
        if (valid_ancestry_.contains(index)) break;
        checked.push_back(index);
    }
    valid_ancestry_.insert(checked.begin(), checked.end());
    return true;
}

void Blockchain::record_permanent_failure(chain::BlockIndex* index) {
    if (!index) {
        return;
    }
    index->status |= chain::BlockStatus::FAILED_VALID;
    block_index_candidates_.erase(index);
    mark_failed_descendants(index);
    if (block_storage_ && !block_storage_->mark_block_failed(index->get_block_hash())) {
        GEO_LOG_ERROR(
            "blockchain",
            "Failed to persist invalid block status for " << HashToHex(index->get_block_hash())
        );
    }
}

void Blockchain::mark_failed_descendants(chain::BlockIndex* failed_root) {
    if (!failed_root) {
        return;
    }

    valid_ancestry_.clear();
    std::map<chain::BlockIndex*, std::vector<chain::BlockIndex*>> children;
    for (const auto& [hash, index] : block_index_) {
        (void)hash;
        if (index->prev) children[index->prev].push_back(index.get());
    }
    std::vector<chain::BlockIndex*> pending{failed_root};
    while (!pending.empty()) {
        auto* parent = pending.back();
        pending.pop_back();
        const auto found = children.find(parent);
        if (found == children.end()) continue;
        for (auto* child : found->second) {
            child->status |= chain::BlockStatus::FAILED_CHILD;
            block_index_candidates_.erase(child);
            pending.push_back(child);
        }
    }
}

chain::BlockIndex* Blockchain::find_most_work_chain() {
    if (block_index_candidates_.empty()) {
        return nullptr;
    }

    // block_index_candidates_ отсортирован по chainwork так, что первый элемент
    // имеет МАКСИМАЛЬНЫЙ chainwork (см. BlockIndexWorkComparator).
    for (auto it = block_index_candidates_.begin(); it != block_index_candidates_.end(); ++it) {
        chain::BlockIndex* candidate = *it;
        if (!candidate) {
            continue;
        }
        if (!is_chain_valid(candidate)) {
            continue;
        }
        return candidate;
    }
    return nullptr;
}

chain::BlockIndex* Blockchain::find_most_work_chain_allowed(
    consensus::ValidationState* first_rejection
) {
    bool have_rejection = false;
    for (chain::BlockIndex* candidate : block_index_candidates_) {
        if (!candidate || !is_chain_valid(candidate)) {
            continue;
        }

        consensus::ValidationState transition_state;
        if (check_chain_transition(candidate, transition_state)) {
            return candidate;
        }
        if (!have_rejection && first_rejection) {
            *first_rejection = transition_state;
            have_rejection = true;
        }
    }
    return nullptr;
}

bool Blockchain::check_chain_transition(
    const chain::BlockIndex* target_tip,
    consensus::ValidationState& state
) const {
    if (!target_tip) {
        return state.error("target tip is null");
    }

    const chain::BlockIndex* old_tip = chain_active_.tip();
    if (!old_tip || old_tip == target_tip) {
        return true;
    }

    const chain::BlockIndex* fork = chain_active_.find_fork(target_tip);
    if (!fork) {
        return state.invalid(
            consensus::RejectCode::CHECKPOINT,
            "reorg-no-common-fork",
            "candidate chain has no common ancestor with active chain",
            false
        );
    }

    if (has_policy_checkpoint_ && !allow_deep_reorg_) {
        if (target_tip->height < policy_checkpoint_height_) {
            return state.invalid(
                consensus::RejectCode::CHECKPOINT,
                "policy-checkpoint-tip-below",
                "candidate chain tip below local policy checkpoint",
                false
            );
        }

        const chain::BlockIndex* checkpoint = target_tip->get_ancestor(policy_checkpoint_height_);
        if (!checkpoint || checkpoint->get_block_hash() != policy_checkpoint_hash_) {
            return state.invalid(
                consensus::RejectCode::CHECKPOINT,
                "policy-checkpoint-mismatch",
                "candidate chain diverges before local policy checkpoint",
                false
            );
        }
    }

    const Height reorg_depth = chain_active_.height() - fork->height;
    if (!allow_deep_reorg_ && reorg_depth > 0 &&
        !consensus::is_reorg_depth_allowed(static_cast<uint64_t>(reorg_depth))) {
        return state.invalid(
            consensus::RejectCode::CHECKPOINT,
            "reorg-too-deep",
            "reorganization depth exceeds MAX_REORG_DEPTH",
            false
        );
    }

    return true;
}

bool Blockchain::connect_block(
    const Block& block,
    consensus::ValidationState& state,
    chain::BlockIndex* index
) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    (void)index;
    if (!chain_state_) {
        return state.error("chainstate is null");
    }
    return chain_state_->apply_block(block, state, skip_expensive_block_verify_);
}

bool Blockchain::disconnect_block(
    const Block& block,
    consensus::ValidationState& state,
    chain::BlockIndex* index
) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    (void)index;
    if (!chain_state_) {
        return state.error("chainstate is null");
    }
    if (!chain_state_->revert_block(block)) {
        return state.error("revert_block failed");
    }
    return true;
}

bool Blockchain::connect_tip(consensus::ValidationState& state, chain::BlockIndex* index) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    if (!index) {
        return state.error("null index");
    }
    Block block;
    if (!load_block_from_disk(index, block)) {
        return state.error("failed to load block from disk");
    }

    chain::BlockIndex* old_tip = chain_active_.tip();
    storage::ActiveChainTransitionRecord transition;
    transition.kind = storage::ActiveChainTransitionKind::CONNECT;
    transition.old_tip_exists = old_tip != nullptr;
    if (old_tip) {
        transition.old_height = old_tip->height;
        transition.old_tip_hash = old_tip->get_block_hash();
    }
    transition.new_height = index->height;
    transition.new_tip_hash = index->get_block_hash();
    if (block_storage_ && !block_storage_->begin_active_chain_transition(transition)) {
        return state.error("failed to persist connect transition intent");
    }

    if (!connect_block(block, state, index)) {
        if (block_storage_ && !block_storage_->clear_active_chain_transition()) {
            GEO_LOG_WARN("blockchain", "Failed to clear aborted connect transition intent");
        }
        return false;
    }

    // Если apply_block прошёл — undo гарантирован (ChainState enforce).
    index->status |= chain::BlockStatus::HAVE_UNDO;
    index->status |= chain::BlockStatus::VALID_CHAIN;
    index->status |= chain::BlockStatus::VALID_SCRIPTS;

    // Важно: active-chain индекс = SoT, поэтому любые ошибки персистентности
    // должны приводить к откату применения блока.
    chain_active_.set_tip(index);

    if (block_storage_) {
        if (!block_storage_->set_active_block_and_tip(index->height, index->get_block_hash())) {
            GEO_LOG_ERROR("blockchain", "Failed to persist active chain update, rolling back connect_tip");

            // Откат состояния UTXO (undo обязателен)
            consensus::ValidationState rollback_state;
            if (!disconnect_block(block, rollback_state, index)) {
                return state.error("connect_tip persistence failed and rollback failed");
            }
            chain_active_.set_tip(old_tip);
            if (!block_storage_->clear_active_chain_transition()) {
                GEO_LOG_WARN("blockchain", "Failed to clear rolled-back connect transition intent");
            }
            return state.error("failed to persist active chain update");
        }
        if (!block_storage_->clear_active_chain_transition()) {
            GEO_LOG_WARN("blockchain", "Failed to clear committed connect transition intent");
        }
    }

    return true;
}

bool Blockchain::disconnect_tip(consensus::ValidationState& state) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    chain::BlockIndex* tip = chain_active_.tip();
    if (!tip) {
        return true;
    }
    if (!tip->prev) {
        return state.error("cannot disconnect genesis");
    }

    Block block;
    if (!load_block_from_disk(tip, block)) {
        return state.error("failed to load block from disk");
    }

    chain::BlockIndex* new_tip_index = tip->prev;
    storage::ActiveChainTransitionRecord transition;
    transition.kind = storage::ActiveChainTransitionKind::DISCONNECT;
    transition.old_tip_exists = true;
    transition.old_height = tip->height;
    transition.old_tip_hash = tip->get_block_hash();
    transition.new_height = new_tip_index->height;
    transition.new_tip_hash = new_tip_index->get_block_hash();
    if (block_storage_ && !block_storage_->begin_active_chain_transition(transition)) {
        return state.error("failed to persist disconnect transition intent");
    }

    if (!disconnect_block(block, state, tip)) {
        if (block_storage_ && !block_storage_->clear_active_chain_transition()) {
            GEO_LOG_WARN("blockchain", "Failed to clear aborted disconnect transition intent");
        }
        return false;
    }

    chain_active_.set_tip(new_tip_index);

    if (block_storage_) {
        const Height new_height = chain_active_.height();
        const chain::BlockIndex* new_tip = chain_active_.tip();
        const Hash256 new_tip_hash = new_tip ? new_tip->get_block_hash() : Hash256{};

        if (!block_storage_->truncate_active_chain_and_set_tip(new_height + 1, new_tip_hash, new_height)) {
            GEO_LOG_ERROR("blockchain", "Failed to persist active chain update, rolling back disconnect_tip");

            // Пытаемся вернуть UTXO назад применением блока.
            consensus::ValidationState rollback_state;
            if (!connect_block(block, rollback_state, tip)) {
                return state.error("disconnect_tip persistence failed and rollback failed");
            }
            chain_active_.set_tip(tip);
            if (!block_storage_->clear_active_chain_transition()) {
                GEO_LOG_WARN("blockchain", "Failed to clear rolled-back disconnect transition intent");
            }
            return state.error("failed to persist active chain update");
        }
        if (!block_storage_->clear_active_chain_transition()) {
            GEO_LOG_WARN("blockchain", "Failed to clear committed disconnect transition intent");
        }
    }

    return true;
}

bool Blockchain::activate_best_chain(consensus::ValidationState& state, const Block* block) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    (void)block;

    consensus::ValidationState first_rejection;
    chain::BlockIndex* best_tip = find_most_work_chain_allowed(&first_rejection);
    if (!best_tip) {
        if (!first_rejection.is_valid()) {
            state = first_rejection;
            return false;
        }
        return state.error("no best chain found");
    }

    if (chain_active_.tip() == best_tip) {
        return true;
    }

    return transition_to_tip(best_tip, state);
}

bool Blockchain::transition_to_tip(chain::BlockIndex* best_tip, consensus::ValidationState& state) {
    const auto reorg_start = std::chrono::steady_clock::now();
    chain::BlockIndex* old_tip = chain_active_.tip();
    // best_tip уже прошел check_chain_transition() при policy-aware выборе.
    const chain::BlockIndex* fork = chain_active_.find_fork(best_tip);
    const Height fork_height = fork ? fork->height : 0;
    const Height old_height = chain_active_.height();
    const Height new_height = best_tip->height;
    const Height reorg_depth = old_height - fork_height;

    // Запоминаем старую ветку от tip до форка (исключая форк), чтобы уметь восстановиться.
    std::vector<chain::BlockIndex*> old_branch;
    for (chain::BlockIndex* p = old_tip; p && p->height > fork_height; p = p->prev) {
        old_branch.push_back(p);
    }

    const auto restore_old_branch = [&]() {
        consensus::ValidationState rollback_state;
        for (auto it = old_branch.rbegin(); it != old_branch.rend(); ++it) {
            if ((*it)->height <= chain_active_.height()) continue;
            if (!connect_tip(rollback_state, *it)) return false;
        }
        return chain_active_.tip() == old_tip;
    };

    while (chain_active_.height() > fork_height) {
        if (!disconnect_tip(state)) {
            if (!restore_old_branch()) {
                return state.error("reorg disconnect failed and old chain recovery failed");
            }
            return false;
        }
    }

    // Строим список блоков для подключения в правильном порядке.
    std::vector<chain::BlockIndex*> to_connect;
    for (chain::BlockIndex* p = best_tip; p && p->height > fork_height; p = p->prev) {
        to_connect.push_back(p);
    }
    std::reverse(to_connect.begin(), to_connect.end());

    // Подключаем новую ветку; при ошибке восстанавливаем старую.
    for (chain::BlockIndex* idx : to_connect) {
        if (!connect_tip(state, idx)) {
            const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - reorg_start
            ).count();
            GEO_LOG_WARN(
                "blockchain",
                "Reorg failed: old_height=" << old_height
                << ", candidate_height=" << new_height
                << ", fork_height=" << fork_height
                << ", disconnected=" << old_branch.size()
                << ", connected_before_fail=" << (idx ? (idx->height - fork_height - 1) : 0)
                << ", elapsed_ms=" << elapsed_ms
                << ", reason=" << state.get_reject_reason()
            );
            // A disk/read/commit failure says nothing about consensus validity.
            // Keep the candidate retryable after the local failure is repaired.
            if (state.is_invalid()) {
                record_permanent_failure(idx);
            }

            // Откатываем всё, что успели подключить по новой ветке.
            consensus::ValidationState rollback_state;
            while (chain_active_.height() > fork_height) {
                if (!disconnect_tip(rollback_state)) {
                    return state.error("reorg failed and rollback to fork failed");
                }
            }

            if (!restore_old_branch()) {
                return state.error("reorg failed and rollback to old chain failed");
            }

            return false;
        }
    }

    // connect_tip/disconnect_tip durably update only the changed heights.
    // Startup checks repair an inconsistent index; rewriting it here makes
    // ordinary synchronization quadratic in chain height.

    if (old_height != new_height || old_tip != best_tip) {
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - reorg_start
        ).count();
        if (reorg_depth > 0) {
            GEO_METRIC_INC("blockchain.reorg_applied_total", 1);
            GEO_METRIC_SET("blockchain.reorg_depth", static_cast<int64_t>(reorg_depth));
        }
        GEO_LOG_INFO(
            "blockchain",
            "Reorg applied: old_height=" << old_height
            << ", new_height=" << new_height
            << ", fork_height=" << fork_height
            << ", disconnected=" << old_branch.size()
            << ", connected=" << to_connect.size()
            << ", old_chainwork=" << (old_tip ? Uint256ToHex(old_tip->chain_work) : std::string("0"))
            << ", new_chainwork=" << Uint256ToHex(best_tip->chain_work)
            << ", elapsed_ms=" << elapsed_ms
        );
    }

    maybe_update_policy_checkpoint();

    return true;
}

bool Blockchain::switch_to_tip(const Hash256& tip_hash, consensus::ValidationState& state) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    chain::BlockIndex* target_tip = find_block_index(tip_hash);
    if (!target_tip) {
        state.error("target tip not found");
        record_block_reject_(state, false);
        return false;
    }
    if (!is_chain_valid(target_tip)) {
        state.error("target chain is not valid");
        record_block_reject_(state, false);
        return false;
    }

    if (chain_active_.tip() == target_tip) {
        return true;
    }

    if (!check_chain_transition(target_tip, state)) {
        if (state.get_reject_reason() == "reorg-too-deep") {
            GEO_METRIC_INC("blockchain.reorg_rejected_too_deep_total", 1);
        }
        record_block_reject_(state, true);
        return false;
    }
    if (!transition_to_tip(target_tip, state)) {
        record_block_reject_(state, false);
        return false;
    }
    return true;
}

} // namespace geocoin::core
