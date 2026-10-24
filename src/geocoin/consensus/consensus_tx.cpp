#include "geocoin/consensus/consensus_internal.hpp"

#include "geocoin/consensus/consensus.hpp"
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
