#include "geocoin/state/state.hpp"
#include "geocoin/state/state_internal.hpp"
#include "geocoin/state/storage.hpp"
#include "geocoin/consensus/chainparams.hpp"
#include "geocoin/consensus/consensus.hpp"
#include "geocoin/consensus/proof_of_work.hpp"
#include "geocoin/consensus/sidechain_fee.hpp"
#include "geocoin/consensus/sidechain_checkpoint.hpp"
#include "geocoin/core/merkle.hpp"
#include "geocoin/crypto/addresses.hpp"
#include "geocoin/crypto/signatures.hpp"
#include "geocoin/utils/bytes.hpp"
#include "geocoin/utils/logger.hpp"
#include "geocoin/utils/u64_math.hpp"
#include "geocoin/utils/varint.hpp"
#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <iostream>
#include <set>
#include <random>
#include <sstream>
#include <iomanip>
#include <limits>

namespace geocoin::state {

namespace {
static bool is_feature_allowed(
    const utils::ActivationManifest& manifest,
    const core::Transaction& tx,
    Height evaluation_height
) {
    switch (tx.type) {
        case core::TransactionType::STANDARD:
        case core::TransactionType::ANGLE_COMMITMENT:
            return true;
        case core::TransactionType::MULTISIG_SPEND:
            if (is_hierarchical_multisig_spend(tx)) {
                return manifest.is_hierarchical_multisig_active(evaluation_height) &&
                       manifest.is_delegation_active(evaluation_height);
            }
            return manifest.is_standard_multisig_active(evaluation_height);
        case core::TransactionType::DELEGATION_PROOF:
        case core::TransactionType::REVOKE_DELEGATION:
            return manifest.is_delegation_active(evaluation_height);
        case core::TransactionType::NFT_CREATE:
        case core::TransactionType::NFT_TRANSFER:
        case core::TransactionType::NFT_BURN:
            return manifest.is_nft_active(evaluation_height);
        case core::TransactionType::CADASTRAL_DOC:
            return manifest.is_cadastral_active(evaluation_height);
        case core::TransactionType::LOCK_DEPOSIT:
        case core::TransactionType::TOP_UP_SIDECHAIN_DEPOSIT:
        case core::TransactionType::SPEND_DEPOSIT:
        case core::TransactionType::UNLOCK_DEPOSIT:
        case core::TransactionType::ISSUE_SIDECHAIN:
        case core::TransactionType::REGISTER_ACCOUNTING_SIDECHAIN:
        case core::TransactionType::MINT_SIDECHAIN:
        case core::TransactionType::REDEEM_SIDECHAIN:
        case core::TransactionType::SIDECHAIN_CHECKPOINT:
        case core::TransactionType::UPDATE_SIDECHAIN_COMMITTEE:
        case core::TransactionType::SLASH_DEPOSIT:
        case core::TransactionType::BATCH_SIDECHAIN_CHECKPOINT:
        case core::TransactionType::DECOMMISSION_SIDECHAIN:
            return manifest.is_sidechain_active(evaluation_height);
        case core::TransactionType::CARD_PROGRAM_DEFINE:
        case core::TransactionType::CARD_ISSUE:
        case core::TransactionType::CARD_UPDATE_STATUS:
        case core::TransactionType::CARD_UPDATE_POLICY:
        case core::TransactionType::UPDATE_FOUNDATION_ADDRESS:
        case core::TransactionType::INTEREST_DISTRIBUTION:
            return true;
        default:
            return false;
    }
}

}

bool ChainState::revert_transaction(const core::Transaction& tx, const TxUndo& tx_undo) {
    auto tx_hash = tx.get_hash();

    if (!revert_delegation_nonce_(tx)) {
        return false;
    }

    if (!tx_undo.added_witness_pubkeys.empty()) {
        for (const auto& h : tx_undo.added_witness_pubkeys) {
            const std::string hex = hash256_to_hex(h);
            const bool remove_pubkey = witness_refcount_decrement_(hex);
            if (!remove_pubkey) {
                continue;
            }
            witness_pubkey_cache_.erase(hex);
            auto lru_it = witness_pubkey_lru_index_.find(hex);
            if (lru_it != witness_pubkey_lru_index_.end()) {
                witness_pubkey_lru_.erase(lru_it->second);
                witness_pubkey_lru_index_.erase(lru_it);
            }
            delete_witness_pubkey_from_storage(h);
        }
    }

    if (!revert_nft_undo_(tx_undo)) {
        return false;
    }
    if (!revert_sidechain_undo_(tx_undo)) {
        return false;
    }
    if (!revert_card_undo_(tx_undo)) {
        return false;
    }
    if (!revert_foundation_undo_(tx_undo)) {
        return false;
    }
    if (!revert_multisig_indexes_(tx, tx_undo)) {
        return false;
    }
    if (!revert_interest_distribution_(tx)) {
        return false;
    }
    if (!revert_multisig_hierarchical_spent_(tx, tx_undo)) {
        return false;
    }
    if (const auto authorization = tx.get_protocol_authorization_hash()) {
        const auto key = hash256_to_hex(*authorization);
        protocol_authorization_keys_.erase(key);
        if (storage_ && !storage_->delete_metadata(idx_meta_key("pa", key))) return false;
    }

    // 1. Удаляем созданные UTXO
    for (uint32_t i = 0; i < tx.outputs.size(); i++) {
        if (!remove_utxo(tx_hash, i)) {
            GEO_LOG_ERROR("chainstate", "Failed to remove UTXO during revert");
            return false;
        }
    }

    // 2. Восстанавливаем потраченные UTXO (если не coinbase)
    if (!tx.is_coinbase()) {
        for (const auto& utxo : tx_undo.spent_utxos) {
            add_utxo(utxo);
        }
    }

    // The raw transaction store is normally only an acceleration index. A
    // reverted ordinary transaction must disappear from it, otherwise a
    // standalone ChainState (without BlockStorage) rejects the same valid
    // transaction after a clean reorg. Checkpoints are the deliberate
    // exception: conflicting orphaned checkpoint evidence remains available
    // to get_transaction_for_fraud_proof(), while public active-chain lookups
    // still use BlockStorage + active_tx_index_.
    const bool retain_checkpoint_evidence =
        tx.type == core::TransactionType::SIDECHAIN_CHECKPOINT ||
        tx.type == core::TransactionType::BATCH_SIDECHAIN_CHECKPOINT;
    if (storage_ && !retain_checkpoint_evidence) {
        bool deleted = false;
        try {
            deleted = storage_->delete_transaction(tx_hash);
        } catch (const std::exception& error) {
            GEO_LOG_ERROR(
                "chainstate",
                "Failed to remove reverted transaction from storage: " << error.what()
            );
        } catch (...) {
            GEO_LOG_ERROR("chainstate", "Failed to remove reverted transaction from storage");
        }
        if (!deleted) {
            return false;
        }
    }

    return true;
}

bool ChainState::apply_transaction(
    const core::Transaction& tx,
    Height height,
    bool is_coinbase,
    consensus::ValidationState* validation,
    const Hash256* known_txid,
    bool prevalidated
) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const bool owns_transition = !transition_in_progress_;
    if (owns_transition) {
        transition_in_progress_ = true;
        transition_persistence_ok_ = true;
    }
    struct TransitionScope {
        bool& in_progress;
        bool& persistence_ok;
        bool owner;
        ~TransitionScope() {
            if (owner) {
                in_progress = false;
                persistence_ok = true;
            }
        }
    } transition_scope{transition_in_progress_, transition_persistence_ok_, owns_transition};

    // Failures after validation can be caused by persistence; never infer an
    // invalid block from a failed state mutation alone.
    if (validation) {
        validation->error("transaction-state-transition-failed");
    }
    const auto finish = [this, validation, owns_transition, &tx](bool success) {
        if (success) {
            if (const auto authorization = tx.get_protocol_authorization_hash()) {
                const auto key = hash256_to_hex(*authorization);
                protocol_authorization_keys_.insert(key);
                if (storage_) {
                    record_persistence_result_(
                        storage_->put_metadata(idx_meta_key("pa", key), bytes{1}),
                        "persist protocol authorization");
                }
            }
        }
        if (success && !transition_persistence_ok_) {
            success = false;
        }
        if (!success && owns_transition && !transition_persistence_ok_) {
            // A standalone transaction does not have a BlockUndo envelope from
            // which every specialised index can be restored. Refuse further
            // use until the caller rebuilds from the active chain.
            derived_state_storage_valid_ = false;
            state_transition_poisoned_ = true;
        }
        if (success && validation) validation->reset();
        return success;
    };

    try {
      if (state_transition_poisoned_) {
        GEO_LOG_ERROR(
            "chainstate",
            "Cannot apply transaction: a failed state transition requires a rebuild"
        );
        return false;
    }

    // The caller selects a block position, but cannot turn a regular payment
    // into an input-free reward or route a typed transaction around validation.
    if (is_coinbase != tx.is_coinbase()) {
        if (validation) {
            validation->invalid(consensus::RejectCode::INVALID, "bad-cb-context");
        }
        return false;
    }
    if (is_coinbase) {
        consensus::ValidationState coinbase_validation;
        if ((!prevalidated && !consensus::check_transaction(tx, coinbase_validation)) ||
            !consensus::check_coinbase_height(tx, height, coinbase_validation)) {
            if (validation) *validation = coinbase_validation;
            return false;
        }
        if (tx.lock_time != 0 &&
            !consensus::is_final_transaction(tx, height, transaction_validation_time_())) {
            if (validation) validation->invalid(consensus::RejectCode::INVALID, "bad-txns-nonfinal");
            return false;
        }
    }

    // КРИТИЧНО: Полная консенсусная валидация каждой транзакции блока
    // Block::verify() вызывает tx.verify(), но MULTISIG_SPEND/DELEGATION_* проверяются
    // только через ChainState::validate_transaction() (MultisigVerifier).
    // Поэтому enforce'им validate_transaction здесь, до любых мутаций состояния.
    if (!is_coinbase) {
        // The activation manifest is operator-controlled local policy. It
        // may keep a transaction out of this node's mempool/RPC, but it must
        // never change validity of a block accepted from the network.
        if (!validate_transaction_impl(tx, false, height, known_txid, prevalidated)) {
            GEO_LOG_ERROR("chainstate", "apply_transaction: validate_transaction failed");
            if (validation) {
                validation->invalid(consensus::RejectCode::INVALID, "bad-txns-state-validation");
            }
            return false;
        }

        // Контекстные консенсусные проверки (anti-replay / uniqueness)
        consensus::ValidationState ctx_state;
        consensus::ContextualTxLookup lookup;
        lookup.get_transaction_by_txid = [this](const Hash256& hash, core::Transaction& out_tx) -> bool {
            auto tx_ptr = this->get_transaction(hash);
            if (!tx_ptr) {
                return false;
            }
            out_tx = *tx_ptr;
            return true;
        };
        lookup.has_angle_commitment_key = [this](const Hash256& key) -> bool {
            return this->has_angle_commitment_key(key);
        };
        lookup.has_multisig_spend_key = [this](const Hash256& key) -> bool {
            return this->has_multisig_spend_key(key);
        };
        lookup.has_hierarchical_spend_key = [this](const Hash256& key) -> bool {
            return this->has_hierarchical_spend_key(key);
        };
        lookup.has_delegation_proof_key = [this](const Hash256& key) -> bool {
            return this->has_delegation_proof_key(key);
        };
        lookup.has_revoke_delegation_key = [this](const Hash256& key) -> bool {
            return this->has_revoke_delegation_key(key);
        };

        lookup.has_nft_id_key = [this](const Hash256& key) -> bool {
            return this->has_nft_id_key(key);
        };

        lookup.has_sidechain_deposit_key = [this](const Hash256& key) -> bool {
            return this->has_sidechain_deposit_key(key);
        };

        lookup.has_sidechain_registry_key = [this](const Hash256& key) -> bool {
            return this->has_sidechain_registry_key(key);
        };
        lookup.signatures_verified =
            prevalidated ||
            tx.type == core::TransactionType::STANDARD ||
            tx.type == core::TransactionType::MULTISIG_SPEND ||
            tx.type == core::TransactionType::DELEGATION_PROOF ||
            tx.type == core::TransactionType::REVOKE_DELEGATION ||
            tx.type == core::TransactionType::UPDATE_FOUNDATION_ADDRESS;
        if (!consensus::contextual_check_transaction(tx, ctx_state, lookup)) {
            GEO_LOG_ERROR(
                "chainstate",
                "apply_transaction: contextual_check_transaction failed: " << ctx_state.get_reject_reason()
            );
            if (validation) *validation = ctx_state;
            return false;
        }
    }

    if (tx.type == core::TransactionType::ANGLE_COMMITMENT) {
        return finish(apply_angle_commitment_(tx, height));
    }
    if (tx.type == core::TransactionType::DELEGATION_PROOF) {
        return finish(apply_delegation_proof_(tx, height));
    }
    if (tx.type == core::TransactionType::REVOKE_DELEGATION) {
        return finish(apply_revoke_delegation_(tx, height));
    }
    if (core::is_nft_transition_type(tx.type)) {
        return finish(apply_nft_transaction_(tx, height));
    }
    if (core::is_sidechain_transition_type(tx.type)) {
        return finish(apply_sidechain_transaction_(tx, height));
    }
    if (tx.type == core::TransactionType::UPDATE_FOUNDATION_ADDRESS) {
        return finish(apply_foundation_transaction_(tx, height));
    }
    if (tx.type == core::TransactionType::INTEREST_DISTRIBUTION) {
        return finish(apply_interest_distribution_(tx, height));
    }

    Amount spent_input_total = 0;
    if (!apply_standard_utxo_effects_(tx, height, is_coinbase, validation, &spent_input_total)) {
        return false;
    }

    if (core::is_card_transition_type(tx.type)) {
        return finish(apply_card_transaction_(tx, height));
    }

    if (tx.type == core::TransactionType::MULTISIG_SPEND) {
        return finish(apply_multisig_spend_effects_(tx, spent_input_total));
    }

      return finish(true);
    } catch (const std::exception& error) {
        derived_state_storage_valid_ = false;
        state_transition_poisoned_ = true;
        transition_persistence_ok_ = false;
        if (validation) validation->error("transaction-state-transition-exception");
        GEO_LOG_ERROR("chainstate", "Transaction state transition threw: " << error.what());
        return false;
    } catch (...) {
        derived_state_storage_valid_ = false;
        state_transition_poisoned_ = true;
        transition_persistence_ok_ = false;
        if (validation) validation->error("transaction-state-transition-exception");
        GEO_LOG_ERROR("chainstate", "Transaction state transition threw an unknown exception");
        return false;
    }
}

Timestamp ChainState::transaction_validation_time_() const {
    return validation_block_time_ ? *validation_block_time_ :
        static_cast<Timestamp>(std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

bool ChainState::validate_transaction(const core::Transaction& tx) const {
    return validate_transaction_impl(tx, true);
}

bool ChainState::validate_transaction_impl(
    const core::Transaction& tx,
    bool enforce_local_feature_policy,
    std::optional<Height> applying_height,
    const Hash256* known_txid,
    bool syntax_already_checked
) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    if (state_transition_poisoned_) {
        GEO_LOG_ERROR(
            "chainstate",
            "Cannot validate transaction: a failed state transition requires a rebuild"
        );
        return false;
    }

    // Every public validation/application entry point enforces the same common
    // rules before hashing, signature checks or state-dependent dispatch.
    consensus::ValidationState common_validation;
    if (!syntax_already_checked && !consensus::check_transaction(tx, common_validation)) {
        return false;
    }

    // Coinbase has no spending signature, but its envelope is still validated.
    if (tx.is_coinbase()) {
        if (tx.lock_time != 0 && (current_height_ == (std::numeric_limits<Height>::max)() ||
            !consensus::is_final_transaction(tx, current_height_ + 1, transaction_validation_time_()))) return false;
        return true;
    }

    if (current_height_ == (std::numeric_limits<Height>::max)()) return false;
    const Height evaluation_height = applying_height ? *applying_height : current_height_ + 1;
    if (tx.lock_time != 0) {
        if (!consensus::is_final_transaction(tx, evaluation_height, transaction_validation_time_())) return false;
    }

    const Hash256 txid = known_txid != nullptr ? *known_txid : tx.get_hash();
    if (has_active_transaction(txid)) {
        return false;
    }
    if (const auto authorization = tx.get_protocol_authorization_hash();
        authorization && has_protocol_authorization_key(*authorization)) {
        return false;
    }

    if (enforce_local_feature_policy &&
        !is_feature_allowed(activation_manifest_, tx, evaluation_height)) {
        GEO_LOG_ERROR(
            "chainstate",
            "Transaction type disabled by local activation policy at height " << evaluation_height
        );
        return false;
    }

    // Typed transactions which consume ordinary UTXOs must prove both parts of
    // ownership: the witness signature must be valid for this transaction and
    // its public key must derive the address stored in the referenced UTXO.
    // Transaction::verify() alone cannot perform the latter check because it has
    // no access to the UTXO set.

    if (tx.type == core::TransactionType::ANGLE_COMMITMENT) {
        return validate_angle_commitment_(tx);
    }
    if (tx.type == core::TransactionType::DELEGATION_PROOF) {
        return validate_delegation_proof_(tx);
    }
    if (tx.type == core::TransactionType::REVOKE_DELEGATION) {
        return validate_revoke_delegation_(tx);
    }
    if (core::is_nft_transition_type(tx.type)) {
        return validate_nft_transaction_(tx);
    }
    if (core::is_card_transition_type(tx.type)) {
        return validate_card_transaction_(tx);
    }
    if (tx.type == core::TransactionType::UPDATE_FOUNDATION_ADDRESS) {
        return validate_foundation_transaction_(tx, evaluation_height, !applying_height.has_value());
    }
    if (tx.type == core::TransactionType::INTEREST_DISTRIBUTION) {
        return validate_interest_distribution_(tx);
    }
    if (core::is_sidechain_transition_type(tx.type)) {
        return validate_sidechain_transaction_(tx, !applying_height.has_value());
    }
    if (tx.type == core::TransactionType::MULTISIG_SPEND) {
        return validate_multisig_spend_(tx);
    }

    if (tx.type == core::TransactionType::CADASTRAL_DOC) {
        if (!nft_registry_.exists(tx.cadastral_nft_id)) return false;
        const auto parcel = nft_registry_.get_metadata(tx.cadastral_nft_id);
        const auto owner = nft_registry_.get_owner(tx.cadastral_nft_id);
        if (!parcel.cadastral || tx.cadastral_owner != owner ||
            (!tx.cadastral_number.empty() && tx.cadastral_number != parcel.cadastral->cadastral_number)) return false;
        bool owner_authorized = false;
        for (const auto& input : tx.inputs) {
            const auto* utxo = find_utxo_unlocked_(input.prev_tx_hash, input.vout);
            owner_authorized = owner_authorized || (utxo && utxo->address == owner);
        }
        if (!owner_authorized) return false;
    }
    // === STANDARD / CADASTRAL_DOC and other ordinary UTXO transactions ===
    return verify_owned_utxo_inputs_(tx);
}

} // namespace geocoin::state
