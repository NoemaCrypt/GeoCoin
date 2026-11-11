#include "geocoin/consensus/consensus_internal.hpp"

#include "geocoin/consensus/chainparams.hpp"
#include "geocoin/consensus/consensus.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <utility>

namespace geocoin::consensus {

bool is_delegation_active_at(const core::Transaction& proof, Timestamp time) {
    return proof.type == core::TransactionType::DELEGATION_PROOF &&
           time >= proof.valid_from && (proof.valid_until == 0 || time <= proof.valid_until);
}

bool is_delegated_spend_valid_at(
    const core::Transaction& spend,
    const core::Transaction& proof,
    Timestamp block_time
) {
    const Timestamp difference = spend.timestamp > block_time ?
        spend.timestamp - block_time : block_time - spend.timestamp;
    return is_delegation_active_at(proof, block_time) && difference <= params::MAX_FUTURE_BLOCK_TIME;
}

bool check_multisig_transaction(
    const core::Transaction& tx,
    ValidationState& state,
    bool& complete
) {
    complete = false;

    if (tx.type == core::TransactionType::ANGLE_COMMITMENT) {
        if (tx.commitment_network_id != Params().network_id()) {
            return state.invalid(RejectCode::INVALID, "bad-angle-commitment-network",
                "ANGLE_COMMITMENT belongs to another network");
        }

        if (tx.inputs.size() != 1) {
            return state.invalid(RejectCode::INVALID, "bad-angle-commitment",
                "ANGLE_COMMITMENT must have exactly 1 input");
        }

        if (tx.inputs[0].vout != 0xFFFFFFFE) {
            return state.invalid(RejectCode::INVALID, "bad-angle-commitment",
                "ANGLE_COMMITMENT has invalid vout marker");
        }

        if (!tx.outputs.empty()) {
            return state.invalid(RejectCode::INVALID, "bad-angle-commitment",
                "ANGLE_COMMITMENT must have no outputs");
        }

        if (tx.timestamp != 0 || tx.lock_time != 0 ||
            !tx.inputs[0].script_sig.empty() ||
            tx.inputs[0].sequence != 0xFFFFFFFFu) {
            return state.invalid(RejectCode::INVALID, "bad-angle-commitment",
                "ANGLE_COMMITMENT has non-canonical fields");
        }
        for (const auto& witness : tx.witnesses) {
            const bool has_key_hash = std::any_of(
                witness.public_key_hash.begin(),
                witness.public_key_hash.end(),
                [](byte value) { return value != 0; }
            );
            if (has_key_hash || !witness.public_key.empty() || !witness.signature.empty()) {
                return state.invalid(RejectCode::INVALID, "bad-angle-commitment",
                    "ANGLE_COMMITMENT must not carry an input witness");
            }
        }

        if (tx.multisig_address.str().empty()) {
            return state.invalid(RejectCode::INVALID, "bad-angle-commitment",
                "ANGLE_COMMITMENT has empty multisig address");
        }

        bool is_null_hash = true;
        for (auto byte : tx.spending_tx_hash) {
            if (byte != 0) {
                is_null_hash = false;
                break;
            }
        }
        if (is_null_hash) {
            return state.invalid(RejectCode::INVALID, "bad-angle-commitment",
                "ANGLE_COMMITMENT has null spending_tx_hash");
        }

        if (tx.signer_address.str().empty()) {
            return state.invalid(RejectCode::INVALID, "bad-angle-commitment",
                "ANGLE_COMMITMENT has empty signer address");
        }

        constexpr size_t kFalconSigMin = 4 + 2 + 2 + 1 + 1;
        constexpr size_t kFalconSigMax = 4096;
        if (tx.inputs[0].signature.size() < kFalconSigMin ||
            tx.inputs[0].signature.size() > kFalconSigMax) {
            return state.invalid(RejectCode::INVALID, "bad-angle-commitment",
                "ANGLE_COMMITMENT has invalid signature size");
        }

        Hash256 expected_intent{};
        try {
            expected_intent = tx.get_angle_commitment_message_hash();
        } catch (...) {
            return state.invalid(RejectCode::INVALID, "bad-angle-commitment",
                "ANGLE_COMMITMENT intent cannot be encoded");
        }
        if (tx.inputs[0].prev_tx_hash != expected_intent) {
            return state.invalid(RejectCode::INVALID, "bad-angle-commitment",
                "ANGLE_COMMITMENT anti-replay key does not match signed intent");
        }

        complete = true;
        return true;
    }

    if (tx.type == core::TransactionType::MULTISIG_SPEND) {
        if (tx.inputs.empty()) {
            return state.invalid(RejectCode::INVALID, "bad-multisig-spend",
                "MULTISIG_SPEND must have inputs");
        }

        if (tx.outputs.empty()) {
            return state.invalid(RejectCode::INVALID, "bad-multisig-spend",
                "MULTISIG_SPEND must have outputs");
        }

        if (tx.m == 0 || tx.n == 0 || tx.m > tx.n ||
            tx.n > params::MAX_MULTISIG_PARTICIPANTS) {
            return state.invalid(RejectCode::INVALID, "bad-multisig-spend",
                "MULTISIG_SPEND has invalid M-of-N configuration");
        }

        if (tx.angle_commitment_hashes.size() < tx.m ||
            tx.angle_commitment_hashes.size() > tx.n) {
            return state.invalid(RejectCode::INVALID, "bad-multisig-spend",
                "MULTISIG_SPEND has invalid angle commitment count");
        }
        if (std::set<Hash256>(
                tx.angle_commitment_hashes.begin(),
                tx.angle_commitment_hashes.end()).size() != tx.angle_commitment_hashes.size()) {
            return state.invalid(RejectCode::INVALID, "bad-multisig-spend",
                "MULTISIG_SPEND has duplicate angle commitments");
        }
        const bool hierarchical_spend = std::any_of(
            tx.delegation_proof_txid.begin(),
            tx.delegation_proof_txid.end(),
            [](byte value) { return value != 0; }
        );
        if ((!hierarchical_spend && tx.spend_participants.size() != tx.n) ||
            (hierarchical_spend && !tx.spend_participants.empty())) {
            return state.invalid(RejectCode::INVALID, "bad-multisig-spend",
                "MULTISIG_SPEND has a non-canonical participant list");
        }

        return true;
    }

    if (tx.type == core::TransactionType::DELEGATION_PROOF) {
        if (tx.inputs.empty()) {
            return state.invalid(RejectCode::INVALID, "bad-delegation-proof",
                "DELEGATION_PROOF must pay its fee from primary multisig UTXOs");
        }

        if (tx.outputs.size() > 1 || tx.lock_time != 0) {
            return state.invalid(RejectCode::INVALID, "bad-delegation-proof",
                "DELEGATION_PROOF has non-canonical fee fields");
        }
        for (const auto& witness : tx.witnesses) {
            const bool has_key_hash = std::any_of(
                witness.public_key_hash.begin(), witness.public_key_hash.end(),
                [](byte value) { return value != 0; });
            if (has_key_hash || !witness.public_key.empty() || !witness.signature.empty()) {
                return state.invalid(RejectCode::INVALID, "bad-delegation-proof",
                    "DELEGATION_PROOF must not carry ordinary input witnesses");
            }
        }
        std::set<std::pair<Hash256, uint32_t>> delegation_inputs;
        for (const auto& input : tx.inputs) {
            const bool null_prevout = std::all_of(
                input.prev_tx_hash.begin(), input.prev_tx_hash.end(),
                [](byte value) { return value == 0; });
            const bool nonzero_signature_hash = std::any_of(
                input.signature_hash.begin(), input.signature_hash.end(),
                [](byte value) { return value != 0; });
            if (null_prevout || input.vout >= 0xFFFFFFFCu ||
                !input.script_sig.empty() || input.sequence != 0xFFFFFFFFu ||
                !input.signature.empty() || nonzero_signature_hash ||
                !delegation_inputs.emplace(input.prev_tx_hash, input.vout).second) {
                return state.invalid(RejectCode::INVALID, "bad-delegation-proof",
                    "DELEGATION_PROOF has a non-canonical or duplicate fee input");
            }
        }
        if (!tx.outputs.empty() &&
            (tx.outputs[0].value < static_cast<Amount>(params::DUST_LIMIT) ||
             tx.outputs[0].address.str() != tx.primary_multisig_address.str() ||
             !tx.outputs[0].script_pubkey.empty())) {
            return state.invalid(RejectCode::INVALID, "bad-delegation-proof",
                "DELEGATION_PROOF change must return to the primary multisig address");
        }

        if (tx.primary_m == 0 || tx.primary_n == 0 ||
            tx.primary_m > tx.primary_n ||
            tx.primary_n > params::MAX_MULTISIG_PARTICIPANTS ||
            tx.delegated_m == 0 || tx.delegated_n == 0 ||
            tx.delegated_m > tx.delegated_n ||
            tx.delegated_n > params::MAX_MULTISIG_PARTICIPANTS ||
            tx.primary_participants.size() != tx.primary_n ||
            tx.delegated_participants.size() != tx.delegated_n) {
            return state.invalid(RejectCode::INVALID, "bad-delegation-proof",
                "DELEGATION_PROOF has invalid M-of-N configuration");
        }
        if (tx.angle_commitment_hashes.size() < tx.primary_m ||
            tx.angle_commitment_hashes.size() > tx.primary_n ||
            std::set<Hash256>(
                tx.angle_commitment_hashes.begin(),
                tx.angle_commitment_hashes.end()).size() != tx.angle_commitment_hashes.size()) {
            return state.invalid(RejectCode::INVALID, "bad-delegation-proof",
                "DELEGATION_PROOF has invalid angle commitments");
        }

        if (tx.primary_multisig_address.str().empty()) {
            return state.invalid(RejectCode::INVALID, "bad-delegation-proof",
                "DELEGATION_PROOF has empty primary multisig address");
        }

        if (tx.delegated_multisig_address.str().empty()) {
            return state.invalid(RejectCode::INVALID, "bad-delegation-proof",
                "DELEGATION_PROOF has empty delegated multisig address");
        }

        if (tx.max_total_delegated <= 0) {
            return state.invalid(RejectCode::INVALID, "bad-delegation-proof",
                "DELEGATION_PROOF has invalid max_total_delegated");
        }
        if (tx.max_per_transaction < 0 ||
            (tx.max_per_transaction > 0 &&
             tx.max_per_transaction > tx.max_total_delegated)) {
            return state.invalid(RejectCode::INVALID, "bad-delegation-proof",
                "DELEGATION_PROOF has invalid max_per_transaction");
        }
        if (tx.max_percent_of_balance > 100) {
            return state.invalid(RejectCode::INVALID, "bad-delegation-proof",
                "DELEGATION_PROOF percentage exceeds 100");
        }
        if (tx.valid_until != 0 && tx.valid_from > tx.valid_until) {
            return state.invalid(RejectCode::INVALID, "bad-delegation-proof",
                "DELEGATION_PROOF has invalid validity interval");
        }

        complete = true;
        return true;
    }

    if (tx.type == core::TransactionType::REVOKE_DELEGATION) {
        if (tx.inputs.empty()) {
            return state.invalid(RejectCode::INVALID, "bad-revoke-delegation",
                "REVOKE_DELEGATION must pay its fee from primary multisig UTXOs");
        }

        if (tx.outputs.size() > 1 || tx.lock_time != 0) {
            return state.invalid(RejectCode::INVALID, "bad-revoke-delegation",
                "REVOKE_DELEGATION has non-canonical fee fields");
        }
        for (const auto& witness : tx.witnesses) {
            const bool has_key_hash = std::any_of(
                witness.public_key_hash.begin(), witness.public_key_hash.end(),
                [](byte value) { return value != 0; });
            if (has_key_hash || !witness.public_key.empty() || !witness.signature.empty()) {
                return state.invalid(RejectCode::INVALID, "bad-revoke-delegation",
                    "REVOKE_DELEGATION must not carry ordinary input witnesses");
            }
        }
        std::set<std::pair<Hash256, uint32_t>> revoke_inputs;
        for (const auto& input : tx.inputs) {
            const bool null_prevout = std::all_of(
                input.prev_tx_hash.begin(), input.prev_tx_hash.end(),
                [](byte value) { return value == 0; });
            const bool nonzero_signature_hash = std::any_of(
                input.signature_hash.begin(), input.signature_hash.end(),
                [](byte value) { return value != 0; });
            if (null_prevout || input.vout >= 0xFFFFFFFCu ||
                !input.script_sig.empty() || input.sequence != 0xFFFFFFFFu ||
                !input.signature.empty() || nonzero_signature_hash ||
                !revoke_inputs.emplace(input.prev_tx_hash, input.vout).second) {
                return state.invalid(RejectCode::INVALID, "bad-revoke-delegation",
                    "REVOKE_DELEGATION has a non-canonical or duplicate fee input");
            }
        }

        bool is_null_hash = true;
        for (auto byte : tx.revoked_delegation_proof_txid) {
            if (byte != 0) {
                is_null_hash = false;
                break;
            }
        }
        if (is_null_hash) {
            return state.invalid(RejectCode::INVALID, "bad-revoke-delegation",
                "REVOKE_DELEGATION has null revoked_delegation_proof_txid");
        }

        if (tx.angle_commitment_hashes.empty() ||
            tx.angle_commitment_hashes.size() > params::MAX_ANGLE_COMMITMENTS ||
            std::set<Hash256>(
                tx.angle_commitment_hashes.begin(),
                tx.angle_commitment_hashes.end()).size() != tx.angle_commitment_hashes.size()) {
            return state.invalid(RejectCode::INVALID, "bad-revoke-delegation",
                "REVOKE_DELEGATION has invalid angle commitments");
        }

        complete = true;
        return true;
    }

    return true;
}

bool contextual_check_multisig_transaction(
    const core::Transaction& tx,
    ValidationState& state,
    const ContextualTxLookup& lookup
) {
    if (tx.type == core::TransactionType::ANGLE_COMMITMENT) {
        if (tx.inputs.empty()) {
            return state.invalid(RejectCode::INVALID, "bad-angle-commitment-context",
                "ANGLE_COMMITMENT has no inputs");
        }

        const Hash256& prev_tx_hash = tx.inputs[0].prev_tx_hash;
        if (lookup.has_angle_commitment_key && lookup.has_angle_commitment_key(prev_tx_hash)) {
            return state.invalid(RejectCode::INVALID, "bad-angle-commitment-duplicate",
                "ANGLE_COMMITMENT already exists for this spending_tx and signer");
        }

        const Hash256& spending_tx_hash = tx.spending_tx_hash;
        if (lookup.has_multisig_spend_key && lookup.has_multisig_spend_key(spending_tx_hash)) {
            return state.invalid(RejectCode::INVALID, "bad-angle-commitment-spent",
                "ANGLE_COMMITMENT for already executed spending transaction");
        }

        return true;
    }

    if (tx.type == core::TransactionType::MULTISIG_SPEND) {
        bool is_hierarchical = false;
        for (auto byte : tx.delegation_proof_txid) {
            if (byte != 0) {
                is_hierarchical = true;
                break;
            }
        }

        if (is_hierarchical) {
            Hash256 hierarchical_key = tx.get_hierarchical_spend_hash();
            if (lookup.has_hierarchical_spend_key && lookup.has_hierarchical_spend_key(hierarchical_key)) {
                return state.invalid(RejectCode::INVALID, "bad-hierarchical-spend-duplicate",
                    "HIERARCHICAL_SPEND with this spend hash already exists");
            }
        } else {
            Hash256 spending_tx_hash = tx.get_hash_without_commitments();
            if (lookup.has_multisig_spend_key && lookup.has_multisig_spend_key(spending_tx_hash)) {
                return state.invalid(RejectCode::INVALID, "bad-multisig-spend-duplicate",
                    "MULTISIG_SPEND with this spending_tx_hash already exists");
            }
        }

        return true;
    }

    if (tx.type == core::TransactionType::DELEGATION_PROOF) {
        Hash256 delegation_hash = tx.get_delegation_proof_hash();
        if (lookup.has_delegation_proof_key && lookup.has_delegation_proof_key(delegation_hash)) {
            return state.invalid(RejectCode::INVALID, "bad-delegation-proof-duplicate",
                "DELEGATION_PROOF with this delegation_hash already exists");
        }

        return true;
    }

    if (tx.type == core::TransactionType::REVOKE_DELEGATION) {
        const Hash256& delegation_txid = tx.revoked_delegation_proof_txid;
        core::Transaction delegation_tx;

        if (!lookup.get_transaction_by_txid || !lookup.get_transaction_by_txid(delegation_txid, delegation_tx)) {
            return state.invalid(RejectCode::INVALID, "bad-revoke-delegation-notfound",
                "REVOKE_DELEGATION references non-existent delegation");
        }

        if (delegation_tx.type != core::TransactionType::DELEGATION_PROOF) {
            return state.invalid(RejectCode::INVALID, "bad-revoke-delegation-type",
                "REVOKE_DELEGATION references non-delegation transaction");
        }

        if (lookup.has_revoke_delegation_key && lookup.has_revoke_delegation_key(delegation_txid)) {
            return state.invalid(RejectCode::INVALID, "bad-revoke-delegation-duplicate",
                "REVOKE_DELEGATION already exists for this delegation");
        }

        return true;
    }

    return true;
}

bool check_block_angle_package(const core::Block& block, ValidationState& state) {
    const auto consumes_angle_commitments = [](const core::Transaction& tx) {
        return tx.type == core::TransactionType::MULTISIG_SPEND ||
               tx.type == core::TransactionType::DELEGATION_PROOF ||
               tx.type == core::TransactionType::REVOKE_DELEGATION;
    };

    // ANGLE_COMMITMENT is an ephemeral signature envelope, not an independent
    // piece of chain state.  Requiring it immediately before one consuming root
    // transaction prevents free permanent chain bloat and makes a single root
    // fee account for the complete package.  Pre-launch consensus deliberately
    // does not accept commitments confirmed by an older block.
    std::map<Hash256, size_t> commitment_positions;
    std::set<Hash256> consumed_commitments;
    for (size_t i = 1; i < block.transactions.size(); ++i) {
        const auto& tx = block.transactions[i];
        if (tx.type == core::TransactionType::ANGLE_COMMITMENT) {
            const Hash256 hash = tx.get_hash();
            if (!commitment_positions.emplace(hash, i).second) {
                return state.invalid(
                    RejectCode::INVALID,
                    "bad-angle-package-duplicate",
                    "duplicate ANGLE_COMMITMENT in block"
                );
            }
            continue;
        }
        if (!consumes_angle_commitments(tx)) {
            continue;
        }

        for (const auto& commitment_hash : tx.angle_commitment_hashes) {
            const auto position = commitment_positions.find(commitment_hash);
            if (position == commitment_positions.end() || position->second >= i) {
                return state.invalid(
                    RejectCode::INVALID,
                    "bad-angle-package-order",
                    "commitment must precede its consumer in the same block"
                );
            }
            if (!consumed_commitments.insert(commitment_hash).second) {
                return state.invalid(
                    RejectCode::INVALID,
                    "bad-angle-package-reuse",
                    "commitment cannot authorize more than one transaction"
                );
            }
        }
    }
    if (consumed_commitments.size() != commitment_positions.size()) {
        return state.invalid(
            RejectCode::INVALID,
            "bad-angle-package-orphan",
            "standalone ANGLE_COMMITMENT is not allowed in a block"
        );
    }

    return true;
}

} // namespace geocoin::consensus
