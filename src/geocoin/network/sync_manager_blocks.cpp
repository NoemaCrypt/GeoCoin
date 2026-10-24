#include "geocoin/network/sync_manager.hpp"
#include "geocoin/network/sync_manager_internal.hpp"
#include "geocoin/network/messages.hpp"
#include "geocoin/core/blockchain.hpp"
#include "geocoin/consensus/chainparams.hpp"
#include "geocoin/consensus/consensus.hpp"
#include "geocoin/consensus/validation.hpp"
#include "geocoin/utils/logger.hpp"
#include "geocoin/utils/metrics.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace geocoin::network {
namespace {

// Each GETDATA asks for at most four blocks. Reserve part of the peer's
// 32-request / 128-response policy window for transaction and witness traffic.
constexpr size_t kBlockRequestsPerWindow = 24;
constexpr uint64_t kBlockRequestWindowMs = 10100;

bool block_request_budget_available(PeerSyncInfo& info, uint64_t now_ms) {
    auto& requests = info.recent_block_requests_ms;
    while (!requests.empty() && now_ms >= requests.front() &&
           now_ms - requests.front() >= kBlockRequestWindowMs) {
        requests.pop_front();
    }
    return requests.size() < kBlockRequestsPerWindow;
}

inline bool SyncDebugEnabled() {
    return detail::sync_debug_enabled();
}

inline uint64_t SteadyNowMs() {
    return detail::steady_now_ms();
}

inline void SetSyncStatusMetric(SyncStatus status) {
    detail::set_sync_status_metric(status);
}

std::string HashToHex(const Hash256& hash) {
    return detail::hash_to_hex(hash);
}

double UpdateEwma(double previous, double sample) {
    constexpr double kAlpha = 0.20;
    return previous <= 0.0 ? sample : (previous * (1.0 - kAlpha)) + (sample * kAlpha);
}

}  // namespace

void SyncManager::forget_download_retry_locked(const Hash256& hash) {
    if (downloading_blocks_.contains(hash)) return;
    download_attempts_.erase(hash);
    download_avoid_peer_.erase(hash);
}

void SyncManager::requeue_block_download(const Hash256& hash, const std::string& avoid_peer) {
    const bool active = blockchain_ && blockchain_->get_block_position(hash).in_active_chain;
    std::lock_guard<std::mutex> lock(queue_mutex_);
    if (downloading_blocks_.find(hash) != downloading_blocks_.end()) return;
    const auto height = header_heights_.find(hash);
    if (active || height == header_heights_.end() || !is_best_header_locked(height->second, hash)) {
        forget_download_retry_locked(hash);
        return;
    }
    if (download_queue_set_.insert(hash).second) {
        download_queue_.emplace(height->second, hash);
    }
    if (!avoid_peer.empty()) download_avoid_peer_[hash] = avoid_peer;
}

void SyncManager::handle_inv(
    std::shared_ptr<Peer> peer,
    const std::vector<Hash256>& block_hashes
) {
    if (!chain_state_ || !blockchain_) {
        return;
    }

    if (SyncDebugEnabled()) {
        GEO_LOG_DEBUG(
            "sync",
            "INV: " << block_hashes.size() << " blocks from "
                << (peer ? peer->get_address() : "unknown")
        );
    }

    bool need_headers_refresh = false;
    bool queued_new_blocks = false;
    Height highest_queued_height = 0;
    const auto* active_tip = blockchain_->get_tip();
    const uint256 active_work = active_tip ? active_tip->chain_work : uint256{};
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        for (const auto& hash : block_hashes) {
            const auto position = blockchain_->get_block_position(hash);
            const auto* index = position.indexed ? blockchain_->find_block_index(hash) : nullptr;
            if ((index && index->is_failed()) || position.in_active_chain ||
                (position.has_data && index && index->chain_work <= active_work)) {
                continue;
            }
            if (downloading_blocks_.find(hash) != downloading_blocks_.end()) {
                continue;
            }
            if (download_queue_set_.find(hash) != download_queue_set_.end()) {
                continue;
            }
            auto hit = header_heights_.find(hash);
            if (hit == header_heights_.end()) {
                need_headers_refresh = true;
                continue;
            }
            if (!is_best_header_locked(hit->second, hash)) {
                continue;
            }
            if (download_queue_set_.insert(hash).second) {
                download_queue_.emplace(hit->second, hash);
                queued_new_blocks = true;
                highest_queued_height = std::max(highest_queued_height, hit->second);
            }
        }
    }

    if (SyncDebugEnabled()) {
        GEO_LOG_DEBUG(
            "sync",
            "Download queue size: " << download_queue_.size()
        );
    }

    if (need_headers_refresh && peer) {
        // INV proves only that the peer knows a hash, not a chain height.
        // Use its VERSION value solely to record an untrusted discovery hint.
        activate_headers_sync(peer, peer->get_info().height);
    }

    // Если получили новые объявления блоков после COMPLETED, снова переходим в SYNCING.
    if (queued_new_blocks) {
        status_.store(SyncStatus::SYNCING, std::memory_order_relaxed);
        SetSyncStatusMetric(SyncStatus::SYNCING);
        // hit->second came from the validated header index. Unlike the peer's
        // VERSION height it is safe to expose as a sync target.
        const Height hinted_target = highest_queued_height;
        Height current_target = target_height_.load(std::memory_order_relaxed);
        while (hinted_target > current_target &&
               !target_height_.compare_exchange_weak(current_target, hinted_target, std::memory_order_relaxed)) {
        }
    }
}

bool SyncManager::handle_block(std::shared_ptr<Peer> peer, const core::Block& block) {
    const Hash256 block_hash = block.get_hash();
    GEO_METRIC_INC("sync.blocks_received_total", 1);
    if (!blockchain_) {
        return false;
    }

    const Height our_height = blockchain_->get_height();

    Height block_height = 0;
    bool block_height_known = false;
    const chain::BlockIndex* block_prev_index = nullptr;
    if (block.header.prev_block_hash == Hash256{}) {
        block_height = 0;
        block_height_known = true;
    } else if (const auto* prev_index = blockchain_->find_block_index(block.header.prev_block_hash)) {
        block_prev_index = prev_index;
        block_height = prev_index->height + 1;
        block_height_known = true;
    }

    if (SyncDebugEnabled()) {
        GEO_LOG_DEBUG(
            "sync",
            "Received block: height=" << (block_height_known ? std::to_string(block_height) : "unknown")
                << " from " << (peer ? peer->get_address() : "unknown")
        );
    }

    BlockDownloadInfo download_info;
    bool had_download_info = false;
    bool response_from_wrong_peer = false;
    uint64_t response_elapsed_ms = 0;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        auto download_it = downloading_blocks_.find(block_hash);
        if (download_it != downloading_blocks_.end()) {
            download_info = download_it->second;
            had_download_info = true;
            const uint64_t now_ms = SteadyNowMs();
            if (download_info.requested_ms > 0 && now_ms >= download_info.requested_ms) {
                response_elapsed_ms = now_ms - download_info.requested_ms;
            }
            response_from_wrong_peer =
                peer && !download_info.peer_address.empty() &&
                peer->get_address() != download_info.peer_address;
        }
        if (!response_from_wrong_peer) {
            downloading_blocks_.erase(block_hash);
            download_queue_set_.erase(block_hash);
            for (auto it = download_queue_.begin(); it != download_queue_.end(); ++it) {
                if (it->second == block_hash) {
                    download_queue_.erase(it);
                    break;
                }
            }
        }
    }

    if (response_from_wrong_peer) {
        GEO_METRIC_INC("sync.blocks_wrong_peer_total", 1);
        GEO_LOG_WARN(
            "sync",
            "Ignoring BLOCK response from " << peer->get_address()
                << "; hash was requested from " << download_info.peer_address
        );
        return false;
    }

    const std::string requested_peer_address = had_download_info
        ? download_info.peer_address
        : (peer ? peer->get_address() : std::string{});
    if (!requested_peer_address.empty()) {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        auto it = sync_peers_.find(requested_peer_address);
        if (it != sync_peers_.end()) {
            it->second.requested_blocks.erase(block_hash);
        }
    }

    bool download_outcome_recorded = false;
    auto record_success_once = [&]() {
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            download_attempts_.erase(block_hash);
            download_avoid_peer_.erase(block_hash);
        }
        if (had_download_info && !download_outcome_recorded) {
            record_block_download_success(download_info, response_elapsed_ms);
            download_outcome_recorded = true;
        }
    };
    auto record_failure_once = [&](bool invalid_block) {
        if (download_outcome_recorded) {
            return;
        }
        if (had_download_info) {
            record_block_download_failure(download_info, invalid_block);
            download_outcome_recorded = true;
        } else if (invalid_block && peer) {
            BlockDownloadInfo source_info;
            source_info.peer = peer;
            source_info.peer_address = peer->get_address();
            record_block_download_failure(source_info, true);
            download_outcome_recorded = true;
        }
    };
    auto retry_failed_download = [&](bool invalid_block) {
        if (invalid_block) {
            record_failure_once(true);
        } else {
            // The node's mutation service may be unavailable or fail locally.
            // These failures say nothing about the source peer's block quality.
            GEO_METRIC_INC("sync.blocks_local_fail_total", 1);
        }
        if (discard_failed_header_branch(block_hash) ||
            discard_failed_header_branch(block.header.prev_block_hash)) return;
        requeue_block_download(block_hash, invalid_block ? requested_peer_address : std::string{});
    };

    if (const auto* index = blockchain_->find_block_index(block_hash);
        (index && index->is_failed()) || (block_prev_index && block_prev_index->is_failed())) {
        retry_failed_download(true);
        return false;
    }

    if (SyncDebugEnabled()) {
        GEO_LOG_DEBUG("sync", "Our current height: " << our_height);
    }

    if (!block_height_known) {
        if (SyncDebugEnabled()) {
            GEO_LOG_DEBUG("sync", "Block prev hash is unknown yet, buffering orphan and requesting chain continuation");
        }
        if (peer) {
            activate_headers_sync(peer, peer->get_info().height);
        }

        // Unsolicited unknown-parent bodies are never useful in headers-first
        // sync and previously allowed 256 x 4 MB of unvalidated orphan data.
        if (!had_download_info) {
            GEO_METRIC_INC("sync.orphan_blocks_unsolicited_total", 1);
            // A late response can arrive after timeout requeued this header.
            // Refusing the unassigned body must not consume that queued demand.
            requeue_block_download(block_hash);
            return false;
        }

        consensus::ValidationState orphan_state;
        if (!consensus::check_block(
                block,
                orphan_state,
                consensus::ValidationMode::HEADER_ONLY)) {
            GEO_LOG_WARN(
                "sync",
                "Rejecting malformed requested orphan block: "
                    << orphan_state.get_reject_reason()
            );
            retry_failed_download(true);
            return false;
        }
        store_orphan_block(block_hash, block, peer);
        GEO_METRIC_INC("sync.orphan_blocks_stored_total", 1);
        record_success_once();
        return false;
    }

    // Специальная обработка genesis блока (height=0)
    if (block_height == 0) {
        const Hash256 expected_genesis = consensus::Params().get_genesis_hash();

        if (block_hash != expected_genesis) {
            GEO_LOG_ERROR(
                "sync",
                "Received wrong genesis block from peer "
                    << (peer ? peer->get_address() : "unknown")
                    << ". Peer is on a different network."
            );

            disconnect_peer(peer, "wrong-genesis-block");
            record_failure_once(true);
            return false;
        }

        if (blockchain_->get_block_position(block_hash).in_active_chain) {
            if (SyncDebugEnabled()) {
                GEO_LOG_DEBUG("sync", "Genesis already exists, skipping");
            }
            record_success_once();
            drain_orphan_blocks(block_hash);
            refresh_sync_target();
            return true;
        }

        GEO_LOG_INFO(
            "sync",
            "Received correct genesis block for network: " << consensus::Params().network_id()
        );
    }

    auto on_block_applied = [&](const core::Block& applied_block, Height applied_height) {
        blocks_downloaded_.fetch_add(1, std::memory_order_relaxed);
        blocks_verified_.fetch_add(1, std::memory_order_relaxed);

        const size_t window_blocks = throughput_window_blocks_.fetch_add(1, std::memory_order_relaxed) + 1;
        if (window_blocks >= 100) {
            const uint64_t now_ms = SteadyNowMs();
            const uint64_t window_start = throughput_window_start_ms_.exchange(now_ms, std::memory_order_relaxed);
            throughput_window_blocks_.store(0, std::memory_order_relaxed);
            if (window_start > 0 && now_ms > window_start) {
                const double elapsed_s = static_cast<double>(now_ms - window_start) / 1000.0;
                const double rate = (elapsed_s > 0.0) ? (100.0 / elapsed_s) : 0.0;
                GEO_LOG_INFO("sync", "Sync throughput: " << std::fixed << std::setprecision(2) << rate << " blocks/s");
            }
        }

        if (applied_height % 100 == 0) {
            GEO_LOG_INFO(
                "sync",
                "Applied blocks up to height " << applied_height << " / "
                    << target_height_.load(std::memory_order_relaxed)
            );
        }

        BlockCallback callback;
        {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            callback = block_callback_;
        }
        if (callback) {
            callback(applied_block, applied_height);
        }

        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            prune_headers_state_locked(applied_height);
        }

        drain_orphan_blocks(applied_block.get_hash());
    };

    auto timed_apply = [&](const core::Block& candidate, Height candidate_height, bool alternative_branch) {
        const auto start = std::chrono::steady_clock::now();
        const auto submission = verify_and_apply_block(candidate);
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start
        ).count();

        if (elapsed_ms > 100) {
            GEO_LOG_WARN(
                "sync",
                "Slow block processing: height=" << candidate_height << ", elapsed=" << elapsed_ms << "ms"
            );
        }

        if (!submission.is_accepted()) {
            GEO_METRIC_INC("sync.blocks_applied_fail_total", 1);
            if (alternative_branch) {
                GEO_LOG_WARN("sync", "Failed to apply alternative block at height " << candidate_height);
            } else {
                GEO_LOG_ERROR("sync", "Failed to apply block at height " << candidate_height);
            }
            return submission;
        }
        GEO_METRIC_INC("sync.blocks_applied_ok_total", 1);
        return submission;
    };

    if (block_height == our_height + 1) {
        if (SyncDebugEnabled()) {
            GEO_LOG_DEBUG("sync", "Applying next block at height " << block_height);
        }

        if (SyncDebugEnabled() && block_height == 1) {
            GEO_LOG_DEBUG(
                "sync",
                "Block 1 prev_block_hash[0..3]="
                    << std::hex
                    << (int)block.header.prev_block_hash[0]
                    << (int)block.header.prev_block_hash[1]
                    << (int)block.header.prev_block_hash[2]
                    << (int)block.header.prev_block_hash[3]
                    << "..." << std::dec
            );
        }

        const auto submission = timed_apply(block, block_height, false);
        if (!submission.is_accepted()) {
            retry_failed_download(submission.outcome == services::SubmissionOutcome::REJECTED);
            return false;
        }
        on_block_applied(block, block_height);

        while (true) {
            const Height next_height = blockchain_->get_height() + 1;
            std::shared_ptr<core::Block> pending_block;
            {
                std::lock_guard<std::mutex> lock(pending_mutex_);
                auto it = pending_blocks_.find(next_height);
                if (it == pending_blocks_.end()) {
                    break;
                }
                pending_block = it->second;
                pending_blocks_.erase(it);
            }

            if (SyncDebugEnabled()) {
                GEO_LOG_DEBUG("sync", "Applying buffered block at height " << next_height);
            }

            if (!pending_block) break;
            if (!timed_apply(*pending_block, next_height, false).is_accepted()) {
                if (!discard_failed_header_branch(pending_block->get_hash())) {
                    requeue_block_download(pending_block->get_hash());
                }
                break;
            }

            on_block_applied(*pending_block, next_height);
        }
    } else if (block_height > our_height + 1) {
        if (SyncDebugEnabled()) {
            GEO_LOG_DEBUG(
                "sync",
                "Buffering out-of-order block (expected " << (our_height + 1)
                    << ", got " << block_height << ")"
            );
        }
        consensus::ValidationState pending_state;
        if (!block_prev_index ||
            !consensus::contextual_check_block_header(
                block.header,
                pending_state,
                block_prev_index) ||
            !consensus::check_block(
                block,
                pending_state,
                consensus::ValidationMode::HEADER_ONLY)) {
            GEO_LOG_WARN(
                "sync",
                "Rejecting invalid out-of-order block before buffering: "
                    << pending_state.get_reject_reason()
            );
            retry_failed_download(true);
            return false;
        }
        std::vector<Hash256> evicted;
        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            const auto existing = pending_blocks_.find(block_height);
            if (existing != pending_blocks_.end() && existing->second &&
                existing->second->get_hash() != block_hash) {
                evicted.push_back(existing->second->get_hash());
            }
            pending_blocks_[block_height] = std::make_shared<core::Block>(block);
            while (pending_blocks_.size() > max_pending_blocks_) {
                auto last = std::prev(pending_blocks_.end());
                if (last->second) evicted.push_back(last->second->get_hash());
                pending_blocks_.erase(last);
            }
        }
        for (const auto& hash : evicted) requeue_block_download(hash);

        if (block_height > our_height + 2 && status_.load(std::memory_order_relaxed) == SyncStatus::SYNCING) {
            if (SyncDebugEnabled()) {
                GEO_LOG_DEBUG("sync", "Gap detected, requesting missing blocks...");
            }
            send_getheaders(peer);
        }
    } else {
        // Блок с высотой <= нашей текущей высоты
        // Это может быть:
        // 1. Дубликат (уже есть в активной цепи)
        // 2. Блок из альтернативной цепи (форк)

        if (SyncDebugEnabled()) {
            GEO_LOG_DEBUG(
                "sync",
                "Received block at height " << block_height << " (our height: " << our_height << ")"
            );
        }

        // An index can outlive a failed body write or chain activation. Only
        // active blocks are already applied; side bodies must reach the writer.
        if (blockchain_->get_block_position(block_hash).in_active_chain) {
            if (SyncDebugEnabled()) {
                GEO_LOG_DEBUG("sync", "Block already known, skipping");
            }
            record_success_once();
            drain_orphan_blocks(block_hash);
            refresh_sync_target();
            return true;
        }

        // Это новый блок на той же высоте (возможный форк)
        // Сохраняем его и проверяем не лучше ли его цепь
        if (SyncDebugEnabled()) {
            GEO_LOG_DEBUG("sync", "New block at existing height, checking if better chain...");
        }

        const auto submission = timed_apply(block, block_height, true);
        if (!submission.is_accepted()) {
            retry_failed_download(submission.outcome == services::SubmissionOutcome::REJECTED);
            return false;
        }

        // A retained side-branch body supplies parent context even before its
        // chain has enough work to replace the active tip.
        drain_orphan_blocks(block_hash);

        if (SyncDebugEnabled()) {
            GEO_LOG_DEBUG("sync", "Alternative block processed");
        }
    }

    record_success_once();

    // Проверяем, завершена ли синхронизация
    refresh_sync_target();
    const Height current_height = blockchain_->get_height();
    if (status_.load(std::memory_order_relaxed) == SyncStatus::COMPLETED) {
        GEO_LOG_INFO(
            "sync",
            "Sync completed. Height: " << current_height << " / "
                << target_height_.load(std::memory_order_relaxed)
        );
        // update() intentionally stops running once status becomes COMPLETED.
        // Refresh queue gauges here so monitoring does not retain the last
        // in-progress sample after the final responses have been consumed.
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            GEO_METRIC_SET("sync.download_queue", static_cast<int64_t>(download_queue_.size()));
            GEO_METRIC_SET("sync.downloading", static_cast<int64_t>(downloading_blocks_.size()));
        }
        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            GEO_METRIC_SET("sync.pending_blocks", static_cast<int64_t>(pending_blocks_.size()));
        }
        {
            std::lock_guard<std::mutex> lock(orphan_mutex_);
            GEO_METRIC_SET("sync.orphan_blocks", static_cast<int64_t>(orphan_blocks_.size()));
        }
    }

    GEO_METRIC_SET("sync.current_height", static_cast<int64_t>(current_height));
    GEO_METRIC_SET("sync.target_height", static_cast<int64_t>(target_height_.load(std::memory_order_relaxed)));

    return true;
}

void SyncManager::update() {
    // Discovery can be pending before any target is validated, or after the
    // last known body completes. Service it independently of download status.
    update_headers_requests();
    if (status_.load(std::memory_order_relaxed) != SyncStatus::SYNCING) {
        return;
    }

    // Проверяем таймауты
    check_timeouts();

    // Сторож: если синхронизация простаивает (нет запросов и нет очереди),
    // переотправляем GETBLOCKS, чтобы получить следующую порцию INV.
    // Это защищает от ситуации, когда пир/сеть перестали присылать INV и мы остаёмся на месте,
    // хотя соединение живое (PING/PONG идут).
    const Height current_height = blockchain_ ? blockchain_->get_height() : 0;
    const Height target_height = target_height_.load(std::memory_order_relaxed);
    bool queue_idle = false;
    bool more_work = false;
    bool pending_idle = false;
    bool orphan_idle = false;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        queue_idle = download_queue_.empty() && downloading_blocks_.empty();
        const auto* active_tip = blockchain_ ? blockchain_->get_tip() : nullptr;
        more_work = active_tip && best_header_tip_work_ > active_tip->chain_work;
        GEO_METRIC_SET("sync.download_queue", static_cast<int64_t>(download_queue_.size()));
        GEO_METRIC_SET("sync.downloading", static_cast<int64_t>(downloading_blocks_.size()));
    }
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_idle = pending_blocks_.empty();
        GEO_METRIC_SET("sync.pending_blocks", static_cast<int64_t>(pending_blocks_.size()));
    }
    {
        std::lock_guard<std::mutex> lock(orphan_mutex_);
        orphan_idle = orphan_blocks_.empty();
        GEO_METRIC_SET("sync.orphan_blocks", static_cast<int64_t>(orphan_blocks_.size()));
    }
    {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        GEO_METRIC_SET("sync.active_peers", static_cast<int64_t>(sync_peers_.size()));
    }
    GEO_METRIC_SET("sync.current_height", static_cast<int64_t>(current_height));
    GEO_METRIC_SET("sync.target_height", static_cast<int64_t>(target_height));

    if (queue_idle && pending_idle && orphan_idle && (more_work || current_height < target_height)) {
        const uint64_t now_ms = SteadyNowMs();
        const uint64_t last_request_ms = last_getblocks_request_ms_.load(std::memory_order_relaxed);
        const uint64_t elapsed_ms = (now_ms > last_request_ms) ? (now_ms - last_request_ms) : 0;

        // Не чаще чем раз в 10 секунд
        if (elapsed_ms > 10000) {
            auto peer = find_best_peer();
            if (peer) {
                if (SyncDebugEnabled()) {
                    GEO_LOG_DEBUG("sync", "update: idle detected, re-sending GETHEADERS");
                }
                send_getheaders(peer);
                last_getblocks_request_ms_.store(now_ms, std::memory_order_relaxed);
            }
        }
    }

    // Логируем состояние синхронизации
    static int log_counter = 0;
    if (++log_counter % 100 == 0) {  // Каждые 10 секунд (100 * 100ms)
        if (SyncDebugEnabled()) {
            size_t queue_size = 0;
            size_t downloading_size = 0;
            size_t pending_size = 0;
            size_t orphan_size = 0;
            {
                std::lock_guard<std::mutex> lock(queue_mutex_);
                queue_size = download_queue_.size();
                downloading_size = downloading_blocks_.size();
            }
            {
                std::lock_guard<std::mutex> lock(pending_mutex_);
                pending_size = pending_blocks_.size();
            }
            {
                std::lock_guard<std::mutex> lock(orphan_mutex_);
                orphan_size = orphan_blocks_.size();
            }
            GEO_LOG_DEBUG("sync", "update: status=SYNCING");
            GEO_LOG_DEBUG("sync", "  Download queue: " << queue_size);
            GEO_LOG_DEBUG("sync", "  Downloading: " << downloading_size);
            GEO_LOG_DEBUG("sync", "  Pending blocks: " << pending_size);
            GEO_LOG_DEBUG("sync", "  Orphan blocks: " << orphan_size);
            GEO_LOG_DEBUG("sync", "  Current height: " << (chain_state_ ? chain_state_->get_height() : 0));
            GEO_LOG_DEBUG("sync", "  Target height: " << target_height_.load(std::memory_order_relaxed));
        }
    }

    maybe_adjust_download_window();

    // Usually downloads start at active tip + 1.  A more-work competing
    // header chain is different: its first missing body may be at or below
    // the active height.  Starting the window strictly at tip + 1 used to
    // request only the fork tip, buffer it as an orphan, and permanently
    // strand the fork ancestors below the window.  Anchor the window at the
    // earliest queued block on the globally best validated header path so a
    // crossing-work fork is downloaded parent-first and can be activated.
    Height base_height = blockchain_ ? (blockchain_->get_height() + 1) : 0;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        for (const auto& [height, hash] : download_queue_) {
            if (is_best_header_locked(height, hash)) {
                base_height = std::min(base_height, height);
                break;
            }
        }
    }
    size_t active_window_size = 1;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        active_window_size = std::max<size_t>(1, download_window_size_);
    }
    const Height window_delta = static_cast<Height>(active_window_size - 1);
    const Height max_window_height =
        (base_height > (std::numeric_limits<Height>::max() - window_delta))
            ? std::numeric_limits<Height>::max()
            : (base_height + window_delta);

    auto requeue_block = [&](Height height, const Hash256& hash) {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (download_queue_set_.insert(hash).second) {
            download_queue_.emplace(height, hash);
        }
    };

    auto pop_next_candidate = [&](Height& selected_height, Hash256& block_hash) -> bool {
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            if (download_queue_.empty()) {
                return false;
            }

            auto begin_it = download_queue_.lower_bound(base_height);
            if (begin_it == download_queue_.end() || begin_it->first > max_window_height) {
                return false;
            }

            auto it = begin_it;
            for (; it != download_queue_.end() && it->first <= max_window_height; ++it) {
                if (!is_best_header_locked(it->first, it->second)) {
                    continue;
                }
                if (downloading_blocks_.find(it->second) != downloading_blocks_.end()) {
                    continue;
                }
                break;
            }
            if (it == download_queue_.end() || it->first > max_window_height) {
                return false;
            }

            selected_height = it->first;
            block_hash = it->second;
            download_queue_.erase(it);
            download_queue_set_.erase(block_hash);
            return true;
        }
    };

    auto peer_remaining_capacity = [&](const std::shared_ptr<Peer>& selected_peer) -> size_t {
        if (!selected_peer) {
            return 0;
        }
        std::lock_guard<std::mutex> lock(peers_mutex_);
        auto it = sync_peers_.find(selected_peer->get_address());
        if (it == sync_peers_.end()) {
            return 0;
        }
        const size_t used = it->second.requested_blocks.size();
        if (used >= max_blocks_per_peer_) {
            return 0;
        }
        return max_blocks_per_peer_ - used;
    };

    auto global_remaining_capacity = [&]() -> size_t {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (downloading_blocks_.size() >= max_parallel_downloads_) {
            return 0;
        }
        return max_parallel_downloads_ - downloading_blocks_.size();
    };

    while (true) {
        Hash256 block_hash{};
        Height selected_height = 0;
        if (!pop_next_candidate(selected_height, block_hash)) {
            break;
        }

        const auto position = blockchain_ ? blockchain_->get_block_position(block_hash) :
                                           core::BlockPositionSnapshot{};
        if (position.in_active_chain) {
            {
                std::lock_guard<std::mutex> lock(queue_mutex_);
                forget_download_retry_locked(block_hash);
            }
            refresh_sync_target();
            continue;
        }
        if (position.has_data) {
            // A local apply failure can leave the full body on disk. Retry it
            // through the same mutation service without another network copy.
            // A persistent local failure must not spin in this update loop.
            const uint64_t now_ms = SteadyNowMs();
            if (now_ms < next_local_block_retry_ms_.load(std::memory_order_relaxed)) {
                requeue_block(selected_height, block_hash);
                break;
            }
            next_local_block_retry_ms_.store(now_ms + 1000, std::memory_order_relaxed);
            core::Block stored;
            bool loaded = false;
            try {
                loaded = blockchain_->get_block(block_hash, stored);
            } catch (const std::exception& error) {
                GEO_LOG_WARN("sync", "Local block retry read failed: " << error.what());
            }
            if (!loaded) {
                GEO_METRIC_INC("sync.local_block_read_fail_total", 1);
                requeue_block(selected_height, block_hash);
            } else {
                GEO_METRIC_INC("sync.local_block_retries_total", 1);
                if (handle_block(nullptr, stored)) {
                    next_local_block_retry_ms_.store(0, std::memory_order_relaxed);
                }
            }
            break;
        }

        auto peer = find_best_peer_for_block(block_hash);
        if (!peer) {
            requeue_block(selected_height, block_hash);
            break;
        }

        const std::string peer_address = peer->get_address();
        const size_t peer_capacity = peer_remaining_capacity(peer);
        const size_t global_capacity = global_remaining_capacity();
        size_t batch_limit = std::min(max_blocks_per_getdata_, peer_capacity);
        batch_limit = std::min(batch_limit, global_capacity);
        if (batch_limit == 0) {
            requeue_block(selected_height, block_hash);
            break;
        }

        std::vector<std::pair<Height, Hash256>> batch;
        batch.reserve(batch_limit);
        batch.emplace_back(selected_height, block_hash);

        while (batch.size() < batch_limit) {
            Hash256 next_hash{};
            Height next_height = 0;
            if (!pop_next_candidate(next_height, next_hash)) {
                break;
            }

            const auto next_position = blockchain_ ? blockchain_->get_block_position(next_hash) :
                                                    core::BlockPositionSnapshot{};
            if (next_position.in_active_chain) {
                {
                    std::lock_guard<std::mutex> lock(queue_mutex_);
                    forget_download_retry_locked(next_hash);
                }
                refresh_sync_target();
                continue;
            }
            if (next_position.has_data) {
                requeue_block(next_height, next_hash);
                break;
            }

            auto next_peer = find_best_peer_for_block(next_hash);
            if (!next_peer || next_peer->get_address() != peer_address) {
                requeue_block(next_height, next_hash);
                break;
            }

            batch.emplace_back(next_height, next_hash);
        }

        std::vector<Hash256> hashes;
        hashes.reserve(batch.size());
        for (const auto& [height, hash] : batch) {
            (void)height;
            hashes.push_back(hash);
        }

        if (!request_blocks(peer, hashes)) {
            for (const auto& [height, hash] : batch) {
                requeue_block(height, hash);
            }
            break;
        }
        const uint64_t requested_ms = SteadyNowMs();
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            for (const auto& [height, hash] : batch) {
                uint32_t& attempts = download_attempts_[hash];
                ++attempts;
                downloading_blocks_[hash] = BlockDownloadInfo{
                    peer,
                    peer_address,
                    height,
                    requested_ms,
                    attempts
                };
            }
        }
        {
            std::lock_guard<std::mutex> lock(peers_mutex_);
            auto it = sync_peers_.find(peer->get_address());
            if (it != sync_peers_.end()) {
                it->second.last_request = std::chrono::steady_clock::now();
                for (const auto& [height, hash] : batch) {
                    (void)height;
                    it->second.requested_blocks.insert(hash);
                }
            }
        }
    }
}

void SyncManager::set_max_parallel_downloads(size_t max_parallel) {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    max_parallel_downloads_ = std::max<size_t>(1, max_parallel);
}

bool SyncManager::request_blocks(
    std::shared_ptr<Peer> peer,
    const std::vector<Hash256>& hashes
) {
    if (!peer || hashes.empty()) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        auto& info = get_sync_peer_locked(peer);
        const uint64_t now_ms = SteadyNowMs();
        if (!block_request_budget_available(info, now_ms)) return false;
        info.recent_block_requests_ms.push_back(now_ms);
    }

    // Создаем GETDATA сообщение
    InvMessage getdata;

    for (const auto& hash : hashes) {
        InvMessage::InvItem item;
        item.type = InvMessage::InvType::BLOCK;
        item.hash = hash;
        getdata.items.push_back(item);
    }

    bytes payload = getdata.serialize();
    bytes message = create_message(MessageType::GETDATA, payload);

    const ssize_t sent = peer->send(message);
    if (sent <= 0 || static_cast<size_t>(sent) != message.size()) {
        GEO_METRIC_INC("sync.getdata_send_fail_total", 1);
        return false;
    }

    GEO_METRIC_INC("sync.getdata_sent_total", static_cast<int64_t>(hashes.size()));
    last_getblocks_request_ms_.store(SteadyNowMs(), std::memory_order_relaxed);
    return true;
}

std::shared_ptr<Peer> SyncManager::find_best_peer() {
    return find_best_peer_impl(std::string{}, false);
}

std::shared_ptr<Peer> SyncManager::find_best_peer_for_block(const Hash256& block_hash) {
    std::string avoid_address;
    Height required_height = 0;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        auto it = download_avoid_peer_.find(block_hash);
        if (it != download_avoid_peer_.end()) {
            avoid_address = it->second;
        }
        const auto header = header_heights_.find(block_hash);
        if (header != header_heights_.end()) required_height = header->second;
    }

    auto peer = find_best_peer_impl(avoid_address, true, required_height);
    if (!peer && !avoid_address.empty()) {
        peer = find_best_peer_impl(std::string{}, true, required_height);
    }
    return peer;
}

std::shared_ptr<Peer> SyncManager::find_best_peer_impl(
    const std::string& avoid_address,
    bool enforce_download_capacity,
    Height required_height
) {
    std::shared_ptr<Peer> best_peer = nullptr;
    size_t min_requests = std::numeric_limits<size_t>::max();
    double best_score = std::numeric_limits<double>::max();
    std::unique_lock<std::mutex> queue_lock(queue_mutex_, std::defer_lock);
    if (required_height > 0) queue_lock.lock();
    std::lock_guard<std::mutex> lock(peers_mutex_);

    auto supplied_header_path = [&](const PeerSyncInfo& info) {
        if (required_height == 0 || !info.peer || !info.peer->is_connected() ||
            info.peer->get_state() != PeerState::READY) return false;
        const auto tip = header_heights_.find(info.last_header_hash);
        return tip != header_heights_.end() && tip->second >= required_height &&
            is_best_header_locked(tip->second, info.last_header_hash);
    };
    const bool have_header_source = std::any_of(sync_peers_.begin(), sync_peers_.end(),
        [&](const auto& entry) {
            return entry.first != avoid_address && supplied_header_path(entry.second);
        });

    for (auto& [addr, info] : sync_peers_) {
        if (!info.peer || !info.peer->is_connected()) {
            continue;
        }

        if (info.peer->get_state() != PeerState::READY) {
            continue;
        }
        // A previously fast peer may still be one block behind a new miner.
        // Prefer peers which supplied this validated branch, even when their
        // capacity needs to replenish; querying a lagging peer silently loses
        // the request until the full block-download timeout.
        if (have_header_source && !supplied_header_path(info)) continue;

        const size_t requests = info.requested_blocks.size();
        const size_t effective_peer_cap = std::max<size_t>(
            1,
            std::min(max_blocks_per_peer_, max_parallel_downloads_)
        );
        if (enforce_download_capacity && requests >= effective_peer_cap) {
            continue;
        }
        if (enforce_download_capacity && !block_request_budget_available(info, SteadyNowMs())) {
            continue;
        }

        if (!avoid_address.empty() && addr == avoid_address && sync_peers_.size() > 1) {
            continue;
        }

        const auto peer_info = info.peer->get_info();
        double score =
            static_cast<double>(info.bad_headers_score) * 100000.0 +
            static_cast<double>(info.invalid_block_score) * 50000.0 +
            static_cast<double>(info.timeout_score) * 20000.0 +
            static_cast<double>(requests) * 250.0;

        if (info.response_time_ewma_ms > 0.0) {
            score += std::min(info.response_time_ewma_ms, 60000.0) / 100.0;
        }
        if ((peer_info.services & NODE_NETWORK) != 0) {
            score -= 50.0;
        }
        if ((peer_info.services & NODE_ARCHIVE) != 0) {
            score -= 10.0;
        }
        if ((peer_info.services & NODE_NETWORK_LIMITED) != 0) {
            score += 25.0;
        }
        if (info.completed_blocks > 0) {
            score -= static_cast<double>(std::min<uint64_t>(info.completed_blocks, 100)) * 0.5;
        }

        if (score < best_score || (score == best_score && requests < min_requests)) {
            best_score = score;
            min_requests = requests;
            best_peer = info.peer;
        }
    }

    return best_peer;
}

void SyncManager::check_timeouts() {
    const uint64_t now_ms = SteadyNowMs();
    std::vector<std::pair<Hash256, BlockDownloadInfo>> timed_out;

    {
        std::lock_guard<std::mutex> queue_lock(queue_mutex_);
        std::lock_guard<std::mutex> peers_lock(peers_mutex_);
        for (const auto& [hash, info] : downloading_blocks_) {
            bool expired = false;
            if (!info.peer || !info.peer->is_connected() || info.peer->get_state() != PeerState::READY) {
                expired = true;
            } else {
                const uint64_t elapsed_ms =
                    (now_ms > info.requested_ms) ? (now_ms - info.requested_ms) : 0;
                expired = elapsed_ms > block_download_timeout_ms_;
            }

            if (expired) {
                timed_out.emplace_back(hash, info);
            }
        }

        for (const auto& [hash, info] : timed_out) {
            downloading_blocks_.erase(hash);
            if (!info.peer_address.empty()) {
                auto peer_it = sync_peers_.find(info.peer_address);
                if (peer_it != sync_peers_.end()) {
                    peer_it->second.requested_blocks.erase(hash);
                    peer_it->second.timed_out_blocks += 1;
                    if (peer_it->second.timeout_score < std::numeric_limits<uint32_t>::max()) {
                        peer_it->second.timeout_score += 1;
                    }
                }
            }

            auto height_it = header_heights_.find(hash);
            Height height = info.height;
            if (height_it != header_heights_.end()) {
                height = height_it->second;
            } else if (height == 0 && blockchain_) {
                height = blockchain_->get_height() + 1;
            }
            if (is_best_header_locked(height, hash)) {
                if (!info.peer_address.empty()) download_avoid_peer_[hash] = info.peer_address;
                if (download_queue_set_.insert(hash).second) download_queue_.emplace(height, hash);
            } else {
                forget_download_retry_locked(hash);
            }
        }
    }
    if (!timed_out.empty()) {
        GEO_METRIC_INC("sync.download_timeouts_total", static_cast<int64_t>(timed_out.size()));
        GEO_METRIC_SET("sync.download_timeouts_last_batch", static_cast<int64_t>(timed_out.size()));
    }
}

void SyncManager::record_block_download_success(const BlockDownloadInfo& info, uint64_t elapsed_ms) {
    if (info.peer_address.empty()) {
        return;
    }

    std::lock_guard<std::mutex> lock(peers_mutex_);
    auto it = sync_peers_.find(info.peer_address);
    if (it == sync_peers_.end()) {
        return;
    }

    it->second.completed_blocks += 1;
    if (it->second.timeout_score > 0) {
        it->second.timeout_score -= 1;
    }
    if (it->second.invalid_block_score > 0 && (it->second.completed_blocks % 32) == 0) {
        it->second.invalid_block_score -= 1;
    }
    if (elapsed_ms > 0) {
        it->second.response_time_ewma_ms = UpdateEwma(
            it->second.response_time_ewma_ms,
            static_cast<double>(elapsed_ms)
        );
        GEO_METRIC_SET("sync.peer_response_ewma_ms", static_cast<int64_t>(it->second.response_time_ewma_ms));
    }
}

void SyncManager::record_block_download_failure(const BlockDownloadInfo& info, bool invalid_block) {
    if (info.peer_address.empty()) {
        return;
    }

    std::shared_ptr<Peer> peer_to_disconnect;
    {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        auto it = sync_peers_.find(info.peer_address);
        if (it == sync_peers_.end()) {
            return;
        }

        it->second.failed_blocks += 1;
        if (invalid_block && it->second.invalid_block_score < std::numeric_limits<uint32_t>::max()) {
            it->second.invalid_block_score += 1;
        }
        if (invalid_block && it->second.invalid_block_score >= 4) {
            peer_to_disconnect = it->second.peer;
        }
    }

    if (peer_to_disconnect) {
        GEO_LOG_WARN("sync", "Disconnecting peer for repeated invalid blocks: " << info.peer_address);
        disconnect_peer(peer_to_disconnect, "sync-invalid-blocks");
    }
}

void SyncManager::maybe_adjust_download_window() {
    size_t current_window = 0;
    size_t queue_size = 0;
    size_t downloading_size = 0;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        current_window = download_window_size_;
        queue_size = download_queue_.size();
        downloading_size = downloading_blocks_.size();
    }

    size_t pending_size = 0;
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_size = pending_blocks_.size();
    }

    size_t orphan_size = 0;
    {
        std::lock_guard<std::mutex> lock(orphan_mutex_);
        orphan_size = orphan_blocks_.size();
    }

    uint32_t max_timeout_score = 0;
    uint32_t max_invalid_score = 0;
    {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        for (const auto& [addr, info] : sync_peers_) {
            (void)addr;
            max_timeout_score = std::max(max_timeout_score, info.timeout_score);
            max_invalid_score = std::max(max_invalid_score, info.invalid_block_score);
        }
    }

    size_t next_window = current_window;
    const bool backpressure =
        pending_size > (max_pending_blocks_ / 2) ||
        orphan_size > (max_orphan_blocks_ / 2) ||
        max_timeout_score >= 3 ||
        max_invalid_score >= 2;

    if (backpressure && current_window > min_download_window_size_) {
        next_window = std::max(min_download_window_size_, current_window / 2);
    } else if (!backpressure &&
               queue_size > current_window &&
               downloading_size >= std::max<size_t>(1, max_parallel_downloads_ / 2) &&
               current_window < max_download_window_size_) {
        next_window = std::min(max_download_window_size_, current_window + min_download_window_size_);
    }

    if (next_window == current_window) {
        GEO_METRIC_SET("sync.download_window", static_cast<int64_t>(current_window));
        return;
    }

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        download_window_size_ = next_window;
    }

    GEO_METRIC_SET("sync.download_window", static_cast<int64_t>(next_window));
    if (SyncDebugEnabled()) {
        GEO_LOG_DEBUG(
            "sync",
            "Adjusted download window: " << current_window << " -> " << next_window
        );
    }
}

services::BlockSubmissionResult SyncManager::verify_and_apply_block(const core::Block& block) {
    services::BlockSubmissionResult submission;
    if (!blockchain_) {
        GEO_LOG_ERROR("sync", "verify_and_apply_block: No blockchain instance");
        submission.reject_reason = "blockchain-unavailable";
        return submission;
    }

    // ВАЖНО: Используем Blockchain::process_new_block() который:
    // 1. Валидирует блок (check_block)
    // 2. Сохраняет на диск через BlockStorage (accept_block)
    // 3. Применяет к UTXO через ChainState (activate_best_chain)
    // Это правильный путь как в Bitcoin!

    BlockSubmitter submitter;
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        submitter = block_submitter_;
    }

    try {
        if (submitter) {
            submission = submitter(block);
        } else {
            // Standalone users share the node's distinction between a rejected
            // block and a local state error. Production uses its single writer.
            consensus::ValidationState state;
            const bool accepted = blockchain_->process_new_block(block, state);
            submission.outcome = accepted ? services::SubmissionOutcome::ACCEPTED :
                (state.is_error() ? services::SubmissionOutcome::INTERNAL_ERROR :
                                   services::SubmissionOutcome::REJECTED);
            submission.validation_result = state.get_result();
            submission.reject_code = state.get_reject_code();
            submission.reject_reason = state.get_reject_reason();
            submission.debug_message = state.get_debug_message();
        }
    } catch (const std::exception& error) {
        submission.outcome = services::SubmissionOutcome::INTERNAL_ERROR;
        submission.reject_reason = "block-processing-exception";
        submission.debug_message = error.what();
    } catch (...) {
        submission.outcome = services::SubmissionOutcome::INTERNAL_ERROR;
        submission.reject_reason = "block-processing-exception";
    }
    if (!submission.is_accepted()) {
        GEO_LOG_ERROR(
            "sync",
            "Block submission failed"
                << ", hash=" << HashToHex(block.get_hash())
                << ", prev_hash=" << HashToHex(block.header.prev_block_hash)
                << ", reason=" << submission.reject_reason
                << ", debug=" << submission.debug_message
        );
    }

    return submission;
}

void SyncManager::send_getblocks(std::shared_ptr<Peer> peer) {
    if (!peer || !blockchain_) {
        GEO_LOG_ERROR("sync", "send_getblocks: peer or blockchain is null");
        return;
    }

    // Создаем block locator (список хешей наших известных блоков)
    std::vector<Hash256> locator_hashes;

    // ВАЖНО: Используем blockchain_->get_height() (активная цепь)
    Height our_height = blockchain_->get_height();

    if (SyncDebugEnabled()) {
        GEO_LOG_DEBUG("sync", "send_getblocks: our_height=" << our_height);
    }

    // Добавляем текущий tip
    if (our_height > 0) {
        core::Block tip_block;
        if (blockchain_->get_block_by_height(our_height, tip_block)) {
            locator_hashes.push_back(tip_block.get_hash());
            if (SyncDebugEnabled()) {
                GEO_LOG_DEBUG("sync", "send_getblocks: Added tip block at height " << our_height);
            }
        }
    }

    // Добавляем блоки с экспоненциальным шагом (как в Bitcoin)
    // Высоты: tip, tip-1, tip-2, tip-4, tip-8, tip-16, ...
    Height step = 1;
    Height height = our_height;

    while (height > 0 && locator_hashes.size() < 10) {
        if (height > step) {
            height -= step;
        } else {
            height = 0;
        }

        core::Block block;
        if (blockchain_->get_block_by_height(height, block)) {
            locator_hashes.push_back(block.get_hash());
        }

        step *= 2;
    }

    // Всегда добавляем genesis
    if (our_height >= 0) {
        core::Block genesis;
        if (blockchain_->get_block_by_height(0, genesis)) {
            Hash256 genesis_hash = genesis.get_hash();
            if (std::find(locator_hashes.begin(), locator_hashes.end(), genesis_hash) == locator_hashes.end()) {
                locator_hashes.push_back(genesis_hash);
                if (SyncDebugEnabled()) {
                    GEO_LOG_DEBUG("sync", "send_getblocks: Added genesis block");
                }
            }
        }
    }

    if (SyncDebugEnabled()) {
        GEO_LOG_DEBUG("sync", "send_getblocks: Locator has " << locator_hashes.size() << " hashes");
    }

    // Создаем GETBLOCKS сообщение
    GetBlocksMessage msg;
    msg.version = 1;
    msg.locator_hashes = locator_hashes;
    msg.stop_hash = Hash256{};  // Пустой = получить все до конца

    bytes payload = msg.serialize();
    bytes message = create_message(MessageType::GETBLOCKS, payload);

    peer->send(message);
    GEO_METRIC_INC("sync.getblocks_sent_total", 1);
    last_getblocks_request_ms_.store(SteadyNowMs(), std::memory_order_relaxed);

    if (SyncDebugEnabled()) {
        GEO_LOG_DEBUG(
            "sync",
            "Sent GETBLOCKS to " << peer->get_address()
                << " (our_height=" << our_height << ", locator_size=" << locator_hashes.size() << ")"
        );
    }
}

}  // namespace geocoin::network
