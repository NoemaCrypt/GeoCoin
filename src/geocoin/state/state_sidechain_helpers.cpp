#include "geocoin/state/state_sidechain_internal.hpp"
#include "geocoin/state/state_internal.hpp"

#include "geocoin/consensus/chainparams.hpp"
#include "geocoin/consensus/consensus.hpp"
#include "geocoin/consensus/sidechain_checkpoint.hpp"
#include "geocoin/consensus/sidechain_fee.hpp"
#include "geocoin/consensus/sidechain_rules.hpp"
#include "geocoin/crypto/addresses.hpp"
#include "geocoin/crypto/geo_area.hpp"
#include "geocoin/crypto/signatures.hpp"
#include "geocoin/utils/bytes.hpp"

#include <limits>
#include <set>
#include <utility>
#include <vector>

namespace geocoin::state {

bool verify_checkpoint_for_registry(
    const core::BatchCheckpointItem& item,
    const SidechainRegistryEntry& entry
) {
    if (entry.status != SidechainStatus::ACTIVE &&
        entry.status != SidechainStatus::DECOMMISSIONING) {
        return false;
    }
    if (entry.kind == core::SidechainKind::FINANCIAL) {
        return entry.checkpoint_policy_id ==
                   consensus::params::FINANCIAL_CHECKPOINT_POLICY_ID &&
               entry.checkpoint_version ==
                   consensus::params::FINANCIAL_CHECKPOINT_VERSION &&
               item.version == consensus::params::FINANCIAL_CHECKPOINT_VERSION &&
               consensus::sidechain_checkpoint::verify_financial_v2(
                   item,
                   entry.issuance_limit,
                   entry.total_issued
               );
    }
    if (entry.kind == core::SidechainKind::ACCOUNTING) {
        return entry.checkpoint_policy_id ==
                   consensus::params::ACCOUNTING_CHECKPOINT_POLICY_ID &&
               entry.checkpoint_version ==
                   consensus::params::ACCOUNTING_CHECKPOINT_VERSION &&
               item.version == consensus::params::ACCOUNTING_CHECKPOINT_VERSION &&
               consensus::sidechain_checkpoint::verify_accounting_v1(
                   item,
                   entry.accounting_schema_hash
               );
    }
    return false;
}

bool validate_slash_fraud_proof(const SidechainReadView& state, const core::Transaction& tx) {
    if (utils::is_null_hash256(tx.sidechain_fraud_checkpoint_a) ||
        utils::is_null_hash256(tx.sidechain_fraud_checkpoint_b) ||
        (tx.sidechain_fraud_checkpoint_a == tx.sidechain_fraud_checkpoint_b &&
         tx.sidechain_fraud_checkpoint_index_a == tx.sidechain_fraud_checkpoint_index_b)) {
        return false;
    }

    struct ResolvedCheckpointRef {
        Hash256 sidechain_id{};
        Hash256 checkpoint{};
        Height checkpoint_height = 0;
    };

    auto resolve_ref = [&state](
        const Hash256& ref_txid,
        uint16_t ref_index,
        ResolvedCheckpointRef& out
    ) -> bool {
        auto ref_tx = state.get_transaction_for_fraud_proof(ref_txid);
        if (!ref_tx) {
            return false;
        }

        if (ref_tx->type == core::TransactionType::SIDECHAIN_CHECKPOINT) {
            if (ref_index != 0xFFFF) {
                return false;
            }
            out.sidechain_id = ref_tx->sidechain_id;
            out.checkpoint = ref_tx->sidechain_checkpoint;
            out.checkpoint_height = ref_tx->sidechain_checkpoint_height;
            return true;
        }

        if (ref_tx->type == core::TransactionType::BATCH_SIDECHAIN_CHECKPOINT) {
            if (ref_index == 0xFFFF) {
                return false;
            }
            const size_t idx = static_cast<size_t>(ref_index);
            if (idx >= ref_tx->sidechain_batch_checkpoints.size()) {
                return false;
            }
            const auto& item = ref_tx->sidechain_batch_checkpoints[idx];
            out.sidechain_id = item.sidechain_id;
            out.checkpoint = item.checkpoint;
            out.checkpoint_height = item.checkpoint_height;
            return true;
        }

        return false;
    };

    ResolvedCheckpointRef a_ref{};
    ResolvedCheckpointRef b_ref{};
    if (!resolve_ref(tx.sidechain_fraud_checkpoint_a, tx.sidechain_fraud_checkpoint_index_a, a_ref) ||
        !resolve_ref(tx.sidechain_fraud_checkpoint_b, tx.sidechain_fraud_checkpoint_index_b, b_ref)) {
        return false;
    }

    if (a_ref.sidechain_id != tx.sidechain_id || b_ref.sidechain_id != tx.sidechain_id) {
        return false;
    }
    if (a_ref.checkpoint_height == 0 ||
        b_ref.checkpoint_height == 0 ||
        a_ref.checkpoint_height != b_ref.checkpoint_height) {
        return false;
    }
    return a_ref.checkpoint != b_ref.checkpoint;
}

bool verify_committee_signature_set(
    const std::vector<Signature>& signatures,
    const Hash256& committee_id,
    const SidechainRegistryEntry& entry,
    const bytes& payload
) {
    if (entry.committee_m == 0 || entry.committee_n == 0) {
        return false;
    }
    if (entry.committee_members.size() != entry.committee_n) {
        return false;
    }
    if (signatures.size() > consensus::params::MAX_SIDECHAIN_COMMITTEE_SIGNATURES) {
        return false;
    }
    const Hash256 expected_id = consensus::compute_sidechain_committee_id(
        entry.committee_m,
        entry.committee_n,
        entry.committee_members
    );
    if (committee_id != expected_id) {
        return false;
    }
    if (signatures.size() < entry.committee_m) {
        return false;
    }

    Hash256 msg32 = crypto::sha256(payload);
    std::set<std::string> used;
    size_t ok = 0;
    for (const auto& sig : signatures) {
        Address signer;
        if (!crypto::recover_signer_address_from_signed_message(msg32, sig, signer)) {
            continue;
        }
        const std::string s = signer.str();
        if (used.count(s) > 0) {
            continue;
        }
        bool allowed = false;
        for (const auto& a : entry.committee_members) {
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
    return ok >= entry.committee_m;
}

bool verify_committee_signatures(
    const core::Transaction& tx,
    const SidechainRegistryEntry& entry,
    const bytes& payload
) {
    return verify_committee_signature_set(
        tx.sidechain_committee_signatures,
        tx.sidechain_committee_id,
        entry,
        payload
    );
}

bool verify_committee_signatures_for_item(
    const core::BatchCheckpointItem& item,
    const SidechainRegistryEntry& entry,
    const bytes& payload
) {
    return verify_committee_signature_set(item.committee_signatures, item.committee_id, entry, payload);
}

bool registry_issuance_limit_from_deposit(
    const SidechainRegistryEntry& entry,
    Amount deposit_amount,
    Amount& out
) {
    if (entry.kind != core::SidechainKind::FINANCIAL) {
        out = 0;
        return true;
    }
    return consensus::financial_issuance_limit_from_deposit(entry.multiplier, deposit_amount, out);
}

bool lock_covers_deposit(const core::Transaction& tx, Amount total_input) {
    Amount total_output = 0;
    Amount required = 0;
    return tx.sidechain_amount >= 0 &&
           checked_sum_outputs(tx.outputs, total_output) &&
           checked_add_amount(total_output, tx.sidechain_amount, required) &&
           total_input >= required;
}

bool top_up_covers_deposit(
    const core::Transaction& tx,
    Amount total_input,
    Amount deposit_amount,
    Amount& topped_up_amount
) {
    Amount total_output = 0;
    Amount required = 0;
    return checked_sum_outputs(tx.outputs, total_output) &&
           tx.sidechain_amount > 0 &&
           checked_add_amount(total_output, tx.sidechain_amount, required) &&
           total_input >= required &&
           checked_add_amount(deposit_amount, tx.sidechain_amount, topped_up_amount);
}

bool sum_existing_input_values(
    const SidechainReadView& state,
    const core::Transaction& tx,
    Amount& total_input
) {
    total_input = 0;
    std::set<std::pair<Hash256, uint32_t>> seen;
    for (const auto& input : tx.inputs) {
        if (!seen.emplace(input.prev_tx_hash, input.vout).second) {
            return false;
        }
        UTXO utxo;
        if (!state.get_utxo(input.prev_tx_hash, input.vout, utxo) ||
            !checked_accumulate_nonnegative(utxo.value, total_input)) {
            return false;
        }
    }
    return true;
}

bool top_up_registry_ok(
    const SidechainReadView& state,
    const core::Transaction& tx,
    SidechainDeposit& deposit,
    SidechainRegistryEntry& registry,
    bool& has_financial_registry,
    Amount& updated_issuance_limit
) {
    has_financial_registry = false;
    updated_issuance_limit = 0;
    if (!state.get_sidechain_deposit(tx.sidechain_id, deposit) ||
        deposit.owner.str() != tx.sidechain_owner.str()) {
        return false;
    }
    if (!state.get_sidechain_registry(tx.sidechain_id, registry)) {
        return true;
    }
    if (registry.status != SidechainStatus::ACTIVE) {
        return false;
    }
    if (registry.kind != core::SidechainKind::FINANCIAL) {
        return true;
    }
    has_financial_registry = true;
    Amount extra_capacity = 0;
    return checked_mul_amount(tx.sidechain_amount, registry.multiplier, extra_capacity) &&
           checked_add_amount(registry.issuance_limit, extra_capacity, updated_issuance_limit);
}

bool spend_deposit_ok(
    const SidechainReadView& state,
    const core::Transaction& tx,
    Height height,
    SidechainDeposit& deposit
) {
    if (!state.get_sidechain_deposit(tx.sidechain_id, deposit) ||
        deposit.owner.str() != tx.sidechain_owner.str() ||
        height < deposit.unlock_height ||
        state.has_sidechain_registry_key(tx.sidechain_id)) {
        return false;
    }
    Amount total_output = 0;
    return checked_sum_outputs(tx.outputs, total_output) &&
           total_output == tx.sidechain_amount &&
           tx.sidechain_amount <= deposit.amount;
}

bool unlock_deposit_ok(
    const SidechainReadView& state,
    const core::Transaction& tx,
    Height height,
    SidechainDeposit& deposit,
    SidechainRegistryEntry& registry,
    bool& registered
) {
    if (!state.get_sidechain_deposit(tx.sidechain_id, deposit)) {
        return false;
    }
    registered = state.get_sidechain_registry(tx.sidechain_id, registry);
    if (registered &&
        (registry.status != SidechainStatus::DECOMMISSIONING ||
         registry.decommission_height >
             std::numeric_limits<Height>::max() -
                 consensus::params::SIDECHAIN_DECOMMISSION_CHALLENGE_BLOCKS ||
         height < registry.decommission_height +
             consensus::params::SIDECHAIN_DECOMMISSION_CHALLENGE_BLOCKS)) {
        return false;
    }
    if (deposit.owner.str() != tx.sidechain_owner.str() ||
        height < deposit.unlock_height) {
        return false;
    }
    Amount total_output = 0;
    return checked_sum_outputs(tx.outputs, total_output) &&
           total_output == deposit.amount;
}

bool issue_sidechain_ok(const SidechainReadView& state, const core::Transaction& tx, IssuePrep& out) {
    if (state.has_sidechain_registry_key(tx.sidechain_id)) {
        return false;
    }
    if (!state.get_sidechain_deposit(tx.sidechain_id, out.deposit) ||
        out.deposit.owner.str() != tx.sidechain_owner.str() ||
        !crypto::verify_address(tx.sidechain_owner) ||
        tx.sidechain_fee_bps > consensus::params::MAX_SIDECHAIN_FEE_BPS) {
        return false;
    }
    const Amount fee = consensus::calc_sidechain_fee(0, tx.sidechain_fee_bps);
    return out.deposit.amount >= fee &&
           checked_sub_amount(out.deposit.amount, fee, out.remaining_deposit) &&
           consensus::financial_issuance_limit_from_deposit(
               tx.sidechain_multiplier, out.remaining_deposit, out.issuance_limit) &&
           !tx.sidechain_token_symbol.empty() &&
           tx.sidechain_token_symbol.size() <= consensus::params::MAX_TOKEN_SYMBOL &&
           consensus::is_printable_ascii(tx.sidechain_token_symbol) &&
           !utils::is_null_hash256(tx.sidechain_checkpoint) &&
           tx.sidechain_checkpoint == tx.sidechain_id &&
           tx.sidechain_multiplier >= consensus::params::MIN_SIDECHAIN_MULTIPLIER &&
           tx.sidechain_max_multiplier >= consensus::params::MIN_SIDECHAIN_MULTIPLIER &&
           tx.sidechain_multiplier <= tx.sidechain_max_multiplier &&
           tx.sidechain_max_multiplier <= consensus::params::MAX_SIDECHAIN_MULTIPLIER &&
           tx.sidechain_checkpoint_policy_id ==
               consensus::params::FINANCIAL_CHECKPOINT_POLICY_ID &&
           tx.sidechain_checkpoint_version ==
               consensus::params::FINANCIAL_CHECKPOINT_VERSION &&
           consensus::sidechain_committee_id_matches(
               tx.sidechain_committee_m,
               tx.sidechain_committee_n,
               tx.sidechain_committee_members,
               tx.sidechain_committee_id,
               true);
}

bool register_accounting_ok(
    const SidechainReadView& state,
    const core::Transaction& tx,
    RegisterPrep& out
) {
    if (state.has_sidechain_registry_key(tx.sidechain_id)) {
        return false;
    }
    if (!state.get_sidechain_deposit(tx.sidechain_id, out.deposit) ||
        out.deposit.owner.str() != tx.sidechain_owner.str() ||
        !crypto::verify_address(tx.sidechain_owner)) {
        return false;
    }
    const Amount fee = consensus::calc_sidechain_fee(0, 0);
    return out.deposit.amount >= fee &&
           checked_sub_amount(out.deposit.amount, fee, out.remaining_deposit) &&
           !utils::is_null_hash256(tx.sidechain_checkpoint) &&
           tx.sidechain_checkpoint == tx.sidechain_id &&
           !tx.sidechain_namespace.empty() &&
           tx.sidechain_namespace.size() <= consensus::params::MAX_SIDECHAIN_NAMESPACE &&
           consensus::is_printable_ascii(tx.sidechain_namespace) &&
           !utils::is_null_hash256(tx.sidechain_schema_hash) &&
           tx.sidechain_token_symbol.empty() &&
           tx.sidechain_multiplier == 0 && tx.sidechain_fee_bps == 0 &&
           tx.sidechain_max_multiplier == 0 &&
           tx.sidechain_checkpoint_policy_id ==
               consensus::params::ACCOUNTING_CHECKPOINT_POLICY_ID &&
           tx.sidechain_checkpoint_version ==
               consensus::params::ACCOUNTING_CHECKPOINT_VERSION &&
           consensus::sidechain_committee_id_matches(
               tx.sidechain_committee_m,
               tx.sidechain_committee_n,
               tx.sidechain_committee_members,
               tx.sidechain_committee_id,
               true) &&
           !state.has_accounting_namespace(tx.sidechain_namespace);
}

bool mint_sidechain_ok(const SidechainReadView& state, const core::Transaction& tx, MintPrep& out) {
    if (!state.get_sidechain_registry(tx.sidechain_id, out.entry) ||
        out.entry.kind != core::SidechainKind::FINANCIAL ||
        out.entry.status != SidechainStatus::ACTIVE ||
        !consensus::is_supported_checkpoint_policy_version(
            out.entry.checkpoint_policy_id, out.entry.checkpoint_version) ||
        !consensus::mint_fits_issuance_capacity(
            out.entry.issuance_limit, out.entry.total_issued, tx.sidechain_amount)) {
        return false;
    }
    if (!state.get_sidechain_deposit(tx.sidechain_id, out.deposit)) {
        return false;
    }
    out.fee = consensus::calc_sidechain_fee(tx.sidechain_amount, out.entry.fee_bps);
    if (out.deposit.amount < out.fee) {
        return false;
    }
    bytes payload = core::build_sidechain_payload_for_signing(tx);
    return verify_committee_signatures(tx, out.entry, payload);
}

bool redeem_sidechain_ok(const SidechainReadView& state, const core::Transaction& tx, RedeemPrep& out) {
    if (!state.get_sidechain_registry(tx.sidechain_id, out.entry) ||
        out.entry.kind != core::SidechainKind::FINANCIAL ||
        out.entry.status != SidechainStatus::ACTIVE ||
        !consensus::is_supported_checkpoint_policy_version(
            out.entry.checkpoint_policy_id, out.entry.checkpoint_version) ||
        tx.sidechain_amount <= 0 ||
        out.entry.total_issued < tx.sidechain_amount) {
        return false;
    }
    if (!state.get_sidechain_deposit(tx.sidechain_id, out.deposit)) {
        return false;
    }
    const Amount fee = consensus::calc_sidechain_fee(tx.sidechain_amount, out.entry.fee_bps);
    Amount total_output = 0;
    if (!checked_add_amount(tx.sidechain_amount, fee, out.debit) ||
        out.deposit.amount < out.debit ||
        !checked_sum_outputs(tx.outputs, total_output) ||
        total_output != tx.sidechain_amount) {
        return false;
    }
    bytes payload = core::build_sidechain_payload_for_signing(tx);
    return verify_committee_signatures(tx, out.entry, payload);
}

bool slash_deposit_ok(const SidechainReadView& state, const core::Transaction& tx, SlashPrep& out) {
    if (!state.get_sidechain_registry(tx.sidechain_id, out.entry) ||
        !consensus::is_supported_checkpoint_policy_version(
            out.entry.checkpoint_policy_id, out.entry.checkpoint_version) ||
        tx.sidechain_amount <= 0) {
        return false;
    }
    if (!state.get_sidechain_deposit(tx.sidechain_id, out.deposit) ||
        out.deposit.amount < tx.sidechain_amount ||
        !validate_slash_fraud_proof(state, tx)) {
        return false;
    }
    Amount total_output = 0;
    if (!checked_sum_outputs(tx.outputs, total_output) ||
        total_output > tx.sidechain_amount) {
        return false;
    }
    bytes payload = core::build_sidechain_payload_for_signing(tx);
    return verify_committee_signatures(tx, out.entry, payload);
}

bool update_committee_ok(
    const SidechainReadView& state,
    const core::Transaction& tx,
    SidechainRegistryEntry& entry
) {
    if (!state.get_sidechain_registry(tx.sidechain_id, entry) ||
        entry.status != SidechainStatus::ACTIVE ||
        !consensus::is_supported_checkpoint_policy_version(
            entry.checkpoint_policy_id, entry.checkpoint_version) ||
        !consensus::sidechain_committee_id_matches(
            tx.sidechain_committee_m,
            tx.sidechain_committee_n,
            tx.sidechain_committee_members,
            tx.sidechain_committee_id,
            false)) {
        return false;
    }
    bytes payload = core::build_sidechain_committee_update_payload_for_signing(tx);
    const Hash256 current_committee_id = consensus::compute_sidechain_committee_id(
        entry.committee_m,
        entry.committee_n,
        entry.committee_members
    );
    return verify_committee_signature_set(
        tx.sidechain_committee_signatures,
        current_committee_id,
        entry,
        payload);
}

bool decommission_sidechain_ok(
    const SidechainReadView& state,
    const core::Transaction& tx,
    SidechainRegistryEntry& entry
) {
    if (!tx.inputs.empty() || !tx.outputs.empty() ||
        tx.sidechain_amount != 0 || tx.sidechain_unlock_height != 0 ||
        !utils::is_null_hash256(tx.sidechain_checkpoint)) {
        return false;
    }
    if (!state.get_sidechain_registry(tx.sidechain_id, entry) ||
        entry.status != SidechainStatus::ACTIVE) {
        return false;
    }
    SidechainDeposit deposit;
    if (!state.get_sidechain_deposit(tx.sidechain_id, deposit) ||
        deposit.owner.str() != tx.sidechain_owner.str() ||
        (entry.total_issued != 0 && deposit.amount != 0)) {
        return false;
    }
    bytes payload = core::build_sidechain_payload_for_signing(tx);
    return verify_committee_signatures(tx, entry, payload);
}

bool batch_item_ok(
    const SidechainReadView& state,
    const core::BatchCheckpointItem& item,
    Height height,
    SidechainDeposit& deposit,
    SidechainRegistryEntry& entry
) {
    if (item.checkpoint_height == 0 ||
        item.checkpoint_height > consensus::params::MAX_SIDECHAIN_CHECKPOINT_HEIGHT ||
        item.fee == 0) {
        return false;
    }
    if (!state.get_sidechain_registry(item.sidechain_id, entry) ||
        !verify_checkpoint_for_registry(item, entry)) {
        return false;
    }
    if (!state.get_sidechain_deposit(item.sidechain_id, deposit) ||
        deposit.checkpoint == item.checkpoint ||
        deposit.amount < item.fee ||
        item.checkpoint_height <= deposit.last_sidechain_checkpoint_height) {
        return false;
    }
    if (deposit.last_checkpoint_height > 0 &&
        height < deposit.last_checkpoint_height +
            consensus::params::MIN_SIDECHAIN_CHECKPOINT_INTERVAL) {
        return false;
    }
    const Amount min_fee = consensus::calculate_sidechain_checkpoint_min_fee(
        height,
        deposit.last_checkpoint_height
    );
    if (item.fee < min_fee) {
        return false;
    }
    bytes payload = core::build_sidechain_batch_checkpoint_payload_for_signing(item);
    return verify_committee_signatures_for_item(item, entry, payload);
}

bool batch_checkpoint_ok(
    const SidechainReadView& state,
    const core::Transaction& tx,
    Height height,
    std::vector<std::pair<Hash256, SidechainDeposit>>* updates
) {
    if (tx.sidechain_batch_checkpoints.empty()) {
        return false;
    }
    std::set<std::string> seen;
    for (const auto& item : tx.sidechain_batch_checkpoints) {
        const std::string key = utils::hash256_to_hex(item.sidechain_id);
        if (!seen.insert(key).second) {
            return false;
        }
        SidechainDeposit deposit;
        SidechainRegistryEntry entry;
        if (!batch_item_ok(state, item, height, deposit, entry)) {
            return false;
        }
        if (updates != nullptr) {
            deposit.amount -= item.fee;
            deposit.checkpoint = item.checkpoint;
            deposit.last_checkpoint_height = height;
            deposit.last_sidechain_checkpoint_height = item.checkpoint_height;
            updates->emplace_back(item.sidechain_id, deposit);
        }
    }
    return true;
}

} // namespace geocoin::state
