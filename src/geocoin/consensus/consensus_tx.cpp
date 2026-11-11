#include "geocoin/consensus/consensus_internal.hpp"

#include "geocoin/consensus/chainparams.hpp"
#include "geocoin/consensus/consensus.hpp"
#include "geocoin/crypto/signatures.hpp"
#include "geocoin/utils/u64_math.hpp"

#include <algorithm>
#include <set>

namespace geocoin::consensus {

bool is_final_transaction(const core::Transaction& tx, Height block_height, Timestamp block_time) {
    if (tx.lock_time == 0 || tx.lock_time <
        (tx.lock_time < LOCK_TIME_THRESHOLD ? block_height : block_time)) return true;
    // Input-free protocol transactions have no sequence override.
    if (tx.inputs.empty()) return false;
    return std::all_of(tx.inputs.begin(), tx.inputs.end(), [](const core::TxInput& input) {
        return input.sequence == core::TxInput::SEQUENCE_FINAL;
    });
}

bool is_transaction_hardening_active(Height block_height) {
    return block_height >= Params().transaction_hardening_activation_height();
}

bool is_coinbase_spend_mature(Height spend_height, Height coinbase_height) {
    if (spend_height < coinbase_height) {
        return false;
    }
    const Height maturity = Params().coinbase_maturity();
    const Height depth = spend_height - coinbase_height;
    if (is_transaction_hardening_active(spend_height)) {
        return depth >= maturity;
    }
    return depth + 1 >= maturity;
}

bool has_canonical_signature_encodings(const core::Transaction& tx) {
    // Coinbase witness bytes are never verified as signatures, and the block
    // hash already fixes them, so no third party can re-encode them.
    if (tx.is_coinbase()) {
        return true;
    }
    const auto canonical = [](const Signature& signature) {
        return crypto::has_canonical_signature_encoding(signature);
    };
    for (const auto& input : tx.inputs) {
        if (!canonical(input.signature)) return false;
    }
    for (const auto& witness : tx.witnesses) {
        if (!canonical(witness.signature)) return false;
    }
    if (tx.nft_create && !canonical(tx.nft_create->signature)) return false;
    if (tx.nft_transfer && !canonical(tx.nft_transfer->signature)) return false;
    if (tx.nft_burn && !canonical(tx.nft_burn->signature)) return false;
    if (!canonical(tx.sidechain_signature)) return false;
    for (const auto& signature : tx.sidechain_committee_signatures) {
        if (!canonical(signature)) return false;
    }
    for (const auto& item : tx.sidechain_batch_checkpoints) {
        for (const auto& signature : item.committee_signatures) {
            if (!canonical(signature)) return false;
        }
    }
    for (const auto& signature : tx.foundation_signatures) {
        if (!canonical(signature)) return false;
    }
    return true;
}

bool is_revoke_delegation_change_standard(const core::Transaction& tx) {
    if (tx.type != core::TransactionType::REVOKE_DELEGATION || tx.outputs.empty()) {
        return true;
    }
    const auto& change = tx.outputs[0];
    return change.value >= static_cast<Amount>(params::DUST_LIMIT) && change.script_pubkey.empty();
}

bool check_common_utxo_transaction(
    const core::Transaction& tx,
    ValidationState& state
) {
    const bool is_coinbase = tx.is_coinbase();
    // is_coinbase() already checks the complete null-outpoint input marker.
    if (!is_coinbase && tx.type == core::TransactionType::STANDARD) {
        // Обычная транзакция (не ANGLE_COMMITMENT, не MULTISIG_SPEND)

        // Должны быть входы
        if (tx.inputs.empty()) {
            return state.invalid(RejectCode::INVALID, "bad-txns-vin-empty", "no inputs");
        }

        // Проверяем дублирование входов
        std::set<std::pair<Hash256, uint32_t>> seen_inputs;
        for (const auto& input : tx.inputs) {
            auto key = std::make_pair(input.prev_tx_hash, input.vout);
            if (seen_inputs.count(key)) {
                return state.invalid(RejectCode::INVALID, "bad-txns-inputs-duplicate", "duplicate inputs");
            }
            seen_inputs.insert(key);
        }
    }

    // Проверяем выходы (если они есть)
    if (!tx.outputs.empty()) {
        // Проверяем значения выходов
        Amount total_output = 0;
        for (const auto& output : tx.outputs) {
            if (output.value < 0) {
                return state.invalid(RejectCode::INVALID, "bad-txns-vout-negative", "negative output value");
            }

            // Coinbase amounts are fixed by subsidy and fee sharing, including
            // rewards below dust after later halvings. Their output count is
            // independently constrained by check_coinbase_outputs().
            if (!is_coinbase && output.value > 0 && output.value < params::DUST_LIMIT) {
                return state.invalid(RejectCode::INVALID, "bad-txns-dust",
                    "output value below dust limit");
            }

            // Проверяем переполнение до сложения: signed overflow в C++ является UB.
            Amount next_total = 0;
            if (!checked_add_amount(total_output, output.value, next_total)) {
                return state.invalid(RejectCode::INVALID, "bad-txns-txouttotal-toolarge", "output total overflow");
            }
            total_output = next_total;
        }

        // Проверяем максимальное значение (21 миллионов * 100 миллионов angles)
        constexpr Amount MAX_MONEY = 21000000LL * 100000000LL;
        if (total_output > MAX_MONEY) {
            return state.invalid(RejectCode::INVALID, "bad-txns-txouttotal-toolarge", "output total too large");
        }
    } else {
        // Standard transactions, including coinbase, must have an output.
        if (tx.type == core::TransactionType::STANDARD ||
            tx.type == core::TransactionType::CADASTRAL_DOC) {
            return state.invalid(RejectCode::INVALID, "bad-txns-vout-empty", "no outputs");
        }
    }
    return true;
}

} // namespace geocoin::consensus
