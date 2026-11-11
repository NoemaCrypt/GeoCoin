#include "geocoin/consensus/consensus_internal.hpp"

#include "geocoin/consensus/consensus.hpp"
#include "geocoin/consensus/sidechain_rules.hpp"
#include "geocoin/crypto/addresses.hpp"
#include "geocoin/crypto/geo_area.hpp"
#include "geocoin/utils/bytes.hpp"
#include "geocoin/utils/u64_math.hpp"
#include "geocoin/utils/varint.hpp"

#include <set>
#include <utility>

namespace geocoin::consensus {

bool check_sidechain_transaction(
    const core::Transaction& tx,
    ValidationState& state,
    bool& complete
) {
    complete = false;
    if (tx.type == core::TransactionType::BATCH_SIDECHAIN_CHECKPOINT) {
        if (!tx.inputs.empty()) {
            return state.invalid(RejectCode::INVALID, "bad-sidechain-batch", "BATCH_SIDECHAIN_CHECKPOINT must have no inputs");
        }
        if (!tx.outputs.empty()) {
            return state.invalid(RejectCode::INVALID, "bad-sidechain-batch", "BATCH_SIDECHAIN_CHECKPOINT must have no outputs");
        }
        if (tx.sidechain_batch_checkpoints.empty() ||
            tx.sidechain_batch_checkpoints.size() > params::MAX_BATCH_SIDECHAIN_CHECKPOINTS) {
            return state.invalid(RejectCode::INVALID, "bad-sidechain-batch", "Invalid batch size");
        }
        std::set<Hash256> uniq;
        for (const auto& item : tx.sidechain_batch_checkpoints) {
            bool null_sc = true;
            for (auto b : item.sidechain_id) { if (b != 0) { null_sc = false; break; } }
            bool null_cp = true;
            for (auto b : item.checkpoint) { if (b != 0) { null_cp = false; break; } }
            bool null_committee = true;
            for (auto b : item.committee_id) { if (b != 0) { null_committee = false; break; } }
            if (null_sc || null_cp || null_committee) {
                return state.invalid(RejectCode::INVALID, "bad-sidechain-batch", "Invalid item fields");
            }
            if (item.checkpoint_height == 0 || item.fee == 0) {
                return state.invalid(RejectCode::INVALID, "bad-sidechain-batch", "Invalid item height/fee");
            }
            if (item.checkpoint_height > params::MAX_SIDECHAIN_CHECKPOINT_HEIGHT) {
                return state.invalid(RejectCode::INVALID, "bad-sidechain-batch", "checkpoint_height too large");
            }
            if (item.version != params::FINANCIAL_CHECKPOINT_VERSION &&
                item.version != params::ACCOUNTING_CHECKPOINT_VERSION) {
                return state.invalid(RejectCode::INVALID, "bad-sidechain-batch", "Unsupported checkpoint version");
            }
            if (item.proof.empty()) {
                return state.invalid(RejectCode::INVALID, "bad-sidechain-batch", "Checkpoint proof is required");
            }
            if (item.proof.size() > params::MAX_BATCH_CHECKPOINT_PROOF) {
                return state.invalid(RejectCode::INVALID, "bad-sidechain-batch", "Proof too large");
            }
            if (item.committee_signatures.empty() ||
                item.committee_signatures.size() > params::MAX_SIDECHAIN_COMMITTEE_SIGNATURES) {
                return state.invalid(RejectCode::INVALID, "bad-sidechain-batch", "Invalid committee signatures");
            }
            if (!uniq.insert(item.sidechain_id).second) {
                return state.invalid(RejectCode::INVALID, "bad-sidechain-batch", "Duplicate sidechain_id in batch");
            }
        }
        complete = true;
        return true;
    }

        // Сайдчейн-депозиты

        bool is_null_hash = true;
        for (auto byte : tx.sidechain_id) {
            if (byte != 0) {
                is_null_hash = false;
                break;
            }
        }
        if (is_null_hash) {
            return state.invalid(RejectCode::INVALID, "bad-deposit", "sidechain_id is null");
        }

        if (tx.sidechain_owner.str().empty()) {
            return state.invalid(RejectCode::INVALID, "bad-deposit", "sidechain_owner is empty");
        }
        if (!crypto::verify_address(tx.sidechain_owner)) {
            return state.invalid(RejectCode::INVALID, "bad-deposit", "sidechain_owner is invalid");
        }

        if (tx.type != core::TransactionType::MINT_SIDECHAIN &&
            tx.type != core::TransactionType::REDEEM_SIDECHAIN &&
            tx.type != core::TransactionType::SIDECHAIN_CHECKPOINT &&
            tx.type != core::TransactionType::UPDATE_SIDECHAIN_COMMITTEE &&
            tx.type != core::TransactionType::SLASH_DEPOSIT) {
            if (tx.sidechain_signature.empty()) {
                return state.invalid(RejectCode::INVALID, "bad-deposit", "sidechain_signature is empty");
            }
        }

        if (tx.type == core::TransactionType::LOCK_DEPOSIT) {
            if (tx.inputs.empty()) {
                return state.invalid(RejectCode::INVALID, "bad-deposit-lock", "LOCK_DEPOSIT must have inputs");
            }
            if (tx.sidechain_amount <= 0) {
                return state.invalid(RejectCode::INVALID, "bad-deposit-lock", "LOCK_DEPOSIT amount is zero");
            }
            if (tx.sidechain_unlock_height == 0) {
                return state.invalid(RejectCode::INVALID, "bad-deposit-lock", "LOCK_DEPOSIT unlock_height is zero");
            }

            // Проверяем дублирование входов (как STANDARD)
            std::set<std::pair<Hash256, uint32_t>> seen_inputs;
            for (const auto& input : tx.inputs) {
                auto key = std::make_pair(input.prev_tx_hash, input.vout);
                if (seen_inputs.count(key)) {
                    return state.invalid(RejectCode::INVALID, "bad-txns-inputs-duplicate", "duplicate inputs");
                }
                seen_inputs.insert(key);
            }
        } else if (tx.type == core::TransactionType::TOP_UP_SIDECHAIN_DEPOSIT) {
            if (tx.inputs.empty()) {
                return state.invalid(RejectCode::INVALID, "bad-deposit-topup", "TOP_UP_SIDECHAIN_DEPOSIT must have inputs");
            }
            if (tx.sidechain_amount <= 0) {
                return state.invalid(RejectCode::INVALID, "bad-deposit-topup", "TOP_UP_SIDECHAIN_DEPOSIT amount is zero");
            }
            if (tx.sidechain_unlock_height != 0 || tx.sidechain_checkpoint != Hash256{}) {
                return state.invalid(RejectCode::INVALID, "bad-deposit-topup", "TOP_UP_SIDECHAIN_DEPOSIT unlock/checkpoint must be zero");
            }
            std::set<std::pair<Hash256, uint32_t>> seen_inputs;
            for (const auto& input : tx.inputs) {
                if (!seen_inputs.emplace(input.prev_tx_hash, input.vout).second) {
                    return state.invalid(RejectCode::INVALID, "bad-txns-inputs-duplicate", "duplicate inputs");
                }
            }
        } else if (tx.type == core::TransactionType::SPEND_DEPOSIT) {
            if (!tx.inputs.empty()) {
                return state.invalid(RejectCode::INVALID, "bad-deposit-spend", "SPEND_DEPOSIT must have no inputs");
            }
            if (tx.outputs.empty()) {
                return state.invalid(RejectCode::INVALID, "bad-deposit-spend", "SPEND_DEPOSIT must have outputs");
            }
            if (tx.sidechain_amount <= 0) {
                return state.invalid(RejectCode::INVALID, "bad-deposit-spend", "SPEND_DEPOSIT amount is zero");
            }
            if (tx.sidechain_unlock_height != 0) {
                return state.invalid(RejectCode::INVALID, "bad-deposit-spend", "SPEND_DEPOSIT unlock_height must be 0");
            }
        } else if (tx.type == core::TransactionType::UNLOCK_DEPOSIT) {
            if (!tx.inputs.empty()) {
                return state.invalid(RejectCode::INVALID, "bad-deposit-unlock", "UNLOCK_DEPOSIT must have no inputs");
            }
            if (tx.outputs.empty()) {
                return state.invalid(RejectCode::INVALID, "bad-deposit-unlock", "UNLOCK_DEPOSIT must have outputs");
            }
            if (tx.sidechain_amount != 0) {
                return state.invalid(RejectCode::INVALID, "bad-deposit-unlock", "UNLOCK_DEPOSIT amount must be 0");
            }
            if (tx.sidechain_unlock_height != 0) {
                return state.invalid(RejectCode::INVALID, "bad-deposit-unlock", "UNLOCK_DEPOSIT unlock_height must be 0");
            }
        } else if (tx.type == core::TransactionType::ISSUE_SIDECHAIN) {
            if (!tx.inputs.empty()) {
                return state.invalid(RejectCode::INVALID, "bad-sidechain-issue", "ISSUE_SIDECHAIN must have no inputs");
            }
            if (!tx.outputs.empty()) {
                return state.invalid(RejectCode::INVALID, "bad-sidechain-issue", "ISSUE_SIDECHAIN must have no outputs");
            }
            if (tx.sidechain_amount != 0 || tx.sidechain_unlock_height != 0) {
                return state.invalid(RejectCode::INVALID, "bad-sidechain-issue", "ISSUE_SIDECHAIN amount/unlock must be 0");
            }
            if (utils::is_null_hash256(tx.sidechain_checkpoint) ||
                tx.sidechain_checkpoint != tx.sidechain_id) {
                return state.invalid(RejectCode::INVALID, "bad-sidechain-issue", "sidechain_id must equal genesis hash");
            }
            if (tx.sidechain_token_symbol.empty() ||
                tx.sidechain_token_symbol.size() > params::MAX_TOKEN_SYMBOL) {
                return state.invalid(RejectCode::INVALID, "bad-sidechain-issue", "Invalid token symbol");
            }
            if (!is_printable_ascii(tx.sidechain_token_symbol)) {
                return state.invalid(RejectCode::INVALID, "bad-sidechain-issue", "token symbol contains non-printable characters");
            }
            if (tx.sidechain_multiplier < params::MIN_SIDECHAIN_MULTIPLIER ||
                tx.sidechain_max_multiplier < params::MIN_SIDECHAIN_MULTIPLIER ||
                tx.sidechain_multiplier > tx.sidechain_max_multiplier ||
                tx.sidechain_max_multiplier > params::MAX_SIDECHAIN_MULTIPLIER) {
                return state.invalid(RejectCode::INVALID, "bad-sidechain-issue", "Invalid multiplier");
            }
            if (tx.sidechain_fee_bps > params::MAX_SIDECHAIN_FEE_BPS) {
                return state.invalid(RejectCode::INVALID, "bad-sidechain-issue", "Invalid fee bps");
            }
            if (tx.sidechain_checkpoint_policy_id != params::FINANCIAL_CHECKPOINT_POLICY_ID ||
                tx.sidechain_checkpoint_version != params::FINANCIAL_CHECKPOINT_VERSION) {
                return state.invalid(RejectCode::INVALID, "bad-sidechain-issue", "Financial checkpoint policy must be 2/2");
            }
            if (!sidechain_committee_config_valid(
                    tx.sidechain_committee_m,
                    tx.sidechain_committee_n,
                    tx.sidechain_committee_members)) {
                return state.invalid(RejectCode::INVALID, "bad-sidechain-issue", "Invalid committee");
            }
        } else if (tx.type == core::TransactionType::REGISTER_ACCOUNTING_SIDECHAIN) {
            if (!tx.inputs.empty() || !tx.outputs.empty()) {
                return state.invalid(RejectCode::INVALID, "bad-sidechain-accounting", "Accounting registration must have no inputs or outputs");
            }
            if (tx.sidechain_amount != 0 || tx.sidechain_unlock_height != 0) {
                return state.invalid(RejectCode::INVALID, "bad-sidechain-accounting", "Accounting registration amount/unlock must be zero");
            }
            if (tx.sidechain_checkpoint == Hash256{} || tx.sidechain_checkpoint != tx.sidechain_id) {
                return state.invalid(RejectCode::INVALID, "bad-sidechain-accounting", "sidechain_id must equal genesis hash");
            }
            if (tx.sidechain_namespace.empty() ||
                tx.sidechain_namespace.size() > params::MAX_SIDECHAIN_NAMESPACE ||
                !is_printable_ascii(tx.sidechain_namespace)) {
                return state.invalid(RejectCode::INVALID, "bad-sidechain-accounting", "Invalid accounting namespace");
            }
            if (tx.sidechain_schema_hash == Hash256{}) {
                return state.invalid(RejectCode::INVALID, "bad-sidechain-accounting", "schema_hash is required");
            }
            if (!tx.sidechain_token_symbol.empty() || tx.sidechain_multiplier != 0 ||
                tx.sidechain_fee_bps != 0 || tx.sidechain_max_multiplier != 0) {
                return state.invalid(RejectCode::INVALID, "bad-sidechain-accounting", "Accounting sidechain cannot define issuance economics");
            }
            if (tx.sidechain_checkpoint_policy_id != params::ACCOUNTING_CHECKPOINT_POLICY_ID ||
                tx.sidechain_checkpoint_version != params::ACCOUNTING_CHECKPOINT_VERSION) {
                return state.invalid(RejectCode::INVALID, "bad-sidechain-accounting", "Accounting checkpoint policy must be 3/1");
            }
            if (!sidechain_committee_config_valid(
                    tx.sidechain_committee_m,
                    tx.sidechain_committee_n,
                    tx.sidechain_committee_members)) {
                return state.invalid(RejectCode::INVALID, "bad-sidechain-accounting", "Invalid accounting committee");
            }
        } else if (tx.type == core::TransactionType::MINT_SIDECHAIN) {
            if (!tx.inputs.empty()) {
                return state.invalid(RejectCode::INVALID, "bad-sidechain-mint", "MINT_SIDECHAIN must have no inputs");
            }
            if (!tx.outputs.empty()) {
                return state.invalid(RejectCode::INVALID, "bad-sidechain-mint", "MINT_SIDECHAIN must have no outputs");
            }
            if (tx.sidechain_amount <= 0 || tx.sidechain_unlock_height != 0) {
                return state.invalid(RejectCode::INVALID, "bad-sidechain-mint", "MINT_SIDECHAIN amount is zero");
            }
            if (utils::is_null_hash256(tx.sidechain_committee_id)) {
                return state.invalid(RejectCode::INVALID, "bad-sidechain-mint", "committee_id required");
            }
            if (tx.sidechain_committee_signatures.empty() ||
                tx.sidechain_committee_signatures.size() > params::MAX_SIDECHAIN_COMMITTEE_SIGNATURES) {
                return state.invalid(RejectCode::INVALID, "bad-sidechain-mint", "invalid committee signatures");
            }
        } else if (tx.type == core::TransactionType::REDEEM_SIDECHAIN) {
            if (!tx.inputs.empty()) {
                return state.invalid(RejectCode::INVALID, "bad-sidechain-redeem", "REDEEM_SIDECHAIN must have no inputs");
            }
            if (tx.outputs.empty()) {
                return state.invalid(RejectCode::INVALID, "bad-sidechain-redeem", "REDEEM_SIDECHAIN must have outputs");
            }
            if (tx.sidechain_amount <= 0 || tx.sidechain_unlock_height != 0) {
                return state.invalid(RejectCode::INVALID, "bad-sidechain-redeem", "REDEEM_SIDECHAIN amount is zero");
            }
            if (utils::is_null_hash256(tx.sidechain_committee_id)) {
                return state.invalid(RejectCode::INVALID, "bad-sidechain-redeem", "committee_id required");
            }
            if (tx.sidechain_committee_signatures.empty() ||
                tx.sidechain_committee_signatures.size() > params::MAX_SIDECHAIN_COMMITTEE_SIGNATURES) {
                return state.invalid(RejectCode::INVALID, "bad-sidechain-redeem", "invalid committee signatures");
            }
        } else if (tx.type == core::TransactionType::SLASH_DEPOSIT) {
            if (!tx.inputs.empty()) {
                return state.invalid(RejectCode::INVALID, "bad-deposit-slash", "SLASH_DEPOSIT must have no inputs");
            }
            if (tx.sidechain_amount <= 0 || tx.sidechain_unlock_height != 0) {
                return state.invalid(RejectCode::INVALID, "bad-deposit-slash", "SLASH_DEPOSIT amount is zero");
            }
            if (utils::is_null_hash256(tx.sidechain_committee_id)) {
                return state.invalid(RejectCode::INVALID, "bad-deposit-slash", "committee_id required");
            }
            if (tx.sidechain_committee_signatures.empty() ||
                tx.sidechain_committee_signatures.size() > params::MAX_SIDECHAIN_COMMITTEE_SIGNATURES) {
                return state.invalid(RejectCode::INVALID, "bad-deposit-slash", "invalid committee signatures");
            }
            if (utils::is_null_hash256(tx.sidechain_fraud_checkpoint_a) ||
                utils::is_null_hash256(tx.sidechain_fraud_checkpoint_b)) {
                return state.invalid(RejectCode::INVALID, "bad-deposit-slash", "invalid fraud proof txids");
            }
            if (tx.sidechain_fraud_checkpoint_a == tx.sidechain_fraud_checkpoint_b &&
                tx.sidechain_fraud_checkpoint_index_a == tx.sidechain_fraud_checkpoint_index_b) {
                return state.invalid(RejectCode::INVALID, "bad-deposit-slash", "duplicate fraud proof reference");
            }
            Amount total_output = 0;
            for (const auto& output : tx.outputs) {
                Amount next_total = 0;
                if (!checked_add_amount(total_output, output.value, next_total)) {
                    return state.invalid(
                        RejectCode::INVALID,
                        "bad-deposit-slash",
                        "slash output total overflow"
                    );
                }
                total_output = next_total;
            }
            if (total_output > tx.sidechain_amount) {
                return state.invalid(RejectCode::INVALID, "bad-deposit-slash", "outputs exceed slash amount");
            }
        } else if (tx.type == core::TransactionType::SIDECHAIN_CHECKPOINT) {
            return state.invalid(
                RejectCode::INVALID,
                "legacy-sidechain-checkpoint-disabled",
                "Use BATCH_SIDECHAIN_CHECKPOINT with a registry-compatible proof"
            );
        } else if (tx.type == core::TransactionType::UPDATE_SIDECHAIN_COMMITTEE) {
            if (!tx.inputs.empty()) {
                return state.invalid(RejectCode::INVALID, "bad-sidechain-committee", "UPDATE_SIDECHAIN_COMMITTEE must have no inputs");
            }
            if (!tx.outputs.empty()) {
                return state.invalid(RejectCode::INVALID, "bad-sidechain-committee", "UPDATE_SIDECHAIN_COMMITTEE must have no outputs");
            }
            if (tx.sidechain_amount != 0 || tx.sidechain_unlock_height != 0) {
                return state.invalid(RejectCode::INVALID, "bad-sidechain-committee", "UPDATE_SIDECHAIN_COMMITTEE amount/unlock must be 0");
            }
            if (!sidechain_committee_config_valid(
                    tx.sidechain_committee_m,
                    tx.sidechain_committee_n,
                    tx.sidechain_committee_members)) {
                return state.invalid(RejectCode::INVALID, "bad-sidechain-committee", "Invalid committee");
            }
            if (utils::is_null_hash256(tx.sidechain_committee_id)) {
                return state.invalid(RejectCode::INVALID, "bad-sidechain-committee", "committee_id required");
            }
            if (tx.sidechain_committee_signatures.empty() ||
                tx.sidechain_committee_signatures.size() > params::MAX_SIDECHAIN_COMMITTEE_SIGNATURES) {
                return state.invalid(RejectCode::INVALID, "bad-sidechain-committee", "invalid committee signatures");
            }
        } else if (tx.type == core::TransactionType::DECOMMISSION_SIDECHAIN) {
            if (!tx.inputs.empty()) {
                return state.invalid(RejectCode::INVALID, "bad-sidechain-decommission", "DECOMMISSION_SIDECHAIN must have no inputs");
            }
            if (!tx.outputs.empty()) {
                return state.invalid(RejectCode::INVALID, "bad-sidechain-decommission", "DECOMMISSION_SIDECHAIN must have no outputs");
            }
            if (tx.sidechain_amount != 0 || tx.sidechain_unlock_height != 0) {
                return state.invalid(RejectCode::INVALID, "bad-sidechain-decommission", "DECOMMISSION_SIDECHAIN amount/unlock must be 0");
            }
            if (tx.sidechain_checkpoint != Hash256{}) {
                return state.invalid(RejectCode::INVALID, "bad-sidechain-decommission", "DECOMMISSION_SIDECHAIN checkpoint must be null");
            }
            if (tx.sidechain_committee_id == Hash256{}) {
                return state.invalid(RejectCode::INVALID, "bad-sidechain-decommission", "committee_id required");
            }
            if (tx.sidechain_committee_signatures.empty() ||
                tx.sidechain_committee_signatures.size() > params::MAX_SIDECHAIN_COMMITTEE_SIGNATURES) {
                return state.invalid(RejectCode::INVALID, "bad-sidechain-decommission", "invalid committee signatures");
            }
        }
        // Продолжаем обычные проверки выходов ниже
    return true;
}


bool is_supported_checkpoint_policy_version(uint8_t policy_id, uint8_t version) {
    return (policy_id == params::FINANCIAL_CHECKPOINT_POLICY_ID &&
            version == params::FINANCIAL_CHECKPOINT_VERSION) ||
           (policy_id == params::ACCOUNTING_CHECKPOINT_POLICY_ID &&
            version == params::ACCOUNTING_CHECKPOINT_VERSION);
}

Amount calculate_sidechain_checkpoint_fee(uint64_t total_weight_bytes) {
    if (total_weight_bytes == 0) {
        return params::MIN_TX_FEE;
    }
    const uint64_t rate_per_kb = params::FEE_RATE_PER_KB;
    uint64_t fee = utils::mul_div_u64_ceil(total_weight_bytes, rate_per_kb, 1000u);
    if (fee < static_cast<uint64_t>(params::MIN_TX_FEE)) {
        fee = static_cast<uint64_t>(params::MIN_TX_FEE);
    }
    return static_cast<Amount>(fee);
}

Amount calculate_sidechain_checkpoint_min_fee(Height current_height, Height last_checkpoint_height) {
    if (last_checkpoint_height == 0 || current_height <= last_checkpoint_height) {
        return params::MIN_TX_FEE;
    }
    const Height delta = current_height - last_checkpoint_height;
    const Height soft = params::SIDECHAIN_CHECKPOINT_SOFT_INTERVAL;
    if (delta >= soft) {
        return params::MIN_TX_FEE;
    }
    const Height denom = (delta == 0 ? 1 : delta);
    uint64_t multiplier = static_cast<uint64_t>((soft + denom - 1) / denom); // ceil
    if (multiplier > params::MAX_SIDECHAIN_CHECKPOINT_FEE_MULTIPLIER) {
        multiplier = params::MAX_SIDECHAIN_CHECKPOINT_FEE_MULTIPLIER;
    }
    uint64_t fee = utils::mul_div_u64_floor(static_cast<uint64_t>(params::MIN_TX_FEE), multiplier, 1u);
    if (fee < static_cast<uint64_t>(params::MIN_TX_FEE)) {
        fee = static_cast<uint64_t>(params::MIN_TX_FEE);
    }
    return static_cast<Amount>(fee);
}

Hash256 compute_sidechain_committee_id(uint8_t m, uint8_t n, const std::vector<Address>& members) {
    bytes data;
    data.push_back(m);
    data.push_back(n);
    const auto count = utils::encode_varint(members.size());
    data.insert(data.end(), count.begin(), count.end());
    for (const auto& a : members) {
        const std::string s = a.str();
        const auto len = utils::encode_varint(s.size());
        data.insert(data.end(), len.begin(), len.end());
        data.insert(data.end(), s.begin(), s.end());
    }
    return crypto::sha256(data);
}

} // namespace geocoin::consensus
