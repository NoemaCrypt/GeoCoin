#pragma once

#include "geocoin/state/state.hpp"

#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace geocoin::state {

struct SidechainUpdates {
    std::map<Hash256, std::optional<SidechainDeposit>> deposits;
    std::map<Hash256, std::optional<SidechainRegistryEntry>> registries;
};

struct SidechainOverlay : SidechainUpdates {
    std::map<std::string, bool> namespaces;
};

class SidechainReadView {
public:
    SidechainReadView(const ChainState& state, const SidechainOverlay* overlay = nullptr)
        : state_(state), overlay_(overlay) {}
    bool get_sidechain_deposit(const Hash256& id, SidechainDeposit& out) const;
    bool get_sidechain_registry(const Hash256& id, SidechainRegistryEntry& out) const;
    bool has_sidechain_deposit_key(const Hash256& id) const;
    bool has_sidechain_registry_key(const Hash256& id) const;
    bool has_accounting_namespace(const std::string& name) const;
    bool get_utxo(const Hash256& hash, uint32_t vout, UTXO& out) const {
        return state_.get_utxo(hash, vout, out);
    }
    std::unique_ptr<core::Transaction> get_transaction_for_fraud_proof(const Hash256& hash) const {
        return state_.get_transaction_for_fraud_proof(hash);
    }
    bool has_active_transaction(const Hash256& hash) const {
        return state_.has_active_transaction(hash);
    }

    // Transaction hardening rules (strict committee signature lists and
    // active-chain fraud evidence). Consensus from the activation height;
    // relay validation enables them regardless of height.
    SidechainReadView with_hardening(bool enabled) const {
        SidechainReadView view = *this;
        view.hardening_ = view.hardening_ || enabled;
        return view;
    }
    bool hardening() const { return hardening_; }
private:
    const ChainState& state_;
    const SidechainOverlay* overlay_;
    bool hardening_ = false;
};

struct SidechainTransition : SidechainUpdates {
    bool spends_inputs = false;
    bool creates_outputs = false;
};

bool prepare_sidechain_transition(
    const SidechainReadView& state, const core::Transaction& tx, Height height,
    SidechainTransition& out, std::optional<Amount> collected_input = std::nullopt);

bool verify_checkpoint_for_registry(
    const core::BatchCheckpointItem& item,
    const SidechainRegistryEntry& entry
);
bool validate_slash_fraud_proof(const SidechainReadView& state, const core::Transaction& tx);
// strict: every listed signature must verify and come from a distinct
// committee member. Otherwise invalid, duplicate and non-member entries are
// skipped, which lets third parties append entries and change the txid.
bool verify_committee_signature_set(
    const std::vector<Signature>& signatures,
    const Hash256& committee_id,
    const SidechainRegistryEntry& entry,
    const bytes& payload,
    bool strict
);
bool verify_committee_signatures(
    const core::Transaction& tx,
    const SidechainRegistryEntry& entry,
    const bytes& payload,
    bool strict
);
bool verify_committee_signatures_for_item(
    const core::BatchCheckpointItem& item,
    const SidechainRegistryEntry& entry,
    const bytes& payload,
    bool strict
);
bool registry_issuance_limit_from_deposit(
    const SidechainRegistryEntry& entry,
    Amount deposit_amount,
    Amount& out
);
bool lock_covers_deposit(const core::Transaction& tx, Amount total_input);
bool top_up_covers_deposit(
    const core::Transaction& tx,
    Amount total_input,
    Amount deposit_amount,
    Amount& topped_up_amount
);
bool sum_existing_input_values(
    const SidechainReadView& state,
    const core::Transaction& tx,
    Amount& total_input
);
bool top_up_registry_ok(
    const SidechainReadView& state,
    const core::Transaction& tx,
    SidechainDeposit& deposit,
    SidechainRegistryEntry& registry,
    bool& has_financial_registry,
    Amount& updated_issuance_limit
);
bool spend_deposit_ok(
    const SidechainReadView& state,
    const core::Transaction& tx,
    Height height,
    SidechainDeposit& deposit
);
bool unlock_deposit_ok(
    const SidechainReadView& state,
    const core::Transaction& tx,
    Height height,
    SidechainDeposit& deposit,
    SidechainRegistryEntry& registry,
    bool& registered
);

struct IssuePrep {
    SidechainDeposit deposit;
    Amount remaining_deposit = 0;
    Amount issuance_limit = 0;
};

bool issue_sidechain_ok(const SidechainReadView& state, const core::Transaction& tx, IssuePrep& out);

struct RegisterPrep {
    SidechainDeposit deposit;
    Amount remaining_deposit = 0;
};

bool register_accounting_ok(
    const SidechainReadView& state,
    const core::Transaction& tx,
    RegisterPrep& out
);

struct MintPrep {
    SidechainRegistryEntry entry;
    SidechainDeposit deposit;
    Amount fee = 0;
};

bool mint_sidechain_ok(const SidechainReadView& state, const core::Transaction& tx, MintPrep& out);

struct RedeemPrep {
    SidechainRegistryEntry entry;
    SidechainDeposit deposit;
    Amount debit = 0;
};

bool redeem_sidechain_ok(const SidechainReadView& state, const core::Transaction& tx, RedeemPrep& out);

struct SlashPrep {
    SidechainRegistryEntry entry;
    SidechainDeposit deposit;
};

bool slash_deposit_ok(const SidechainReadView& state, const core::Transaction& tx, SlashPrep& out);
bool update_committee_ok(
    const SidechainReadView& state,
    const core::Transaction& tx,
    SidechainRegistryEntry& entry
);
bool decommission_sidechain_ok(
    const SidechainReadView& state,
    const core::Transaction& tx,
    SidechainRegistryEntry& entry
);
bool batch_item_ok(
    const SidechainReadView& state,
    const core::BatchCheckpointItem& item,
    Height height,
    SidechainDeposit& deposit,
    SidechainRegistryEntry& entry
);
bool batch_checkpoint_ok(
    const SidechainReadView& state,
    const core::Transaction& tx,
    Height height,
    std::vector<std::pair<Hash256, SidechainDeposit>>* updates
);

} // namespace geocoin::state
