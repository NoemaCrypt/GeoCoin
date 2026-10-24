#include "geocoin/state/state.hpp"
#include "geocoin/state/state_internal.hpp"
#include "geocoin/storage/witness_payload.hpp"

#include "geocoin/consensus/consensus.hpp"
#include "geocoin/consensus/proof_of_work.hpp"
#include "geocoin/crypto/geo_area.hpp"
#include "geocoin/crypto/signatures.hpp"
#include "geocoin/utils/bytes.hpp"
#include "geocoin/utils/logger.hpp"
#include "geocoin/utils/varint.hpp"

#include <algorithm>
#include <exception>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <utility>

namespace geocoin::state {

bool ChainState::apply_block(const core::Block& block, bool already_verified) {
    consensus::ValidationState validation;
    return apply_block(block, validation, already_verified);
}

bool ChainState::apply_block(const core::Block& block,
                             consensus::ValidationState& validation,
                             bool already_verified) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    // Only explicit consensus checks below may classify a candidate INVALID.
    // Storage and rollback failures must leave it eligible for a later retry.
    validation.reset();
    validation.error("chainstate-apply-failed");
    const auto invalid = [&](const char* reason) {
        return validation.invalid(consensus::RejectCode::INVALID, reason);
    };

    if (!derived_state_storage_valid_) {
        GEO_LOG_ERROR(
            "chainstate",
            "Cannot apply block: derived state is invalid and requires a rebuild"
        );
        return false;
    }

    struct _BlockTimeScope {
        ChainState& s;
        std::optional<Timestamp> prev;
        explicit _BlockTimeScope(ChainState& st, Timestamp t) : s(st), prev(st.validation_block_time_) {
            st.validation_block_time_ = t;
        }
        ~_BlockTimeScope() { s.validation_block_time_ = prev; }
    } block_time_scope(*this, block.header.timestamp);

    const bool is_genesis = (block.header.prev_block_hash == Hash256{});
    if (!is_genesis && current_height_ == (std::numeric_limits<Height>::max)()) {
        GEO_LOG_ERROR("chainstate", "Cannot apply block: height overflow");
        return false;
    }
    const Height applying_height = is_genesis ? 0 : (current_height_ + 1);
    for (const auto& tx : block.transactions) {
        if (!consensus::is_final_transaction(tx, applying_height, block.header.timestamp)) {
            return invalid("bad-txns-nonfinal");
        }
    }

    // КРИТИЧНО (production): Undo данные обязательны так же, как и сами блоки.
    // Если мы не можем гарантировать возможность отката (нет persistent Storage),
    // мы не имеем права применять блок.
    if (!is_genesis && !storage_) {
        GEO_LOG_ERROR("chainstate", "Cannot apply block: persistent storage is not configured (undo is required)");
        return false;
    }

    // КРИТИЧНО: Проверяем блок ПЕРЕД применением!
    // Это включает проверку PoW, Merkle root и подписей всех транзакций
    // Исключение: Genesis блок (height == 0) не проверяется
    if (!is_genesis && !already_verified && !block.verify()) {
        GEO_LOG_ERROR("chainstate", "Block verification failed");
        if (storage_) {
            storage_->abort_batch();
        }
        return invalid("bad-block-verification");
    }

    // Проверяем, что блок следующий по высоте (и связан с текущим tip)
    if (!is_genesis) {
        if (current_tip_hash_ != Hash256{} && block.header.prev_block_hash != current_tip_hash_) {
            auto hash_prefix = [](const Hash256& h) {
                std::ostringstream oss;
                oss << std::hex << std::setfill('0');
                for (int i = 0; i < 8; ++i) {
                    oss << std::setw(2) << static_cast<int>(h[i]);
                }
                return oss.str();
            };
            GEO_LOG_ERROR(
                "chainstate",
                "Block prev_hash mismatch: expected tip=" << hash_prefix(current_tip_hash_)
            );
            if (storage_) {
                storage_->abort_batch();
            }
            return false;
        }
    }

    if (!is_genesis && applying_height != current_height_ + 1) {
        GEO_LOG_ERROR(
            "chainstate",
            "Block height mismatch: expected=" << (current_height_ + 1)
            << ", got=" << applying_height
        );
        if (storage_) {
            storage_->abort_batch();
        }
        return false;
    }

    // Создаем undo данные для блока
    std::vector<Hash256> txids;
    txids.reserve(block.transactions.size());
    for (const auto& tx : block.transactions) {
        txids.push_back(tx.get_hash());
    }
    const Hash256 block_hash = block.get_hash();

    BlockUndo block_undo;
    block_undo.height = applying_height;
    block_undo.block_hash = block_hash;
    block_undo.tx_undos.reserve(block.transactions.size());

    const Height previous_height = current_height_;
    const Hash256 previous_tip_hash = current_tip_hash_;
    const Amount previous_interest_pool_balance = interest_pool_balance_;
    bool active_tx_index_written = false;
    bool journal_preexisting = false;
    bytes previous_journal;

    // apply_transaction is expected to be atomic on failure. Only transactions that
    // returned true are included here; dependencies require strict reverse order.
    auto rollback_applied_transactions = [&](size_t applied_count, const char* reason) -> bool {
        const size_t available = std::min(block.transactions.size(), block_undo.tx_undos.size());
        if (applied_count > available) {
            GEO_LOG_ERROR(
                "chainstate",
                "Rollback metadata mismatch after " << reason
                    << ": requested=" << applied_count << ", available=" << available
            );
            applied_count = available;
        }

        bool rollback_ok = true;
        for (size_t j = applied_count; j > 0; --j) {
            const size_t tx_index = j - 1;
            if (!revert_transaction(block.transactions[tx_index], block_undo.tx_undos[tx_index])) {
                GEO_LOG_ERROR(
                    "chainstate",
                    "Failed to rollback transaction #" << tx_index << " after " << reason
                );
                rollback_ok = false;
            }
        }

        if (active_tx_index_written) {
            erase_active_tx_index_for_block(block);
            active_tx_index_written = false;
        }

        current_height_ = previous_height;
        current_tip_hash_ = previous_tip_hash;
        interest_pool_balance_ = previous_interest_pool_balance;
        undo_log_.erase(hash256_to_hex(block_undo.block_hash));

        if (!rollback_ok) {
            derived_state_storage_valid_ = false;
            state_transition_poisoned_ = true;
            GEO_LOG_ERROR(
                "chainstate",
                "ChainState rollback was incomplete after " << reason
                    << "; recovery journal is intentionally retained"
            );
        }
        return rollback_ok;
    };

    auto fail_with_rollback = [&](size_t applied_count, const char* reason) -> bool {
        // A failed write from the forward transition must not contaminate the
        // result of the compensating operations themselves.
        transition_persistence_ok_ = true;
        const bool rollback_ok = rollback_applied_transactions(applied_count, reason);
        const bool rollback_persistence_ok = transition_persistence_ok_;
        if (!rollback_ok || !rollback_persistence_ok || state_transition_poisoned_) {
            derived_state_storage_valid_ = false;
            state_transition_poisoned_ = true;
            validation.error("chainstate-rollback-failed");
        }
        if (storage_) {
            storage_->abort_batch();
            if (rollback_ok && rollback_persistence_ok && !state_transition_poisoned_) {
                // A non-durable earlier commit deliberately leaves its journal
                // behind. A rejected later candidate must restore that marker
                // instead of clearing the older durability debt.
                const bool journal_ok = journal_preexisting
                    ? storage_->put_metadata("chainstate_journal", previous_journal)
                    : clear_state_journal();
                if (!journal_ok) {
                    GEO_LOG_WARN(
                        "chainstate",
                        "Failed to restore recovery journal after clean rollback: " << reason
                    );
                }
            }
        }
        return false;
    };

    // КРИТИЧНО: Консенсусная проверка coinbase награды (reward + fees).
    // Без этого возможна произвольная инфляция.
    Amount block_fees = 0;
    Amount expected_reward = 0;
    Amount interest_pool_delta = 0;

    if (!block.transactions.empty()) {
        // Fee checks run before state mutation, but must price every
        // transaction against the state its predecessors in this block leave.
        for (const Amount fee : project_block_transaction_fees(block.transactions, 1, applying_height)) {
            Amount next_fees = 0;
            if (fee < 0 || !checked_add_amount(block_fees, fee, next_fees)) {
                GEO_LOG_ERROR("chainstate", "Block fee total overflow");
                return invalid("bad-block-fee-total");
            }
            block_fees = next_fees;
        }

        expected_reward = consensus::ProofOfWork::calculate_block_reward(applying_height);

        consensus::ValidationState coinbase_state;
        if (!consensus::check_coinbase_outputs(
                block.transactions[0],
                applying_height,
                expected_reward,
                block_fees,
                get_foundation_address(),
                coinbase_state)) {
            GEO_LOG_ERROR(
                "chainstate",
                "Coinbase reward validation failed: " << coinbase_state.get_reject_reason()
            );
            validation = coinbase_state;
            return false;
        }

        if (consensus::is_foundation_reward_redirected_to_pool(applying_height)) {
            Amount total_reward = 0;
            if (!checked_add_amount(expected_reward, block_fees, total_reward)) {
                GEO_LOG_ERROR("chainstate", "Block reward total overflow");
                return invalid("bad-block-reward-total");
            }
            interest_pool_delta = consensus::calculate_foundation_reward_cut(total_reward);
            if (interest_pool_delta > 0 &&
                interest_pool_balance_ > std::numeric_limits<Amount>::max() - interest_pool_delta) {
                GEO_LOG_ERROR("chainstate", "Interest pool overflow while applying block");
                return invalid("bad-interest-pool-overflow");
            }
        }
    }

    // INTEREST_DISTRIBUTION: на квартальной высоте транзакция обязательна,
    // если есть корректно вычислимое распределение. Вне квартальной высоты запрещена.
    size_t interest_tx_count = 0;
    size_t interest_tx_index = 0;
    for (size_t i = 1; i < block.transactions.size(); ++i) {
        if (block.transactions[i].type == core::TransactionType::INTEREST_DISTRIBUTION) {
            interest_tx_count++;
            interest_tx_index = i;
        }
    }

    if (consensus::is_interest_distribution_due(applying_height)) {
        if (interest_tx_count != 0) {
            if (interest_tx_count != 1 || interest_tx_index != 1) {
                return invalid("bad-interest-distribution-position");
            }
            // apply_transaction validates the exact distribution once, after
            // coinbase. Outputs created at applying_height are ineligible, so
            // that calculation sees the same balances as the preceding tip.
        } else {
            std::vector<core::TxOutput> expected_outputs;
            Amount expected_total = 0;
            if (build_interest_distribution_outputs_(applying_height, expected_outputs, expected_total)) {
                return invalid("bad-interest-distribution-position");
            }
        }
    } else if (interest_tx_count != 0) {
        return invalid("bad-interest-distribution-height");
    }

    if (!is_genesis) {
        std::set<Hash256> seen_block_txids;
        for (size_t i = 1; i < block.transactions.size(); ++i) {
            const Hash256& tx_hash = txids[i];
            if (!seen_block_txids.insert(tx_hash).second) {
                GEO_LOG_ERROR("chainstate", "Duplicate non-coinbase transaction in block");
                return invalid("bad-txns-duplicate");
            }
            if (has_active_transaction(tx_hash)) {
                GEO_LOG_ERROR("chainstate", "Transaction already exists in active chain");
                return invalid("bad-txns-already-in-chain");
            }
        }
    }

    if (transition_in_progress_) {
        GEO_LOG_ERROR("chainstate", "Nested block state transition is not allowed");
        validation.error("chainstate-transition-already-active");
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
    try {
      if (storage_) {
        journal_preexisting = storage_->has_metadata("chainstate_journal");
        if (journal_preexisting &&
            !storage_->get_metadata("chainstate_journal", previous_journal)) {
            GEO_LOG_ERROR("chainstate", "Failed to preserve the existing chainstate journal");
            return false;
        }
        if (!write_state_journal(applying_height, block_hash, kJournalPhaseApplying)) {
            GEO_LOG_ERROR("chainstate", "Failed to write chainstate journal");
            return false;
        }
        journal_written = true;
        // Batch all state writes for this block. The recovery marker above is
        // durable and deliberately lives outside the batch.
        storage_->begin_batch();
      }
      mutation_started = true;

    // Применяем все транзакции
    for (size_t i = 0; i < block.transactions.size(); i++) {
        const auto& tx = block.transactions[i];
        bool is_coinbase = (i == 0 && tx.is_coinbase());

        // Создаем undo для транзакции
        TxUndo tx_undo;
        if (tx.type == core::TransactionType::ANGLE_COMMITMENT && angle_store_) {
            tx_undo.angle_store_preexisted = angle_store_->has_commitment(txids[i]);
        }

        // Сохраняем UTXO которые будут потрачены
        // ANGLE_COMMITMENT is the only multisig envelope without real UTXOs.
        // Delegation/revoke roots pay a small fee and therefore need undo data.
        if (!is_coinbase &&
            tx.type != core::TransactionType::ANGLE_COMMITMENT &&
            tx.type != core::TransactionType::SPEND_DEPOSIT &&
            tx.type != core::TransactionType::UNLOCK_DEPOSIT &&
            tx.type != core::TransactionType::ISSUE_SIDECHAIN &&
            tx.type != core::TransactionType::REGISTER_ACCOUNTING_SIDECHAIN &&
            tx.type != core::TransactionType::MINT_SIDECHAIN &&
            tx.type != core::TransactionType::REDEEM_SIDECHAIN &&
            tx.type != core::TransactionType::SIDECHAIN_CHECKPOINT &&
            tx.type != core::TransactionType::UPDATE_SIDECHAIN_COMMITTEE &&
            tx.type != core::TransactionType::SLASH_DEPOSIT &&
            tx.type != core::TransactionType::BATCH_SIDECHAIN_CHECKPOINT &&
            tx.type != core::TransactionType::INTEREST_DISTRIBUTION) {
            tx_undo.spent_utxos.reserve(tx.inputs.size());
            for (const auto& input : tx.inputs) {
                UTXO utxo;
                if (get_utxo(input.prev_tx_hash, input.vout, utxo)) {
                    tx_undo.spent_utxos.push_back(std::move(utxo));
                } else {
                    // UTXO не найден - откатываем блок
                    GEO_LOG_ERROR("chainstate", "UTXO not found for input, reverting block");
                    invalid("bad-txns-inputs-missing-or-spent");
                    return fail_with_rollback(i, "missing transaction input");
                }
            }
        }

        if (!is_coinbase) {
            prepare_nft_undo_(tx, tx_undo);
            prepare_sidechain_undo_(tx, tx_undo);
            prepare_foundation_undo_(tx, tx_undo);
            prepare_card_undo_(tx, tx_undo);
        }

        if (!is_coinbase) {
            std::set<std::string> seen_witness_pubkeys;
            for (const auto& w : tx.witnesses) {
                if (w.public_key.empty()) {
                    continue;
                }
                if (crypto::sha256(w.public_key) != w.public_key_hash) {
                    continue;
                }
                const std::string hex = hash256_to_hex(w.public_key_hash);
                if (seen_witness_pubkeys.insert(hex).second) {
                    tx_undo.added_witness_pubkeys.push_back(w.public_key_hash);
                }
            }
        }

        block_undo.tx_undos.push_back(std::move(tx_undo));

        // Применяем транзакцию
        consensus::ValidationState tx_validation;
        if (!apply_transaction(
                tx,
                applying_height,
                is_coinbase,
                &tx_validation,
                &txids[i],
                !is_genesis)) {
            validation = tx_validation;
            GEO_LOG_ERROR("chainstate", "Failed to apply transaction #" << i << ", reverting block");
            if (!state_transition_poisoned_ && !transition_persistence_ok_) {
                return fail_with_rollback(i + 1, "transaction persistence failure");
            }
            return fail_with_rollback(i, "transaction apply failure");
        }

        if (!is_coinbase) {
            register_witness_pubkeys_from_tx(tx);
            if (!transition_persistence_ok_) {
                return fail_with_rollback(i + 1, "witness persistence failure");
            }
        }
    }

    block_undo.interest_pool_delta = interest_pool_delta;

    // Сохраняем undo данные
    if (!save_block_undo(block_undo)) {
        GEO_LOG_ERROR("chainstate", "Failed to save undo data (cannot guarantee rollback)");
        return fail_with_rollback(block.transactions.size(), "undo persistence failure");
    }
    if (!prune_expired_undo_(applying_height)) {
        GEO_LOG_ERROR("chainstate", "Failed to prune expired undo data");
        return fail_with_rollback(block.transactions.size(), "undo pruning failure");
    }

    if (is_genesis) {
        persist_foundation_config_to_storage();
    }
    if (interest_pool_delta > 0 || is_genesis) {
        interest_pool_balance_ += interest_pool_delta;
        persist_interest_pool_balance_to_storage();
        if (!transition_persistence_ok_) {
            return fail_with_rollback(block.transactions.size(), "interest pool persistence failure");
        }
    }
    // Обновляем высоту
    if (!set_height(applying_height) || !set_tip_hash(block_hash) ||
        !persist_utxo_count_to_storage()) {
        return fail_with_rollback(block.transactions.size(), "chain position persistence failure");
    }

    // Сохраняем блок в хранилище.
    // В основном потоке Blockchain блок уже был записан на этапе accept_block().
    // Повторная запись здесь дублирует I/O и может ухудшать отзывчивость при майнинге.
    // Поэтому пишем только если блока еще нет (fallback для автономного использования ChainState).
    if (block_storage_) {
        if (!block_storage_->has_block(block_hash)) {
            // Для Genesis блока отключаем верификацию (он может быть premined/захардкожен)
            if (!block_storage_->save_block(block, applying_height, !is_genesis)) {
                GEO_LOG_ERROR("chainstate", "Failed to save block to BlockStorage");
                return fail_with_rollback(block.transactions.size(), "block persistence failure");
            }
        }

        // Кешируем witness для недавних блоков
        bool has_witness = false;
        for (const auto& tx : block.transactions) {
            for (const auto& w : tx.witnesses) {
                if (!w.signature.empty() || !w.public_key.empty()) {
                    has_witness = true;
                    break;
                }
            }
            if (has_witness) {
                break;
            }
        }
        if (has_witness) {
            cache_witness_payload(block_hash, storage::serialize_witness_payload(block));
        }


    } else {
        GEO_LOG_WARN("chainstate", "block_storage_ is null, block not saved to disk");
    }

    // Обновляем active tx-index только после успешного применения всех транзакций.
    // Для persistent storage запись идёт в текущий batch.
    active_tx_index_written = true;
    persist_active_tx_index_for_block(block, applying_height, block_undo.tx_undos, txids);
    if (!transition_persistence_ok_) {
        return fail_with_rollback(block.transactions.size(), "active transaction index persistence failure");
    }

    if (storage_) {
        if (!storage_->commit_batch()) {
            GEO_LOG_ERROR("chainstate", "Failed to commit chainstate batch");
            storage_->abort_batch();

            // commit_batch may have failed after a partial backend write. Apply a
            // compensating batch for the previous state; keep the journal unless
            // both the in-memory rollback and compensation succeed.
            storage_->begin_batch();
            transition_persistence_ok_ = true;
            const bool rollback_ok = rollback_applied_transactions(
                block.transactions.size(),
                "chainstate commit failure"
            );
            const bool position_ok = set_height(previous_height) &&
                set_tip_hash(previous_tip_hash) && persist_utxo_count_to_storage();
            persist_interest_pool_balance_to_storage();
            const bool undo_deleted = storage_->delete_metadata(
                "undo:" + hash256_to_hex(block_undo.block_hash)
            );
            const bool compensation_ok = rollback_ok && transition_persistence_ok_ &&
                position_ok && undo_deleted && storage_->commit_batch();
            if (!compensation_ok) {
                derived_state_storage_valid_ = false;
                state_transition_poisoned_ = true;
                storage_->abort_batch();
                GEO_LOG_ERROR("chainstate", "Failed to commit compensating rollback batch");
            }
            if (rollback_ok && compensation_ok && storage_->last_batch_commit_was_durable()) {
                const bool journal_ok = journal_preexisting
                    ? storage_->put_metadata("chainstate_journal", previous_journal)
                    : clear_state_journal();
                if (!journal_ok) {
                    GEO_LOG_WARN("chainstate", "Failed to restore journal after compensating rollback");
                }
            }
            return false;
        }

        if (storage_->last_batch_commit_was_durable() && !clear_state_journal()) {
            GEO_LOG_WARN("chainstate", "Failed to clear chainstate journal");
        }
    }

    // Persistent storage is the authority for disconnects. Keeping every
    // serialized undo object in RAM duplicates an ever-growing history.
    undo_log_.erase(hash256_to_hex(block_undo.block_hash));

    // ANGLE_COMMITMENT transactions are same-block authorization envelopes.
    // Their full transactions remain available through the persistent tx index,
    // while the verifier only needs the in-memory objects until the consuming
    // transaction has been checked.  Keeping every confirmed envelope here
    // would otherwise grow three redundant indexes for the lifetime of the
    // chain.  Do this only after the block is durably committed so every failure
    // path can still use the normal transaction rollback semantics.
    if (angle_store_) {
        for (size_t i = 0; i < block.transactions.size(); ++i) {
            if (block.transactions[i].type == core::TransactionType::ANGLE_COMMITMENT) {
                angle_store_->remove_commitment(txids[i]);
            }
        }
    }

    if (block_storage_ && !archive_mode_ && !is_genesis && prune_depth_ > 0 && applying_height > prune_depth_) {
        try {
            const Height prune_height = applying_height - prune_depth_;
            // Retry the oldest unfinished height and drain a bounded backlog.
            if (!prune_witness_up_to(prune_height, 8)) {
                GEO_LOG_WARN("chainstate", "Witness maintenance deferred before height " << prune_height);
            }
        } catch (const std::exception& error) {
            GEO_LOG_WARN("chainstate", "Deferred witness maintenance failed: " << error.what());
        }
    }

    validation.reset();
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
        validation.error("chainstate-transition-exception");
        GEO_LOG_ERROR("chainstate", "Block state transition threw: " << error.what());
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
        validation.error("chainstate-transition-exception");
        GEO_LOG_ERROR("chainstate", "Block state transition threw an unknown exception");
        return false;
    }
}

} // namespace geocoin::state
