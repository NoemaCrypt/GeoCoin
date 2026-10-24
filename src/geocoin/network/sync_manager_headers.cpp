#include "geocoin/network/sync_manager.hpp"
#include "geocoin/network/sync_manager_internal.hpp"
#include "geocoin/network/messages.hpp"
#include "geocoin/network/network_utils.hpp"
#include "geocoin/core/blockchain.hpp"
#include "geocoin/chain/block_index.hpp"
#include "geocoin/consensus/chainparams.hpp"
#include "geocoin/consensus/consensus.hpp"
#include "geocoin/consensus/proof_of_work.hpp"
#include "geocoin/consensus/validation.hpp"
#include "geocoin/utils/logger.hpp"
#include "geocoin/utils/metrics.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

namespace geocoin::network {
namespace {

// HEADERS policy accepts 16 messages per ten seconds. Keep our solicited
// responses below that budget even when block announcements arrive in bursts.
constexpr uint64_t kHeadersRequestIntervalMs = 1000;
constexpr uint64_t kHeadersResponseTimeoutMs = 10000;

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

}  // namespace

PeerSyncInfo& SyncManager::get_sync_peer_locked(const std::shared_ptr<Peer>& peer) {
    auto& info = sync_peers_[peer->get_address()];
    if (info.peer != peer) {
        // A new connection must not inherit an exhausted discovery or an
        // outstanding request from the previous connection to this address.
        info = PeerSyncInfo{};
        info.peer = peer;
    }
    return info;
}

bool SyncManager::is_best_header_locked(Height height, const Hash256& hash) const {
    const auto it = best_header_by_height_.find(height);
    return it != best_header_by_height_.end() && it->second == hash;
}

uint256 SyncManager::minimum_new_header_work() const {
    const Height depth = consensus::params::MAX_REORG_DEPTH;
    if (!blockchain_ || depth == 0 || blockchain_->get_policy_checkpoint_info().allow_deep_reorg) {
        return {};
    }
    const auto* tip = blockchain_->get_tip();
    if (!tip || tip->height <= depth) {
        return {};
    }
    const auto* floor = tip->get_ancestor(tip->height - depth);
    return floor ? floor->chain_work : uint256{};
}

namespace {

std::string side_header_source_key(const std::shared_ptr<Peer>& peer) {
    if (!peer) {
        return {};
    }
    const std::string address = peer->get_address();
    std::string host;
    uint16_t port = 0;
    std::string key = NetworkUtils::split_host_port(address, host, port)
        ? NetworkUtils::abuse_key(host) : std::string{};
    return key.empty() ? address : key;
}

} // namespace

void SyncManager::apply_network_header_limits() {
    const auto& params = consensus::Params();
    if (params.fixed_difficulty() || params.allow_min_difficulty_blocks()) {
        // Cheap headers must not occupy the mainnet-sized index. Best-path
        // entries are still kept through IBD; only side branches are evicted
        // once this cap is crossed.
        max_headers_index_size_ = 32768;
        max_side_headers_per_source_ = 256;
    }
}

bool SyncManager::has_side_header_budget(const std::shared_ptr<Peer>& peer, size_t header_count) {
    return charge_side_header_budget_impl(peer, header_count, false);
}

bool SyncManager::charge_side_header_budget(const std::shared_ptr<Peer>& peer, size_t header_count) {
    return charge_side_header_budget_impl(peer, header_count, true);
}

bool SyncManager::charge_side_header_budget_impl(
    const std::shared_ptr<Peer>& peer, size_t header_count, bool consume
) {
    if (!peer || header_count == 0) {
        return true;
    }
    constexpr auto kRefillPeriod = std::chrono::hours(24);
    constexpr size_t kMaxTrackedSources = 4096;

    const std::string key = side_header_source_key(peer);
    const double capacity = static_cast<double>(max_side_headers_per_source_);
    const double refill_per_second =
        capacity / std::chrono::duration<double>(kRefillPeriod).count();
    const auto now = std::chrono::steady_clock::now();
    const auto available = [&](const SideHeaderBudget& budget) {
        const double elapsed = std::chrono::duration<double>(now - budget.updated).count();
        return std::min(capacity, budget.tokens + elapsed * refill_per_second);
    };

    std::lock_guard<std::mutex> lock(peers_mutex_);
    auto it = side_header_budgets_.find(key);
    if (it == side_header_budgets_.end()) {
        if (side_header_budgets_.size() >= kMaxTrackedSources) {
            const auto fullest = std::max_element(
                side_header_budgets_.begin(), side_header_budgets_.end(),
                [&](const auto& lhs, const auto& rhs) {
                    return available(lhs.second) < available(rhs.second);
                });
            side_header_budgets_.erase(fullest);
        }
        it = side_header_budgets_.emplace(key, SideHeaderBudget{capacity, now}).first;
    } else {
        it->second.tokens = available(it->second);
        it->second.updated = now;
    }
    if (it->second.tokens < static_cast<double>(header_count)) {
        return false;
    }
    if (consume) {
        it->second.tokens -= static_cast<double>(header_count);
    }
    return true;
}

void SyncManager::refresh_sync_target() {
    if (!blockchain_) return;
    const auto* active_tip = blockchain_->get_tip();
    const Height local_height = active_tip ? active_tip->height : 0;
    const uint256 active_work = active_tip ? active_tip->chain_work : uint256{};
    std::lock_guard<std::mutex> lock(queue_mutex_);
    if (active_tip && (best_header_tip_hash_ == Hash256{} || best_header_tip_work_ < active_work)) {
        rebuild_best_header_chain_locked(active_tip->get_block_hash(), local_height, active_work);
    }
    // Height is only a progress gauge: a shorter chain can have greater work.
    // Replacing the best branch must also be allowed to lower the target.
    const bool more_work = best_header_tip_work_ > active_work;
    const Height target = more_work ? std::max(local_height, best_header_tip_height_) : local_height;
    target_height_.store(target, std::memory_order_relaxed);
    const auto status = more_work ? SyncStatus::SYNCING : SyncStatus::COMPLETED;
    status_.store(status, std::memory_order_relaxed);
    SetSyncStatusMetric(status);
    GEO_METRIC_SET("sync.target_height", static_cast<int64_t>(target));
    GEO_METRIC_SET("sync.current_height", static_cast<int64_t>(local_height));
}

bool SyncManager::discard_failed_header_branch(const Hash256& hash) {
    auto* failed = blockchain_ ? blockchain_->find_block_index(hash) : nullptr;
    if (!failed || !failed->is_failed()) return false;
    while (failed->prev && failed->prev->is_failed()) failed = failed->prev;

    std::unordered_set<Hash256, Hash256Hasher> discarded{failed->get_block_hash()};
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        std::unordered_multimap<Hash256, Hash256, Hash256Hasher> children;
        std::vector<Hash256> pending{failed->get_block_hash()};
        for (const auto& [child_hash, header] : headers_index_) {
            children.emplace(header.prev_hash, child_hash);
            // Header pruning can remove links between the failed root and
            // retained descendants. Blockchain keeps failure flags on indexed
            // descendants, including anchors of header-only continuations.
            for (const auto& indexed_hash : {child_hash, header.prev_hash}) {
                const auto* index = blockchain_->find_block_index(indexed_hash);
                if (index && index->is_failed() && discarded.insert(indexed_hash).second) {
                    pending.push_back(indexed_hash);
                }
            }
        }
        for (size_t i = 0; i < pending.size(); ++i) {
            const auto [begin, end] = children.equal_range(pending[i]);
            for (auto it = begin; it != end; ++it) {
                if (discarded.insert(it->second).second) pending.push_back(it->second);
            }
        }
        for (const auto& discarded_hash : discarded) {
            headers_index_.erase(discarded_hash);
            header_heights_.erase(discarded_hash);
            downloading_blocks_.erase(discarded_hash);
            download_queue_set_.erase(discarded_hash);
            download_attempts_.erase(discarded_hash);
            download_avoid_peer_.erase(discarded_hash);
        }
        for (auto it = download_queue_.begin(); it != download_queue_.end();) {
            if (discarded.contains(it->second)) it = download_queue_.erase(it);
            else ++it;
        }

        const auto* active_tip = blockchain_->get_tip();
        Hash256 best_hash = active_tip ? active_tip->get_block_hash() : Hash256{};
        Height best_height = active_tip ? active_tip->height : 0;
        uint256 best_work = active_tip ? active_tip->chain_work : uint256{};
        if (!discarded.contains(best_header_tip_hash_) && best_header_tip_work_ > best_work) {
            best_hash = best_header_tip_hash_;
            best_height = best_header_tip_height_;
            best_work = best_header_tip_work_;
        }
        for (const auto& [candidate_hash, header] : headers_index_) {
            if (header.cumulative_work > best_work) {
                best_hash = candidate_hash;
                best_height = header.height;
                best_work = header.cumulative_work;
            }
        }
        rebuild_best_header_chain_locked(best_hash, best_height, best_work);
        GEO_METRIC_SET("sync.download_queue", static_cast<int64_t>(download_queue_.size()));
        GEO_METRIC_SET("sync.downloading", static_cast<int64_t>(downloading_blocks_.size()));
    }
    {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        for (auto& [address, peer] : sync_peers_) {
            std::erase_if(peer.requested_blocks, [&](const Hash256& requested) {
                return discarded.contains(requested);
            });
        }
    }
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        for (auto it = pending_blocks_.begin(); it != pending_blocks_.end();) {
            if (it->second && discarded.contains(it->second->get_hash())) it = pending_blocks_.erase(it);
            else ++it;
        }
        GEO_METRIC_SET("sync.pending_blocks", static_cast<int64_t>(pending_blocks_.size()));
    }
    {
        std::lock_guard<std::mutex> lock(orphan_mutex_);
        for (auto it = orphan_blocks_.begin(); it != orphan_blocks_.end();) {
            if (discarded.contains(it->first)) {
                erase_orphan_index_locked(it->first);
                it = orphan_blocks_.erase(it);
            } else {
                ++it;
            }
        }
        GEO_METRIC_SET("sync.orphan_blocks", static_cast<int64_t>(orphan_blocks_.size()));
    }
    GEO_METRIC_INC("sync.failed_header_branches_total", 1);
    refresh_sync_target();
    return true;
}

void SyncManager::rebuild_best_header_chain_locked(
    const Hash256& tip_hash,
    Height tip_height,
    const uint256& tip_work
) {
    std::map<Height, Hash256> best_path;
    Hash256 cursor = tip_hash;
    size_t walked = 0;
    const auto* active_tip = blockchain_ ? blockchain_->get_tip() : nullptr;
    const uint256 active_work = active_tip ? active_tip->chain_work : uint256{};

    while (cursor != Hash256{} && walked <= headers_index_.size()) {
        auto it = headers_index_.find(cursor);
        if (it == headers_index_.end()) {
            const auto* index = blockchain_ ? blockchain_->find_block_index(cursor) : nullptr;
            const auto position = blockchain_ ? blockchain_->get_block_position(cursor) :
                                               core::BlockPositionSnapshot{};
            if (!index || index->is_failed() || position.in_active_chain ||
                (position.has_data && index->chain_work <= active_work)) break;
            // A known anchor may still need its body or a local apply retry.
            // Restore that trusted ancestry even when HEADERS starts at a child.
            const auto header = index->get_block_header();
            it = headers_index_.emplace(cursor, HeaderEntry{
                index->height, header.prev_block_hash, header.timestamp, index->chain_work, header
            }).first;
            header_heights_[cursor] = index->height;
        }
        best_path[it->second.height] = cursor;
        cursor = it->second.prev_hash;
        ++walked;
    }

    best_header_by_height_.swap(best_path);
    best_header_tip_hash_ = tip_hash;
    best_header_tip_height_ = tip_height;
    best_header_tip_work_ = tip_work;

    // Удаляем stale-запросы другой ветки. Уже скачиваемые блоки не отменяем:
    // валидный fork-body всё равно полезно сохранить для fork choice.
    for (auto it = download_queue_.begin(); it != download_queue_.end();) {
        if (!is_best_header_locked(it->first, it->second)) {
            download_queue_set_.erase(it->second);
            forget_download_retry_locked(it->second);
            it = download_queue_.erase(it);
        } else {
            ++it;
        }
    }

    // Запрашиваем весь неизвестный путь к глобально лучшему tip, включая
    // родителей, которые на своей высоте раньше не были локальным победителем.
    for (const auto& [height, hash] : best_header_by_height_) {
        const auto position = blockchain_ ? blockchain_->get_block_position(hash) :
                                           core::BlockPositionSnapshot{};
        const auto header = headers_index_.find(hash);
        if (position.in_active_chain ||
            (position.has_data && header != headers_index_.end() && header->second.cumulative_work <= active_work) ||
            downloading_blocks_.find(hash) != downloading_blocks_.end()) {
            continue;
        }
        if (download_queue_set_.insert(hash).second) {
            download_queue_.emplace(height, hash);
        }
    }
}

void SyncManager::prune_headers_state_locked(Height current_height) {
    Height cutoff = 0;
    if (current_height > headers_prune_keep_depth_) {
        cutoff = current_height - headers_prune_keep_depth_;
    }

    if (headers_index_.size() > max_headers_index_size_) {
        const size_t overflow = headers_index_.size() - max_headers_index_size_;
        const Height extra = static_cast<Height>((overflow / 8) + 1);
        if (current_height > extra) {
            cutoff = std::max(cutoff, current_height - extra);
        }
    }

    for (auto it = headers_index_.begin(); it != headers_index_.end();) {
        if (it->second.height < cutoff) {
            header_heights_.erase(it->first);
            download_queue_set_.erase(it->first);
            forget_download_retry_locked(it->first);
            it = headers_index_.erase(it);
        } else {
            ++it;
        }
    }

    for (auto it = best_header_by_height_.begin(); it != best_header_by_height_.end() && it->first < cutoff;) {
        it = best_header_by_height_.erase(it);
    }

    for (auto it = download_queue_.begin(); it != download_queue_.end();) {
        bool erase = (it->first < cutoff);
        if (!erase) {
            if (!is_best_header_locked(it->first, it->second)) {
                erase = true;
            }
        }
        if (erase) {
            download_queue_set_.erase(it->second);
            forget_download_retry_locked(it->second);
            it = download_queue_.erase(it);
        } else {
            ++it;
        }
    }

    // max_headers_index_size_ — жесткий предел для боковых веток. Сам
    // best-path не удаляем раньше применения блоков, иначе потеряем контекст
    // проверки и список недостающих ancestors.
    if (headers_index_.size() > max_headers_index_size_) {
        std::vector<std::pair<Height, Hash256>> non_best;
        non_best.reserve(headers_index_.size());
        for (const auto& [hash, entry] : headers_index_) {
            if (!is_best_header_locked(entry.height, hash)) {
                non_best.emplace_back(entry.height, hash);
            }
        }
        std::sort(non_best.begin(), non_best.end(), [](const auto& a, const auto& b) {
            if (a.first != b.first) {
                return a.first < b.first;
            }
            return a.second < b.second;
        });

        for (const auto& [height, hash] : non_best) {
            (void)height;
            if (headers_index_.size() <= max_headers_index_size_) {
                break;
            }
            headers_index_.erase(hash);
            header_heights_.erase(hash);
            download_queue_set_.erase(hash);
            forget_download_retry_locked(hash);
        }
    }
}

void SyncManager::activate_headers_sync(std::shared_ptr<Peer> peer, Height hinted_target) {
    if (!peer || !blockchain_) {
        return;
    }

    const Height local_height = blockchain_->get_height();

    {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        auto& info = get_sync_peer_locked(peer);
        info.advertised_height = std::max(info.advertised_height, hinted_target);
        info.headers_exhausted = false;
        info.last_request = std::chrono::steady_clock::now();
    }

    // hinted_target came from VERSION/INV and is not authenticated consensus
    // state. Keep the public target pinned to active/validated headers only.
    refresh_sync_target();

    GEO_METRIC_SET("sync.target_height", static_cast<int64_t>(target_height_.load(std::memory_order_relaxed)));
    GEO_METRIC_SET("sync.current_height", static_cast<int64_t>(local_height));
    send_getheaders(peer);
}

bool SyncManager::handle_headers(
    std::shared_ptr<Peer> peer,
    const std::vector<core::BlockHeader>& headers
) {
    if (!blockchain_) {
        return false;
    }
    if (peer) {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        auto it = sync_peers_.find(peer->get_address());
        if (it != sync_peers_.end() && it->second.peer == peer) {
            it->second.headers_request_pending = false;
        }
    }
    auto penalize_peer = [&](uint32_t score_inc) {
        if (!peer) {
            return;
        }
        std::lock_guard<std::mutex> lock(peers_mutex_);
        auto it = sync_peers_.find(peer->get_address());
        if (it == sync_peers_.end()) {
            return;
        }
        if (it->second.bad_headers_score > std::numeric_limits<uint32_t>::max() - score_inc) {
            it->second.bad_headers_score = std::numeric_limits<uint32_t>::max();
        } else {
            it->second.bad_headers_score += score_inc;
        }
        if (it->second.bad_headers_score >= 8) {
            GEO_LOG_WARN("sync", "Disconnecting peer for invalid headers: " << peer->get_address());
            disconnect_peer(peer, "sync-invalid-headers");
        }
    };

    auto mark_peer_headers_exhausted = [&](Height validated_height) {
        if (!peer) {
            return;
        }
        std::lock_guard<std::mutex> lock(peers_mutex_);
        auto it = sync_peers_.find(peer->get_address());
        if (it == sync_peers_.end()) {
            return;
        }
        it->second.height = std::max(it->second.height, validated_height);
        it->second.headers_exhausted = true;
    };

    auto reject_failed_parent = [&](const Hash256& hash) {
        if (!discard_failed_header_branch(hash)) return false;
        GEO_METRIC_INC("sync.headers_rejected_total", 1);
        penalize_peer(2);
        return true;
    };

    if (headers.empty()) {
        GEO_METRIC_INC("sync.headers_empty_total", 1);
        const Height our_height = blockchain_->get_height();
        mark_peer_headers_exhausted(our_height);
        const Height peer_height = peer ? peer->get_info().height : 0;
        if (peer && peer_height > our_height) {
            GEO_METRIC_INC("sync.headers_empty_from_ahead_peer_total", 1);
            GEO_LOG_WARN(
                "sync",
                "Peer claimed higher starting height but returned no headers: peer="
                    << peer->get_address()
                    << ", local_height=" << our_height
                    << ", peer_startingheight=" << peer_height
                    << ", target_height=" << target_height_.load(std::memory_order_relaxed)
                    << ". Treating startingheight as a sync hint only."
            );
            penalize_peer(1);
        }
        refresh_sync_target();
        return true;
    }

    if (SyncDebugEnabled()) {
        GEO_LOG_DEBUG("sync", "handle_headers: received " << headers.size() << " headers");
    }

    std::vector<std::unique_ptr<chain::BlockIndex>> validation_nodes;
    validation_nodes.reserve(headers.size() + 64);

    auto get_header_entry = [&](const Hash256& hash, HeaderEntry& out) -> bool {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        auto it = headers_index_.find(hash);
        if (it == headers_index_.end()) {
            return false;
        }
        out = it->second;
        return true;
    };

    Hash256 failed_anchor{};
    auto restore_prev_index_context = [&](const Hash256& tip_hash) -> chain::BlockIndex* {
        if (tip_hash == Hash256{}) {
            return nullptr;
        }

        std::vector<std::pair<Hash256, HeaderEntry>> chain_path;
        chain_path.reserve(64);

        Hash256 cursor = tip_hash;
        chain::BlockIndex* anchor = nullptr;

        // Difficulty needs the latest DAA window plus the MTP ancestors of
        // its oldest element. Rebuilding the entire header-first chain here
        // made every small HEADERS message O(chain length), allowing a peer
        // to force repeated copies of hundreds of thousands of entries.
        constexpr size_t kMaxValidationContext =
            static_cast<size_t>(consensus::params::DIFFICULTY_DAA_WINDOW) +
            static_cast<size_t>(consensus::params::MEDIAN_TIME_SPAN);

        while (cursor != Hash256{}) {
            if (auto* known = blockchain_->find_block_index(cursor)) {
                if (known->is_failed()) {
                    failed_anchor = cursor;
                    return nullptr;
                }
                anchor = known;
                break;
            }

            if (chain_path.size() >= kMaxValidationContext) {
                break;
            }

            HeaderEntry entry;
            if (!get_header_entry(cursor, entry)) {
                return nullptr;
            }
            chain_path.emplace_back(cursor, std::move(entry));
            cursor = chain_path.back().second.prev_hash;
        }

        chain::BlockIndex* prev = anchor;
        for (auto it = chain_path.rbegin(); it != chain_path.rend(); ++it) {
            auto idx = std::make_unique<chain::BlockIndex>(it->second.header);
            idx->hash = it->first;
            idx->prev = prev;
            idx->height = it->second.height;
            if (prev &&
                (prev->height == (std::numeric_limits<Height>::max)() ||
                 idx->height != prev->height + 1)) {
                return nullptr;
            }
            idx->chain_work = it->second.cumulative_work;
            prev = idx.get();
            validation_nodes.push_back(std::move(idx));
        }

        return prev;
    };

    std::vector<Hash256> header_hashes(headers.size());
    std::vector<char> header_hash_ready(headers.size(), 0);
    const auto header_hash_at = [&](size_t index) -> const Hash256& {
        if (!header_hash_ready[index]) {
            header_hashes[index] = headers[index].get_hash();
            header_hash_ready[index] = 1;
        }
        return header_hashes[index];
    };

    size_t first_unknown = 0;
    chain::BlockIndex* known_anchor = nullptr;
    while (first_unknown < headers.size()) {
        const Hash256& header_hash = header_hash_at(first_unknown);
        if (auto* known = blockchain_->find_block_index(header_hash)) {
            if (reject_failed_parent(header_hash)) return false;
            const auto position = blockchain_->get_block_position(header_hash);
            const auto* active_tip = blockchain_->get_tip();
            if ((!position.has_data && !position.in_active_chain) ||
                (active_tip && known->chain_work > active_tip->chain_work)) break;
            known_anchor = known;
            ++first_unknown;
            continue;
        }
        break;
    }

    if (first_unknown == headers.size()) {
        if (SyncDebugEnabled()) {
            GEO_LOG_DEBUG("sync", "handle_headers: all headers are already known");
        }
        const Height our_height = blockchain_->get_height();
        mark_peer_headers_exhausted(our_height);
        if (peer && peer->get_info().height > our_height) {
            GEO_METRIC_INC("sync.headers_known_only_from_ahead_peer_total", 1);
            penalize_peer(1);
        }
        refresh_sync_target();
        return true;
    }

    if (SyncDebugEnabled() && first_unknown > 0) {
        GEO_LOG_DEBUG("sync", "handle_headers: skipped " << first_unknown << " known leading headers");
    }

    Hash256 prev_hash = headers[first_unknown].prev_block_hash;
    if (prev_hash == Hash256{} &&
        header_hash_at(first_unknown) != consensus::Params().get_genesis_hash()) {
        GEO_METRIC_INC("sync.headers_rejected_total", 1);
        GEO_LOG_WARN(
            "sync",
            "Rejecting headers rooted at a foreign genesis, peer="
                << (peer ? peer->get_address() : "unknown")
        );
        penalize_peer(2);
        return false;
    }
    chain::BlockIndex* validation_prev_index = nullptr;
    if (known_anchor && known_anchor->get_block_hash() == prev_hash) {
        validation_prev_index = known_anchor;
    } else {
        validation_prev_index = restore_prev_index_context(prev_hash);
    }
    if (failed_anchor != Hash256{} && reject_failed_parent(failed_anchor)) return false;
    const bool have_prev = (prev_hash == Hash256{}) || (validation_prev_index != nullptr);
    if (!have_prev) {
        GEO_METRIC_INC("sync.headers_rejected_total", 1);
        GEO_LOG_WARN(
            "sync",
            "Rejecting headers: unknown parent context"
                << ", peer=" << (peer ? peer->get_address() : "unknown")
                << ", first_unknown=" << first_unknown
                << ", prev_hash=" << HashToHex(prev_hash)
                << ", local_height=" << blockchain_->get_height()
                << ", peer_startingheight=" << (peer ? peer->get_info().height : 0)
        );
        penalize_peer(1);
        if (peer) {
            send_getheaders(peer);
        }
        return false;
    }

    Height current_height = validation_prev_index ? validation_prev_index->height : static_cast<Height>(-1);
    Hash256 current_prev_hash = prev_hash;
    uint256 current_cumulative_work;
    if (validation_prev_index) {
        current_cumulative_work = validation_prev_index->chain_work;
    }

    refresh_sync_target();

    const uint256 minimum_work = minimum_new_header_work();

    // Project work from bits only. A side batch that cannot beat the current
    // best tip is charged to the per-source budget before PoW validation.
    uint256 best_tip_work;
    size_t unknown_new_headers = 0;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        best_tip_work = best_header_tip_work_;
        for (size_t i = first_unknown; i < headers.size(); ++i) {
            if (headers_index_.find(header_hash_at(i)) == headers_index_.end()) {
                ++unknown_new_headers;
            }
        }
    }
    uint256 projected_work = current_cumulative_work;
    chain::BlockIndex* projected_prev = validation_prev_index;
    std::vector<std::unique_ptr<chain::BlockIndex>> projected_nodes;
    projected_nodes.reserve(headers.size() - first_unknown);
    for (size_t i = first_unknown; i < headers.size(); ++i) {
        projected_work = consensus::calculate_chainwork(projected_prev, headers[i].bits);
        auto projected = std::make_unique<chain::BlockIndex>(headers[i]);
        projected->hash = header_hash_at(i);
        projected->prev = projected_prev;
        projected->height = projected_prev ? (projected_prev->height + 1) : 0;
        projected->chain_work = projected_work;
        projected_prev = projected.get();
        projected_nodes.push_back(std::move(projected));
    }
    if (projected_work <= best_tip_work &&
        !has_side_header_budget(peer, unknown_new_headers)) {
        GEO_METRIC_INC("sync.side_header_budget_exhausted_total", 1);
        GEO_LOG_WARN(
            "sync",
            "Rejecting headers: side-branch budget exhausted before validation"
                << ", peer=" << (peer ? peer->get_address() : "unknown")
                << ", new_side_headers=" << unknown_new_headers
        );
        penalize_peer(8);
        refresh_sync_target();
        return false;
    }

    std::vector<std::pair<Hash256, HeaderEntry>> validated_headers;
    validated_headers.reserve(headers.size() - first_unknown);

    for (size_t i = first_unknown; i < headers.size(); ++i) {
        const auto& header = headers[i];
        const Hash256& header_hash = header_hash_at(i);

        if (i > first_unknown && header.prev_block_hash != current_prev_hash) {
            GEO_METRIC_INC("sync.headers_rejected_total", 1);
            GEO_LOG_WARN(
                "sync",
                "Rejecting headers: non-contiguous sequence"
                    << ", peer=" << (peer ? peer->get_address() : "unknown")
                    << ", header_index=" << i
                    << ", expected_prev=" << HashToHex(current_prev_hash)
                    << ", actual_prev=" << HashToHex(header.prev_block_hash)
            );
            penalize_peer(2);
            refresh_sync_target();
            return false;
        }

        consensus::ValidationState header_state;
        if (!consensus::check_block_header(header, header_state) ||
            !consensus::contextual_check_block_header(header, header_state, validation_prev_index)) {
            GEO_METRIC_INC("sync.headers_rejected_total", 1);
            GEO_LOG_WARN(
                "sync",
                "Rejecting invalid header"
                    << ", peer=" << (peer ? peer->get_address() : "unknown")
                    << ", header_hash=" << HashToHex(header_hash)
                    << ", prev_height=" << (validation_prev_index ? validation_prev_index->height : static_cast<Height>(0))
                    << ", bits=" << header.bits
                    << ", reason=" << header_state.get_reject_reason()
            );
            penalize_peer(2);
            refresh_sync_target();
            return false;
        }

        const uint256 next_cumulative_work = consensus::calculate_chainwork(validation_prev_index, header.bits);
        // Work grows along the batch, so the first new header decides for all.
        if (i == first_unknown && next_cumulative_work <= minimum_work) {
            GEO_METRIC_INC("sync.headers_below_reorg_floor_total", 1);
            GEO_LOG_WARN(
                "sync",
                "Rejecting headers: branch forks below the reorg limit"
                    << ", peer=" << (peer ? peer->get_address() : "unknown")
                    << ", header_hash=" << HashToHex(header_hash)
            );
            penalize_peer(1);
            refresh_sync_target();
            return false;
        }

        auto validated_index = std::make_unique<chain::BlockIndex>(header);
        validated_index->hash = header_hash;
        validated_index->prev = validation_prev_index;
        validated_index->height = validation_prev_index ? (validation_prev_index->height + 1) : 0;
        validated_index->chain_work = next_cumulative_work;
        validation_prev_index = validated_index.get();
        validation_nodes.push_back(std::move(validated_index));

        ++current_height;
        current_cumulative_work = next_cumulative_work;
        validated_headers.emplace_back(header_hash, HeaderEntry{
            current_height, header.prev_block_hash, header.timestamp, current_cumulative_work, header
        });
        current_prev_hash = header_hash;
    }

    // Rebuilding the best path and pruning each walk the whole index, so the
    // batch is committed at once instead of repeating them for every header.
    size_t new_side_headers = 0;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (current_cumulative_work <= best_header_tip_work_) {
            for (const auto& [hash, entry] : validated_headers) {
                (void)entry;
                if (headers_index_.find(hash) == headers_index_.end()) {
                    ++new_side_headers;
                }
            }
        }
    }
    if (!charge_side_header_budget(peer, new_side_headers)) {
        GEO_METRIC_INC("sync.side_header_budget_exhausted_total", 1);
        GEO_LOG_WARN(
            "sync",
            "Rejecting headers: side-branch budget exhausted"
                << ", peer=" << (peer ? peer->get_address() : "unknown")
                << ", new_side_headers=" << new_side_headers
        );
        penalize_peer(8);
        refresh_sync_target();
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        for (auto& [hash, entry] : validated_headers) {
            header_heights_[hash] = entry.height;
            headers_index_[hash] = std::move(entry);
        }
        if (current_cumulative_work > best_header_tip_work_) {
            rebuild_best_header_chain_locked(current_prev_hash, current_height, current_cumulative_work);
        }
        prune_headers_state_locked(blockchain_->get_height());
    }

    refresh_sync_target();

    GEO_METRIC_INC("sync.headers_received_total", static_cast<int64_t>(headers.size()));
    GEO_METRIC_SET("sync.target_height", static_cast<int64_t>(target_height_.load(std::memory_order_relaxed)));

    if (peer) {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        auto it = sync_peers_.find(peer->get_address());
        if (it != sync_peers_.end()) {
            it->second.height = std::max(it->second.height, current_height);
            it->second.last_header_hash = header_hash_at(headers.size() - 1);
            it->second.headers_exhausted = headers.size() < kMaxHeadersPerMessage;
            if (it->second.bad_headers_score > 0) {
                --it->second.bad_headers_score;
            }
        }
    }
    if (headers.size() >= kMaxHeadersPerMessage && peer) {
        send_getheaders(peer);
    }

    return true;
}

std::vector<core::BlockHeader> SyncManager::handle_getheaders(
    std::shared_ptr<Peer> peer,
    const std::vector<Hash256>& locator_hashes,
    const Hash256& stop_hash
) {
    if (SyncDebugEnabled()) {
        GEO_LOG_DEBUG("sync", "handle_getheaders: request from " << (peer ? peer->get_address() : "unknown"));
        GEO_LOG_DEBUG("sync", "  Locator hashes: " << locator_hashes.size());
    }

    std::vector<core::BlockHeader> headers;

    if (!blockchain_) {
        GEO_LOG_ERROR("sync", "handle_getheaders: No blockchain");
        return headers;
    }

    // Find a locator that is actually on our active chain.  Merely knowing a
    // block index is insufficient: the locator may name a side-fork block at
    // the same height.  Treating that as common used to skip the active fork
    // ancestors and return a header whose parent the requester did not know,
    // causing honest peers to be penalized and disconnected during a reorg.
    Height start_height = 0;
    for (const auto& hash : locator_hashes) {
        const auto* common_index = blockchain_->find_block_index(hash);
        if (!common_index) {
            continue;
        }
        Hash256 active_hash{};
        if (!blockchain_->get_active_block_hash_by_height(common_index->height, active_hash) ||
            active_hash != hash) {
            continue;
        }
        start_height = common_index->height + 1;
        if (SyncDebugEnabled()) {
            GEO_LOG_DEBUG("sync", "  Found active common block at height " << common_index->height);
        }
        break;
    }

    Height current_height = start_height;
    const Height max_height = blockchain_->get_height();

    if (SyncDebugEnabled()) {
        GEO_LOG_DEBUG("sync", "  Sending headers from " << start_height << " to " << max_height);
    }

    while (current_height <= max_height && headers.size() < kMaxHeadersPerMessage) {
        Hash256 active_hash{};
        bool appended = false;
        if (blockchain_->get_active_block_hash_by_height(current_height, active_hash)) {
            if (const auto* idx = blockchain_->find_block_index(active_hash)) {
                headers.push_back(idx->header);
                appended = true;
            }
        }

        if (!appended) {
            core::Block b;
            if (blockchain_->get_block_by_height(current_height, b)) {
                active_hash = b.get_hash();
                headers.push_back(b.header);
                appended = true;
            }
        }

        if (appended) {
            if (active_hash == stop_hash) {
                break;
            }
        }
        ++current_height;
    }

    if (SyncDebugEnabled()) {
        GEO_LOG_DEBUG("sync", "  Returning " << headers.size() << " headers");
    }

    return headers;
}

void SyncManager::send_getheaders(std::shared_ptr<Peer> peer) {
    if (!peer || !blockchain_) {
        GEO_LOG_ERROR("sync", "send_getheaders: peer or blockchain is null");
        return;
    }
    if (!peer->is_connected() || peer->get_state() != PeerState::READY) return;

    const uint64_t now_ms = SteadyNowMs();
    Hash256 header_locator{};
    {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        auto& info = get_sync_peer_locked(peer);
        info.headers_request_queued = true;
        const uint64_t elapsed = now_ms >= info.last_headers_request_ms
            ? now_ms - info.last_headers_request_ms : 0;
        if (info.last_headers_request_ms != 0 &&
            (elapsed < kHeadersRequestIntervalMs ||
             (info.headers_request_pending && elapsed < kHeadersResponseTimeoutMs))) {
            return;
        }
        info.headers_request_pending = true;
        info.headers_request_queued = false;
        info.last_headers_request_ms = now_ms;
        header_locator = info.last_header_hash;
    }

    std::vector<Hash256> locator_hashes;
    const Height our_height = blockchain_->get_height();

    // Continue from validated headers while their bodies are still in flight.
    // An active-tip-only locator repeats the same full page during catch-up.
    if (header_locator != Hash256{}) {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        const auto* index = blockchain_->find_block_index(header_locator);
        if (index ? !index->is_failed() : headers_index_.contains(header_locator)) {
            locator_hashes.push_back(header_locator);
        }
    }

    auto add_active_hash = [&](Height height) {
        Hash256 active_hash{};
        if (blockchain_->get_active_block_hash_by_height(height, active_hash) &&
            std::find(locator_hashes.begin(), locator_hashes.end(), active_hash) == locator_hashes.end()) {
            locator_hashes.push_back(active_hash);
            return true;
        }

        core::Block block;
        if (blockchain_->get_block_by_height(height, block)) {
            const Hash256 block_hash = block.get_hash();
            if (std::find(locator_hashes.begin(), locator_hashes.end(), block_hash) == locator_hashes.end()) {
                locator_hashes.push_back(block_hash);
            }
            return true;
        }

        return false;
    };

    if (our_height > 0) {
        add_active_hash(our_height);
    }

    Height step = 1;
    Height height = our_height;
    while (height > 0 && locator_hashes.size() < 32) {
        if (height > step) {
            height -= step;
        } else {
            height = 0;
        }
        add_active_hash(height);
        step *= 2;
    }

    add_active_hash(0);

    GetBlocksMessage msg;
    msg.version = 1;
    msg.locator_hashes = locator_hashes;
    msg.stop_hash = Hash256{};

    bytes payload = msg.serialize();
    bytes message = create_message(MessageType::GETHEADERS, payload);
    if (peer->send(message) <= 0) {
        GEO_METRIC_INC("sync.getheaders_send_fail_total", 1);
        return;
    }
    GEO_METRIC_INC("sync.getheaders_sent_total", 1);

    last_getblocks_request_ms_.store(now_ms, std::memory_order_relaxed);

    if (SyncDebugEnabled()) {
        GEO_LOG_DEBUG(
            "sync",
            "Sent GETHEADERS to " << peer->get_address()
                << " (our_height=" << our_height << ", locator_size=" << locator_hashes.size() << ")"
        );
    }
}

void SyncManager::update_headers_requests() {
    const uint64_t now_ms = SteadyNowMs();
    std::vector<std::shared_ptr<Peer>> ready;
    bool timed_out = false;
    {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        for (auto& [address, info] : sync_peers_) {
            if (!info.peer || !info.peer->is_connected() ||
                info.peer->get_state() != PeerState::READY) continue;
            const uint64_t elapsed = now_ms >= info.last_headers_request_ms
                ? now_ms - info.last_headers_request_ms : 0;
            if (info.headers_request_pending && elapsed >= kHeadersResponseTimeoutMs) {
                timed_out = true;
                ready.push_back(info.peer);
            } else if (info.headers_request_queued && !info.headers_request_pending &&
                       elapsed >= kHeadersRequestIntervalMs) {
                ready.push_back(info.peer);
            }
        }
    }
    // A silent discovery peer supplies no chain work. Do not hold IBD open
    // on its unvalidated hint while retrying the bounded header request.
    if (timed_out) refresh_sync_target();
    for (const auto& peer : ready) send_getheaders(peer);
}

}  // namespace geocoin::network
