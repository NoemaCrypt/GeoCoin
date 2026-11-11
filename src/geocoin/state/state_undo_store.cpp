#include "geocoin/state/state.hpp"

#include "geocoin/utils/logger.hpp"

#include <exception>
#include <string>
#include <utility>
#include <vector>

namespace geocoin::state {

bool ChainState::save_block_undo(const BlockUndo& undo) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    const auto key = hash256_to_hex(undo.block_hash);
    if (storage_) {
        // Runtime ownership is not serialized. The caller retains its undo for
        // failed-apply rollback; successful disconnects use Storage's batch view.
        return storage_->put_metadata("undo:" + key, undo.serialize());
    }

    // The mempool/chain ownership marker is meaningful only while rolling back
    // the currently applying candidate. Once the block commits, a future chain
    // disconnect must remove its ANGLE_COMMITMENT from the shared store.
    BlockUndo durable_undo = undo;
    for (auto& tx_undo : durable_undo.tx_undos) {
        tx_undo.angle_store_preexisted = false;
    }

    undo_log_[key] = std::move(durable_undo);
    return true;
}

bool ChainState::load_block_undo(const Hash256& block_hash, BlockUndo& undo) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    const auto key = hash256_to_hex(block_hash);
    if (storage_) {
        bytes data;
        std::string storage_key = "undo:" + key;
        if (storage_->get_metadata(storage_key, data)) {
            try {
                undo = BlockUndo::deserialize(data);
                return true;
            } catch (const std::exception& e) {
                GEO_LOG_ERROR(
                    "chainstate",
                    "Corrupted undo record for block " << key.substr(0, 16)
                        << "...: " << e.what()
                );
                return false;
            }
        }
        return false;
    }

    const auto it = undo_log_.find(key);
    if (it != undo_log_.end()) {
        undo = it->second;
        return true;
    }

    return false;
}

bool ChainState::prune_expired_undo_(Height applying_height) {
    if (undo_retention_depth_ == 0 || applying_height <= undo_retention_depth_ ||
        !block_storage_) {
        return true;
    }

    const Height expired_height = applying_height - undo_retention_depth_;
    Hash256 expired_hash{};
    if (!block_storage_->get_active_block_hash(expired_height, expired_hash)) {
        GEO_LOG_WARN(
            "chainstate",
            "Cannot resolve active block while pruning undo at height " << expired_height
        );
        return false;
    }

    const std::string key = hash256_to_hex(expired_hash);
    undo_log_.erase(key);
    if (!storage_) {
        return true;
    }

    try {
        return storage_->delete_metadata("undo:" + key);
    } catch (const std::exception& error) {
        GEO_LOG_ERROR("chainstate", "Failed to prune expired undo: " << error.what());
    } catch (...) {
        GEO_LOG_ERROR("chainstate", "Failed to prune expired undo");
    }
    return false;
}

bool ChainState::prune_expired_undo_history_() {
    if (!storage_ || undo_retention_depth_ == 0 ||
        current_height_ <= undo_retention_depth_) {
        return true;
    }

    const Height oldest_retained_height =
        current_height_ - undo_retention_depth_ + 1;
    std::vector<std::string> expired_keys;
    bool scan_ok = true;

    try {
        for (const auto& key : storage_->get_keys_with_prefix("undo:")) {
            bytes data;
            if (!storage_->get_metadata(key, data)) {
                GEO_LOG_WARN("chainstate", "Cannot read undo record while applying retention: " << key);
                scan_ok = false;
                continue;
            }
            try {
                const auto undo = BlockUndo::deserialize(data);
                if (undo.height < oldest_retained_height) {
                    expired_keys.push_back(key);
                    undo_log_.erase(hash256_to_hex(undo.block_hash));
                }
            } catch (const std::exception& error) {
                GEO_LOG_ERROR(
                    "chainstate",
                    "Cannot inspect undo record " << key << " while applying retention: "
                        << error.what()
                );
                scan_ok = false;
            }
        }

        if (expired_keys.empty()) {
            return scan_ok;
        }

        storage_->begin_batch();
        for (const auto& key : expired_keys) {
            if (!storage_->delete_metadata(key)) {
                storage_->abort_batch();
                return false;
            }
        }
        if (!storage_->commit_batch()) {
            storage_->abort_batch();
            return false;
        }
    } catch (const std::exception& error) {
        try {
            storage_->abort_batch();
        } catch (...) {
        }
        GEO_LOG_ERROR("chainstate", "Failed to prune expired undo history: " << error.what());
        return false;
    } catch (...) {
        try {
            storage_->abort_batch();
        } catch (...) {
        }
        GEO_LOG_ERROR("chainstate", "Failed to prune expired undo history");
        return false;
    }

    GEO_LOG_INFO(
        "chainstate",
        "Pruned " << expired_keys.size() << " expired undo records below height "
            << oldest_retained_height
    );
    return scan_ok;
}

} // namespace geocoin::state
