#pragma once

#include "geocoin/services/chain_mutation.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>

namespace geocoin::core {
class Blockchain;
struct ActiveTipSnapshot;
}

namespace geocoin::network {
class Mempool;
}

namespace geocoin::node {

/**
 * Single-writer executor for all active-chain and mempool mutations.
 *
 * Consensus validation remains in Blockchain/ChainState.  This class only
 * establishes ownership and ordering: callers from RPC, P2P and mining wait
 * for the same worker instead of racing through independent mutex graphs.
 */
class ChainCoordinator final : public services::ChainMutationService {
public:
    using EventSink = std::function<bool(services::ChainMutationEvent)>;

    ChainCoordinator(
        std::shared_ptr<core::Blockchain> blockchain,
        std::shared_ptr<network::Mempool> mempool,
        EventSink event_sink = {}
    );
    ~ChainCoordinator() override;

    ChainCoordinator(const ChainCoordinator&) = delete;
    ChainCoordinator& operator=(const ChainCoordinator&) = delete;

    bool start();
    void stop();
    bool is_running() const;
    uint64_t latest_event_sequence() const;

    services::BlockSubmissionResult process_block(const core::Block& block) override;
    services::ChainTipActivationResult activate_tip(const Hash256& tip_hash) override;
    services::TransactionSubmissionResult admit_transaction(const core::Transaction& tx) override;
    services::TransactionSubmissionResult admit_transaction_from_peer(
        const core::Transaction& tx,
        std::string source_peer
    );

private:
    services::BlockSubmissionResult process_block_unlocked_(const core::Block& block);
    services::ChainTipActivationResult activate_tip_unlocked_(const Hash256& tip_hash);
    services::TransactionSubmissionResult admit_transaction_unlocked_(
        const core::Transaction& tx,
        const std::string& source_peer
    );
    void reconcile_mempool_(const services::ChainTransition& transition);
    uint64_t publish_active_chain_transition_(
        const core::ActiveTipSnapshot& before,
        const core::ActiveTipSnapshot& after
    );
    void emit_event_(services::ChainMutationEvent event);
    void worker_loop_();

    std::shared_ptr<core::Blockchain> blockchain_;
    std::shared_ptr<network::Mempool> mempool_;
    EventSink event_sink_;
    std::atomic<uint64_t> next_event_sequence_{1};

    mutable std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<std::function<void()>> queue_;
    std::thread worker_;
    bool accepting_{false};
    bool stopping_{false};
};

} // namespace geocoin::node
