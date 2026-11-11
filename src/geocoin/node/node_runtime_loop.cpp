#include "geocoin/node/node.hpp"

#include "geocoin/utils/metrics.hpp"

#include <chrono>
#include <set>
#include <thread>
#include <utility>

namespace geocoin::node {

namespace {

using Clock = std::chrono::steady_clock;

class PeriodicDeadline {
public:
    explicit PeriodicDeadline(Clock::duration interval)
        : interval_(interval), next_(Clock::now() + interval) {}

    bool is_due(Clock::time_point now) {
        if (now < next_) {
            return false;
        }
        // Schedule from the actual execution time. If a slow storage/network
        // operation delayed the loop, do not run a burst of catch-up tasks.
        next_ = now + interval_;
        return true;
    }

private:
    Clock::duration interval_;
    Clock::time_point next_;
};

constexpr auto kUpdateInterval = std::chrono::milliseconds(100);
constexpr auto kIbdModeRefreshInterval = std::chrono::seconds(1);
// Admission can be highly concurrent.  Relaying from the event callback would
// create one INV frame per accepted transaction and make honest traffic trip
// the peer INV rate limiter.  A short periodic drain coalesces the mempool's
// relay set into bounded INV batches while keeping propagation sub-second.
constexpr auto kTransactionRelayInterval = std::chrono::milliseconds(500);
constexpr auto kSyncRetryInterval = std::chrono::seconds(10);
constexpr auto kMempoolExpiryInterval = std::chrono::minutes(1);

} // namespace

void Node::update_loop() {
    bool last_ibd_mode = true;
    if (blockchain_) {
        blockchain_->set_ibd_mode(last_ibd_mode);
    }

    std::set<std::string> last_outbound_peers;
    PeriodicDeadline ibd_mode_refresh(kIbdModeRefreshInterval);
    PeriodicDeadline inventory_relay(kTransactionRelayInterval);
    PeriodicDeadline sync_retry(kSyncRetryInterval);
    PeriodicDeadline mempool_expiry(kMempoolExpiryInterval);

    while (running_) {
        const auto iteration_started = Clock::now();

        peer_manager_->update();
        request_pending_transactions();

        // Every successful outbound connection remains a retry target if it
        // disappears later.
        std::set<std::string> current_outbound_peers;
        for (const auto& peer_stat : peer_manager_->get_peer_stats()) {
            if (!peer_stat.inbound && !peer_stat.address.empty()) {
                current_outbound_peers.insert(peer_stat.address);
            }
        }
        if (!current_outbound_peers.empty() || !last_outbound_peers.empty()) {
            std::lock_guard<std::mutex> lock(outbound_reconnect_targets_mutex_);
            outbound_reconnect_targets_.insert(
                current_outbound_peers.begin(),
                current_outbound_peers.end()
            );
            for (const auto& address : last_outbound_peers) {
                if (current_outbound_peers.count(address) == 0) {
                    outbound_reconnect_targets_.insert(address);
                }
            }
        }
        last_outbound_peers = std::move(current_outbound_peers);

        sync_manager_->update();

        const auto now = Clock::now();
        if (ibd_mode_refresh.is_due(now)) {
            const bool ibd_now = is_initial_block_download();
            if (ibd_now != last_ibd_mode) {
                last_ibd_mode = ibd_now;
                if (blockchain_) {
                    blockchain_->set_ibd_mode(ibd_now);
                }
            }
        }

        if (inventory_relay.is_due(now)) {
            relay_inventory();
        }

        if (mempool_ && mempool_expiry.is_due(now)) {
            (void)mempool_->expire_old_transactions();
        }

        if (sync_retry.is_due(now) &&
            is_initial_block_download() &&
            !sync_manager_->is_syncing()) {
            const Height our_height = get_active_height();
            for (const auto& peer : peer_manager_->get_authenticated_peers()) {
                const auto info = peer->get_info();
                if (info.height > our_height) {
                    sync_manager_->start_sync(peer, info.height);
                }
            }
        }

        if (blockchain_) {
            GEO_METRIC_SET("node.height", static_cast<int64_t>(blockchain_->get_height()));
        }
        if (peer_manager_) {
            GEO_METRIC_SET("node.connected_peers", static_cast<int64_t>(peer_manager_->get_peer_count()));
        }
        if (mempool_) {
            GEO_METRIC_SET("node.mempool_txs", static_cast<int64_t>(mempool_->size()));
        }

        std::this_thread::sleep_until(iteration_started + kUpdateInterval);
    }

    if (blockchain_) {
        blockchain_->set_ibd_mode(false);
    }
}

} // namespace geocoin::node
