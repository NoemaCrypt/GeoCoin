#include "geocoin/state/state_sidechain_internal.hpp"

#include "geocoin/consensus/consensus.hpp"

namespace geocoin::state {
namespace {
template <typename Record>
bool read_overlay(const std::map<Hash256, std::optional<Record>>& records,
                  const Hash256& id, Record& out, bool& found) {
    const auto it = records.find(id);
    found = it != records.end();
    if (!found || !it->second) return false;
    out = *it->second;
    return true;
}
}

bool SidechainReadView::get_sidechain_deposit(const Hash256& id, SidechainDeposit& out) const {
    if (overlay_) {
        bool found = false;
        const bool present = read_overlay(overlay_->deposits, id, out, found);
        if (found) return present;
    }
    return state_.get_sidechain_deposit(id, out);
}

bool SidechainReadView::get_sidechain_registry(const Hash256& id, SidechainRegistryEntry& out) const {
    if (overlay_) {
        bool found = false;
        const bool present = read_overlay(overlay_->registries, id, out, found);
        if (found) return present;
    }
    return state_.get_sidechain_registry(id, out);
}

bool SidechainReadView::has_sidechain_deposit_key(const Hash256& id) const {
    if (overlay_) {
        const auto it = overlay_->deposits.find(id);
        if (it != overlay_->deposits.end()) return it->second.has_value();
    }
    return state_.has_sidechain_deposit_key(id);
}

bool SidechainReadView::has_sidechain_registry_key(const Hash256& id) const {
    if (overlay_) {
        const auto it = overlay_->registries.find(id);
        if (it != overlay_->registries.end()) return it->second.has_value();
    }
    return state_.has_sidechain_registry_key(id);
}

bool SidechainReadView::has_accounting_namespace(const std::string& name) const {
    if (overlay_) {
        const auto it = overlay_->namespaces.find(name);
        if (it != overlay_->namespaces.end()) return it->second;
    }
    return state_.has_accounting_namespace(name);
}

bool prepare_sidechain_transition(
    const SidechainReadView& state, const core::Transaction& tx, Height height,
    SidechainTransition& out, std::optional<Amount> collected_input
) {
    SidechainTransition next;
    const auto prepare = [&]() -> bool {
        if (tx.type == core::TransactionType::LOCK_DEPOSIT) {
            Amount input = collected_input.value_or(0);
            if (state.has_sidechain_deposit_key(tx.sidechain_id) ||
                (!collected_input && !sum_existing_input_values(state, tx, input)) ||
                !lock_covers_deposit(tx, input)) return false;
            SidechainDeposit deposit;
            deposit.owner = tx.sidechain_owner;
            deposit.amount = tx.sidechain_amount;
            deposit.unlock_height = tx.sidechain_unlock_height;
            deposit.checkpoint = tx.sidechain_checkpoint;
            deposit.last_checkpoint_height = 0;
            deposit.last_sidechain_checkpoint_height = 0;
            next.deposits[tx.sidechain_id] = std::move(deposit);
            next.spends_inputs = next.creates_outputs = true;
            return true;
        }

        if (tx.type == core::TransactionType::TOP_UP_SIDECHAIN_DEPOSIT) {
            SidechainDeposit deposit;
            SidechainRegistryEntry registry;
            bool financial = false;
            Amount limit = 0, amount = 0, input = collected_input.value_or(0);
            if (!top_up_registry_ok(state, tx, deposit, registry, financial, limit) ||
                (!collected_input && !sum_existing_input_values(state, tx, input)) ||
                !top_up_covers_deposit(tx, input, deposit.amount, amount)) return false;
            deposit.amount = amount;
            next.deposits[tx.sidechain_id] = std::move(deposit);
            if (financial) {
                registry.issuance_limit = limit;
                next.registries[tx.sidechain_id] = std::move(registry);
            }
            next.spends_inputs = next.creates_outputs = true;
            return true;
        }

        if (tx.type == core::TransactionType::SPEND_DEPOSIT) {
            SidechainDeposit deposit;
            if (!spend_deposit_ok(state, tx, height, deposit)) return false;
            deposit.amount -= tx.sidechain_amount;
            next.deposits[tx.sidechain_id] = std::move(deposit);
            next.creates_outputs = true;
            return true;
        }

        if (tx.type == core::TransactionType::UNLOCK_DEPOSIT) {
            SidechainDeposit deposit;
            SidechainRegistryEntry registry;
            bool registered = false;
            if (!unlock_deposit_ok(state, tx, height, deposit, registry, registered)) return false;
            next.deposits[tx.sidechain_id] = std::nullopt;
            if (registered) next.registries[tx.sidechain_id] = std::nullopt;
            next.creates_outputs = true;
            return true;
        }

        if (tx.type == core::TransactionType::ISSUE_SIDECHAIN ||
            tx.type == core::TransactionType::REGISTER_ACCOUNTING_SIDECHAIN) {
            SidechainRegistryEntry entry;
            if (tx.type == core::TransactionType::ISSUE_SIDECHAIN) {
                IssuePrep prep;
                if (!issue_sidechain_ok(state, tx, prep)) return false;
                prep.deposit.amount = prep.remaining_deposit;
                next.deposits[tx.sidechain_id] = std::move(prep.deposit);
                entry.kind = core::SidechainKind::FINANCIAL;
                entry.multiplier = tx.sidechain_multiplier;
                entry.fee_bps = tx.sidechain_fee_bps;
                entry.max_multiplier = tx.sidechain_max_multiplier;
                entry.token_symbol = tx.sidechain_token_symbol;
                entry.issuance_limit = prep.issuance_limit;
            } else {
                RegisterPrep prep;
                if (!register_accounting_ok(state, tx, prep)) return false;
                prep.deposit.amount = prep.remaining_deposit;
                next.deposits[tx.sidechain_id] = std::move(prep.deposit);
                entry.kind = core::SidechainKind::ACCOUNTING;
                entry.issuance_limit = 0;
                entry.multiplier = entry.fee_bps = entry.max_multiplier = 0;
                entry.accounting_namespace = tx.sidechain_namespace;
                entry.accounting_schema_hash = tx.sidechain_schema_hash;
            }
            entry.status = SidechainStatus::ACTIVE;
            entry.decommission_height = 0;
            entry.owner = tx.sidechain_owner;
            entry.total_issued = 0;
            entry.committee_m = tx.sidechain_committee_m;
            entry.committee_n = tx.sidechain_committee_n;
            entry.committee_members = tx.sidechain_committee_members;
            entry.committee_id = consensus::compute_sidechain_committee_id(
                tx.sidechain_committee_m, tx.sidechain_committee_n, tx.sidechain_committee_members);
            entry.checkpoint_policy_id = tx.sidechain_checkpoint_policy_id;
            entry.checkpoint_version = tx.sidechain_checkpoint_version;
            next.registries[tx.sidechain_id] = std::move(entry);
            return true;
        }

        if (tx.type == core::TransactionType::MINT_SIDECHAIN) {
            MintPrep prep;
            if (!mint_sidechain_ok(state, tx, prep) ||
                !checked_add_amount(prep.entry.total_issued, tx.sidechain_amount, prep.entry.total_issued)) return false;
            prep.deposit.amount -= prep.fee;
            next.deposits[tx.sidechain_id] = std::move(prep.deposit);
            next.registries[tx.sidechain_id] = std::move(prep.entry);
            return true;
        }

        if (tx.type == core::TransactionType::REDEEM_SIDECHAIN) {
            RedeemPrep prep;
            if (!redeem_sidechain_ok(state, tx, prep)) return false;
            prep.deposit.amount -= prep.debit;
            prep.entry.total_issued -= tx.sidechain_amount;
            next.deposits[tx.sidechain_id] = std::move(prep.deposit);
            next.registries[tx.sidechain_id] = std::move(prep.entry);
            next.creates_outputs = true;
            return true;
        }

        if (tx.type == core::TransactionType::SLASH_DEPOSIT) {
            SlashPrep prep;
            if (!slash_deposit_ok(state, tx, prep)) return false;
            prep.deposit.amount -= tx.sidechain_amount;
            Amount limit = 0;
            if (!registry_issuance_limit_from_deposit(prep.entry, prep.deposit.amount, limit)) return false;
            next.deposits[tx.sidechain_id] = std::move(prep.deposit);
            if (prep.entry.kind == core::SidechainKind::FINANCIAL) {
                prep.entry.issuance_limit = limit;
                next.registries[tx.sidechain_id] = std::move(prep.entry);
            }
            next.creates_outputs = true;
            return true;
        }

        if (tx.type == core::TransactionType::BATCH_SIDECHAIN_CHECKPOINT) {
            std::vector<std::pair<Hash256, SidechainDeposit>> updates;
            if (!batch_checkpoint_ok(state, tx, height, &updates)) return false;
            for (auto& [id, deposit] : updates) next.deposits[id] = std::move(deposit);
            return true;
        }

        if (tx.type == core::TransactionType::UPDATE_SIDECHAIN_COMMITTEE) {
            SidechainRegistryEntry entry;
            if (!update_committee_ok(state, tx, entry)) return false;
            entry.committee_m = tx.sidechain_committee_m;
            entry.committee_n = tx.sidechain_committee_n;
            entry.committee_members = tx.sidechain_committee_members;
            entry.committee_id = tx.sidechain_committee_id;
            next.registries[tx.sidechain_id] = std::move(entry);
            return true;
        }

        if (tx.type == core::TransactionType::DECOMMISSION_SIDECHAIN) {
            SidechainRegistryEntry entry;
            if (!decommission_sidechain_ok(state, tx, entry)) return false;
            entry.status = SidechainStatus::DECOMMISSIONING;
            entry.decommission_height = height;
            next.registries[tx.sidechain_id] = std::move(entry);
            return true;
        }

        return false;
    };
    if (!prepare()) return false;
    out = std::move(next);
    return true;
}
} // namespace geocoin::state
