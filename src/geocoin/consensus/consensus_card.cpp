#include "geocoin/consensus/consensus_internal.hpp"

#include "geocoin/consensus/consensus.hpp"
#include "geocoin/consensus/card_rules.hpp"
#include "geocoin/consensus/sidechain_rules.hpp"
#include "geocoin/crypto/addresses.hpp"
#include "geocoin/utils/bytes.hpp"

#include <set>
#include <string>
#include <utility>

namespace geocoin::consensus {

bool valid_card_corporate_policy(const core::CardCorporatePolicy& policy) {
    auto valid_authority = [](const core::CardAuthority& authority) {
        return !authority.members.empty() &&
               authority.members.size() <= params::MAX_CARD_AUTHORITY_MEMBERS &&
               authority.quorum > 0 && authority.quorum <= authority.members.size() &&
               is_sorted_unique_addresses(authority.members);
    };
    if (policy.version == 0) {
        return policy.mint.members.empty() && policy.freeze.members.empty() &&
               policy.operations.members.empty() && policy.recovery.members.empty() &&
               policy.mint.quorum == 0 && policy.freeze.quorum == 0 &&
               policy.operations.quorum == 0 && policy.recovery.quorum == 0 &&
               utils::is_null_hash256(policy.compliance_policy_hash) && policy.max_active_cards == 0;
    }
    return policy.version == 1 && valid_authority(policy.mint) && valid_authority(policy.freeze) &&
           valid_authority(policy.operations) && valid_authority(policy.recovery);
}

bool valid_card_validity(uint64_t valid_from, uint64_t valid_to) {
    return valid_from > 0 && (valid_to == 0 || valid_to > valid_from);
}

bool check_card_transaction(const core::Transaction& tx, ValidationState& state) {
        if (tx.is_coinbase()) {
            return state.invalid(RejectCode::INVALID, "bad-card-tx", "CARD_* transaction cannot be coinbase");
        }
        if (tx.inputs.empty()) {
            return state.invalid(RejectCode::INVALID, "bad-card-tx", "CARD_* transaction must have inputs");
        }

        std::set<std::pair<Hash256, uint32_t>> seen_inputs;
        for (const auto& input : tx.inputs) {
            auto key = std::make_pair(input.prev_tx_hash, input.vout);
            if (seen_inputs.count(key)) {
                return state.invalid(RejectCode::INVALID, "bad-txns-inputs-duplicate", "duplicate inputs");
            }
            seen_inputs.insert(key);
        }

        if (utils::is_null_hash256(tx.card_program_id)) {
            return state.invalid(RejectCode::INVALID, "bad-card-tx", "card_program_id is null");
        }
        if (tx.card_issuer.str().empty()) {
            return state.invalid(RejectCode::INVALID, "bad-card-tx", "card_issuer is empty");
        }
        if (!crypto::verify_address(tx.card_issuer)) {
            return state.invalid(RejectCode::INVALID, "bad-card-tx", "card_issuer is invalid");
        }

        if (tx.type == core::TransactionType::CARD_PROGRAM_DEFINE) {
            if (tx.card_chip_vendor.empty() ||
                tx.card_chip_vendor.size() > params::MAX_CARD_CHIP_VENDOR) {
                return state.invalid(RejectCode::INVALID, "bad-card-program", "invalid card_chip_vendor");
            }
            if (!is_printable_ascii(tx.card_chip_vendor)) {
                return state.invalid(RejectCode::INVALID, "bad-card-program", "card_chip_vendor contains non-printable characters");
            }
            if (tx.card_chip_os.empty() ||
                tx.card_chip_os.size() > params::MAX_CARD_CHIP_OS) {
                return state.invalid(RejectCode::INVALID, "bad-card-program", "invalid card_chip_os");
            }
            if (!is_printable_ascii(tx.card_chip_os)) {
                return state.invalid(RejectCode::INVALID, "bad-card-program", "card_chip_os contains non-printable characters");
            }
            if (tx.card_applet_aid.empty() ||
                tx.card_applet_aid.size() > params::MAX_CARD_APPLET_AID) {
                return state.invalid(RejectCode::INVALID, "bad-card-program", "invalid card_applet_aid");
            }
            if (!is_printable_ascii(tx.card_applet_aid)) {
                return state.invalid(RejectCode::INVALID, "bad-card-program", "card_applet_aid contains non-printable characters");
            }
            if (tx.card_applet_version == 0) {
                return state.invalid(RejectCode::INVALID, "bad-card-program", "card_applet_version must be > 0");
            }
            if (utils::is_null_hash256(tx.card_applet_package_hash)) {
                return state.invalid(RejectCode::INVALID, "bad-card-program", "card_applet_package_hash is null");
            }
            if (utils::is_null_hash256(tx.card_scope_id)) {
                return state.invalid(RejectCode::INVALID, "bad-card-program", "card_scope_id is null");
            }
            if (!utils::is_null_hash256(tx.card_uid_hash) || !utils::is_null_hash256(tx.card_pubkey_hash)) {
                return state.invalid(RejectCode::INVALID, "bad-card-program", "CARD_PROGRAM_DEFINE must not include card instance hashes");
            }
            if (tx.card_status != 0) {
                return state.invalid(RejectCode::INVALID, "bad-card-program", "CARD_PROGRAM_DEFINE card_status must be 0");
            }
            if (!valid_card_corporate_policy(tx.card_corporate_policy)) {
                return state.invalid(RejectCode::INVALID, "bad-card-program", "invalid card corporate policy");
            }
        } else if (tx.type == core::TransactionType::CARD_ISSUE) {
            if (utils::is_null_hash256(tx.card_uid_hash)) {
                return state.invalid(RejectCode::INVALID, "bad-card-issue", "card_uid_hash is null");
            }
            if (utils::is_null_hash256(tx.card_pubkey_hash)) {
                return state.invalid(RejectCode::INVALID, "bad-card-issue", "card_pubkey_hash is null");
            }
            if (tx.card_valid_from == 0) {
                return state.invalid(RejectCode::INVALID, "bad-card-issue", "card_valid_from must be > 0");
            }
            if (!valid_card_validity(tx.card_valid_from, tx.card_valid_to)) {
                return state.invalid(RejectCode::INVALID, "bad-card-issue", "card_valid_to must be greater than card_valid_from");
            }
            if (tx.card_status != 0) {
                return state.invalid(RejectCode::INVALID, "bad-card-issue", "CARD_ISSUE card_status must be 0 (active)");
            }
            if (utils::is_null_hash256(tx.card_metadata_hash)) {
                return state.invalid(RejectCode::INVALID, "bad-card-issue", "card_metadata_hash is null");
            }
        } else if (tx.type == core::TransactionType::CARD_UPDATE_STATUS) {
            if (utils::is_null_hash256(tx.card_uid_hash)) {
                return state.invalid(RejectCode::INVALID, "bad-card-update", "card_uid_hash is null");
            }
            if (tx.card_status > 2) {
                return state.invalid(RejectCode::INVALID, "bad-card-update", "invalid card_status");
            }
        } else {
            if (utils::is_null_hash256(tx.card_scope_id)) {
                return state.invalid(RejectCode::INVALID, "bad-card-policy-update", "card_scope_id is null");
            }
            if (tx.card_corporate_policy.version != 1 || !valid_card_corporate_policy(tx.card_corporate_policy)) {
                return state.invalid(RejectCode::INVALID, "bad-card-policy-update", "invalid card corporate policy");
            }
            if (!utils::is_null_hash256(tx.card_uid_hash) ||
                !utils::is_null_hash256(tx.card_pubkey_hash) ||
                tx.card_valid_from != 0 ||
                tx.card_valid_to != 0 ||
                tx.card_status != 0 ||
                !utils::is_null_hash256(tx.card_metadata_hash)) {
                return state.invalid(RejectCode::INVALID, "bad-card-policy-update", "unexpected card instance fields");
            }
        }
    return true;
}

} // namespace geocoin::consensus
