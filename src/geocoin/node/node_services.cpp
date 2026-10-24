#include "geocoin/node/node.hpp"
#include "geocoin/node/chain_coordinator.hpp"

#include "geocoin/network/addrman.hpp"
#include "geocoin/network/messages.hpp"
#include "geocoin/utils/logger.hpp"
#include "geocoin/utils/metrics.hpp"

#include <exception>
#include <limits>
#include <mutex>
#include <utility>

namespace geocoin::node {

NodeServiceHandle Node::service_handle() {
    const auto self = shared_from_this();
    return NodeServiceHandle{
        .chain_query = std::static_pointer_cast<ChainQueryService>(self),
        .info = std::static_pointer_cast<NodeInfoService>(self),
        .mining = std::static_pointer_cast<MiningControlService>(self),
        .network = std::static_pointer_cast<NetworkControlService>(self),
        .events = std::static_pointer_cast<NodeEventService>(self),
        .mutations = std::static_pointer_cast<services::ChainMutationService>(self),
    };
}

NodeComponentHandle Node::component_handle() {
    return NodeComponentHandle{
        .services = service_handle(),
        .blockchain = blockchain_,
        .chain_state = chain_state_,
        .mempool = mempool_,
        .peer_manager = peer_manager_,
        .sync_manager = sync_manager_,
    };
}

bool Node::connect_to_peer(const std::string& address) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_control_mutex_);
    if (!running_) {
        return false;
    }

    const bool success = peer_manager_->add_outbound_peer(address, private_key_, true);
    if (success) {
        {
            std::lock_guard<std::mutex> lock(outbound_reconnect_targets_mutex_);
            outbound_reconnect_targets_.insert(address);
        }
        std::shared_ptr<network::AddrMan> addrman_snapshot;
        {
            std::lock_guard<std::mutex> lock(addrman_mutex_);
            if (!addrman_) {
                addrman_ = std::make_shared<network::AddrMan>();
            }
            addrman_snapshot = addrman_;
        }
        addrman_snapshot->add(address, "manual-connect");
        addrman_snapshot->good(address);
    }
    return success;
}

bool Node::submit_transaction(const core::Transaction& tx) {
    return admit_transaction(tx).is_accepted();
}

services::TransactionSubmissionResult Node::admit_transaction(
    const core::Transaction& tx
) {
    return admit_transaction_from_(tx, nullptr);
}

services::TransactionSubmissionResult Node::admit_transaction_from_(
    const core::Transaction& tx,
    std::shared_ptr<network::Peer> source_peer
) {
    // Do not hold lifecycle_control_mutex_ while waiting on the coordinator.
    // stop() joins all producers before destroying the coordinator, while the
    // atomic running flag closes the public admission gate.  Holding the
    // lifecycle mutex here would deadlock when stop() joins a mining thread
    // that is currently publishing a block.
    if (!running_.load(std::memory_order_acquire)) {
        GEO_METRIC_INC("node.submit_tx_rejected_total", 1);
        services::TransactionSubmissionResult result;
        result.outcome = services::SubmissionOutcome::UNAVAILABLE;
        result.txid = tx.get_hash();
        result.reject_reason = "node-not-running";
        return result;
    }

    const std::string source_address = source_peer ? source_peer->get_address() : std::string{};
    auto result = chain_coordinator_->admit_transaction_from_peer(tx, source_address);
    if (!result.is_accepted()) {
        GEO_METRIC_INC("node.submit_tx_rejected_total", 1);
        return result;
    }
    GEO_METRIC_INC("node.submit_tx_ok_total", 1);
    return result;
}

services::BlockSubmissionResult Node::process_block(const core::Block& block) {
    if (!running_.load(std::memory_order_acquire)) {
        services::BlockSubmissionResult result;
        result.outcome = services::SubmissionOutcome::UNAVAILABLE;
        result.reject_reason = "node-not-running";
        return result;
    }

    return chain_coordinator_->process_block(block);
}

services::ChainTipActivationResult Node::activate_tip(const Hash256& tip_hash) {
    if (!running_.load(std::memory_order_acquire)) {
        services::ChainTipActivationResult result;
        result.requested_tip_hash = tip_hash;
        result.outcome = services::SubmissionOutcome::UNAVAILABLE;
        result.reject_reason = "node-not-running";
        return result;
    }

    return chain_coordinator_->activate_tip(tip_hash);
}

void Node::set_transaction_callback(TransactionCallback cb) {
    const uint64_t latest_sequence = chain_coordinator_
        ? chain_coordinator_->latest_event_sequence()
        : 0;
    std::lock_guard<std::mutex> lock(tx_callback_mutex_);
    tx_callback_ = std::move(cb);
    tx_callback_min_sequence_ = latest_sequence == (std::numeric_limits<uint64_t>::max)()
        ? latest_sequence
        : latest_sequence + 1;
}

void Node::clear_transaction_callback() {
    std::lock_guard<std::mutex> lock(tx_callback_mutex_);
    tx_callback_ = nullptr;
    tx_callback_min_sequence_ = (std::numeric_limits<uint64_t>::max)();
}

void Node::set_chain_mutation_event_callback(ChainMutationEventCallback cb) {
    const uint64_t latest_sequence = chain_coordinator_
        ? chain_coordinator_->latest_event_sequence()
        : 0;
    std::lock_guard<std::mutex> lock(chain_event_callback_mutex_);
    chain_event_callback_ = std::move(cb);
    chain_event_callback_min_sequence_ = latest_sequence == (std::numeric_limits<uint64_t>::max)()
        ? latest_sequence
        : latest_sequence + 1;
}

void Node::clear_chain_mutation_event_callback() {
    std::lock_guard<std::mutex> lock(chain_event_callback_mutex_);
    chain_event_callback_ = nullptr;
    chain_event_callback_min_sequence_ = (std::numeric_limits<uint64_t>::max)();
}

Amount Node::get_balance(const Address& address) const {
    return chain_state_->get_balance(address);
}

bool Node::get_block(const Hash256& block_hash, core::Block& block) const {
    return chain_state_->get_block(block_hash, block);
}

bool Node::get_block_by_height(Height height, core::Block& block) const {
    return chain_state_->get_block_by_height(height, block);
}

bool Node::get_transaction(const Hash256& tx_hash, core::Transaction& tx) const {
    // Сначала проверяем mempool
    if (mempool_->get_transaction(tx_hash, tx)) {
        return true;
    }

    // Поиск в активной цепи через ChainState.
    // Не читаем tx-index storage_ напрямую: старые datadir могли содержать
    // stale записи до введения epoch-aware active-tx индекса.
    if (chain_state_) {
        auto tx_ptr = chain_state_->get_transaction(tx_hash);
        if (tx_ptr) {
            tx = *tx_ptr;
            return true;
        }
    }

    return false;
}

std::vector<core::Transaction> Node::get_mempool_transactions() const {
    return mempool_ ? mempool_->get_all_transactions() : std::vector<core::Transaction>{};
}

void Node::handle_chain_mutation_event(const services::ChainMutationEvent& event) {
    if (event.kind == services::ChainMutationEventKind::TRANSACTION_ACCEPTED) {
        notify_transaction_callback(event.transaction, 0, event.observed_timestamp, event.sequence);
        // The update loop drains and batches the mempool relay set every
        // 500 ms.  For P2P submissions the source peer was marked as knowing
        // the tx before admission, so per-peer inventory filtering still
        // avoids echoing it back without sacrificing batching.
    } else if (event.kind == services::ChainMutationEventKind::ACTIVE_CHAIN_CHANGED) {
        handle_active_chain_transition(event.transition);
    } else {
        GEO_LOG_WARN(
            "node-events",
            "Observer stream gap: sequence " << event.gap.first_missing_sequence
                << ".." << event.gap.last_missing_sequence
        );
        // Relay can recover through normal peer synchronization. Pruning is a
        // local housekeeping operation and can be caught up immediately.
        if (event.gap.chain_transition_lost) {
            prune_old_block_bodies_if_needed();
        }
    }

    ChainMutationEventCallback callback;
    {
        std::lock_guard<std::mutex> lock(chain_event_callback_mutex_);
        if (event.sequence == 0 || event.sequence >= chain_event_callback_min_sequence_) {
            callback = chain_event_callback_;
        }
    }
    if (callback) {
        // The dispatcher must observe delivery failure to schedule an ordered
        // recovery marker, including when this was the last committed event.
        callback(event);
    }
}

void Node::handle_active_chain_transition(const services::ChainTransition& transition) {
    for (const auto& change : transition.connected) {
        if (!change.body_available) {
            continue;
        }
        for (const auto& tx : change.block.transactions) {
            notify_transaction_callback(
                tx,
                change.height,
                change.block.header.timestamp,
                transition.sequence
            );
        }
    }

    if (!transition.connected.empty() && transition.connected.back().body_available) {
        // A tip hash is sufficient for headers-first discovery of every new
        // ancestor. Coalesce rapid commits instead of pushing unsolicited
        // bodies which a lagging peer would discard and download again.
        std::lock_guard<std::mutex> lock(relay_mutex_);
        pending_block_inventory_ = transition.connected.back().block.get_hash();
    }

    prune_old_block_bodies_if_needed();
}

void Node::handle_new_transaction(const core::Transaction& tx, std::shared_ptr<network::Peer> source_peer) {
    (void)admit_transaction_from_(tx, std::move(source_peer));
}

void Node::notify_transaction_callback(
    const core::Transaction& tx,
    Height height,
    uint64_t timestamp,
    uint64_t event_sequence
) const {
    TransactionCallback cb;
    {
        std::lock_guard<std::mutex> lock(tx_callback_mutex_);
        if (event_sequence == 0 || event_sequence >= tx_callback_min_sequence_) {
            cb = tx_callback_;
        }
    }

    if (cb) {
        try {
            cb(tx, height, timestamp);
        } catch (const std::exception& error) {
            GEO_LOG_ERROR("node-events", "Transaction callback failed: " << error.what());
        } catch (...) {
            GEO_LOG_ERROR("node-events", "Transaction callback failed with a non-standard exception");
        }
    }
}


} // namespace geocoin::node
