#include "geocoin/state/state_card_internal.hpp"

#include "geocoin/utils/bytes.hpp"
#include "geocoin/utils/logger.hpp"

#include <string>

namespace geocoin::state {

void ChainState::prepare_card_undo_(const core::Transaction& tx, TxUndo& tx_undo) const {
    if (tx.type == core::TransactionType::CARD_PROGRAM_DEFINE) {
        const std::string program_hex = hash256_to_hex(tx.card_program_id);
        if (card_programs_.find(program_hex) == card_programs_.end()) {
            tx_undo.card_undo.kind = 1;
            tx_undo.card_undo.program_id = tx.card_program_id;
        }
        return;
    }
    if (tx.type == core::TransactionType::CARD_ISSUE) {
        const std::string uid_hex = hash256_to_hex(tx.card_uid_hash);
        if (card_instances_.find(uid_hex) == card_instances_.end()) {
            tx_undo.card_undo.kind = 2;
            tx_undo.card_undo.program_id = tx.card_program_id;
            tx_undo.card_undo.card_uid_hash = tx.card_uid_hash;
            tx_undo.card_undo.card_pubkey_hash = tx.card_pubkey_hash;
        }
        return;
    }
    if (tx.type == core::TransactionType::CARD_UPDATE_STATUS) {
        const std::string uid_hex = hash256_to_hex(tx.card_uid_hash);
        auto it = card_instances_.find(uid_hex);
        if (it != card_instances_.end()) {
            tx_undo.card_undo.kind = 3;
            tx_undo.card_undo.program_id = it->second.program_id;
            tx_undo.card_undo.card_uid_hash = it->second.uid_hash;
            tx_undo.card_undo.card_pubkey_hash = it->second.pubkey_hash;
            tx_undo.card_undo.prev_issuer = it->second.issuer.str();
            tx_undo.card_undo.prev_valid_from = it->second.valid_from;
            tx_undo.card_undo.prev_valid_to = it->second.valid_to;
            tx_undo.card_undo.prev_status = it->second.status;
            tx_undo.card_undo.prev_metadata_hash = it->second.metadata_hash;
            tx_undo.card_undo.prev_issued_height = it->second.issued_height;
            tx_undo.card_undo.prev_updated_height = it->second.updated_height;
        }
        return;
    }
    if (tx.type == core::TransactionType::CARD_UPDATE_POLICY) {
        const std::string program_hex = hash256_to_hex(tx.card_program_id);
        auto it = card_programs_.find(program_hex);
        if (it != card_programs_.end()) {
            tx_undo.card_undo.kind = 4;
            tx_undo.card_undo.program_id = tx.card_program_id;
            tx_undo.card_undo.prev_program = it->second;
        }
    }
}

bool ChainState::revert_card_undo_(const TxUndo& tx_undo) {
    if (tx_undo.card_undo.kind == 0) {
        return true;
    }
    if (tx_undo.card_undo.kind == 1) {
        const std::string program_hex = hash256_to_hex(tx_undo.card_undo.program_id);
        card_programs_.erase(program_hex);
        delete_card_program_from_storage(tx_undo.card_undo.program_id);
        return true;
    }
    if (tx_undo.card_undo.kind == 2) {
        erase_card_instance_(tx_undo.card_undo.card_uid_hash);
        delete_card_instance_from_storage(tx_undo.card_undo.card_uid_hash);
        return true;
    }
    if (tx_undo.card_undo.kind == 3) {
        CardInstanceEntry restored;
        restored.program_id = tx_undo.card_undo.program_id;
        restored.issuer = Address(tx_undo.card_undo.prev_issuer);
        restored.uid_hash = tx_undo.card_undo.card_uid_hash;
        restored.pubkey_hash = tx_undo.card_undo.card_pubkey_hash;
        restored.valid_from = tx_undo.card_undo.prev_valid_from;
        restored.valid_to = tx_undo.card_undo.prev_valid_to;
        restored.status = tx_undo.card_undo.prev_status;
        restored.metadata_hash = tx_undo.card_undo.prev_metadata_hash;
        restored.issued_height = tx_undo.card_undo.prev_issued_height;
        restored.updated_height = tx_undo.card_undo.prev_updated_height;

        cache_card_instance_(restored);
        persist_card_instance_to_storage(restored.uid_hash, restored);
        return true;
    }
    if (tx_undo.card_undo.kind == 4) {
        card_programs_[hash256_to_hex(tx_undo.card_undo.program_id)] = tx_undo.card_undo.prev_program;
        persist_card_program_to_storage(tx_undo.card_undo.program_id, tx_undo.card_undo.prev_program);
        return true;
    }
    return true;
}

void ChainState::erase_card_instance_(const Hash256& uid) {
    const auto it = card_instances_.find(hash256_to_hex(uid));
    if (it == card_instances_.end()) return;
    if (it->second.status != 2) {
        const auto program = hash256_to_hex(it->second.program_id);
        if (--card_active_counts_.at(program) == 0) card_active_counts_.erase(program);
    }
    card_pubkey_to_uid_.erase(hash256_to_hex(it->second.pubkey_hash));
    card_instances_.erase(it);
}

void ChainState::cache_card_instance_(const CardInstanceEntry& card) {
    erase_card_instance_(card.uid_hash);
    const auto uid = hash256_to_hex(card.uid_hash);
    card_instances_[uid] = card;
    card_pubkey_to_uid_[hash256_to_hex(card.pubkey_hash)] = uid;
    if (card.status != 2) ++card_active_counts_[hash256_to_hex(card.program_id)];
}

bool ChainState::apply_card_transaction_(const core::Transaction& tx, Height height) {
    CardTransition transition;
    if (!prepare_card_effects(CardReadView(*this), tx, height, transition)) return false;
    if (transition.program) {
        card_programs_[hash256_to_hex(tx.card_program_id)] = *transition.program;
        persist_card_program_to_storage(tx.card_program_id, *transition.program);
    }
    if (transition.card) {
        cache_card_instance_(*transition.card);
        persist_card_instance_to_storage(transition.card->uid_hash, *transition.card);
    }
    return true;
}

bool ChainState::validate_card_transaction_(const core::Transaction& tx) const {
    std::string_view error;
    if (!validate_card_transition(CardReadView(*this), tx, &error)) {
        if (!error.empty()) GEO_LOG_ERROR("chainstate", error);
        return false;
    }
    return verify_owned_utxo_inputs_(tx);
}

} // namespace geocoin::state
