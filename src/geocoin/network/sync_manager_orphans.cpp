#include "geocoin/network/sync_manager.hpp"
#include "geocoin/network/sync_manager_internal.hpp"

#include "geocoin/utils/metrics.hpp"

#include <algorithm>
#include <mutex>
#include <utility>

namespace geocoin::network {

void SyncManager::erase_orphan_index_locked(const Hash256& block_hash) {
    auto block_it = orphan_blocks_.find(block_hash);
    if (block_it == orphan_blocks_.end() || !block_it->second.block) {
        return;
    }

    const Hash256 prev_hash = block_it->second.block->header.prev_block_hash;
    auto range = orphan_blocks_by_prev_.equal_range(prev_hash);
    for (auto it = range.first; it != range.second;) {
        if (it->second == block_hash) {
            it = orphan_blocks_by_prev_.erase(it);
        } else {
            ++it;
        }
    }
}

void SyncManager::store_orphan_block(
    const Hash256& block_hash,
    const core::Block& block,
    std::shared_ptr<Peer> peer
) {
    if (chain_state_ && chain_state_->has_block(block_hash)) {
        return;
    }

    const uint64_t now_ms = detail::steady_now_ms();
    std::vector<Hash256> evicted;
    {
        std::lock_guard<std::mutex> lock(orphan_mutex_);

        if (orphan_blocks_.find(block_hash) != orphan_blocks_.end()) {
            erase_orphan_index_locked(block_hash);
        }

        orphan_blocks_[block_hash] = OrphanBlockEntry{
            std::make_shared<core::Block>(block),
            std::move(peer),
            now_ms
        };
        orphan_blocks_by_prev_.emplace(block.header.prev_block_hash, block_hash);

        while (orphan_blocks_.size() > max_orphan_blocks_) {
            auto oldest = orphan_blocks_.begin();
            for (auto it = orphan_blocks_.begin(); it != orphan_blocks_.end(); ++it) {
                if (it->second.received_ms < oldest->second.received_ms) {
                    oldest = it;
                }
            }
            evicted.push_back(oldest->first);
            erase_orphan_index_locked(oldest->first);
            orphan_blocks_.erase(oldest);
        }

        GEO_METRIC_SET("sync.orphan_blocks", static_cast<int64_t>(orphan_blocks_.size()));
    }
    // Dropping a cached body must not drop the need for that body. Release the
    // buffer lock before touching the scheduler's header and download state.
    for (const auto& hash : evicted) requeue_block_download(hash);
}

std::vector<SyncManager::OrphanBlockEntry> SyncManager::take_orphan_children(const Hash256& parent_hash) {
    std::vector<OrphanBlockEntry> children;
    std::lock_guard<std::mutex> lock(orphan_mutex_);

    auto range = orphan_blocks_by_prev_.equal_range(parent_hash);
    std::vector<Hash256> child_hashes;
    for (auto it = range.first; it != range.second; ++it) {
        child_hashes.push_back(it->second);
    }
    orphan_blocks_by_prev_.erase(parent_hash);

    children.reserve(child_hashes.size());
    for (const auto& child_hash : child_hashes) {
        auto it = orphan_blocks_.find(child_hash);
        if (it == orphan_blocks_.end()) {
            continue;
        }
        children.push_back(std::move(it->second));
        orphan_blocks_.erase(it);
    }

    GEO_METRIC_SET("sync.orphan_blocks", static_cast<int64_t>(orphan_blocks_.size()));
    return children;
}

void SyncManager::drain_orphan_blocks(const Hash256& parent_hash) {
    auto children = take_orphan_children(parent_hash);
    if (children.empty()) {
        return;
    }

    GEO_METRIC_INC("sync.orphan_blocks_drained_total", static_cast<int64_t>(children.size()));
    for (const auto& child : children) {
        if (!child.block) {
            continue;
        }
        handle_block(child.peer, *child.block);
    }
}

} // namespace geocoin::network
