#include "geocoin/network/sync_manager.hpp"
#include "geocoin/network/sync_manager_internal.hpp"
#include "geocoin/consensus/chainparams.hpp"
#include "geocoin/core/blockchain.hpp"
#include "geocoin/utils/logger.hpp"
#include "geocoin/utils/metrics.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <utility>

namespace geocoin::network {

namespace {
inline bool SyncDebugEnabled() {
    return detail::sync_debug_enabled();
}

inline uint64_t SteadyNowMs() {
    return detail::steady_now_ms();
}

inline void SetSyncStatusMetric(SyncStatus status) {
    detail::set_sync_status_metric(status);
}

}

SyncManager::SyncManager(
    std::shared_ptr<state::ChainState> chain_state,
    std::shared_ptr<core::Blockchain> blockchain
)
    : chain_state_(chain_state)
    , blockchain_(blockchain)
    , status_(SyncStatus::NOT_STARTED)
    , target_height_(0)
    , blocks_downloaded_(0)
    , blocks_verified_(0)
    , max_parallel_downloads_(16)
    , last_getblocks_request_ms_(SteadyNowMs())
{
    throughput_window_start_ms_.store(SteadyNowMs(), std::memory_order_relaxed);
    SetSyncStatusMetric(status_.load(std::memory_order_relaxed));
    GEO_METRIC_SET("sync.target_height", 0);
    GEO_METRIC_SET("sync.current_height", 0);
    GEO_METRIC_SET("sync.download_queue", 0);
    GEO_METRIC_SET("sync.downloading", 0);
    GEO_METRIC_SET("sync.pending_blocks", 0);
    GEO_METRIC_SET("sync.orphan_blocks", 0);
    GEO_METRIC_SET("sync.active_peers", 0);
    apply_network_header_limits();
}

SyncManager::~SyncManager() {
    stop_sync();
}

void SyncManager::set_block_callback(BlockCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    block_callback_ = callback;
}

void SyncManager::set_block_submitter(BlockSubmitter submitter) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    block_submitter_ = std::move(submitter);
}

void SyncManager::set_peer_disconnect_callback(PeerDisconnectCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    peer_disconnect_callback_ = std::move(callback);
}

void SyncManager::clear_callbacks() {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    block_callback_ = {};
    block_submitter_ = {};
    peer_disconnect_callback_ = {};
}

void SyncManager::disconnect_peer(std::shared_ptr<Peer> peer, const std::string& reason) {
    if (!peer) {
        return;
    }

    PeerDisconnectCallback callback;
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        callback = peer_disconnect_callback_;
    }
    if (callback) {
        callback(peer, reason);
        return;
    }
    peer->disconnect();
}

bool SyncManager::start_sync(std::shared_ptr<Peer> peer, Height peer_height) {
    if (SyncDebugEnabled()) {
        GEO_LOG_DEBUG(
            "sync",
            "start_sync: peer=" << (peer ? peer->get_address() : "null")
                << ", peer_height=" << peer_height
        );
    }

    if (!peer || !blockchain_) {
        GEO_LOG_ERROR(
            "sync",
            "start_sync: invalid args: peer=" << (peer ? "ok" : "null")
                << ", blockchain=" << (blockchain_ ? "ok" : "null")
        );
        return false;
    }

    // ВАЖНО: Используем blockchain_->get_height() (активная цепь)
    // а не chain_state_->get_height() (UTXO высота)
    Height our_height = blockchain_->get_height();

    if (SyncDebugEnabled()) {
        GEO_LOG_DEBUG(
            "sync",
            "start_sync: our_height=" << our_height << ", peer_height=" << peer_height
        );
    }

    // VERSION.startingheight is entirely peer-controlled. It is useful for
    // deciding whether to ask for headers, but it must never become a sync
    // target: otherwise one authenticated peer can advertise UINT64_MAX and
    // keep IBD/mining disabled forever without producing a single header.
    refresh_sync_target();

    // Если мы уже синхронизированы
    if (peer_height <= our_height) {
        if (SyncDebugEnabled()) {
            GEO_LOG_DEBUG("sync", "start_sync: peer not ahead, skipping");
        }
        return false;
    }

    bool already_exhausted = false;
    {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        auto& info = get_sync_peer_locked(peer);
        info.advertised_height = std::max(info.advertised_height, peer_height);
        already_exhausted = info.headers_exhausted && info.height <= our_height;
        info.last_request = std::chrono::steady_clock::now();
    }

    // The peer already answered the locator without any new validated
    // headers. Do not let a stale VERSION claim trigger a request every node
    // update cycle. A fresh INV calls activate_headers_sync() and clears this
    // state so honest peers can announce later blocks normally.
    if (already_exhausted) {
        return false;
    }

    // Discovery is scheduled separately from validated block downloads. An
    // unverified VERSION claim must not pause mining while its reply is due.
    GEO_METRIC_INC("sync.start_total", 1);
    GEO_METRIC_SET("sync.target_height", static_cast<int64_t>(target_height_.load(std::memory_order_relaxed)));
    GEO_METRIC_SET("sync.current_height", static_cast<int64_t>(blockchain_->get_height()));

    if (SyncDebugEnabled()) {
        GEO_LOG_DEBUG("sync", "start_sync: send_getblocks");
    }

    send_getheaders(peer);

    if (SyncDebugEnabled()) {
        GEO_LOG_DEBUG("sync", "start_sync: done");
    }

    return true;
}

void SyncManager::stop_sync() {
    const Height local_height = blockchain_
        ? blockchain_->get_height()
        : (chain_state_ ? chain_state_->get_height() : 0);
    status_.store(SyncStatus::NOT_STARTED, std::memory_order_relaxed);
    target_height_.store(local_height, std::memory_order_relaxed);
    SetSyncStatusMetric(SyncStatus::NOT_STARTED);
    GEO_METRIC_INC("sync.stop_total", 1);
    {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        sync_peers_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        downloading_blocks_.clear();
        download_queue_.clear();
        download_queue_set_.clear();
        download_attempts_.clear();
        download_avoid_peer_.clear();
        header_heights_.clear();
        headers_index_.clear();
        best_header_by_height_.clear();
        best_header_tip_hash_ = {};
        best_header_tip_work_ = {};
        best_header_tip_height_ = 0;
    }
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_blocks_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(orphan_mutex_);
        orphan_blocks_.clear();
        orphan_blocks_by_prev_.clear();
    }
    next_local_block_retry_ms_.store(0, std::memory_order_relaxed);
    GEO_METRIC_SET("sync.download_queue", 0);
    GEO_METRIC_SET("sync.downloading", 0);
    GEO_METRIC_SET("sync.pending_blocks", 0);
    GEO_METRIC_SET("sync.orphan_blocks", 0);
    GEO_METRIC_SET("sync.active_peers", 0);
    GEO_METRIC_SET("sync.current_height", static_cast<int64_t>(local_height));
    GEO_METRIC_SET("sync.target_height", static_cast<int64_t>(local_height));
}

bool SyncManager::is_syncing() const {
    return status_.load(std::memory_order_relaxed) == SyncStatus::SYNCING;
}

SyncStats SyncManager::get_stats() const {
    SyncStats stats = {};
    stats.status = status_.load(std::memory_order_relaxed);
    stats.current_height = blockchain_ ? blockchain_->get_height() : (chain_state_ ? chain_state_->get_height() : 0);
    stats.target_height = target_height_.load(std::memory_order_relaxed);
    stats.blocks_downloaded = blocks_downloaded_.load(std::memory_order_relaxed);
    stats.blocks_verified = blocks_verified_.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        stats.active_peers = sync_peers_.size();
    }
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        stats.download_queue_size = download_queue_.size();
        stats.downloading_blocks = downloading_blocks_.size();
    }
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        stats.pending_blocks = pending_blocks_.size();
    }
    {
        std::lock_guard<std::mutex> lock(orphan_mutex_);
        stats.orphan_blocks = orphan_blocks_.size();
    }

    if (stats.target_height > 0) {
        stats.progress = std::min(1.0, static_cast<double>(stats.current_height) / stats.target_height);
    }

    return stats;
}

} // namespace geocoin::network
