#include "geocoin/state/state_card_internal.hpp"

#include "geocoin/utils/bytes.hpp"

namespace geocoin::state {

bool CardReadView::get_program(const Hash256& id, CardProgramEntry& out) const {
    const auto key = utils::hash256_to_hex(id);
    if (overlay_) {
        const auto it = overlay_->programs.find(key);
        if (it != overlay_->programs.end()) { out = it->second; return true; }
    }
    const auto it = state_.card_programs_.find(key);
    if (it == state_.card_programs_.end()) return false;
    out = it->second;
    return true;
}

bool CardReadView::get_card(const Hash256& uid, CardInstanceEntry& out) const {
    const auto key = utils::hash256_to_hex(uid);
    if (overlay_) {
        const auto it = overlay_->cards.find(key);
        if (it != overlay_->cards.end()) { out = it->second; return true; }
    }
    const auto it = state_.card_instances_.find(key);
    if (it == state_.card_instances_.end()) return false;
    out = it->second;
    return true;
}

bool CardReadView::has_pubkey(const Hash256& hash) const {
    const auto key = utils::hash256_to_hex(hash);
    return (overlay_ && overlay_->pubkeys.count(key) > 0) || state_.card_pubkey_to_uid_.count(key) > 0;
}

size_t CardReadView::active_card_count(const Hash256& program) const {
    const auto key = utils::hash256_to_hex(program);
    if (overlay_) {
        const auto it = overlay_->active_counts.find(key);
        if (it != overlay_->active_counts.end()) return it->second;
    }
    const auto it = state_.card_active_counts_.find(key);
    return it == state_.card_active_counts_.end() ? 0 : it->second;
}

bool validate_card_transition(const CardReadView& state, const core::Transaction& tx,
                              std::string_view* error) {
    const auto reject = [error](std::string_view reason) {
        if (error) *error = reason;
        return false;
    };
    if (!core::is_card_transition_type(tx.type)) return false;
    bool has_issuer_input = false;
    std::set<std::string> input_addresses;
    for (const auto& input : tx.inputs) {
        UTXO utxo;
        if (!state.get_utxo(input.prev_tx_hash, input.vout, utxo)) {
            return false;
        }
        input_addresses.insert(utxo.address.str());
        if (utxo.address.str() == tx.card_issuer.str()) {
            has_issuer_input = true;
        }
    }

    auto authority_is_authorized = [&input_addresses](const core::CardAuthority& authority) {
        size_t signer_count = 0;
        for (const auto& member : authority.members) {
            if (input_addresses.count(member.str()) > 0) {
                ++signer_count;
            }
        }
        return signer_count >= authority.quorum;
    };

    auto policy_is_authorized = [&](const core::CardCorporatePolicy& policy,
                                    const core::CardAuthority& authority) {
        return policy.version == 0 ? has_issuer_input : authority_is_authorized(authority);
    };

    CardProgramEntry program;
    if (tx.type == core::TransactionType::CARD_PROGRAM_DEFINE) {
        if (state.get_program(tx.card_program_id, program)) {
            return reject("CARD_PROGRAM_DEFINE already exists");
        }
        if (!policy_is_authorized(
                tx.card_corporate_policy,
                tx.card_corporate_policy.recovery)) {
            return reject("CARD_PROGRAM_DEFINE lacks recovery authority quorum");
        }
        return true;
    }

    if (!state.get_program(tx.card_program_id, program)) {
        return reject("CARD_* program not found");
    }
    if (program.issuer.str() != tx.card_issuer.str()) {
        return reject("CARD_* issuer does not match program issuer");
    }

    if (tx.type == core::TransactionType::CARD_UPDATE_POLICY) {
        if (tx.card_scope_id != program.scope_id) {
            return reject("CARD_UPDATE_POLICY scope mismatch");
        }
        if (!policy_is_authorized(program.corporate_policy, program.corporate_policy.recovery)) {
            return reject("CARD_UPDATE_POLICY lacks recovery authority quorum");
        }
        return true;
    }

    if (tx.type == core::TransactionType::CARD_ISSUE) {
        if (tx.card_policy_id != program.policy_id || tx.card_scope_id != program.scope_id) {
            return reject("CARD_ISSUE policy/scope mismatch with program");
        }
        CardInstanceEntry existing;
        if (state.get_card(tx.card_uid_hash, existing)) {
            return reject("CARD_ISSUE duplicate card_uid_hash");
        }
        if (state.has_pubkey(tx.card_pubkey_hash)) {
            return reject("CARD_ISSUE duplicate card_pubkey_hash");
        }
        if (!policy_is_authorized(program.corporate_policy, program.corporate_policy.mint)) {
            return reject("CARD_ISSUE lacks mint authority quorum");
        }
        if (program.corporate_policy.max_active_cards != 0) {
            if (state.active_card_count(tx.card_program_id) >= program.corporate_policy.max_active_cards) {
                return reject("CARD_ISSUE exceeds max_active_cards policy limit");
            }
        }
        return true;
    }

    CardInstanceEntry card;
    if (!state.get_card(tx.card_uid_hash, card)) {
        return reject("CARD_UPDATE_STATUS card_uid_hash not found");
    }
    if (card.program_id != tx.card_program_id) {
        return reject("CARD_UPDATE_STATUS program mismatch");
    }
    if (card.issuer.str() != tx.card_issuer.str()) {
        return reject("CARD_UPDATE_STATUS issuer mismatch");
    }
    if (tx.card_status == card.status) {
        return reject("CARD_UPDATE_STATUS no-op transition");
    }
    if (card.status == 2) {
        return reject("CARD_UPDATE_STATUS revoked card is immutable");
    }
    if (card.status == 0 && tx.card_status != 1 && tx.card_status != 2) {
        return false;
    }
    if (card.status == 1 && tx.card_status != 0 && tx.card_status != 2) {
        return false;
    }
    const core::CardAuthority& required_authority =
        tx.card_status == 0 ? program.corporate_policy.operations : program.corporate_policy.freeze;
    if (!policy_is_authorized(program.corporate_policy, required_authority)) {
        return reject("CARD_UPDATE_STATUS lacks required authority quorum");
    }
    return true;
}

bool prepare_card_effects(const CardReadView& state, const core::Transaction& tx,
                          Height height, CardTransition& out) {
    CardTransition next;
    if (tx.type == core::TransactionType::CARD_PROGRAM_DEFINE) {
        CardProgramEntry entry;
        entry.issuer = tx.card_issuer;
        entry.chip_vendor = tx.card_chip_vendor;
        entry.chip_os = tx.card_chip_os;
        entry.applet_aid = tx.card_applet_aid;
        entry.applet_version = tx.card_applet_version;
        entry.applet_package_hash = tx.card_applet_package_hash;
        entry.policy_id = tx.card_policy_id;
        entry.scope_id = tx.card_scope_id;
        entry.corporate_policy = tx.card_corporate_policy;
        entry.created_height = height;
        next.program = std::move(entry);
    } else if (tx.type == core::TransactionType::CARD_ISSUE) {
        CardInstanceEntry card;
        card.program_id = tx.card_program_id;
        card.issuer = tx.card_issuer;
        card.uid_hash = tx.card_uid_hash;
        card.pubkey_hash = tx.card_pubkey_hash;
        card.valid_from = tx.card_valid_from;
        card.valid_to = tx.card_valid_to;
        card.status = tx.card_status;
        card.metadata_hash = tx.card_metadata_hash;
        card.issued_height = height;
        card.updated_height = height;
        next.card = std::move(card);
    } else if (tx.type == core::TransactionType::CARD_UPDATE_STATUS) {
        CardInstanceEntry card;
        if (!state.get_card(tx.card_uid_hash, card)) return false;
        card.status = tx.card_status;
        if (!utils::is_null_hash256(tx.card_metadata_hash)) card.metadata_hash = tx.card_metadata_hash;
        card.updated_height = height;
        next.card = std::move(card);
    } else if (tx.type == core::TransactionType::CARD_UPDATE_POLICY) {
        CardProgramEntry entry;
        if (!state.get_program(tx.card_program_id, entry)) return false;
        entry.policy_id = tx.card_policy_id;
        entry.corporate_policy = tx.card_corporate_policy;
        next.program = std::move(entry);
    } else return false;
    out = std::move(next);
    return true;
}

} // namespace geocoin::state
