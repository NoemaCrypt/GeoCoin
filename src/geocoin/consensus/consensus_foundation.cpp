#include "geocoin/consensus/consensus_internal.hpp"

#include "geocoin/consensus/consensus.hpp"

#include <set>
#include <string>

namespace geocoin::consensus {

bool check_foundation_transaction(const core::Transaction& tx, ValidationState& state) {
    if (tx.type == core::TransactionType::UPDATE_FOUNDATION_ADDRESS) {
        if (!tx.inputs.empty()) {
            return state.invalid(RejectCode::INVALID, "bad-foundation-update", "UPDATE_FOUNDATION_ADDRESS must have no inputs");
        }
        if (!tx.outputs.empty()) {
            return state.invalid(RejectCode::INVALID, "bad-foundation-update", "UPDATE_FOUNDATION_ADDRESS must have no outputs");
        }
        if (tx.foundation_old_address.str().empty() || tx.foundation_new_address.str().empty()) {
            return state.invalid(RejectCode::INVALID, "bad-foundation-update", "Empty foundation address");
        }
        if (tx.foundation_m == 0 || tx.foundation_n == 0 || tx.foundation_m > tx.foundation_n) {
            return state.invalid(RejectCode::INVALID, "bad-foundation-update", "Invalid foundation m/n");
        }
        if (tx.foundation_new_m == 0 || tx.foundation_new_n == 0 || tx.foundation_new_m > tx.foundation_new_n) {
            return state.invalid(RejectCode::INVALID, "bad-foundation-update", "Invalid new foundation m/n");
        }
        if (tx.foundation_n > 15 || tx.foundation_new_n > 15) {
            return state.invalid(RejectCode::INVALID, "bad-foundation-update", "Foundation n too large");
        }
        if (tx.foundation_participants.size() != tx.foundation_n ||
            tx.foundation_new_participants.size() != tx.foundation_new_n) {
            return state.invalid(RejectCode::INVALID, "bad-foundation-update", "Invalid participants count");
        }
        {
            std::set<std::string> uniq;
            for (const auto& a : tx.foundation_participants) {
                if (a.str().empty() || !uniq.insert(a.str()).second) {
                    return state.invalid(RejectCode::INVALID, "bad-foundation-update", "Invalid foundation participants");
                }
            }
        }
        {
            std::set<std::string> uniq;
            for (const auto& a : tx.foundation_new_participants) {
                if (a.str().empty() || !uniq.insert(a.str()).second) {
                    return state.invalid(RejectCode::INVALID, "bad-foundation-update", "Invalid new foundation participants");
                }
            }
        }
        return true;
    }

    if (tx.type == core::TransactionType::INTEREST_DISTRIBUTION) {
        if (!tx.inputs.empty()) {
            return state.invalid(RejectCode::INVALID, "bad-interest-distribution", "INTEREST_DISTRIBUTION must have no inputs");
        }
        if (tx.outputs.empty()) {
            return state.invalid(RejectCode::INVALID, "bad-interest-distribution", "INTEREST_DISTRIBUTION must have outputs");
        }
    }

    return true;
}

} // namespace geocoin::consensus
