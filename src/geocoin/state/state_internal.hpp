#pragma once

#include "geocoin/core/transaction.hpp"
#include "geocoin/types.hpp"

#include <string>

namespace geocoin::state {

constexpr uint8_t kJournalPhaseApplying = 1;
constexpr uint8_t kJournalPhaseReverting = 2;

inline bool metadata_key_starts_with(const std::string& value, const std::string& prefix) {
    return value.rfind(prefix, 0) == 0;
}

inline bool checked_accumulate_nonnegative(Amount value, Amount& total) {
    Amount next = 0;
    if (value < 0 || !checked_add_amount(total, value, next)) {
        return false;
    }
    total = next;
    return true;
}

inline bool checked_sum_outputs(const std::vector<core::TxOutput>& outputs, Amount& total) {
    total = 0;
    for (const auto& output : outputs) {
        if (!checked_accumulate_nonnegative(output.value, total)) {
            return false;
        }
    }
    return true;
}

inline bool is_hierarchical_multisig_spend(const core::Transaction& tx) {
    if (tx.type != core::TransactionType::MULTISIG_SPEND) {
        return false;
    }
    for (auto byte : tx.delegation_proof_txid) {
        if (byte != 0) {
            return true;
        }
    }
    return false;
}

} // namespace geocoin::state
