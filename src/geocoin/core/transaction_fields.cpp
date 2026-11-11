#include "geocoin/core/transaction.hpp"

#include <algorithm>
#include <stdexcept>

namespace geocoin::core {
namespace {
template<class T> bool is_default(const T& value) { return value == 0; }
bool is_default(const Hash256& value) {
    return std::all_of(value.begin(), value.end(), [](byte b) { return b == 0; });
}
bool is_default(const Address& value) { return value.empty(); }
bool is_default(const std::string& value) { return value.empty(); }
template<class T> bool is_default(const std::vector<T>& value) { return value.empty(); }
template<class T> bool is_default(const std::optional<T>& value) { return !value.has_value(); }
bool is_default(const CardAuthority& value) {
    return value.quorum == 0 && value.members.empty();
}
bool is_default(const CardCorporatePolicy& value) {
    return value.version == 0 && is_default(value.mint) && is_default(value.freeze) &&
           is_default(value.operations) && is_default(value.recovery) &&
           is_default(value.compliance_policy_hash) && value.max_active_cards == 0;
}
}

const char* Transaction::unused_payload_field() const noexcept {
    // This is a wire-field check, not a state or signature check. Unsigned drafts
    // are allowed, but a populated field must have a place in this type's body.
    // Keep the groups aligned with the append_*_payload codecs.
    using T = TransactionType;
    const bool angle = type == T::ANGLE_COMMITMENT;
    for (const auto& input : inputs) {
        if (angle) {
            if (!is_default(input.signature_hash)) return "inputs.signature_hash";
        } else if (!input.signature.empty()) {
            return "inputs.signature";
        }
    }
    const bool spend = type == T::MULTISIG_SPEND;
    const bool delegation = type == T::DELEGATION_PROOF;
    const bool revoke = type == T::REVOKE_DELEGATION;

#define FIELD(name, active) if (!(active) && !is_default(name)) return #name
#define DEFAULT_FIELD(name, active, value) if (!(active) && name != (value)) return #name
    FIELD(commitment_network_id, angle);
    FIELD(multisig_address, angle);
    FIELD(spending_tx_hash, angle);
    FIELD(signer_address, angle);
    FIELD(angle_commitment_hashes, spend || delegation || revoke);
    FIELD(m, spend);
    FIELD(n, spend);
    FIELD(spend_participants, spend && is_default(delegation_proof_txid));
    FIELD(delegation_proof_txid, spend);
    FIELD(revoked_delegation_proof_txid, revoke);
    FIELD(primary_multisig_address, delegation);
    FIELD(delegated_multisig_address, delegation);
    FIELD(primary_m, delegation);
    FIELD(primary_n, delegation);
    FIELD(primary_participants, delegation);
    FIELD(delegated_m, delegation);
    FIELD(delegated_n, delegation);
    FIELD(delegated_participants, delegation);
    FIELD(valid_from, delegation);
    FIELD(valid_until, delegation);
    FIELD(nonce, delegation);
    FIELD(max_total_delegated, delegation);
    FIELD(max_per_transaction, delegation);
    FIELD(max_percent_of_balance, delegation);

    const bool cadastral = type == T::CADASTRAL_DOC;
    FIELD(cadastral_doc_hash, cadastral);
    FIELD(cadastral_number, cadastral);
    FIELD(cadastral_owner, cadastral);
    FIELD(cadastral_nft_id, cadastral);

    const bool card = is_card_transition_type(type);
    const bool program = type == T::CARD_PROGRAM_DEFINE;
    const bool issue_card = type == T::CARD_ISSUE;
    const bool status = type == T::CARD_UPDATE_STATUS;
    const bool policy = type == T::CARD_UPDATE_POLICY;
    FIELD(card_program_id, card);
    FIELD(card_issuer, card);
    FIELD(card_chip_vendor, program);
    FIELD(card_chip_os, program);
    FIELD(card_applet_aid, program);
    FIELD(card_applet_version, program);
    FIELD(card_applet_package_hash, program);
    FIELD(card_policy_id, program || issue_card || policy);
    FIELD(card_scope_id, program || issue_card || policy);
    FIELD(card_corporate_policy, program || policy);
    FIELD(card_uid_hash, issue_card || status);
    FIELD(card_pubkey_hash, issue_card);
    FIELD(card_valid_from, issue_card);
    FIELD(card_valid_to, issue_card);
    FIELD(card_status, issue_card || status);
    FIELD(card_metadata_hash, issue_card || status);

    const bool batch = type == T::BATCH_SIDECHAIN_CHECKPOINT;
    const bool sidechain = is_sidechain_transition_type(type) && !batch;
    const bool financial = type == T::ISSUE_SIDECHAIN;
    const bool accounting = type == T::REGISTER_ACCOUNTING_SIDECHAIN;
    const bool registration = financial || accounting;
    const bool checkpoint = type == T::SIDECHAIN_CHECKPOINT;
    const bool committee_update = type == T::UPDATE_SIDECHAIN_COMMITTEE;
    const bool slash = type == T::SLASH_DEPOSIT;
    const bool committee_signed = type == T::MINT_SIDECHAIN || type == T::REDEEM_SIDECHAIN ||
        checkpoint || committee_update || slash || type == T::DECOMMISSION_SIDECHAIN;
    const bool owner_signed = sidechain &&
        (!committee_signed || type == T::DECOMMISSION_SIDECHAIN);
    FIELD(sidechain_id, sidechain);
    FIELD(sidechain_owner, sidechain);
    FIELD(sidechain_amount, sidechain);
    FIELD(sidechain_unlock_height, sidechain);
    FIELD(sidechain_checkpoint, sidechain);
    FIELD(sidechain_checkpoint_height, checkpoint);
    FIELD(sidechain_signature, owner_signed);
    FIELD(sidechain_fraud_checkpoint_a, slash);
    FIELD(sidechain_fraud_checkpoint_b, slash);
    DEFAULT_FIELD(sidechain_fraud_checkpoint_index_a, slash, 0xFFFF);
    DEFAULT_FIELD(sidechain_fraud_checkpoint_index_b, slash, 0xFFFF);
    FIELD(sidechain_committee_m, registration || committee_update);
    FIELD(sidechain_committee_n, registration || committee_update);
    FIELD(sidechain_committee_members, registration || committee_update);
    FIELD(sidechain_committee_id, registration || committee_signed);
    FIELD(sidechain_committee_signatures, committee_signed);
    FIELD(sidechain_token_symbol, financial);
    FIELD(sidechain_multiplier, financial);
    FIELD(sidechain_fee_bps, financial);
    FIELD(sidechain_max_multiplier, financial);
    DEFAULT_FIELD(sidechain_checkpoint_policy_id, registration, 2);
    DEFAULT_FIELD(sidechain_checkpoint_version, registration, 2);
    FIELD(sidechain_namespace, accounting);
    FIELD(sidechain_schema_hash, accounting);
    FIELD(sidechain_batch_checkpoints, batch);

    const bool foundation = type == T::UPDATE_FOUNDATION_ADDRESS;
    FIELD(foundation_old_address, foundation);
    FIELD(foundation_new_address, foundation);
    FIELD(foundation_m, foundation);
    FIELD(foundation_n, foundation);
    FIELD(foundation_participants, foundation);
    FIELD(foundation_new_m, foundation);
    FIELD(foundation_new_n, foundation);
    FIELD(foundation_new_participants, foundation);
    FIELD(foundation_signatures, foundation);
    FIELD(nft_create, type == T::NFT_CREATE);
    FIELD(nft_transfer, type == T::NFT_TRANSFER);
    FIELD(nft_burn, type == T::NFT_BURN);
#undef DEFAULT_FIELD
#undef FIELD
    return nullptr;
}

void Transaction::require_canonical_payload() const {
    if (const char* field = unused_payload_field()) {
        throw std::invalid_argument(std::string("Unused transaction field: ") + field);
    }
}
} // namespace geocoin::core
