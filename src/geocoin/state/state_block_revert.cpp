#include "geocoin/state/state.hpp"
#include "geocoin/state/state_internal.hpp"
#include "geocoin/utils/logger.hpp"

#include <exception>

namespace geocoin::state {

bool ChainState::revert_block(const core::Block& block) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    if (!derived_state_storage_valid_) {
        GEO_LOG_ERROR(
            "chainstate",
            "Cannot revert block: derived state is invalid and requires a rebuild"
        );
        return false;
    }

    // Проверяем, что это текущий блок
    if (block.get_hash() != current_tip_hash_) {
        GEO_LOG_ERROR("chainstate", "Cannot revert block: not current tip");
        return false;
    }

    // Загружаем undo данные
    BlockUndo block_undo;
    Hash256 block_hash = block.get_hash();

    if (!load_block_undo(block_hash, block_undo)) {
        GEO_LOG_ERROR("chainstate", "Cannot revert block: undo data not found");
        return false;
    }

    // Validate the complete undo envelope before the first in-memory mutation.
    // Undo records are persisted data and may be truncated, stale or corrupted.
    if (block_undo.block_hash != block_hash ||
        block_undo.height != current_height_ ||
        block_undo.tx_undos.size() != block.transactions.size()) {
        GEO_LOG_ERROR("chainstate", "Cannot revert block: undo data mismatch");
        return false;
    }

    if (!validate_foundation_undo_(block, block_undo)) {
        GEO_LOG_ERROR("chainstate", "Cannot revert block: foundation undo does not match the authority transitions");
        return false;
    }

    if (block_undo.interest_pool_delta < 0 ||
        (block_undo.interest_pool_delta > 0 &&
         interest_pool_balance_ < block_undo.interest_pool_delta)) {
        GEO_LOG_ERROR("chainstate", "Cannot revert block: invalid interest pool delta");
        return false;
    }

    if (transition_in_progress_) {
        GEO_LOG_ERROR("chainstate", "Nested block revert transition is not allowed");
        return false;
    }
    transition_in_progress_ = true;
    transition_persistence_ok_ = true;
    struct TransitionScope {
        bool& in_progress;
        bool& persistence_ok;
        ~TransitionScope() {
            in_progress = false;
            persistence_ok = true;
        }
    } transition_scope{transition_in_progress_, transition_persistence_ok_};

    bool mutation_started = false;
    bool journal_written = false;
    bool journal_preexisting = false;
    bytes previous_journal;
    try {
      if (storage_) {
        journal_preexisting = storage_->has_metadata("chainstate_journal");
        if (journal_preexisting &&
            !storage_->get_metadata("chainstate_journal", previous_journal)) {
            GEO_LOG_ERROR("chainstate", "Failed to preserve the existing chainstate journal");
            return false;
        }
        if (!write_state_journal(block_undo.height, block_hash, kJournalPhaseReverting)) {
            GEO_LOG_ERROR("chainstate", "Failed to write chainstate revert journal");
            return false;
        }
        journal_written = true;
        storage_->begin_batch();
      }
      mutation_started = true;

    // Откатываем транзакции в обратном порядке
    for (size_t i = block.transactions.size(); i > 0; --i) {
        const size_t tx_index = i - 1;
        const auto& tx = block.transactions[tx_index];
        const auto& tx_undo = block_undo.tx_undos[tx_index];

        if (!revert_transaction(tx, tx_undo)) {
            GEO_LOG_ERROR("chainstate", "Failed to revert transaction #" << tx_index);
            // revert_transaction can discover corrupt undo data after touching
            // caches. Keep the journal and fail closed until startup rebuilds
            // the complete derived state from the active chain.
            derived_state_storage_valid_ = false;
            state_transition_poisoned_ = true;
            if (storage_) {
                storage_->abort_batch();
            }
            return false;
        }
        if (!transition_persistence_ok_) {
            GEO_LOG_ERROR("chainstate", "Persistence failed while reverting transaction #" << tx_index);
            derived_state_storage_valid_ = false;
            state_transition_poisoned_ = true;
            if (storage_) storage_->abort_batch();
            return false;
        }
    }

    if (block_undo.interest_pool_delta > 0) {
        interest_pool_balance_ -= block_undo.interest_pool_delta;
        persist_interest_pool_balance_to_storage();
        if (!transition_persistence_ok_) {
            derived_state_storage_valid_ = false;
            state_transition_poisoned_ = true;
            if (storage_) storage_->abort_batch();
            GEO_LOG_ERROR("chainstate", "Failed to persist reverted interest pool balance");
            return false;
        }
    }

    // Удаляем tx из active-index для откатываемого блока.
    erase_active_tx_index_for_block(block);
    if (!transition_persistence_ok_) {
        derived_state_storage_valid_ = false;
        state_transition_poisoned_ = true;
        if (storage_) storage_->abort_batch();
        GEO_LOG_ERROR("chainstate", "Failed to persist reverted active transaction index");
        return false;
    }

    // Удаляем undo данные
    const std::string undo_key = hash256_to_hex(block_hash);
    undo_log_.erase(undo_key);
    // Position, UTXO count and undo deletion belong to the same durable batch.
    // Once in-memory undo has run, a failed write requires journal recovery.
    const Height reverted_height = current_height_ > 0 ? current_height_ - 1 : 0;
    if ((storage_ && !storage_->delete_metadata("undo:" + undo_key)) ||
        !set_height(reverted_height) ||
        !set_tip_hash(block.header.prev_block_hash) ||
        !persist_utxo_count_to_storage()) {
        derived_state_storage_valid_ = false;
        state_transition_poisoned_ = true;
        if (storage_) storage_->abort_batch();
        GEO_LOG_ERROR("chainstate", "Failed to persist reverted chain position");
        return false;
    }

    if (storage_) {
        if (!storage_->commit_batch()) {
            GEO_LOG_ERROR("chainstate", "Failed to commit chainstate revert batch");
            derived_state_storage_valid_ = false;
            state_transition_poisoned_ = true;
            storage_->abort_batch();
            return false;
        }
        if (storage_->last_batch_commit_was_durable() && !clear_state_journal()) {
            GEO_LOG_WARN("chainstate", "Failed to clear chainstate revert journal");
        }
    }

    return true;
    } catch (const std::exception& error) {
        if (storage_) {
            try {
                storage_->abort_batch();
            } catch (...) {
            }
        }
        if (mutation_started) {
            derived_state_storage_valid_ = false;
            state_transition_poisoned_ = true;
        } else if (journal_written && storage_) {
            try {
                if (journal_preexisting) {
                    (void)storage_->put_metadata("chainstate_journal", previous_journal);
                } else {
                    (void)clear_state_journal();
                }
            } catch (...) {
            }
        }
        GEO_LOG_ERROR("chainstate", "Block revert transition threw: " << error.what());
        return false;
    } catch (...) {
        if (storage_) {
            try {
                storage_->abort_batch();
            } catch (...) {
            }
        }
        if (mutation_started) {
            derived_state_storage_valid_ = false;
            state_transition_poisoned_ = true;
        } else if (journal_written && storage_) {
            try {
                if (journal_preexisting) {
                    (void)storage_->put_metadata("chainstate_journal", previous_journal);
                } else {
                    (void)clear_state_journal();
                }
            } catch (...) {
            }
        }
        GEO_LOG_ERROR("chainstate", "Block revert transition threw an unknown exception");
        return false;
    }
}

} // namespace geocoin::state
