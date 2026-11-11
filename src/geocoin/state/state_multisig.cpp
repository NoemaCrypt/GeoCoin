#include "geocoin/state/state.hpp"
#include "geocoin/state/state_internal.hpp"

#include "geocoin/consensus/chainparams.hpp"
#include "geocoin/consensus/consensus.hpp"
#include "geocoin/utils/bytes.hpp"
#include "geocoin/utils/logger.hpp"
#include "geocoin/utils/u64_math.hpp"

#include <cstdlib>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>

namespace geocoin::state {
namespace {

inline bool SyncDebugEnabled() {
    static const bool enabled = (std::getenv("GEOCOIN_SYNC_DEBUG") != nullptr);
    return enabled;
}

bool calculate_delegated_debit(
    const core::Transaction& tx,
    const Address& primary,
    Amount total_input,
    Amount& debit
) {
    Amount total_output = 0;
    Amount returned_to_primary = 0;
    for (const auto& output : tx.outputs) {
        if (!checked_accumulate_nonnegative(output.value, total_output) ||
            (output.address.str() == primary.str() &&
             !checked_accumulate_nonnegative(output.value, returned_to_primary))) {
            return false;
        }
    }
    // Both payments and miner fees consume the primary wallet's authorization.
    return total_input >= total_output &&
           checked_sub_amount(total_input, returned_to_primary, debit) && debit >= 0;
}

} // namespace

bool ChainState::verify_multisig_control_fee_(
    const core::Transaction& tx,
    const Address& owner,
    Height evaluation_height
) const {
    if (owner.str().empty() || tx.inputs.empty()) {
        return false;
    }
    for (const auto& witness : tx.witnesses) {
        if (!utils::is_null_hash256(witness.public_key_hash) ||
            !witness.public_key.empty() || !witness.signature.empty()) {
            return false;
        }
    }

    Amount total_input = 0;
    std::set<std::pair<Hash256, uint32_t>> seen_inputs;
    for (const auto& input : tx.inputs) {
        if (!seen_inputs.emplace(input.prev_tx_hash, input.vout).second) {
            return false;
        }
        const auto* utxo = find_utxo_unlocked_(input.prev_tx_hash, input.vout);
        if (!utxo || utxo->address.str() != owner.str() || utxo->value < 0 ||
            evaluation_height < utxo->height ||
            !checked_accumulate_nonnegative(utxo->value, total_input)) {
            return false;
        }
        if (utxo->is_coinbase &&
            !consensus::is_coinbase_spend_mature(evaluation_height, utxo->height)) {
            return false;
        }
    }

    Amount total_output = 0;
    for (const auto& output : tx.outputs) {
        if (output.address.str() != owner.str() || output.value < 0 ||
            !checked_accumulate_nonnegative(output.value, total_output)) {
            return false;
        }
    }
    Amount fee = 0;
    return total_input >= total_output &&
           checked_sub_amount(total_input, total_output, fee) &&
           fee >= static_cast<Amount>(consensus::params::MIN_TX_FEE);
}

bool ChainState::apply_angle_commitment_(const core::Transaction& tx, Height height) {
    (void)height;
    const Hash256 tx_hash = tx.get_hash();
    if (!tx.inputs.empty()) {
        const std::string key = hash256_to_hex(tx.inputs[0].prev_tx_hash);
        angle_commitment_keys_.insert(key);
        if (storage_) {
            record_persistence_result_(
                storage_->put_metadata(idx_meta_key("ac", key), bytes{1}),
                "persist angle commitment index"
            );
        }
    }

    angle_store_->add_commitment(tx);
    if (SyncDebugEnabled()) {
        GEO_LOG_DEBUG("chainstate", "Added ANGLE_COMMITMENT to store");
    }
    if (storage_) {
        record_persistence_result_(
            storage_->put_transaction(tx_hash, tx),
            "persist angle commitment transaction"
        );
    }
    return true;
}

bool ChainState::apply_delegation_proof_(const core::Transaction& tx, Height height) {
    const auto tx_hash = tx.get_hash();
    if (!process_standard_utxo_effects_(tx, height, true, false)) {
        return false;
    }
    {
        const std::string key = hash256_to_hex(tx.get_delegation_proof_hash());
        delegation_proof_keys_.insert(key);
        if (storage_) {
            record_persistence_result_(
                storage_->put_metadata(idx_meta_key("dp", key), bytes(tx_hash.begin(), tx_hash.end())),
                "persist delegation proof index"
            );
        }
    }

    std::string primary_addr = tx.primary_multisig_address.str();
    used_delegation_nonces_[primary_addr].insert(tx.nonce);

    const auto txid = hash256_to_hex(tx_hash);
    delegations_[txid] = {tx.primary_multisig_address, tx.nonce, 0, tx.get_delegation_proof_hash()};
    persist_delegation_(txid);

    if (SyncDebugEnabled()) {
        GEO_LOG_DEBUG("chainstate", "Added DELEGATION_PROOF to blockchain (nonce=" << tx.nonce << ")");
    }
    if (storage_) {
        record_persistence_result_(
            storage_->put_transaction(tx_hash, tx),
            "persist delegation proof transaction"
        );
    }
    return true;
}

bool ChainState::apply_revoke_delegation_(const core::Transaction& tx, Height height) {
    const Hash256 tx_hash = tx.get_hash();
    if (!process_standard_utxo_effects_(tx, height, true, false)) {
        return false;
    }
    {
        const std::string key = hash256_to_hex(tx.revoked_delegation_proof_txid);
        revoked_delegations_cache_.insert(key);
        if (storage_) {
            record_persistence_result_(
                storage_->put_metadata(idx_meta_key("rd", key), bytes{1}),
                "persist delegation revocation index"
            );
        }
    }

    if (SyncDebugEnabled()) {
        GEO_LOG_DEBUG(
            "chainstate",
            "Added REVOKE_DELEGATION to blockchain (revoked: "
                << hash256_to_hex(tx.revoked_delegation_proof_txid).substr(0, 16) << "...)"
        );
    }
    if (storage_) {
        record_persistence_result_(
            storage_->put_transaction(tx_hash, tx),
            "persist delegation revocation transaction"
        );
    }
    return true;
}

bool ChainState::apply_multisig_spend_effects_(const core::Transaction& tx, Amount total_input) {
    if (tx.type != core::TransactionType::MULTISIG_SPEND) {
        return true;
    }

    const bool is_hierarchical = is_hierarchical_multisig_spend(tx);
    if (is_hierarchical) {
        const std::string key = hash256_to_hex(tx.get_hierarchical_spend_hash());
        hierarchical_spend_keys_.insert(key);
        if (storage_) {
            record_persistence_result_(
                storage_->put_metadata(idx_meta_key("hs", key), bytes{1}),
                "persist hierarchical spend index"
            );
        }
    } else {
        const std::string key = hash256_to_hex(tx.get_hash_without_commitments());
        multisig_spend_keys_.insert(key);
        if (storage_) {
            record_persistence_result_(
                storage_->put_metadata(idx_meta_key("ms", key), bytes{1}),
                "persist multisig spend index"
            );
        }
    }

    if (!is_hierarchical) {
        return true;
    }

    auto delegation_proof_tx = get_transaction(tx.delegation_proof_txid);
    if (!delegation_proof_tx) return false;

    Amount amount_spent = 0;
    if (!calculate_delegated_debit(
            tx, delegation_proof_tx->primary_multisig_address, total_input, amount_spent)) {
        GEO_LOG_ERROR("chainstate", "Invalid hierarchical debit while applying");
        return false;
    }

    const auto proof_id = hash256_to_hex(tx.delegation_proof_txid);
    const auto entry = delegations_.find(proof_id);
    if (entry == delegations_.end()) return false;
    Amount updated_total = 0;
    auto& spent_total = entry->second.total_spent;
    if (!checked_add_amount(spent_total, amount_spent, updated_total)) {
        GEO_LOG_ERROR("chainstate", "Hierarchical delegated total overflow while applying");
        return false;
    }
    spent_total = updated_total;
    if (!persist_delegation_(proof_id)) return false;

    if (SyncDebugEnabled()) {
        GEO_LOG_DEBUG(
            "chainstate",
            "Updated delegation total_spent: "
                << (spent_total / (double)COIN) << " GC"
        );
    }
    return true;
}

bool ChainState::validate_angle_commitment_(const core::Transaction& tx) const {
    return multisig_verifier_->verify_angle_commitment(tx);
}

bool ChainState::resolve_angle_commitments_(const std::vector<Hash256>& hashes) const {
    for (const auto& hash : hashes) {
        if (angle_store_->has_commitment(hash)) continue;
        const auto tx = get_transaction(hash);
        if (!tx || tx->type != core::TransactionType::ANGLE_COMMITMENT) return false;
        angle_store_->add_commitment(*tx);
    }
    return true;
}

bool ChainState::validate_delegation_proof_(const core::Transaction& tx) const {
    const Height evaluation_height = current_height_ + 1;
    std::string primary_addr = tx.primary_multisig_address.str();
    const auto used_nonces = used_delegation_nonces_.find(primary_addr);
    if (used_nonces != used_delegation_nonces_.end() && used_nonces->second.count(tx.nonce) > 0) {
        GEO_LOG_ERROR(
            "chainstate",
            "Nonce " << tx.nonce << " already used for " << primary_addr
        );
        return false;
    }

    return verify_multisig_control_fee_(tx, tx.primary_multisig_address, evaluation_height) &&
           resolve_angle_commitments_(tx.angle_commitment_hashes) &&
           multisig_verifier_->verify_delegation_proof(tx);
}

bool ChainState::validate_revoke_delegation_(const core::Transaction& tx) const {
    const Height evaluation_height = current_height_ + 1;
    auto revoked_proof = get_transaction(tx.revoked_delegation_proof_txid);
    if (!revoked_proof) {
        GEO_LOG_ERROR("chainstate", "Revoked delegation proof not found");
        return false;
    }

    if (revoked_proof->type != core::TransactionType::DELEGATION_PROOF) {
        GEO_LOG_ERROR("chainstate", "Revoked transaction is not a DELEGATION_PROOF");
        return false;
    }

    if (!tx.outputs.empty() &&
        tx.outputs.front().address.str() != revoked_proof->primary_multisig_address.str()) {
        return false;
    }

    return verify_multisig_control_fee_(
               tx,
               revoked_proof->primary_multisig_address,
               evaluation_height
           ) &&
           resolve_angle_commitments_(tx.angle_commitment_hashes) &&
           resolve_angle_commitments_(revoked_proof->angle_commitment_hashes) &&
           multisig_verifier_->verify_revoke_delegation(tx, *revoked_proof);
}

bool ChainState::validate_multisig_spend_(const core::Transaction& tx) const {
    if (!resolve_angle_commitments_(tx.angle_commitment_hashes)) return false;
    const bool is_hierarchical = is_hierarchical_multisig_spend(tx);

    if (is_hierarchical) {
        if (SyncDebugEnabled()) {
            GEO_LOG_DEBUG("chainstate", "Validating HIERARCHICAL_SPEND...");
        }

        const Timestamp eval_time = transaction_validation_time_();

        auto delegation_proof_tx = get_transaction(tx.delegation_proof_txid);
        if (!delegation_proof_tx) {
            GEO_LOG_ERROR("chainstate", "Delegation proof not found");
            return false;
        }

        if (is_delegation_revoked(tx.delegation_proof_txid)) {
            GEO_LOG_ERROR("chainstate", "Delegation proof has been revoked");
            return false;
        }

        const bool time_valid = validation_block_time_.has_value() ?
            consensus::is_delegated_spend_valid_at(tx, *delegation_proof_tx, eval_time) :
            consensus::is_delegation_active_at(*delegation_proof_tx, eval_time);
        if (!time_valid) {
            GEO_LOG_ERROR("chainstate", "Delegated spend is outside its valid time window");
            return false;
        }

        Amount total_input = 0;
        std::set<std::pair<Hash256, uint32_t>> seen_inputs;
        for (const auto& input : tx.inputs) {
            if (!seen_inputs.emplace(input.prev_tx_hash, input.vout).second) return false;
            const auto* utxo = find_utxo_unlocked_(input.prev_tx_hash, input.vout);
            if (!utxo) {
                GEO_LOG_DEBUG("chainstate", "UTXO not found for HIERARCHICAL_SPEND input");
                return false;
            }
            if (utxo->address.str() != delegation_proof_tx->primary_multisig_address.str()) {
                GEO_LOG_ERROR(
                    "chainstate",
                    "HIERARCHICAL_SPEND input does not belong to primary multisig address"
                );
                return false;
            }
            if (!checked_accumulate_nonnegative(utxo->value, total_input)) return false;
        }

        Amount amount_spent = 0;
        if (!calculate_delegated_debit(
                tx, delegation_proof_tx->primary_multisig_address, total_input, amount_spent)) {
            GEO_LOG_ERROR("chainstate", "Invalid HIERARCHICAL_SPEND debit");
            return false;
        }

        if (delegation_proof_tx->max_per_transaction > 0 &&
            amount_spent > delegation_proof_tx->max_per_transaction) {
            GEO_LOG_ERROR("chainstate", "HIERARCHICAL_SPEND exceeds max_per_transaction");
            return false;
        }

        if (delegation_proof_tx->max_total_delegated > 0) {
            const std::string dp_key = hash256_to_hex(tx.delegation_proof_txid);
            Amount already_spent = 0;
            auto it = delegations_.find(dp_key);
            if (it == delegations_.end()) return false;
            already_spent = it->second.total_spent;
            Amount updated_total = 0;
            if (!checked_add_amount(already_spent, amount_spent, updated_total) ||
                updated_total > delegation_proof_tx->max_total_delegated) {
                GEO_LOG_ERROR("chainstate", "HIERARCHICAL_SPEND exceeds max_total_delegated");
                return false;
            }
        }

        if (delegation_proof_tx->max_percent_of_balance > 0) {
            Amount primary_balance = get_balance(delegation_proof_tx->primary_multisig_address);
            if (primary_balance < 0 || delegation_proof_tx->max_percent_of_balance > 100) {
                return false;
            }
            const uint64_t percent_limit_u = utils::mul_div_u64_floor(
                static_cast<uint64_t>(primary_balance),
                static_cast<uint64_t>(delegation_proof_tx->max_percent_of_balance),
                100
            );
            if (percent_limit_u >
                static_cast<uint64_t>((std::numeric_limits<Amount>::max)())) {
                return false;
            }
            const Amount percent_limit = static_cast<Amount>(percent_limit_u);
            if (amount_spent > percent_limit) {
                GEO_LOG_ERROR("chainstate", "HIERARCHICAL_SPEND exceeds max_percent_of_balance");
                return false;
            }
        }

        return resolve_angle_commitments_(delegation_proof_tx->angle_commitment_hashes) &&
               multisig_verifier_->verify_hierarchical_spend(tx, *delegation_proof_tx);
    }

    if (tx.version != 2) {
        GEO_LOG_ERROR("chainstate", "Invalid tx.version for MULTISIG_SPEND");
        return false;
    }
    if (tx.spend_participants.size() != tx.n) {
        GEO_LOG_ERROR("chainstate", "MULTISIG_SPEND missing spend_participants or count != n");
        return false;
    }

    if (!tx.angle_commitment_hashes.empty()) {
        auto first_commitment = angle_store_->get_by_hash(tx.angle_commitment_hashes[0]);
        if (!first_commitment) {
            GEO_LOG_ERROR("chainstate", "First angle commitment not found");
            return false;
        }

        const Address& expected_multisig_address = first_commitment->multisig_address;
        for (const auto& input : tx.inputs) {
            const auto* utxo = find_utxo_unlocked_(input.prev_tx_hash, input.vout);
            if (!utxo) {
                GEO_LOG_DEBUG("chainstate", "UTXO not found for MULTISIG_SPEND input");
                return false;
            }
            if (utxo->address.str() != expected_multisig_address.str()) {
                GEO_LOG_ERROR(
                    "chainstate",
                    "MULTISIG_SPEND input does not belong to expected multisig address"
                );
                return false;
            }
        }
    }

    return multisig_verifier_->verify_multisig_spend(tx);
}

bool ChainState::revert_delegation_nonce_(const core::Transaction& tx) {
    if (tx.type != core::TransactionType::DELEGATION_PROOF) {
        return true;
    }
    const Hash256 tx_hash = tx.get_hash();
    std::string primary_addr = tx.primary_multisig_address.str();
    used_delegation_nonces_[primary_addr].erase(tx.nonce);

    const auto txid = hash256_to_hex(tx_hash);
    delegations_.erase(txid);
    return !storage_ || record_persistence_result_(storage_->delete_metadata("delegation:" + txid),
                                                   "delete delegation state");
}

bool ChainState::revert_multisig_indexes_(const core::Transaction& tx, const TxUndo& tx_undo) {
    const Hash256 tx_hash = tx.get_hash();
    if (tx.type == core::TransactionType::REVOKE_DELEGATION) {
        const std::string key = hash256_to_hex(tx.revoked_delegation_proof_txid);
        revoked_delegations_cache_.erase(key);
        if (storage_) {
            record_persistence_result_(
                storage_->delete_metadata(idx_meta_key("rd", key)),
                "delete delegation revocation index"
            );
        }
    }

    if (tx.type == core::TransactionType::ANGLE_COMMITMENT) {
        if (angle_store_ && !tx_undo.angle_store_preexisted) {
            angle_store_->remove_commitment(tx_hash);
        }
        if (!tx.inputs.empty()) {
            const std::string key = hash256_to_hex(tx.inputs[0].prev_tx_hash);
            angle_commitment_keys_.erase(key);
            if (storage_) {
                record_persistence_result_(
                    storage_->delete_metadata(idx_meta_key("ac", key)),
                    "delete angle commitment index"
                );
            }
        }
    }

    if (tx.type == core::TransactionType::MULTISIG_SPEND) {
        if (is_hierarchical_multisig_spend(tx)) {
            const std::string key = hash256_to_hex(tx.get_hierarchical_spend_hash());
            hierarchical_spend_keys_.erase(key);
            if (storage_) {
                record_persistence_result_(
                    storage_->delete_metadata(idx_meta_key("hs", key)),
                    "delete hierarchical spend index"
                );
            }
        } else {
            const std::string key = hash256_to_hex(tx.get_hash_without_commitments());
            multisig_spend_keys_.erase(key);
            if (storage_) {
                record_persistence_result_(
                    storage_->delete_metadata(idx_meta_key("ms", key)),
                    "delete multisig spend index"
                );
            }
        }
    }

    if (tx.type == core::TransactionType::DELEGATION_PROOF) {
        const std::string key = hash256_to_hex(tx.get_delegation_proof_hash());
        delegation_proof_keys_.erase(key);
        if (storage_) {
            record_persistence_result_(
                storage_->delete_metadata(idx_meta_key("dp", key)),
                "delete delegation proof index"
            );
        }
    }
    return true;
}

bool ChainState::revert_multisig_hierarchical_spent_(const core::Transaction& tx, const TxUndo& undo) {
    if (!is_hierarchical_multisig_spend(tx)) {
        return true;
    }

    auto delegation_proof_tx = get_transaction(tx.delegation_proof_txid);
    if (!delegation_proof_tx) return false;

    // Spent UTXOs are restored later. Undo carries the original values even
    // when the historical funding transaction has already been pruned.
    if (undo.spent_utxos.size() != tx.inputs.size()) return false;
    Amount total_input = 0;
    for (size_t i = 0; i < tx.inputs.size(); ++i) {
        const auto& utxo = undo.spent_utxos[i];
        const auto& input = tx.inputs[i];
        if (utxo.tx_hash != input.prev_tx_hash || utxo.vout != input.vout ||
            utxo.address.str() != delegation_proof_tx->primary_multisig_address.str() ||
            !checked_accumulate_nonnegative(utxo.value, total_input)) return false;
    }
    Amount amount_spent = 0;
    if (!calculate_delegated_debit(
            tx, delegation_proof_tx->primary_multisig_address, total_input, amount_spent)) {
        GEO_LOG_ERROR("chainstate", "Invalid hierarchical debit while reverting");
        return false;
    }

    const auto proof_id = hash256_to_hex(tx.delegation_proof_txid);
    const auto entry = delegations_.find(proof_id);
    if (entry == delegations_.end()) return false;
    auto& spent_total = entry->second.total_spent;
    if (spent_total < amount_spent) {
        GEO_LOG_ERROR("chainstate", "Hierarchical delegated total underflow while reverting");
        return false;
    }
    spent_total -= amount_spent;
    return persist_delegation_(proof_id);
}

} // namespace geocoin::state
