#include "geocoin/state/state.hpp"

#include "geocoin/consensus/consensus.hpp"
#include "geocoin/crypto/geo_area.hpp"
#include "geocoin/utils/bytes.hpp"

#include <algorithm>
#include <limits>
#include <mutex>
#include <set>
#include <string>
#include <utility>

namespace geocoin::state {

void ChainState::set_prune_depth(Height depth) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (depth == 0) {
        prune_depth_ = consensus::params::WITNESS_PRUNE_DEPTH;
        return;
    }
    prune_depth_ = depth;
}

bool ChainState::prune_block_bodies_below_height(Height keep_from_height, uint64_t* pruned_count) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (pruned_count) *pruned_count = 0;
    return block_storage_ && (!storage_ || !storage_->batch_active()) &&
        block_storage_->prune_block_bodies_below_height(keep_from_height, 0, pruned_count);
}

bool ChainState::prune_witness_up_to(Height target_height, size_t max_blocks) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!block_storage_ || archive_mode_ || (storage_ && storage_->batch_active())) {
        return false;
    }
    return block_storage_->prune_witness_up_to(std::min(target_height, current_height_), max_blocks);
}

bool ChainState::get_pruning_stats(storage::BlockStorage::PruningStats& out) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!block_storage_) {
        return false;
    }
    return block_storage_->get_pruning_stats(out);
}

bool ChainState::get_witness_payload(const Hash256& block_hash, bytes& out) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!block_storage_) {
        return false;
    }
    {
        std::lock_guard<std::mutex> cache_lock(witness_cache_mutex_);
        const std::string key = hash256_to_hex(block_hash);
        auto it = witness_cache_.find(key);
        if (it != witness_cache_.end()) {
            out = it->second;
            return true;
        }
    }
    if (block_storage_->build_witness_payload_from_block(block_hash, out)) {
        return true;
    }
    return block_storage_->get_witness_payload(block_hash, out);
}

bool ChainState::restore_witness_from_payload(const Hash256& block_hash, const bytes& payload) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!block_storage_) {
        return false;
    }
    if (!block_storage_->restore_witness_from_payload(block_hash, payload)) {
        return false;
    }
    cache_witness_payload(block_hash, payload);
    return true;
}

bool ChainState::get_witness_pruned_height(Height& out) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!block_storage_) {
        return false;
    }
    return block_storage_->get_witness_pruned_height(out);
}

void ChainState::persist_witness_pubkey_to_storage(const Hash256& pubkey_hash, const bytes& pubkey) {
    if (!storage_ || pubkey.empty()) {
        return;
    }
    record_persistence_result_(
        storage_->put_metadata("wpk:" + hash256_to_hex(pubkey_hash), pubkey),
        "persist witness public key"
    );
}

void ChainState::delete_witness_pubkey_from_storage(const Hash256& pubkey_hash) {
    if (!storage_) {
        return;
    }
    const std::string hex = hash256_to_hex(pubkey_hash);
    record_persistence_result_(
        storage_->delete_metadata("wpk:" + hex),
        "delete witness public key"
    );
    record_persistence_result_(
        storage_->delete_metadata("wpkref:" + hex),
        "delete witness public key refcount"
    );
}

void ChainState::load_witness_pubkeys_from_storage() {
    witness_pubkey_cache_.clear();
    witness_pubkey_refcounts_.clear();
    witness_pubkey_lru_.clear();
    witness_pubkey_lru_index_.clear();
    witness_pubkey_cache_hits_ = 0;
    witness_pubkey_cache_misses_ = 0;
    witness_pubkey_bytes_saved_ = 0;
    witness_pubkey_stats_dirty_events_ = 0;
    if (!storage_) {
        return;
    }

    const std::string prefix = "wpk:";
    const std::string ref_prefix = "wpkref:";
    size_t payload_count = 0;
    storage_->get_keys_page(prefix, 0, 0, &payload_count);
    size_t valid_pairs = 0;
    bool corrupt = false;
    std::string after;
    constexpr size_t page_size = 1024;
    for (;;) {
        const auto keys = storage_->get_keys_page(ref_prefix, 0, page_size, nullptr, after);
        for (const auto& key : keys) {
            const auto hex = key.substr(ref_prefix.size());
            Hash256 hash{};
            bytes ref_raw, pub;
            uint64_t refcount = 0;
            if (!utils::hex_to_hash256_checked(hex, hash) || utils::hash256_to_hex(hash) != hex ||
                !storage_->get_metadata(key, ref_raw) || !utils::decode_u64_le_exact(ref_raw, refcount) || refcount == 0 ||
                !storage_->get_metadata(prefix + hex, pub) || pub.size() != consensus::params::WITNESS_PUBKEY_SIZE ||
                crypto::sha256(pub) != hash) {
                corrupt = true;
                continue;
            }
            witness_cache_put_(hex, std::move(pub));
            witness_pubkey_refcounts_[hex] = refcount;
            ++valid_pairs;
        }
        if (keys.size() < page_size) break;
        after = keys.back();
    }
    // Every valid ref key has one distinct, validated payload. Equal counts
    // rule out any extra payloads without retaining or rereading their keys.
    // Incomplete pairs invalidate the snapshot; opening never rewrites them.
    if (corrupt || payload_count != valid_pairs) {
        derived_state_storage_valid_ = false;
    }

    load_witness_pubkey_stats_from_storage();
}

void ChainState::load_witness_pubkey_stats_from_storage() {
    if (!storage_) {
        return;
    }

    bytes raw;
    if (!storage_->get_metadata("wpk_stats", raw) || raw.size() != 24) {
        return;
    }

    auto read_u64 = [&](size_t off) -> uint64_t {
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) {
            v |= static_cast<uint64_t>(raw[off + i]) << (i * 8);
        }
        return v;
    };

    witness_pubkey_cache_hits_ = read_u64(0);
    witness_pubkey_cache_misses_ = read_u64(8);
    witness_pubkey_bytes_saved_ = read_u64(16);
    witness_pubkey_stats_dirty_events_ = 0;
}

void ChainState::persist_witness_pubkey_stats_to_storage(bool force) const {
    if (!storage_ || witness_pubkey_stats_dirty_events_ == 0) {
        return;
    }
    if (!force && witness_pubkey_stats_dirty_events_ < WITNESS_STATS_FLUSH_INTERVAL) {
        return;
    }

    bytes raw(24, 0);
    auto write_u64 = [&](size_t off, uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            raw[off + i] = static_cast<byte>((v >> (i * 8)) & 0xFF);
        }
    };

    write_u64(0, witness_pubkey_cache_hits_);
    write_u64(8, witness_pubkey_cache_misses_);
    write_u64(16, witness_pubkey_bytes_saved_);
    try {
        if (storage_->put_metadata("wpk_stats", raw)) {
            witness_pubkey_stats_dirty_events_ = 0;
        }
    } catch (...) {
        // Cache statistics are diagnostic only. A metrics write must never
        // change transaction validity or poison a consensus transition.
    }
}

void ChainState::register_witness_pubkeys_from_tx(const core::Transaction& tx) {
    std::set<std::string> seen;
    for (const auto& w : tx.witnesses) {
        if (w.public_key.empty()) {
            continue;
        }
        if (w.public_key.size() != consensus::params::WITNESS_PUBKEY_SIZE) {
            continue;
        }
        const Hash256 computed = crypto::sha256(w.public_key);
        if (computed != w.public_key_hash) {
            continue;
        }
        const std::string hex = hash256_to_hex(w.public_key_hash);
        if (!seen.insert(hex).second) {
            continue;
        }
        const uint64_t previous_refcount = witness_refcount_get_(hex);
        if (previous_refcount == std::numeric_limits<uint64_t>::max()) {
            throw std::runtime_error("witness pubkey refcount overflow");
        }
        witness_refcount_set_(hex, previous_refcount + 1);

        auto it = witness_pubkey_cache_.find(hex);
        if (it == witness_pubkey_cache_.end()) {
            witness_cache_put_(hex, w.public_key);
        } else {
            witness_cache_touch_lru_(hex);
        }
        if (previous_refcount == 0) {
            persist_witness_pubkey_to_storage(w.public_key_hash, w.public_key);
        }
    }
}

void ChainState::rebuild_witness_pubkey_cache_from_active_chain() {
    witness_pubkey_cache_.clear();
    witness_pubkey_refcounts_.clear();
    witness_pubkey_lru_.clear();
    witness_pubkey_lru_index_.clear();
    if (!block_storage_) {
        return;
    }

    for (Height h = 0; h <= current_height_; ++h) {
        core::Block block;
        if (!block_storage_->get_block_by_height(h, block, false)) {
            continue;
        }
        for (const auto& tx : block.transactions) {
            register_witness_pubkeys_from_tx(tx);
        }
    }
}

void ChainState::cache_witness_payload(const Hash256& block_hash, const bytes& payload) {
    constexpr size_t payload_budget = 32 * 1024 * 1024;
    if (payload.empty() || payload.size() > payload_budget) {
        return;
    }
    const std::string key = hash256_to_hex(block_hash);
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::lock_guard<std::mutex> cache_lock(witness_cache_mutex_);
    auto it = witness_cache_.find(key);
    if (it == witness_cache_.end()) {
        witness_cache_order_.push_back(key);
    }
    witness_cache_[key] = payload;
    size_t retained_bytes = 0;
    for (const auto& [hash, value] : witness_cache_) retained_bytes += value.size();
    while (witness_cache_order_.size() > witness_cache_max_ || retained_bytes > payload_budget) {
        const std::string oldest = witness_cache_order_.front();
        witness_cache_order_.pop_front();
        retained_bytes -= witness_cache_.at(oldest).size();
        witness_cache_.erase(oldest);
    }
}

} // namespace geocoin::state
