#include "geocoin/node/chain_coordinator.hpp"

#include "geocoin/core/blockchain.hpp"
#include "geocoin/network/mempool.hpp"
#include "geocoin/utils/logger.hpp"

#include <algorithm>
#include <chrono>
#include <exception>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

namespace geocoin::node {
namespace {

services::BlockSubmissionResult unavailable_block_result() {
    services::BlockSubmissionResult result;
    result.outcome = services::SubmissionOutcome::UNAVAILABLE;
    result.reject_reason = "chain-coordinator-unavailable";
    return result;
}

services::ChainTipActivationResult unavailable_tip_activation_result(const Hash256& tip_hash) {
    services::ChainTipActivationResult result;
    result.requested_tip_hash = tip_hash;
    result.outcome = services::SubmissionOutcome::UNAVAILABLE;
    result.reject_reason = "chain-coordinator-unavailable";
    return result;
}

services::TransactionSubmissionResult unavailable_transaction_result(const core::Transaction& tx) {
    services::TransactionSubmissionResult result;
    result.outcome = services::SubmissionOutcome::UNAVAILABLE;
    result.txid = tx.get_hash();
    result.reject_reason = "chain-coordinator-unavailable";
    return result;
}

services::ChainBlockChange load_block_change(
    const core::Blockchain& blockchain,
    const chain::BlockIndex* index,
    size_t& approximate_bytes
) {
    services::ChainBlockChange change;
    if (!index) {
        return change;
    }

    change.height = index->height;
    change.hash = index->get_block_hash();
    change.body_available = blockchain.get_block(change.hash, change.block);
    if (change.body_available) {
        try {
            approximate_bytes += change.block.serialize().size();
        } catch (...) {
            // The block was already accepted and deserialized. Size accounting
            // is only a backpressure hint, not a second validation path.
            approximate_bytes += sizeof(core::Block);
        }
    }
    return change;
}

void build_transition(
    const core::Blockchain& blockchain,
    const core::ActiveTipSnapshot& before,
    const core::ActiveTipSnapshot& after,
    services::ChainTransition& transition
) {
    transition.old_tip_hash = before.hash;
    transition.old_height = before.height;
    transition.new_tip_hash = after.hash;
    transition.new_height = after.height;

    const chain::BlockIndex* old_cursor = before.index;
    const chain::BlockIndex* new_cursor = after.index;
    std::vector<const chain::BlockIndex*> disconnected;
    std::vector<const chain::BlockIndex*> connected_reverse;

    while (old_cursor && new_cursor && old_cursor->height > new_cursor->height) {
        disconnected.push_back(old_cursor);
        old_cursor = old_cursor->prev;
    }
    while (old_cursor && new_cursor && new_cursor->height > old_cursor->height) {
        connected_reverse.push_back(new_cursor);
        new_cursor = new_cursor->prev;
    }
    while (old_cursor && new_cursor &&
           old_cursor->get_block_hash() != new_cursor->get_block_hash()) {
        disconnected.push_back(old_cursor);
        connected_reverse.push_back(new_cursor);
        old_cursor = old_cursor->prev;
        new_cursor = new_cursor->prev;
    }

    transition.fork_height = old_cursor ? old_cursor->height : 0;
    transition.disconnected.reserve(disconnected.size());
    for (const auto* index : disconnected) {
        transition.disconnected.push_back(
            load_block_change(blockchain, index, transition.approximate_bytes)
        );
    }

    std::reverse(connected_reverse.begin(), connected_reverse.end());
    transition.connected.reserve(connected_reverse.size());
    for (const auto* index : connected_reverse) {
        transition.connected.push_back(
            load_block_change(blockchain, index, transition.approximate_bytes)
        );
    }
}

uint64_t unix_time_seconds() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count());
}

} // namespace

ChainCoordinator::ChainCoordinator(
    std::shared_ptr<core::Blockchain> blockchain,
    std::shared_ptr<network::Mempool> mempool,
    EventSink event_sink
)
    : blockchain_(std::move(blockchain))
    , mempool_(std::move(mempool))
    , event_sink_(std::move(event_sink))
{
    if (!blockchain_ || !mempool_) {
        throw std::invalid_argument("ChainCoordinator requires blockchain and mempool");
    }
}

ChainCoordinator::~ChainCoordinator() {
    stop();
}

bool ChainCoordinator::start() {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    if (accepting_ || worker_.joinable()) {
        return false;
    }

    stopping_ = false;
    accepting_ = true;
    try {
        worker_ = std::thread(&ChainCoordinator::worker_loop_, this);
    } catch (...) {
        accepting_ = false;
        stopping_ = false;
        throw;
    }
    return true;
}

void ChainCoordinator::stop() {
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        accepting_ = false;
        stopping_ = true;
    }
    queue_cv_.notify_all();

    if (worker_.joinable()) {
        worker_.join();
    }

    std::lock_guard<std::mutex> lock(queue_mutex_);
    stopping_ = false;
    queue_.clear();
}

bool ChainCoordinator::is_running() const {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    return accepting_ && !stopping_;
}

uint64_t ChainCoordinator::latest_event_sequence() const {
    const uint64_t next = next_event_sequence_.load(std::memory_order_acquire);
    return next == 0 ? 0 : next - 1;
}

services::BlockSubmissionResult ChainCoordinator::process_block(const core::Block& block) {
    auto task = std::make_shared<std::packaged_task<services::BlockSubmissionResult()>>(
        [this, block]() { return process_block_unlocked_(block); }
    );
    auto future = task->get_future();

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (!accepting_ || stopping_) {
            return unavailable_block_result();
        }
        queue_.emplace_back([task]() { (*task)(); });
    }
    queue_cv_.notify_one();

    try {
        return future.get();
    } catch (const std::exception& error) {
        services::BlockSubmissionResult result;
        result.outcome = services::SubmissionOutcome::INTERNAL_ERROR;
        result.reject_reason = "chain-coordinator-exception";
        result.debug_message = error.what();
        try {
            const auto active = blockchain_->get_active_tip_snapshot();
            result.active_height = active.height;
            result.active_tip_hash = active.hash;
        } catch (...) {
        }
        return result;
    } catch (...) {
        services::BlockSubmissionResult result;
        result.outcome = services::SubmissionOutcome::INTERNAL_ERROR;
        result.reject_reason = "chain-coordinator-exception";
        try {
            const auto active = blockchain_->get_active_tip_snapshot();
            result.active_height = active.height;
            result.active_tip_hash = active.hash;
        } catch (...) {
        }
        return result;
    }
}

services::ChainTipActivationResult ChainCoordinator::activate_tip(const Hash256& tip_hash) {
    auto task = std::make_shared<std::packaged_task<services::ChainTipActivationResult()>>(
        [this, tip_hash]() { return activate_tip_unlocked_(tip_hash); }
    );
    auto future = task->get_future();

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (!accepting_ || stopping_) {
            return unavailable_tip_activation_result(tip_hash);
        }
        queue_.emplace_back([task]() { (*task)(); });
    }
    queue_cv_.notify_one();

    try {
        return future.get();
    } catch (const std::exception& error) {
        services::ChainTipActivationResult result;
        result.requested_tip_hash = tip_hash;
        result.outcome = services::SubmissionOutcome::INTERNAL_ERROR;
        result.reject_reason = "chain-coordinator-exception";
        result.debug_message = error.what();
        try {
            const auto active = blockchain_->get_active_tip_snapshot();
            result.active_height = active.height;
            result.active_tip_hash = active.hash;
        } catch (...) {
        }
        return result;
    } catch (...) {
        services::ChainTipActivationResult result;
        result.requested_tip_hash = tip_hash;
        result.outcome = services::SubmissionOutcome::INTERNAL_ERROR;
        result.reject_reason = "chain-coordinator-exception";
        try {
            const auto active = blockchain_->get_active_tip_snapshot();
            result.active_height = active.height;
            result.active_tip_hash = active.hash;
        } catch (...) {
        }
        return result;
    }
}

services::TransactionSubmissionResult ChainCoordinator::admit_transaction(
    const core::Transaction& tx
) {
    return admit_transaction_from_peer(tx, {});
}

services::TransactionSubmissionResult ChainCoordinator::admit_transaction_from_peer(
    const core::Transaction& tx,
    std::string source_peer
) {
    auto task = std::make_shared<std::packaged_task<services::TransactionSubmissionResult()>>(
        [this, tx, source_peer = std::move(source_peer)]() {
            return admit_transaction_unlocked_(tx, source_peer);
        }
    );
    auto future = task->get_future();

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (!accepting_ || stopping_) {
            return unavailable_transaction_result(tx);
        }
        queue_.emplace_back([task]() { (*task)(); });
    }
    queue_cv_.notify_one();

    try {
        return future.get();
    } catch (const std::exception& error) {
        services::TransactionSubmissionResult result;
        result.outcome = services::SubmissionOutcome::INTERNAL_ERROR;
        result.txid = tx.get_hash();
        result.reject_reason = "chain-coordinator-exception";
        result.debug_message = error.what();
        return result;
    } catch (...) {
        services::TransactionSubmissionResult result;
        result.outcome = services::SubmissionOutcome::INTERNAL_ERROR;
        result.txid = tx.get_hash();
        result.reject_reason = "chain-coordinator-exception";
        return result;
    }
}

services::BlockSubmissionResult ChainCoordinator::process_block_unlocked_(
    const core::Block& block
) {
    services::BlockSubmissionResult result;
    const core::ActiveTipSnapshot before = blockchain_->get_active_tip_snapshot();

    consensus::ValidationState validation;
    bool accepted = false;
    try {
        accepted = blockchain_->process_new_block(block, validation);
    } catch (const std::exception& error) {
        const core::ActiveTipSnapshot after = blockchain_->get_active_tip_snapshot();
        result.active_height = after.height;
        result.active_tip_hash = after.hash;
        result.active_tip_changed = before.hash != after.hash;
        if (result.active_tip_changed || !blockchain_->get_chain_state()->has_derived_state_snapshot()) {
            result.event_sequence = publish_active_chain_transition_(before, after);
        }
        result.outcome = services::SubmissionOutcome::INTERNAL_ERROR;
        result.reject_reason = "chain-coordinator-exception";
        result.debug_message = error.what();
        return result;
    } catch (...) {
        const core::ActiveTipSnapshot after = blockchain_->get_active_tip_snapshot();
        result.active_height = after.height;
        result.active_tip_hash = after.hash;
        result.active_tip_changed = before.hash != after.hash;
        if (result.active_tip_changed || !blockchain_->get_chain_state()->has_derived_state_snapshot()) {
            result.event_sequence = publish_active_chain_transition_(before, after);
        }
        result.outcome = services::SubmissionOutcome::INTERNAL_ERROR;
        result.reject_reason = "chain-coordinator-exception";
        return result;
    }
    const core::ActiveTipSnapshot after = blockchain_->get_active_tip_snapshot();
    result.active_height = after.height;
    result.active_tip_hash = after.hash;
    result.active_tip_changed = before.hash != after.hash;
    if (result.active_tip_changed || !blockchain_->get_chain_state()->has_derived_state_snapshot()) {
        result.event_sequence = publish_active_chain_transition_(before, after);
    }
    if (!accepted) {
        result.outcome = validation.is_error()
            ? services::SubmissionOutcome::INTERNAL_ERROR
            : services::SubmissionOutcome::REJECTED;
        result.validation_result = validation.get_result();
        result.reject_code = validation.get_reject_code();
        result.reject_reason = validation.get_reject_reason();
        result.debug_message = validation.get_debug_message();
        return result;
    }

    result.outcome = services::SubmissionOutcome::ACCEPTED;
    result.validation_result = consensus::ValidationResult::VALID;

    const Hash256 block_hash = block.get_hash();
    if (const auto* index = blockchain_->find_block_index(block_hash)) {
        result.block_height = index->height;
        Hash256 active_hash{};
        result.block_is_active =
            blockchain_->get_active_block_hash_by_height(index->height, active_hash) &&
            active_hash == block_hash;
    }

    return result;
}

services::ChainTipActivationResult ChainCoordinator::activate_tip_unlocked_(
    const Hash256& tip_hash
) {
    services::ChainTipActivationResult result;
    result.requested_tip_hash = tip_hash;
    const core::ActiveTipSnapshot before = blockchain_->get_active_tip_snapshot();

    consensus::ValidationState validation;
    bool accepted = false;
    try {
        accepted = blockchain_->switch_to_tip(tip_hash, validation);
    } catch (const std::exception& error) {
        const core::ActiveTipSnapshot after = blockchain_->get_active_tip_snapshot();
        result.active_height = after.height;
        result.active_tip_hash = after.hash;
        result.active_tip_changed = before.hash != after.hash;
        if (result.active_tip_changed || !blockchain_->get_chain_state()->has_derived_state_snapshot()) {
            result.event_sequence = publish_active_chain_transition_(before, after);
        }
        result.outcome = services::SubmissionOutcome::INTERNAL_ERROR;
        result.reject_reason = "chain-coordinator-exception";
        result.debug_message = error.what();
        return result;
    } catch (...) {
        const core::ActiveTipSnapshot after = blockchain_->get_active_tip_snapshot();
        result.active_height = after.height;
        result.active_tip_hash = after.hash;
        result.active_tip_changed = before.hash != after.hash;
        if (result.active_tip_changed || !blockchain_->get_chain_state()->has_derived_state_snapshot()) {
            result.event_sequence = publish_active_chain_transition_(before, after);
        }
        result.outcome = services::SubmissionOutcome::INTERNAL_ERROR;
        result.reject_reason = "chain-coordinator-exception";
        return result;
    }
    const core::ActiveTipSnapshot after = blockchain_->get_active_tip_snapshot();
    result.active_height = after.height;
    result.active_tip_hash = after.hash;
    result.active_tip_changed = before.hash != after.hash;
    if (result.active_tip_changed || !blockchain_->get_chain_state()->has_derived_state_snapshot()) {
        result.event_sequence = publish_active_chain_transition_(before, after);
    }
    if (!accepted) {
        result.outcome = validation.is_error()
            ? services::SubmissionOutcome::INTERNAL_ERROR
            : services::SubmissionOutcome::REJECTED;
        result.validation_result = validation.get_result();
        result.reject_code = validation.get_reject_code();
        result.reject_reason = validation.get_reject_reason();
        result.debug_message = validation.get_debug_message();
        return result;
    }

    result.outcome = services::SubmissionOutcome::ACCEPTED;
    result.validation_result = consensus::ValidationResult::VALID;
    return result;
}

services::TransactionSubmissionResult ChainCoordinator::admit_transaction_unlocked_(
    const core::Transaction& tx,
    const std::string& source_peer
) {
    services::TransactionSubmissionResult result;
    result.txid = tx.get_hash();
    if (!mempool_->add_transaction(tx)) {
        result.outcome = services::SubmissionOutcome::REJECTED;
        result.reject_reason = "mempool-rejected";
        return result;
    }

    result.outcome = services::SubmissionOutcome::ACCEPTED;
    result.event_sequence = next_event_sequence_.fetch_add(1, std::memory_order_acq_rel);

    services::ChainMutationEvent event;
    event.sequence = result.event_sequence;
    event.kind = services::ChainMutationEventKind::TRANSACTION_ACCEPTED;
    event.transaction = tx;
    event.observed_timestamp = unix_time_seconds();
    event.source_peer = source_peer;
    try {
        event.approximate_bytes = tx.serialize().size();
    } catch (...) {
        event.approximate_bytes = sizeof(core::Transaction);
    }
    emit_event_(std::move(event));
    return result;
}

void ChainCoordinator::reconcile_mempool_(const services::ChainTransition& transition) {
    std::vector<core::Block> connected_blocks;
    std::set<Hash256> connected_transactions;
    connected_blocks.reserve(transition.connected.size());
    for (const auto& change : transition.connected) {
        if (!change.body_available) {
            GEO_LOG_ERROR(
                "chain-coordinator",
                "Active block body unavailable during mempool reconciliation at height "
                    << change.height
            );
            continue;
        }
        connected_blocks.push_back(change.block);
        for (const auto& tx : change.block.transactions) {
            connected_transactions.insert(tx.get_hash());
        }
    }
    // The complete transition has already been applied. Revalidate survivors
    // once at its final tip, also when no replacement blocks were connected.
    mempool_->reconcile_with_chain(connected_blocks, !transition.disconnected.empty());

    // Reinsert transactions from the disconnected branch in dependency order
    // (fork child first). Coinbase and transactions also present in the new
    // branch are never admitted to the mempool.
    for (auto change_it = transition.disconnected.rbegin();
         change_it != transition.disconnected.rend();
         ++change_it) {
        if (!change_it->body_available) {
            GEO_LOG_WARN(
                "chain-coordinator",
                "Disconnected block body unavailable for mempool recovery at height "
                    << change_it->height
            );
            continue;
        }
        for (const auto& tx : change_it->block.transactions) {
            const Hash256 txid = tx.get_hash();
            if (tx.is_coinbase() ||
                connected_transactions.count(txid) > 0) {
                continue;
            }
            (void)mempool_->add_transaction(tx);
        }
    }
}

uint64_t ChainCoordinator::publish_active_chain_transition_(
    const core::ActiveTipSnapshot& before,
    const core::ActiveTipSnapshot& after
) {
    const uint64_t sequence = next_event_sequence_.fetch_add(1, std::memory_order_acq_rel);
    services::ChainTransition transition;
    transition.sequence = sequence;
    bool state_available = false;
    bool mempool_reconciled = false;
    try {
        state_available = blockchain_->get_chain_state()->has_derived_state_snapshot();
        if (state_available) {
            // Keep the already described fork even if a later body read fails.
            // These are post-commit effects; failure must not change the result
            // of the chain mutation or escape onto the caller's thread.
            build_transition(*blockchain_, before, after, transition);
            reconcile_mempool_(transition);
            mempool_reconciled = true;
            const auto bodies_available = [](const auto& changes) {
                return std::all_of(changes.begin(), changes.end(), [](const auto& change) {
                    return change.body_available;
                });
            };
            if (bodies_available(transition.connected) && bodies_available(transition.disconnected)) {
                services::ChainMutationEvent event;
                event.sequence = sequence;
                event.kind = services::ChainMutationEventKind::ACTIVE_CHAIN_CHANGED;
                event.approximate_bytes = transition.approximate_bytes;
                event.transition = std::move(transition);
                emit_event_(std::move(event));
                return sequence;
            }
        }
    } catch (const std::exception& error) {
        GEO_LOG_ERROR("chain-coordinator", "Post-commit reconciliation failed: " << error.what());
    } catch (...) {
        GEO_LOG_ERROR("chain-coordinator", "Post-commit reconciliation failed with a non-standard exception");
    }

    // A partially checked mempool cannot describe the new active state. Clear
    // its entries and relay bookkeeping on the same writer before publishing
    // recovery. Valid pending transactions can be admitted again afterwards.
    if (!mempool_reconciled) mempool_->clear();
    Height rescan_from = 0;
    if (state_available && before.index && after.index) {
        rescan_from = transition.fork_height < (std::numeric_limits<Height>::max)()
            ? transition.fork_height + 1 : transition.fork_height;
    }
    services::ChainMutationEvent event;
    event.sequence = sequence;
    event.kind = services::ChainMutationEventKind::RESCAN_REQUIRED;
    event.gap = {sequence, sequence, true, rescan_from};
    event.approximate_bytes = 1;
    emit_event_(std::move(event));
    return sequence;
}

void ChainCoordinator::emit_event_(services::ChainMutationEvent event) {
    if (!event_sink_) {
        return;
    }
    try {
        if (!event_sink_(std::move(event))) {
            GEO_LOG_ERROR("chain-coordinator", "Committed mutation event was not accepted by dispatcher");
        }
    } catch (const std::exception& error) {
        GEO_LOG_ERROR("chain-coordinator", "Mutation event sink failed: " << error.what());
    } catch (...) {
        GEO_LOG_ERROR("chain-coordinator", "Mutation event sink failed with a non-standard exception");
    }
}

void ChainCoordinator::worker_loop_() {
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait(lock, [this]() { return stopping_ || !queue_.empty(); });
            if (stopping_ && queue_.empty()) {
                return;
            }
            task = std::move(queue_.front());
            queue_.pop_front();
        }

        try {
            task();
        } catch (const std::exception& error) {
            GEO_LOG_ERROR("chain-coordinator", "Unhandled task exception: " << error.what());
        } catch (...) {
            GEO_LOG_ERROR("chain-coordinator", "Unhandled non-standard task exception");
        }
    }
}

} // namespace geocoin::node
