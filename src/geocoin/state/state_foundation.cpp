#include "geocoin/state/state.hpp"
#include "geocoin/state/records.hpp"
#include "geocoin/state/state_internal.hpp"

#include "geocoin/consensus/consensus.hpp"
#include "geocoin/crypto/geo_area.hpp"
#include "geocoin/crypto/signatures.hpp"
#include "geocoin/utils/logger.hpp"
#include "geocoin/utils/u64_math.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <vector>

namespace geocoin::state {
namespace {
FoundationConfig foundation_config_before(const TxUndo::FoundationUndo& undo) {
    FoundationConfig result{Address(undo.prev_address), undo.prev_m, undo.prev_n, {}};
    result.participants.reserve(undo.prev_participants.size());
    for (const auto& member : undo.prev_participants) result.participants.emplace_back(member);
    return result;
}
} // namespace

void ChainState::prepare_foundation_undo_(const core::Transaction& tx, TxUndo& tx_undo) const {
    if (tx.type == core::TransactionType::UPDATE_FOUNDATION_ADDRESS) {
        tx_undo.foundation_undo.kind = 1;
        tx_undo.foundation_undo.prev_address = foundation_config_.address.str();
        tx_undo.foundation_undo.prev_m = foundation_config_.m;
        tx_undo.foundation_undo.prev_n = foundation_config_.n;
        tx_undo.foundation_undo.prev_participants.clear();
        for (const auto& p : foundation_config_.participants) {
            tx_undo.foundation_undo.prev_participants.push_back(p.str());
        }
    }
}

bool ChainState::validate_foundation_undo_(const core::Block& block, const BlockUndo& undo) const {
    if (undo.tx_undos.size() != block.transactions.size()) return false;
    Address expected_current = foundation_config_.address;
    for (size_t i = block.transactions.size(); i > 0; --i) {
        const auto& tx = block.transactions[i - 1];
        const auto& saved = undo.tx_undos[i - 1].foundation_undo;
        if (tx.type != core::TransactionType::UPDATE_FOUNDATION_ADDRESS) {
            if (saved.kind != 0) return false;
            continue;
        }
        if (saved.kind != 1 || tx.foundation_new_address != expected_current) return false;
        const auto previous = foundation_config_before(saved);
        if (!records::validate_foundation_config(previous) ||
            previous.address != tx.foundation_old_address ||
            previous.m != tx.foundation_m || previous.n != tx.foundation_n) return false;
        expected_current = previous.address;
    }
    return true;
}

bool ChainState::revert_foundation_undo_(const TxUndo& tx_undo) {
    if (tx_undo.foundation_undo.kind == 0) {
        return true;
    }
    if (tx_undo.foundation_undo.kind != 1) return false;
    auto previous = foundation_config_before(tx_undo.foundation_undo);
    if (!records::validate_foundation_config(previous)) return false;
    foundation_config_ = std::move(previous);
    persist_foundation_config_to_storage();
    return true;
}

bool ChainState::apply_foundation_transaction_(const core::Transaction& tx, Height height) {
    (void)height;
    const Hash256 tx_hash = tx.get_hash();
    // apply_transaction has already checked the complete approval under mutex_.

    FoundationConfig new_cfg;
    new_cfg.address = tx.foundation_new_address;
    new_cfg.m = tx.foundation_new_m;
    new_cfg.n = tx.foundation_new_n;
    new_cfg.participants = tx.foundation_new_participants;
    foundation_config_ = std::move(new_cfg);
    persist_foundation_config_to_storage();
    record_persistence_result_(store_applied_transaction_(tx_hash, tx), "persist foundation transaction");
    return true;
}

bool ChainState::validate_foundation_transaction_(const core::Transaction& tx) const {
    // Common rules were checked by validate_transaction_impl before dispatch.
    const FoundationConfig previous{tx.foundation_old_address, tx.foundation_m,
                                    tx.foundation_n, tx.foundation_participants};
    if (previous.address != foundation_config_.address ||
        !records::validate_foundation_config(previous)) {
        return false;
    }

    const FoundationConfig next{tx.foundation_new_address, tx.foundation_new_m,
                                tx.foundation_new_n, tx.foundation_new_participants};
    if (!records::validate_foundation_config(next)) {
        return false;
    }

    if (tx.foundation_signatures.size() < tx.foundation_m) {
        return false;
    }

    bytes payload = core::build_foundation_update_payload_for_signing(tx);
    Hash256 msg32 = crypto::sha256(payload);

    std::set<std::string> used;
    size_t ok = 0;
    for (const auto& sig : tx.foundation_signatures) {
        Address signer;
        if (!crypto::recover_signer_address_from_signed_message(msg32, sig, signer)) {
            return false;
        }
        const std::string s = signer.str();
        if (used.count(s) > 0) {
            continue;
        }
        bool allowed = false;
        for (const auto& a : tx.foundation_participants) {
            if (a.str() == s) {
                allowed = true;
                break;
            }
        }
        if (!allowed) {
            continue;
        }
        used.insert(s);
        ok++;
    }
    if (ok < tx.foundation_m) {
        return false;
    }

    return true;
}

bool ChainState::apply_interest_distribution_(const core::Transaction& tx, Height height) {
    const Hash256 tx_hash = tx.get_hash();
    Amount total_output = 0;
    for (const auto& output : tx.outputs) {
        if (output.value <= 0 || !checked_accumulate_nonnegative(output.value, total_output)) {
            return false;
        }
    }
    if (total_output <= 0 || total_output > interest_pool_balance_) {
        return false;
    }
    if (!interest_distribution_matches_(tx, height)) {
        return false;
    }

    if (!add_tx_outputs_as_utxos_(tx, tx_hash, height)) return false;
    interest_pool_balance_ -= total_output;
    persist_interest_pool_balance_to_storage();
    record_persistence_result_(store_applied_transaction_(tx_hash, tx), "persist interest transaction");
    return true;
}

bool ChainState::interest_distribution_matches_(const core::Transaction& tx, Height height) const {
    if (!consensus::is_interest_distribution_due(height)) {
        return false;
    }

    std::vector<core::TxOutput> expected_outputs;
    Amount distributed_total = 0;
    if (!build_interest_distribution_outputs_(height, expected_outputs, distributed_total)) {
        return false;
    }

    if (tx.outputs.size() != expected_outputs.size()) {
        return false;
    }

    for (size_t i = 0; i < tx.outputs.size(); ++i) {
        const auto& got = tx.outputs[i];
        const auto& exp = expected_outputs[i];
        if (got.value != exp.value || got.address.str() != exp.address.str()) {
            return false;
        }
        if (!got.script_pubkey.empty()) {
            return false;
        }
    }

    return true;
}

bool ChainState::validate_interest_distribution_(const core::Transaction& tx) const {
    // Common rules were checked by validate_transaction_impl before dispatch.
    if (current_height_ == (std::numeric_limits<Height>::max)()) {
        return false;
    }
    return interest_distribution_matches_(tx, current_height_ + 1);
}

bool ChainState::revert_interest_distribution_(const core::Transaction& tx) {
    if (tx.type != core::TransactionType::INTEREST_DISTRIBUTION) {
        return true;
    }
    Amount total_output = 0;
    for (const auto& output : tx.outputs) {
        if (output.value <= 0 ||
            total_output > std::numeric_limits<Amount>::max() - output.value) {
            GEO_LOG_ERROR("chainstate", "Cannot revert interest distribution: invalid output total");
            return false;
        }
        total_output += output.value;
    }
    if (total_output <= 0 ||
        interest_pool_balance_ > std::numeric_limits<Amount>::max() - total_output) {
        GEO_LOG_ERROR("chainstate", "Cannot revert interest distribution: pool overflow");
        return false;
    }
    interest_pool_balance_ += total_output;
    persist_interest_pool_balance_to_storage();
    return true;
}

bool ChainState::build_interest_distribution_outputs_(
    Height distribution_height,
    std::vector<core::TxOutput>& outputs,
    Amount& distributed_total
) const {
    outputs.clear();
    distributed_total = 0;

    if (interest_pool_balance_ <= 0) {
        return false;
    }

    std::map<std::string, Amount> balances;
    Amount total_balance = 0;

    const auto add_balance = [&](const std::string& addr, Amount value) {
        if (value <= 0 || addr.empty()) {
            return true;
        }
        Amount& cur = balances[addr];
        Amount next_address = 0;
        Amount next_total = 0;
        if (!checked_add_amount(cur, value, next_address) ||
            !checked_add_amount(total_balance, value, next_total)) {
            return false;
        }
        cur = next_address;
        total_balance = next_total;
        return true;
    };

    if (utxo_balance_index_valid_) {
        for (const auto& [height, bucket] : utxo_balance_by_height_) {
            if (height >= distribution_height) {
                break;
            }
            for (const auto& [addr, value] : bucket.by_address) {
                if (!add_balance(addr, value)) {
                    return false;
                }
            }
        }
    } else {
        for (const auto& [key, utxo] : utxo_set_) {
            if (utxo.value <= 0 || utxo.height >= distribution_height) {
                continue;
            }
            if (!add_balance(utxo.address.str(), utxo.value)) {
                return false;
            }
        }
    }

    if (total_balance <= 0) {
        return false;
    }

    const uint64_t pool_u = static_cast<uint64_t>(interest_pool_balance_);
    const uint64_t total_u = static_cast<uint64_t>(total_balance);

    // Publish a complete result only. A rejected calculation must not expose
    // partial payouts, and the output buffer never needs room for more than
    // the transaction's output limit even when there are many small holders.
    std::vector<core::TxOutput> planned_outputs;
    planned_outputs.reserve(std::min(balances.size(), consensus::params::MAX_TX_OUTPUTS));
    Amount planned_total = 0;

    for (const auto& [addr, bal] : balances) {
        if (bal <= 0) {
            continue;
        }

        // bal <= total_balance, so the exact quotient is at most pool_u and
        // fits Amount. A 128-bit product is sufficient for two uint64 values.
        const Amount payout = static_cast<Amount>(utils::mul_div_u64_floor(
            pool_u, static_cast<uint64_t>(bal), total_u));
        if (payout < consensus::params::DUST_LIMIT) {
            continue;
        }

        if (planned_total > interest_pool_balance_ - payout ||
            planned_outputs.size() == consensus::params::MAX_TX_OUTPUTS) {
            return false;
        }

        core::TxOutput out;
        out.value = payout;
        out.address = Address(addr);
        planned_outputs.push_back(std::move(out));
        planned_total += payout;
    }

    if (planned_total <= 0) return false;
    // Count alone does not guarantee that the distribution fits the current
    // transaction format (long addresses can exceed its byte limit).
    core::Transaction candidate;
    candidate.type = core::TransactionType::INTEREST_DISTRIBUTION;
    candidate.outputs = std::move(planned_outputs);
    consensus::ValidationState validation;
    if (!consensus::check_transaction(candidate, validation)) return false;
    outputs = std::move(candidate.outputs);
    distributed_total = planned_total;
    return true;
}

} // namespace geocoin::state
