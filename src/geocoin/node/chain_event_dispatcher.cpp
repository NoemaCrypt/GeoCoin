#include "geocoin/node/chain_event_dispatcher.hpp"

#include "geocoin/utils/logger.hpp"

#include <algorithm>
#include <chrono>
#include <exception>
#include <limits>
#include <stdexcept>
#include <utility>

namespace geocoin::node {

ChainEventDispatcher::ChainEventDispatcher(Consumer consumer)
    : consumer_(std::move(consumer))
{
    if (!consumer_) {
        throw std::invalid_argument("ChainEventDispatcher requires a consumer");
    }
}

ChainEventDispatcher::~ChainEventDispatcher() {
    stop();
}

bool ChainEventDispatcher::start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (accepting_ || worker_.joinable()) {
        return false;
    }

    stopping_ = false;
    accepting_ = true;
    try {
        worker_ = std::thread(&ChainEventDispatcher::worker_loop_, this);
    } catch (...) {
        accepting_ = false;
        stopping_ = false;
        throw;
    }
    return true;
}

void ChainEventDispatcher::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        accepting_ = false;
        stopping_ = true;
    }
    work_available_.notify_all();

    if (worker_.joinable()) {
        worker_.join();
    }

    std::lock_guard<std::mutex> lock(mutex_);
    queue_.clear();
    pending_bytes_ = 0;
    // A consumer that still fails during shutdown cannot be drained. Retain
    // its compact recovery marker so a later start can reconcile that work.
    stopping_ = false;
}

bool ChainEventDispatcher::publish(services::ChainMutationEvent event) {
    const size_t event_bytes = std::max<size_t>(1, event.approximate_bytes);
    std::lock_guard<std::mutex> lock(mutex_);
    if (!accepting_ || stopping_) {
        return false;
    }

    // Once a gap exists, keep coalescing until the worker has delivered the
    // marker. This prevents later events from overtaking the recovery barrier.
    if (gap_pending_ || !has_capacity_unlocked_(event_bytes)) {
        const bool first_gap = !gap_pending_;
        record_gap_unlocked_(event);
        if (first_gap) {
            GEO_LOG_WARN("chain-events", "Observer queue saturated; scheduling reconciliation marker");
        }
        work_available_.notify_one();
        return true;
    }

    try {
        queue_.push_back(std::move(event));
    } catch (...) {
        record_gap_unlocked_(event);
        work_available_.notify_one();
        return true;
    }
    pending_bytes_ += event_bytes;
    work_available_.notify_one();
    return true;
}

bool ChainEventDispatcher::is_running() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return accepting_ && !stopping_;
}

bool ChainEventDispatcher::has_capacity_unlocked_(size_t event_bytes) const {
    if (queue_.size() >= MAX_PENDING_EVENTS) {
        return false;
    }
    if (event_bytes > MAX_PENDING_BYTES) {
        return queue_.empty() && pending_bytes_ == 0;
    }
    return pending_bytes_ <= MAX_PENDING_BYTES - event_bytes;
}

void ChainEventDispatcher::record_gap_unlocked_(
    const services::ChainMutationEvent& event
) noexcept {
    uint64_t first_sequence = event.sequence;
    uint64_t last_sequence = event.sequence;
    bool chain_transition_lost = false;
    Height rescan_from_height = 0;

    if (event.kind == services::ChainMutationEventKind::RESCAN_REQUIRED) {
        first_sequence = event.gap.first_missing_sequence;
        last_sequence = event.gap.last_missing_sequence;
        chain_transition_lost = event.gap.chain_transition_lost;
        rescan_from_height = event.gap.rescan_from_height;
    } else if (event.kind == services::ChainMutationEventKind::ACTIVE_CHAIN_CHANGED) {
        chain_transition_lost = true;
        rescan_from_height = event.transition.fork_height <
                (std::numeric_limits<Height>::max)()
            ? event.transition.fork_height + 1
            : event.transition.fork_height;
    }

    if (!gap_pending_) {
        pending_gap_.first_missing_sequence = first_sequence;
        pending_gap_.last_missing_sequence = last_sequence;
        pending_gap_.chain_transition_lost = chain_transition_lost;
        pending_gap_.rescan_from_height = rescan_from_height;
        gap_pending_ = true;
        return;
    }

    if (first_sequence != 0 &&
        (pending_gap_.first_missing_sequence == 0 ||
         first_sequence < pending_gap_.first_missing_sequence)) {
        pending_gap_.first_missing_sequence = first_sequence;
    }
    pending_gap_.last_missing_sequence = std::max(
        pending_gap_.last_missing_sequence,
        last_sequence
    );
    if (chain_transition_lost) {
        if (!pending_gap_.chain_transition_lost) {
            pending_gap_.rescan_from_height = rescan_from_height;
        } else {
            pending_gap_.rescan_from_height = std::min(
                pending_gap_.rescan_from_height,
                rescan_from_height
            );
        }
        pending_gap_.chain_transition_lost = true;
    }
}

services::ChainMutationEvent ChainEventDispatcher::take_gap_unlocked_() noexcept {
    services::ChainMutationEvent event;
    event.sequence = pending_gap_.last_missing_sequence;
    event.kind = services::ChainMutationEventKind::RESCAN_REQUIRED;
    event.gap = pending_gap_;
    event.approximate_bytes = 1;
    pending_gap_ = {};
    gap_pending_ = false;
    return event;
}

void ChainEventDispatcher::worker_loop_() {
    using Clock = std::chrono::steady_clock;
    Clock::time_point retry_after{};
    for (;;) {
        services::ChainMutationEvent event;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            work_available_.wait(lock, [this]() {
                return stopping_ || !queue_.empty() || gap_pending_;
            });
            if (!stopping_ && Clock::now() < retry_after) {
                work_available_.wait_until(lock, retry_after, [this]() { return stopping_; });
            }
            if (queue_.empty()) {
                if (gap_pending_) {
                    event = take_gap_unlocked_();
                } else if (stopping_) {
                    return;
                } else {
                    continue;
                }
            } else {
                event = std::move(queue_.front());
                queue_.pop_front();
                const size_t event_bytes = std::max<size_t>(1, event.approximate_bytes);
                pending_bytes_ = pending_bytes_ >= event_bytes
                    ? pending_bytes_ - event_bytes
                    : 0;
            }
        }

        try {
            consumer_(event);
            retry_after = {};
            continue;
        } catch (const std::exception& error) {
            GEO_LOG_ERROR("chain-events", "Observer failed: " << error.what());
        } catch (...) {
            GEO_LOG_ERROR("chain-events", "Observer failed with a non-standard exception");
        }

        // A callback may have applied only part of the event. Reconcile from
        // authoritative state instead of replaying those side effects. Fold
        // later queued events into the same barrier so none can overtake it.
        {
            std::lock_guard<std::mutex> lock(mutex_);
            record_gap_unlocked_(event);
            for (const auto& queued : queue_) record_gap_unlocked_(queued);
            queue_.clear();
            pending_bytes_ = 0;
            if (stopping_ && event.kind == services::ChainMutationEventKind::RESCAN_REQUIRED) {
                GEO_LOG_ERROR("chain-events", "Observer recovery remains pending after shutdown");
                return;
            }
        }
        retry_after = Clock::now() + std::chrono::seconds(1);
    }
}

} // namespace geocoin::node
