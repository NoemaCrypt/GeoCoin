#include "geocoin/state/state.hpp"

#include "geocoin/consensus/consensus.hpp"
#include "geocoin/utils/bytes.hpp"
#include "geocoin/storage/state_records.hpp"
#include "geocoin/utils/logger.hpp"

#include <algorithm>
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace geocoin::state {

namespace {
bool is_utxo_metadata_key_lifecycle(const std::string& key) {
    return key.rfind("utxo:", 0) == 0;
}

bool has_archive_witness(const core::Transaction& tx) {
    if (tx.is_coinbase()) return true;
    const auto type = tx.type;
    const bool needs_witness = type == core::TransactionType::STANDARD || type == core::TransactionType::NFT_CREATE ||
        type == core::TransactionType::NFT_TRANSFER || type == core::TransactionType::NFT_BURN ||
        type == core::TransactionType::CADASTRAL_DOC;
    if (!needs_witness) return true;
    return !tx.witnesses.empty() && tx.witnesses.size() == tx.inputs.size() &&
        std::all_of(tx.witnesses.begin(), tx.witnesses.end(),
                    [](const core::TxWitness& witness) { return !witness.signature.empty(); });
}

void bind_transaction_archive(const std::shared_ptr<Storage>& storage,
                              const std::shared_ptr<storage::BlockStorage>& blocks) {
    if (!blocks) return;
    if (!storage) { blocks->set_transaction_archiver({}); return; }
    blocks->set_transaction_archiver([weak = std::weak_ptr<Storage>(storage)](const core::Block& block, Height height) {
        const auto target = weak.lock();
        if (!target || target->batch_active()) return false;
        const auto block_hash = block.get_hash();
        for (const auto& tx : block.transactions) {
            const auto hash = tx.get_hash();
            bytes marker;
            storage::ActiveTransactionRecord record;
            if (!target->get_metadata("atx:" + utils::hash256_to_hex(hash), marker) ||
                !storage::ActiveTransactionRecord::decode(marker, record) || record.height != height ||
                record.block_hash != block_hash || record.transaction_count != block.transactions.size()) return false;
            core::Transaction archived;
            if (target->get_transaction(hash, archived)) {
                if (!has_archive_witness(archived)) return false;
                continue;
            }
            if (!has_archive_witness(tx)) return false;
            // Direct writes are synchronous even during IBD. Never delete the
            // only material copy until every archived transaction is durable.
            if (!target->put_transaction(hash, tx)) return false;
        }
        return true;
    });
}

}

void ChainState::set_activation_manifest(const utils::ActivationManifest& manifest) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    activation_manifest_ = manifest;
}

ChainState::ChainState() : ChainState(std::unique_ptr<Storage>{}) {}

ChainState::ChainState(std::unique_ptr<Storage> storage)
    : current_height_(0)
    , block_storage_(nullptr)
    , storage_(nullptr)
    , angle_store_(std::make_shared<multisig::AngleCommitmentStore>())
{
    multisig_verifier_ = std::make_shared<multisig::MultisigVerifier>(angle_store_);
    prune_depth_ = consensus::params::WITNESS_PRUNE_DEPTH;
    undo_retention_depth_ = consensus::params::MAX_REORG_DEPTH;
    set_storage(std::move(storage));
}

void ChainState::set_storage(std::unique_ptr<Storage> storage) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (transition_in_progress_ || (storage_ && storage_->batch_active()) ||
        (storage && storage->batch_active())) {
        throw std::logic_error("Cannot replace state storage during an unfinished transition or batch");
    }
    storage_ = std::move(storage);
    // The previous archive callback must not outlive the state it described.
    bind_transaction_archive({}, block_storage_);
    try {
        clear();
        idx_epoch_.clear();
        journal_recovery_required_ = false;
        load_chain_position_from_storage();
        if (!recover_from_journal_if_needed()) {
            load_index_epoch_from_storage();
            load_delegations_from_storage_();
            load_nft_registry_from_storage();
            load_sidechain_deposits_from_storage();
            load_sidechain_registry_from_storage();
            load_card_registry_from_storage();
            load_foundation_config_from_storage();
            load_interest_pool_balance_from_storage();
            load_witness_pubkeys_from_storage();
        }
        load_utxo_set_from_storage();
        rebuild_active_tx_index_from_active_chain();
        bind_transaction_archive(storage_, block_storage_);
    } catch (...) {
        derived_state_storage_valid_ = false;
        state_transition_poisoned_ = true;
        throw;
    }
}

void ChainState::load_chain_position_from_storage() {
    chain_position_loaded_from_storage_ = false;
    if (!storage_) {
        return;
    }

    Height stored_height = 0;
    bool has_height = false;
    bytes height_data;
    if (storage_->get_metadata("chainstate_height", height_data) && height_data.size() == 8) {
        for (int i = 0; i < 8; ++i) {
            stored_height |= static_cast<Height>(height_data[i]) << (i * 8);
        }
        has_height = true;
    }

    Hash256 stored_tip{};
    bytes tip_hash_data;
    const bool has_tip =
        storage_->get_metadata("chainstate_tip_hash", tip_hash_data) &&
        tip_hash_data.size() == stored_tip.size();
    if (has_tip) {
        std::memcpy(stored_tip.data(), tip_hash_data.data(), stored_tip.size());
    }

    if (has_height) {
        current_height_ = stored_height;
    }
    if (has_tip) {
        current_tip_hash_ = stored_tip;
    }
    chain_position_loaded_from_storage_ = has_height && has_tip;

    if (has_height || has_tip) {
        GEO_LOG_INFO(
            "chainstate",
            "Loaded persisted chain position: height=" << current_height_
                << ", complete=" << (chain_position_loaded_from_storage_ ? "yes" : "no")
        );
    }
}

void ChainState::load_utxo_set_from_storage() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!storage_ || !utxo_set_.empty()) {
        return;
    }

    const std::vector<std::string> keys = storage_->get_keys_with_prefix("utxo:");
    size_t skipped_corrupt = 0;
    for (const auto& key : keys) {
        if (!is_utxo_metadata_key_lifecycle(key)) {
            continue;
        }

        bytes data;
        if (!storage_->get_metadata(key, data)) {
            throw storage::ReadError(storage::ReadStatus::CORRUPT, "UTXO disappeared during snapshot scan");
        }

        try {
            UTXO utxo = UTXO::deserialize(data);
            const std::string canonical_key = utxo.get_key();
            if (canonical_key != key) {
                ++skipped_corrupt;
                continue;
            }
            bytes index_value;
            if (!storage_->get_metadata(utxo.get_address_index_key(), index_value) || !index_value.empty()) {
                derived_state_storage_valid_ = false;
                GEO_LOG_WARN("chainstate", "UTXO address index is incomplete or corrupt");
            }
            track_utxo_inserted_(utxo);
            utxo_set_.insert_or_assign(utxo.memory_key(), std::move(utxo));
        } catch (const storage::ReadError&) {
            throw;
        } catch (const std::exception&) {
            ++skipped_corrupt;
        }
    }

    if (skipped_corrupt != 0) {
        utxo_set_.clear();
        utxo_balance_by_height_.clear();
        utxo_balance_index_valid_ = true;
        throw std::runtime_error(
            "ChainState UTXO snapshot is corrupt; refusing partial state (reindex required)"
        );
    }

    const uint64_t loaded = static_cast<uint64_t>(utxo_set_.size());
    uint64_t stored_count = 0;
    bytes count_data;
    const bool has_count = storage_->get_metadata("chainstate_utxo_count", count_data);
    if (has_count && count_data.size() == 8) {
        for (int i = 0; i < 8; ++i) {
            stored_count |= static_cast<uint64_t>(count_data[i]) << (i * 8);
        }
        if (stored_count != loaded) {
            GEO_LOG_WARN(
                "chainstate",
                "UTXO snapshot count mismatch: loaded=" << loaded << " stored=" << stored_count
                    << "; refusing partial snapshot"
            );
            derived_state_storage_valid_ = false;
        }
    } else if (has_count || storage_->has_metadata("chainstate_height")) {
        derived_state_storage_valid_ = false;
    }
    size_t index_count = 0;
    storage_->get_keys_page("addr:utxo:", 0, 0, &index_count);
    if (index_count != loaded) {
        derived_state_storage_valid_ = false;
    }

    if (loaded > 0) {
        GEO_LOG_INFO(
            "chainstate",
            "Loaded UTXO snapshot from storage: utxos=" << loaded
        );
    }
}

void ChainState::set_ibd_mode(bool enabled) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (storage_) {
        storage_->set_ibd_mode(enabled);
    }
}

void ChainState::set_ibd_flush_interval(size_t blocks) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (storage_) {
        storage_->set_ibd_flush_interval(blocks);
    }
}

void ChainState::set_block_storage(std::shared_ptr<storage::BlockStorage> storage) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    block_storage_ = storage;
    bind_transaction_archive(storage_, block_storage_);

    if (recover_from_storage_mismatch()) {
        return;
    }

    // The derived-state database owns its own position. Never overwrite it
    // with BlockStorage::best, which may point at a non-active fork. Old or
    // empty datadirs fall back to the persisted active-chain tip only.
    if (block_storage_ && !journal_recovery_required_ && !chain_position_loaded_from_storage_) {
        Hash256 active_tip{};
        Height active_height = 0;
        if (block_storage_->get_active_tip(active_tip, active_height)) {
            current_height_ = active_height;
            current_tip_hash_ = active_tip;
            GEO_LOG_INFO("chainstate", "Loaded fallback position from active chain: " << active_height);
        } else {
            current_height_ = block_storage_->get_best_height();
            core::Block best_block;
            if (block_storage_->get_best_block(best_block, true)) {
                current_tip_hash_ = best_block.get_hash();
            }
            GEO_LOG_INFO("chainstate", "Loaded fallback height from BlockStorage: " << current_height_);
        }
    }

    // Attaching a store is read-only with respect to retention. Apply the
    // resolved policy explicitly, before pruning on a state transition.

    // Active tx-index строится строго по active-chain (источник истины),
    // чтобы не использовать потенциально stale tx-ключи из старых datadir.
    rebuild_active_tx_index_from_active_chain();

    if (!storage_) {
        rebuild_witness_pubkey_cache_from_active_chain();
    } else if (witness_pubkey_cache_.empty()) {
        load_witness_pubkeys_from_storage();
    }
}

void ChainState::set_undo_retention_depth(Height depth) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    undo_retention_depth_ = depth;
    if (!prune_expired_undo_history_()) {
        GEO_LOG_WARN("chainstate", "Failed to apply the updated undo retention depth");
    }
}

bool ChainState::set_height(Height height) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    current_height_ = height;

    // The chain position has one persisted representation.
    if (storage_) {
        bytes height_data(8);
        for (int i = 0; i < 8; ++i) {
            height_data[i] = static_cast<byte>((height >> (i * 8)) & 0xFF);
        }
        return storage_->put_metadata("chainstate_height", height_data);
    }
    return true;
}

bool ChainState::set_tip_hash(const Hash256& tip_hash) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    current_tip_hash_ = tip_hash;

    if (!storage_) {
        return true;
    }

    bytes data(32);
    std::memcpy(data.data(), tip_hash.data(), 32);
    return storage_->put_metadata("chainstate_tip_hash", data);
}

} // namespace geocoin::state
