#include "geocoin/node/node.hpp"

#include "geocoin/node/node_status_utils.hpp"
#include "geocoin/utils/metrics.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>

namespace geocoin::node {

NodeConfig Node::get_config() const {
    // Startup/runtime configuration is mutated under lifecycle_control_mutex_,
    // while mining-session fields are mutated under mining_control_mutex_.
    // Taking both produces one coherent value snapshot without exposing config_.
    std::scoped_lock lock(lifecycle_control_mutex_, mining_control_mutex_);
    return config_;
}

Height Node::get_active_height() const {
    return blockchain_ ? blockchain_->get_height() : (chain_state_ ? chain_state_->get_height() : 0);
}

Height Node::get_best_peer_height(Height floor_height) const {
    Height best_peer_height = floor_height;
    if (!peer_manager_) {
        return best_peer_height;
    }

    for (const auto& peer : peer_manager_->get_authenticated_peers()) {
        if (!peer) {
            continue;
        }
        const auto info = peer->get_info();
        best_peer_height = std::max(best_peer_height, info.height);
    }
    return best_peer_height;
}

NodeStatus Node::get_status() const {
    NodeStatus status = {};
    status.running = running_;
    status.syncing = sync_manager_->is_syncing();
    status.current_height = get_active_height();

    auto sync_stats = sync_manager_->get_stats();
    status.target_height = sync_stats.target_height;

    auto peer_stats = peer_manager_->get_stats();
    status.connected_peers = peer_stats.connected_peers;
    status.authenticated_peers = peer_stats.authenticated_peers;

    const Height best_peer_height = get_best_peer_height(status.current_height);
    status.best_peer_height = best_peer_height;
    // Peer VERSION heights are self-reported and remain diagnostic only.
    // Security decisions use the target derived from validated header work.
    status.target_height = std::max(status.target_height, status.current_height);
    status.initial_sync_complete =
        !status.syncing && status.target_height <= status.current_height;
    status.mining_blocked_by_sync =
        status.syncing || detail::is_height_behind(status.current_height, status.target_height);
    if (status.mining_blocked_by_sync) {
        if (detail::is_height_behind(status.current_height, status.target_height)) {
            status.mining_block_reason =
                detail::make_sync_lag_reason(status.current_height, status.target_height);
        } else {
            status.mining_block_reason = "validated header discovery is in progress";
        }
    }

    status.mempool_size = mempool_->size();
    status.total_supply = chain_state_->get_total_supply();

    {
        std::lock_guard<std::mutex> mining_lock(mining_control_mutex_);
        status.mining = mining_.load(std::memory_order_acquire);
        status.mining_threads = mining_threads_.size();
    }
    status.mined_blocks_accepted = mined_blocks_accepted_.load(std::memory_order_relaxed);

    if (status.mining) {
        auto now = std::chrono::steady_clock::now();
        double hashrate = 0.0;

        {
            std::lock_guard<std::mutex> lock(mining_stats_mutex_);
            hashrate = current_hashrate_;
            uint64_t total = total_hashes_.load(std::memory_order_relaxed);
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - mining_last_hashrate_time_
            ).count();

            if (elapsed > 0) {
                double elapsed_seconds = static_cast<double>(elapsed) / 1000.0;
                uint64_t hashes_delta = 0;
                if (total >= last_reported_hashes_) {
                    hashes_delta = total - last_reported_hashes_;
                } else {
                    // Защита от редкой гонки при reset счётчиков между stop/start.
                    hashes_delta = 0;
                }
                double instant_rate = elapsed_seconds > 0.0 ? hashes_delta / elapsed_seconds : 0.0;

                // Экспоненциальное сглаживание для стабильной цифры.
                const double alpha = 0.5;
                current_hashrate_ = alpha * instant_rate + (1.0 - alpha) * current_hashrate_;
                hashrate = current_hashrate_;

                last_reported_hashes_ = total;
                mining_last_hashrate_time_ = now;
            }
        }

        status.hashrate = hashrate;
        GEO_METRIC_SET("node.hashrate_hs", static_cast<int64_t>(hashrate));
    } else {
        status.hashrate = 0.0;
        status.mining_threads = 0;
        GEO_METRIC_SET("node.hashrate_hs", 0);
    }

    return status;
}

bool Node::is_initial_block_download() const {
    if (!blockchain_) {
        return true;
    }

    // ВАЖНО: Используем blockchain_->get_height() (активная цепь).
    Height our_height = blockchain_->get_height();

    // Если у нас только genesis или меньше - точно IBD.
    if (our_height <= 1) {
        return true;
    }

    const auto sync_stats = sync_manager_ ? sync_manager_->get_stats() : network::SyncStats{};
    const Height validated_target = std::max(our_height, sync_stats.target_height);

    // IBD если мы отстаем больше чем на 144 блока (как в Bitcoin - ~24 часа при 10 мин/блок).
    if (detail::is_height_behind_by_more_than(
            our_height,
            validated_target,
            detail::kInitialBlockDownloadLagThreshold
        )) {
        return true;
    }

    // IBD если последний блок старше 24 часов И мы отстаем от пиров.
    core::Block tip_block;
    if (blockchain_->get_block_by_height(our_height, tip_block)) {
        uint64_t now = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();

        if (tip_block.header.timestamp < now - 24 * 60 * 60 &&
            (sync_stats.status == network::SyncStatus::SYNCING ||
             detail::is_height_behind(our_height, validated_target))) {
            return true;
        }
    }

    return false;
}

}  // namespace geocoin::node
