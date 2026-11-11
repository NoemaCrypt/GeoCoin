#pragma once

#include "geocoin/services/chain_mutation.hpp"

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace geocoin::node {

/**
 * Bounded FIFO handoff between the chain writer and observers (wallet, GUI,
 * relay and pruning). Publication never waits for observers. If the queue is
 * saturated or a consumer throws, undelivered events are represented by one
 * ordered RESCAN_REQUIRED marker. Failed reconciliation is retried with a
 * delay; stop interrupts the delay and retains an undelivered marker for a
 * later start. Consumers can reconcile from authoritative chain state.
 */
class ChainEventDispatcher final {
public:
    using Consumer = std::function<void(const services::ChainMutationEvent&)>;

    explicit ChainEventDispatcher(Consumer consumer);
    ~ChainEventDispatcher();

    ChainEventDispatcher(const ChainEventDispatcher&) = delete;
    ChainEventDispatcher& operator=(const ChainEventDispatcher&) = delete;

    bool start();
    void stop();
    bool publish(services::ChainMutationEvent event);
    bool is_running() const;

private:
    bool has_capacity_unlocked_(size_t event_bytes) const;
    void record_gap_unlocked_(const services::ChainMutationEvent& event) noexcept;
    services::ChainMutationEvent take_gap_unlocked_() noexcept;
    void worker_loop_();

    static constexpr size_t MAX_PENDING_EVENTS = 256;
    static constexpr size_t MAX_PENDING_BYTES = 64 * 1024 * 1024;

    Consumer consumer_;
    mutable std::mutex mutex_;
    std::condition_variable work_available_;
    std::deque<services::ChainMutationEvent> queue_;
    size_t pending_bytes_{0};
    services::ChainEventGap pending_gap_{};
    bool gap_pending_{false};
    std::thread worker_;
    bool accepting_{false};
    bool stopping_{false};
};

} // namespace geocoin::node
