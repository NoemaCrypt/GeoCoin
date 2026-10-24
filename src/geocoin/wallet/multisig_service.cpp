#include "geocoin/wallet/multisig_service.hpp"
#include "geocoin/wallet/address_funds.hpp"

#include "geocoin/core/transaction.hpp"
#include "geocoin/crypto/addresses.hpp"
#include "geocoin/consensus/consensus.hpp"
#include "geocoin/consensus/chainparams.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <set>

namespace geocoin::wallet {

namespace {
constexpr Amount kDefaultDraftFee = consensus::params::MIN_TX_FEE;
// A Falcon commitment carries both a public key and a signature. Keep the
// estimate conservative so attaching signatures cannot underfund a package.
constexpr size_t kEstimatedAngleCommitmentBytes = 2048;
constexpr size_t kEstimatedControlInputAndChangeBytes = 256;

uint64_t current_unix_time() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

std::vector<Address> config_participants(const MultisigConfig& config) {
    if (!config.participants.empty()) {
        std::vector<Address> result;
        result.reserve(config.participants.size());
        for (const auto& participant : config.participants) {
            result.push_back(participant.address);
        }
        return result;
    }
    return config.addresses;
}

bool unique_hashes(const std::vector<Hash256>& hashes) {
    return std::set<Hash256>(hashes.begin(), hashes.end()).size() == hashes.size();
}

bool same_participants(const std::vector<Address>& left, const std::vector<Address>& right) {
    if (left.size() != right.size()) {
        return false;
    }
    std::multiset<std::string> left_addresses;
    std::multiset<std::string> right_addresses;
    for (const auto& address : left) {
        left_addresses.insert(address.str());
    }
    for (const auto& address : right) {
        right_addresses.insert(address.str());
    }
    return left_addresses == right_addresses;
}

bool load_spendable_multisig_utxos(
    const Address& owner,
    const state::ChainState& chain_state,
    std::span<const core::Transaction> pending,
    std::vector<state::UTXO>& utxos,
    std::string& error
) {
    const Height height = chain_state.get_height();
    if (height == (std::numeric_limits<Height>::max)()) {
        error = "Chain height is out of range";
        return false;
    }
    utxos = chain_state.get_utxos_for_address(owner);
    const PendingSpendIndex reserved(pending);
    std::erase_if(utxos, [&](const state::UTXO& utxo) {
        return !utxo_is_spendable(utxo, height) || reserved.contains(utxo.tx_hash, utxo.vout);
    });
    // Prefer fewer inputs and use a stable tie-break for equal values.
    std::sort(utxos.begin(), utxos.end(), [](const state::UTXO& left, const state::UTXO& right) {
        if (left.value != right.value) return left.value > right.value;
        if (left.tx_hash != right.tx_hash) return left.tx_hash < right.tx_hash;
        return left.vout < right.vout;
    });
    return true;
}

bool select_control_fee_inputs(
    const Address& owner,
    Amount requested_fee,
    const std::shared_ptr<state::ChainState>& chain_state,
    std::span<const core::Transaction> pending,
    std::vector<core::TxInput>& inputs,
    std::vector<core::TxOutput>& outputs,
    Amount& total_input,
    Amount& fee_used,
    std::string& error
) {
    if (!chain_state) {
        error = "Chain state is not available";
        return false;
    }
    const Amount fee = std::max(requested_fee, kDefaultDraftFee);
    std::vector<state::UTXO> utxos;
    if (!load_spendable_multisig_utxos(owner, *chain_state, pending, utxos, error)) {
        return false;
    }
    Amount selected = 0;
    for (const auto& utxo : utxos) {
        Amount next = 0;
        if (!checked_add_amount(selected, utxo.value, next)) {
            error = "Selected UTXO total is out of range";
            return false;
        }
        selected = next;
        core::TxInput input;
        input.prev_tx_hash = utxo.tx_hash;
        input.vout = utxo.vout;
        inputs.push_back(std::move(input));
        if (selected >= fee) {
            break;
        }
    }
    if (selected < fee) {
        error = "Insufficient primary multisig funds for protocol fee";
        return false;
    }

    Amount change = 0;
    if (!checked_sub_amount(selected, fee, change)) {
        error = "Protocol fee change is out of range";
        return false;
    }
    if (change >= static_cast<Amount>(consensus::params::DUST_LIMIT)) {
        outputs.push_back(core::TxOutput{change, owner, {}});
    } else {
        change = 0;
    }
    total_input = selected;
    if (!checked_sub_amount(selected, change, fee_used) || fee_used < kDefaultDraftFee) {
        error = "Protocol fee is out of range";
        return false;
    }
    return true;
}

Amount estimate_commitment_package_fee_impl(
    const core::Transaction& root,
    size_t maximum_commitments,
    Amount requested_fee,
    std::string& error
) {
    size_t estimated_size = root.serialize().size();
    if (estimated_size > (std::numeric_limits<size_t>::max)() -
            kEstimatedControlInputAndChangeBytes ||
        maximum_commitments >
            ((std::numeric_limits<size_t>::max)() - estimated_size -
             kEstimatedControlInputAndChangeBytes) / kEstimatedAngleCommitmentBytes) {
        error = "Protocol package size estimate is out of range";
        return -1;
    }
    estimated_size += kEstimatedControlInputAndChangeBytes +
        maximum_commitments * kEstimatedAngleCommitmentBytes;

    const uint64_t rate = static_cast<uint64_t>(consensus::params::FEE_RATE_PER_KB);
    if (estimated_size > (std::numeric_limits<uint64_t>::max)() / rate) {
        error = "Protocol package fee estimate is out of range";
        return -1;
    }
    const uint64_t numerator = static_cast<uint64_t>(estimated_size) * rate;
    const uint64_t size_fee = numerator / 1000u + (numerator % 1000u != 0 ? 1u : 0u);
    if (size_fee > static_cast<uint64_t>((std::numeric_limits<Amount>::max)())) {
        error = "Protocol package fee estimate is out of range";
        return -1;
    }
    return std::max({
        requested_fee,
        kDefaultDraftFee,
        static_cast<Amount>(size_fee)
    });
}

bool is_null_hash(const Hash256& hash) {
    return std::all_of(hash.begin(), hash.end(), [](byte value) { return value == 0; });
}

bool make_signer_config(
    uint8_t m,
    uint8_t n,
    const Address& expected_address,
    const std::vector<Address>& participants,
    MultisigConfig& config,
    std::string& error
) {
    try {
        MultisigConfig candidate(m, n, participants);
        candidate.address = expected_address;
        if (!validate_multisig_config(expected_address, candidate)) {
            error = "Signer policy does not match its multisig address";
            return false;
        }
        config = std::move(candidate);
        return true;
    } catch (const std::exception& ex) {
        error = std::string("Invalid signer policy: ") + ex.what();
        return false;
    }
}

bool validate_unsigned_root_envelope(
    const core::Transaction& draft,
    std::string& error
) {
    if (draft.version != 2 ||
        draft.witness_version != consensus::params::WITNESS_FORMAT_VERSION_CURRENT) {
        error = "Unsupported transaction or witness version";
        return false;
    }
    if (!draft.angle_commitment_hashes.empty()) {
        error = "Signing requires the original draft without commitment references";
        return false;
    }
    if (draft.inputs.empty() || draft.inputs.size() > consensus::params::MAX_TX_INPUTS ||
        draft.outputs.size() > consensus::params::MAX_TX_OUTPUTS) {
        error = "Draft has an invalid input/output count";
        return false;
    }

    std::set<std::pair<Hash256, uint32_t>> inputs;
    for (const auto& input : draft.inputs) {
        const bool nonzero_signature_hash = std::any_of(
            input.signature_hash.begin(), input.signature_hash.end(),
            [](byte value) { return value != 0; });
        if (is_null_hash(input.prev_tx_hash) || input.vout >= 0xFFFFFFFCu ||
            !input.script_sig.empty() || input.sequence != 0xFFFFFFFFu ||
            !input.signature.empty() || nonzero_signature_hash ||
            !inputs.emplace(input.prev_tx_hash, input.vout).second) {
            error = "Draft has a non-canonical or duplicate UTXO input";
            return false;
        }
    }
    for (const auto& witness : draft.witnesses) {
        const bool has_key_hash = std::any_of(
            witness.public_key_hash.begin(), witness.public_key_hash.end(),
            [](byte value) { return value != 0; });
        if (has_key_hash || !witness.public_key.empty() || !witness.signature.empty()) {
            error = "Multisig draft must not contain ordinary input witnesses";
            return false;
        }
    }

    Amount total_output = 0;
    for (const auto& output : draft.outputs) {
        Amount next = 0;
        if (output.value < 0 ||
            (output.value > 0 && output.value <
                static_cast<Amount>(consensus::params::DUST_LIMIT)) ||
            !checked_add_amount(total_output, output.value, next)) {
            error = "Draft has an invalid output amount";
            return false;
        }
        total_output = next;
    }
    try {
        if (draft.serialize().size() > consensus::params::MAX_TX_SIZE) {
            error = "Draft exceeds the maximum transaction size";
            return false;
        }
    } catch (const std::exception& ex) {
        error = std::string("Draft cannot be serialized: ") + ex.what();
        return false;
    }
    return true;
}
}

bool estimate_commitment_package_fee(
    const core::Transaction& root,
    size_t maximum_commitments,
    Amount requested_fee,
    Amount& fee,
    std::string& error
) {
    const Amount estimated = estimate_commitment_package_fee_impl(
        root, maximum_commitments, requested_fee, error);
    if (estimated < 0) {
        return false;
    }
    fee = estimated;
    return true;
}

bool derive_commitment_signing_intent(
    const core::Transaction& draft,
    const core::Transaction* delegation_proof,
    CommitmentSigningIntent& intent,
    std::string& error
) {
    if (!validate_unsigned_root_envelope(draft, error)) {
        return false;
    }

    CommitmentSigningIntent derived;
    if (draft.type == core::TransactionType::MULTISIG_SPEND) {
        const bool hierarchical = !is_null_hash(draft.delegation_proof_txid);
        if (!hierarchical) {
            Address expected_address;
            try {
                expected_address = Address(create_multisig_address(
                    draft.m, draft.n, draft.spend_participants).address);
            } catch (const std::exception& ex) {
                error = std::string("Direct multisig policy is invalid: ") + ex.what();
                return false;
            }
            if (draft.outputs.empty() || draft.spend_participants.size() != draft.n ||
                !make_signer_config(
                    draft.m,
                    draft.n,
                    expected_address,
                    draft.spend_participants,
                    derived.signer_config,
                    error)) {
                if (error.empty()) {
                    error = "Direct multisig draft has an invalid signer policy";
                }
                return false;
            }
            derived.kind = CommitmentIntentKind::DIRECT_SPEND;
            derived.signing_hash = draft.get_hash_without_commitments();
        } else {
            if (!delegation_proof ||
                delegation_proof->type != core::TransactionType::DELEGATION_PROOF ||
                delegation_proof->get_hash() != draft.delegation_proof_txid ||
                !draft.spend_participants.empty() ||
                draft.m != delegation_proof->delegated_m ||
                draft.n != delegation_proof->delegated_n ||
                !make_signer_config(
                    delegation_proof->delegated_m,
                    delegation_proof->delegated_n,
                    delegation_proof->delegated_multisig_address,
                    delegation_proof->delegated_participants,
                    derived.signer_config,
                    error)) {
                if (error.empty()) {
                    error = "Hierarchical draft does not match its delegation proof";
                }
                return false;
            }
            derived.kind = CommitmentIntentKind::HIERARCHICAL_SPEND;
            derived.signing_hash = draft.get_hierarchical_spend_hash();
        }
    } else if (draft.type == core::TransactionType::DELEGATION_PROOF) {
        if (draft.outputs.size() > 1 || draft.lock_time != 0 ||
            !make_signer_config(
                draft.primary_m,
                draft.primary_n,
                draft.primary_multisig_address,
                draft.primary_participants,
                derived.signer_config,
                error)) {
            if (error.empty()) {
                error = "Delegation draft has an invalid primary policy";
            }
            return false;
        }
        derived.kind = CommitmentIntentKind::DELEGATION_PROOF;
        derived.signing_hash = draft.get_delegation_proof_hash();
    } else if (draft.type == core::TransactionType::REVOKE_DELEGATION) {
        if (!delegation_proof ||
            delegation_proof->type != core::TransactionType::DELEGATION_PROOF ||
            delegation_proof->get_hash() != draft.revoked_delegation_proof_txid ||
            draft.outputs.size() > 1 || draft.lock_time != 0 ||
            !make_signer_config(
                delegation_proof->primary_m,
                delegation_proof->primary_n,
                delegation_proof->primary_multisig_address,
                delegation_proof->primary_participants,
                derived.signer_config,
                error)) {
            if (error.empty()) {
                error = "Revocation draft does not match its delegation proof";
            }
            return false;
        }
        derived.kind = CommitmentIntentKind::REVOKE_DELEGATION;
        derived.signing_hash = draft.get_revoke_delegation_hash();
    } else {
        error = "Transaction type cannot be authorized by an ANGLE commitment";
        return false;
    }

    intent = std::move(derived);
    return true;
}

bool create_multisig_draft(
    const MultisigConfig& config,
    const Address& to_address,
    Amount amount,
    Amount fee,
    std::shared_ptr<state::ChainState> chain_state,
    MultisigDraftResult& out_result,
    std::string& error,
    std::span<const core::Transaction> pending
) {
    if (!chain_state) {
        error = "Chain state is not available";
        return false;
    }
    if (!config.is_valid()) {
        error = "Invalid multisig config";
        return false;
    }
    if (config.address.str().empty()) {
        error = "Multisig config missing address";
        return false;
    }
    if (amount <= 0) {
        error = "Amount must be positive";
        return false;
    }
    const auto participants = config_participants(config);
    core::Transaction fee_template = core::Transaction::create_multisig_spend(
        {}, {core::TxOutput{amount, to_address, {}}}, {}, config.m, config.n);
    fee_template.spend_participants = participants;
    if (!estimate_commitment_package_fee(
            fee_template, config.n, fee, fee, error)) {
        return false;
    }

    Amount total_required = 0;
    if (!checked_add_amount(amount, fee, total_required)) {
        error = "Amount plus fee is out of range";
        return false;
    }
    std::vector<state::UTXO> utxos;
    if (!load_spendable_multisig_utxos(config.address, *chain_state, pending, utxos, error)) return false;
    if (utxos.empty()) {
        error = "No spendable UTXO found for multisig address";
        return false;
    }

    Amount selected = 0;
    std::vector<core::TxInput> inputs;
    for (const auto& utxo : utxos) {
        core::TxInput input;
        input.prev_tx_hash = utxo.tx_hash;
        input.vout = utxo.vout;
        inputs.push_back(input);
        Amount next_selected = 0;
        if (utxo.value < 0 || !checked_add_amount(selected, utxo.value, next_selected)) {
            error = "Selected UTXO total is out of range";
            return false;
        }
        selected = next_selected;
        if (selected >= total_required) {
            break;
        }
    }

    if (selected < total_required) {
        error = "Insufficient funds for draft";
        return false;
    }

    std::vector<core::TxOutput> outputs;

    core::TxOutput pay_out;
    pay_out.value = amount;
    pay_out.address = to_address;
    outputs.push_back(pay_out);

    Amount change = 0;
    if (!checked_sub_amount(selected, total_required, change)) {
        error = "Draft change is out of range";
        return false;
    }
    if (change >= static_cast<Amount>(consensus::params::DUST_LIMIT)) {
        core::TxOutput change_out;
        change_out.value = change;
        change_out.address = config.address;
        outputs.push_back(change_out);
    } else {
        // Omitted dust becomes part of the actual transaction fee.
        change = 0;
    }

	core::Transaction draft_tx = core::Transaction::create_multisig_spend(
	    inputs,
	    outputs,
	    {},
	    config.m,
	    config.n
	);

	// Ordinary MULTISIG_SPEND commits to the on-chain participant preimage.
	draft_tx.spend_participants = participants;

    Amount total_output = 0;
    Amount fee_used = 0;
    if (!checked_add_amount(amount, change, total_output) ||
        !checked_sub_amount(selected, total_output, fee_used) ||
        fee_used < 0) {
        error = "Draft totals are out of range";
        return false;
    }

	out_result.transaction = draft_tx;
	out_result.spending_tx_hash = draft_tx.get_hash_without_commitments();
    out_result.total_input = selected;
    out_result.total_output = total_output;
    out_result.fee_used = fee_used;
    return true;
}

bool sign_angle_commitment(
    const Address& multisig_address,
    const Hash256& spending_tx_hash,
    const bytes& private_key,
    CommitmentResult& out_result,
    std::string& error
) {
    if (private_key.size() != 32) {
        error = "Private key must be 32 bytes";
        return false;
    }

    try {
        core::Transaction commitment = core::Transaction::create_angle_commitment(
            multisig_address,
            spending_tx_hash,
            private_key,
            Address(""),
            consensus::Params().network_id()
        );
        out_result.commitment_tx = commitment;
        out_result.commitment_hash = commitment.get_hash();
        out_result.signer_address = commitment.signer_address;
        return true;
    } catch (const std::exception& ex) {
        error = ex.what();
        return false;
    }
}

bool sign_configured_angle_commitment(
    const MultisigConfig& config,
    const Hash256& spending_tx_hash,
    const bytes& private_key,
    CommitmentResult& out_result,
    std::string& error
) {
    if (!validate_multisig_config(config.address, config)) {
        error = "Invalid multisig configuration";
        return false;
    }

    Address signer_address;
    if (!crypto::compute_address_from_private_key(private_key, signer_address)) {
        error = "Failed to derive signer address";
        return false;
    }
    const auto participants = config_participants(config);
    const auto participant = std::find_if(
        participants.begin(),
        participants.end(),
        [&signer_address](const Address& address) {
            return address.str() == signer_address.str();
        }
    );
    if (participant == participants.end()) {
        error = "Private key does not belong to the multisig policy";
        return false;
    }

    return sign_angle_commitment(
        config.address,
        spending_tx_hash,
        private_key,
        out_result,
        error
    );
}

bool create_delegation_proof(
    const DelegationProofRequest& request,
    std::shared_ptr<state::ChainState> chain_state,
    DelegationProofResult& out_result,
    std::string& error,
    std::span<const core::Transaction> pending
) {
    if (!validate_multisig_config(request.primary_config.address, request.primary_config) ||
        !validate_multisig_config(request.delegated_config.address, request.delegated_config)) {
        error = "Invalid primary or delegated multisig config";
        return false;
    }

    const uint64_t now = request.timestamp == 0 ? current_unix_time() : request.timestamp;
    if ((request.valid_until > 0 && request.valid_until <= now) ||
        (request.valid_from > 0 && request.valid_until > 0 &&
         request.valid_until <= request.valid_from)) {
        error = "Invalid delegation validity window";
        return false;
    }
    if (request.max_total <= 0 || request.max_per_transaction < 0 ||
        (request.max_per_transaction > 0 &&
         request.max_per_transaction > request.max_total) ||
        request.max_percent_of_balance > 100) {
        error = "Invalid delegation limits";
        return false;
    }

    const auto primary_participants = config_participants(request.primary_config);
    const auto delegated_participants = config_participants(request.delegated_config);
    if (primary_participants.size() != request.primary_config.n ||
        delegated_participants.size() != request.delegated_config.n) {
        error = "Multisig participant count does not match N";
        return false;
    }

    core::Transaction transaction;
    transaction.version = 2;
    transaction.type = core::TransactionType::DELEGATION_PROOF;
    transaction.timestamp = now;
    transaction.primary_multisig_address = request.primary_config.address;
    transaction.delegated_multisig_address = request.delegated_config.address;
    transaction.primary_m = request.primary_config.m;
    transaction.primary_n = request.primary_config.n;
    transaction.delegated_m = request.delegated_config.m;
    transaction.delegated_n = request.delegated_config.n;
    transaction.primary_participants = primary_participants;
    transaction.delegated_participants = delegated_participants;
    transaction.valid_from = request.valid_from;
    transaction.valid_until = request.valid_until;
    transaction.nonce = request.nonce;
    transaction.max_total_delegated = request.max_total;
    transaction.max_per_transaction = request.max_per_transaction;
    transaction.max_percent_of_balance = request.max_percent_of_balance;

    Amount package_fee = 0;
    if (!estimate_commitment_package_fee(
            transaction, request.primary_config.n, request.fee, package_fee, error)) {
        return false;
    }
    if (!select_control_fee_inputs(
            request.primary_config.address,
            package_fee,
            chain_state,
            pending,
            transaction.inputs,
            transaction.outputs,
            out_result.total_input,
            out_result.fee_used,
            error)) {
        return false;
    }

    out_result.transaction = std::move(transaction);
    out_result.delegation_hash = out_result.transaction.get_delegation_proof_hash();
    return true;
}

bool finalize_delegation_proof(
    const core::Transaction& draft,
    const std::vector<Hash256>& commitment_hashes,
    core::Transaction& final_transaction,
    std::string& error
) {
    if (draft.type != core::TransactionType::DELEGATION_PROOF) {
        error = "Draft is not a delegation proof";
        return false;
    }
    if (draft.primary_m == 0 || draft.primary_n == 0 ||
        commitment_hashes.size() < draft.primary_m ||
        commitment_hashes.size() > draft.primary_n) {
        error = "Delegation commitment quorum is invalid";
        return false;
    }
    if (!unique_hashes(commitment_hashes)) {
        error = "Delegation commitments contain duplicates";
        return false;
    }

    final_transaction = draft;
    final_transaction.angle_commitment_hashes = commitment_hashes;
    return true;
}

bool create_hierarchical_spend_draft(
    const HierarchicalSpendRequest& request,
    std::shared_ptr<state::ChainState> chain_state,
    HierarchicalSpendDraftResult& out_result,
    std::string& error,
    std::span<const core::Transaction> pending
) {
    if (!chain_state) {
        error = "Chain state is not available";
        return false;
    }
    if (request.amount <= 0 || request.fee < 0) {
        error = "Invalid amount or fee";
        return false;
    }
    if (!validate_multisig_config(request.delegated_config.address, request.delegated_config)) {
        error = "Invalid delegated multisig config";
        return false;
    }

    const auto proof = chain_state->get_transaction(request.delegation_proof_txid);
    if (!proof || proof->type != core::TransactionType::DELEGATION_PROOF) {
        error = "Delegation proof is not confirmed";
        return false;
    }
    const uint64_t now = request.timestamp == 0 ? current_unix_time() : request.timestamp;
    if (chain_state->is_delegation_revoked(request.delegation_proof_txid) ||
        (proof->valid_from > 0 && now < proof->valid_from) ||
        (proof->valid_until > 0 && now > proof->valid_until)) {
        error = "Delegation proof is not active";
        return false;
    }

    const auto delegated_participants = config_participants(request.delegated_config);
    if (request.delegated_config.address.str() != proof->delegated_multisig_address.str() ||
        request.delegated_config.m != proof->delegated_m ||
        request.delegated_config.n != proof->delegated_n ||
        !same_participants(delegated_participants, proof->delegated_participants)) {
        error = "Delegated config does not match the confirmed proof";
        return false;
    }

    core::Transaction fee_template = core::Transaction::create_multisig_spend(
        {}, {core::TxOutput{request.amount, request.recipient, {}}}, {},
        request.delegated_config.m, request.delegated_config.n);
    fee_template.timestamp = now;
    fee_template.delegation_proof_txid = request.delegation_proof_txid;
    Amount fee = 0;
    if (!estimate_commitment_package_fee(
            fee_template, request.delegated_config.n, request.fee, fee, error)) {
        return false;
    }
    Amount total_needed = 0;
    if (!checked_add_amount(request.amount, fee, total_needed)) {
        error = "Amount plus fee is out of range";
        return false;
    }

    std::vector<state::UTXO> utxos;
    if (!load_spendable_multisig_utxos(
            proof->primary_multisig_address, *chain_state, pending, utxos, error)) return false;

    Amount selected = 0;
    std::vector<core::TxInput> inputs;
    for (const auto& utxo : utxos) {
        Amount next_selected = 0;
        if (utxo.value < 0 || !checked_add_amount(selected, utxo.value, next_selected)) {
            error = "Selected UTXO total is out of range";
            return false;
        }
        inputs.push_back(core::TxInput{utxo.tx_hash, utxo.vout});
        selected = next_selected;
        if (selected >= total_needed) {
            break;
        }
    }
    if (selected < total_needed) {
        error = "Insufficient funds on primary multisig address";
        return false;
    }

    std::vector<core::TxOutput> outputs;
    outputs.push_back(core::TxOutput{request.amount, request.recipient, {}});
    Amount change = selected - total_needed;
    Amount fee_used = fee;
    if (change > 0 && change < static_cast<Amount>(consensus::params::DUST_LIMIT)) {
        if (!checked_add_amount(fee_used, change, fee_used)) {
            error = "Fee overflow while absorbing dust change";
            return false;
        }
        change = 0;
    }
    if (change > 0) {
        outputs.push_back(core::TxOutput{change, proof->primary_multisig_address, {}});
    }

    core::Transaction transaction = core::Transaction::create_multisig_spend(
        inputs,
        outputs,
        {},
        request.delegated_config.m,
        request.delegated_config.n
    );
    transaction.timestamp = now;
    transaction.delegation_proof_txid = request.delegation_proof_txid;
    transaction.spend_participants.clear();

    out_result.transaction = std::move(transaction);
    out_result.spending_hash = out_result.transaction.get_hierarchical_spend_hash();
    out_result.primary_address = proof->primary_multisig_address;
    out_result.total_input = selected;
    out_result.fee_used = fee_used;
    out_result.change = change;
    return true;
}

bool finalize_hierarchical_spend(
    const core::Transaction& draft,
    const std::vector<Hash256>& commitment_hashes,
    std::shared_ptr<state::ChainState> chain_state,
    core::Transaction& final_transaction,
    std::string& error
) {
    if (!chain_state) {
        error = "Chain state is not available";
        return false;
    }
    if (draft.type != core::TransactionType::MULTISIG_SPEND ||
        std::all_of(draft.delegation_proof_txid.begin(), draft.delegation_proof_txid.end(),
                    [](byte value) { return value == 0; })) {
        error = "Draft is not a hierarchical multisig spend";
        return false;
    }
    if (draft.m == 0 || draft.n == 0 ||
        commitment_hashes.size() < draft.m || commitment_hashes.size() > draft.n ||
        !unique_hashes(commitment_hashes)) {
        error = "Hierarchical commitment quorum is invalid";
        return false;
    }

    const auto proof = chain_state->get_transaction(draft.delegation_proof_txid);
    const uint64_t now = current_unix_time();
    if (!proof || proof->type != core::TransactionType::DELEGATION_PROOF ||
        chain_state->is_delegation_revoked(draft.delegation_proof_txid) ||
        (proof->valid_from > 0 && now < proof->valid_from) ||
        (proof->valid_until > 0 && now > proof->valid_until) ||
        draft.m != proof->delegated_m || draft.n != proof->delegated_n) {
        error = "Delegation proof is not active or does not match the draft";
        return false;
    }

    final_transaction = draft;
    final_transaction.angle_commitment_hashes = commitment_hashes;
    return true;
}

bool finalize_multisig_spend(
    const core::Transaction& draft_tx,
    const std::vector<Hash256>& commitment_hashes,
    uint8_t required_signatures,
    core::Transaction& final_tx,
    std::string& error
) {
    if (draft_tx.type != core::TransactionType::MULTISIG_SPEND ||
        !is_null_hash(draft_tx.delegation_proof_txid) ||
        !draft_tx.angle_commitment_hashes.empty() ||
        required_signatures != draft_tx.m ||
        commitment_hashes.size() < draft_tx.m ||
        commitment_hashes.size() > draft_tx.n ||
        !unique_hashes(commitment_hashes)) {
        error = "Direct multisig commitment quorum is invalid";
        return false;
    }

    final_tx = draft_tx;
    final_tx.angle_commitment_hashes = commitment_hashes;
    return true;
}

} // namespace geocoin::wallet
