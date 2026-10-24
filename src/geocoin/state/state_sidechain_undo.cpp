#include "geocoin/state/state.hpp"

#include "geocoin/utils/bytes.hpp"

namespace geocoin::state {

void ChainState::prepare_sidechain_undo_(const core::Transaction& tx, TxUndo& tx_undo) const {
    if (tx.type == core::TransactionType::LOCK_DEPOSIT) {
        if (!has_sidechain_deposit_key(tx.sidechain_id)) {
            tx_undo.deposit_undo.kind = 1;
            tx_undo.deposit_undo.sidechain_id = tx.sidechain_id;
        }
    } else if (tx.type == core::TransactionType::TOP_UP_SIDECHAIN_DEPOSIT ||
               tx.type == core::TransactionType::SPEND_DEPOSIT ||
               tx.type == core::TransactionType::UNLOCK_DEPOSIT ||
               tx.type == core::TransactionType::ISSUE_SIDECHAIN ||
               tx.type == core::TransactionType::REGISTER_ACCOUNTING_SIDECHAIN ||
               tx.type == core::TransactionType::MINT_SIDECHAIN ||
               tx.type == core::TransactionType::REDEEM_SIDECHAIN ||
               tx.type == core::TransactionType::SIDECHAIN_CHECKPOINT ||
               tx.type == core::TransactionType::SLASH_DEPOSIT ||
               tx.type == core::TransactionType::BATCH_SIDECHAIN_CHECKPOINT) {
        if (tx.type == core::TransactionType::BATCH_SIDECHAIN_CHECKPOINT) {
            tx_undo.deposit_undo_batch.clear();
            for (const auto& item : tx.sidechain_batch_checkpoints) {
                SidechainDeposit prev;
                if (get_sidechain_deposit(item.sidechain_id, prev)) {
                    TxUndo::DepositUndo du;
                    du.kind = 2;
                    du.sidechain_id = item.sidechain_id;
                    du.prev_owner = prev.owner.str();
                    du.prev_amount = prev.amount;
                    du.prev_unlock_height = prev.unlock_height;
                    du.prev_checkpoint = prev.checkpoint;
                    du.prev_last_checkpoint_height = prev.last_checkpoint_height;
                    du.prev_last_sidechain_checkpoint_height = prev.last_sidechain_checkpoint_height;
                    tx_undo.deposit_undo_batch.push_back(std::move(du));
                }
            }
        } else {
            SidechainDeposit prev;
            if (get_sidechain_deposit(tx.sidechain_id, prev)) {
                tx_undo.deposit_undo.kind =
                    (tx.type == core::TransactionType::TOP_UP_SIDECHAIN_DEPOSIT ||
                     tx.type == core::TransactionType::SPEND_DEPOSIT ||
                     tx.type == core::TransactionType::SIDECHAIN_CHECKPOINT ||
                     tx.type == core::TransactionType::SLASH_DEPOSIT) ? 2 : 3;
                tx_undo.deposit_undo.sidechain_id = tx.sidechain_id;
                tx_undo.deposit_undo.prev_owner = prev.owner.str();
                tx_undo.deposit_undo.prev_amount = prev.amount;
                tx_undo.deposit_undo.prev_unlock_height = prev.unlock_height;
                tx_undo.deposit_undo.prev_checkpoint = prev.checkpoint;
                tx_undo.deposit_undo.prev_last_checkpoint_height = prev.last_checkpoint_height;
                tx_undo.deposit_undo.prev_last_sidechain_checkpoint_height = prev.last_sidechain_checkpoint_height;
            }
        }
    }

    if (tx.type == core::TransactionType::ISSUE_SIDECHAIN ||
        tx.type == core::TransactionType::REGISTER_ACCOUNTING_SIDECHAIN) {
        if (!has_sidechain_registry_key(tx.sidechain_id)) {
            tx_undo.sidechain_undo.kind = 1;
            tx_undo.sidechain_undo.sidechain_id = tx.sidechain_id;
        }
    } else if (tx.type == core::TransactionType::MINT_SIDECHAIN ||
               tx.type == core::TransactionType::REDEEM_SIDECHAIN ||
               tx.type == core::TransactionType::UPDATE_SIDECHAIN_COMMITTEE ||
               tx.type == core::TransactionType::DECOMMISSION_SIDECHAIN ||
               tx.type == core::TransactionType::UNLOCK_DEPOSIT ||
               tx.type == core::TransactionType::TOP_UP_SIDECHAIN_DEPOSIT ||
               tx.type == core::TransactionType::SLASH_DEPOSIT) {
        SidechainRegistryEntry prev;
        if (get_sidechain_registry(tx.sidechain_id, prev)) {
            tx_undo.sidechain_undo.kind = 2;
            tx_undo.sidechain_undo.sidechain_id = tx.sidechain_id;
            tx_undo.sidechain_undo.prev_owner = prev.owner.str();
            tx_undo.sidechain_undo.prev_limit = prev.issuance_limit;
            tx_undo.sidechain_undo.prev_issued = prev.total_issued;
            tx_undo.sidechain_undo.prev_kind = prev.kind;
            tx_undo.sidechain_undo.prev_status = prev.status;
            tx_undo.sidechain_undo.prev_decommission_height = prev.decommission_height;
            tx_undo.sidechain_undo.prev_multiplier = prev.multiplier;
            tx_undo.sidechain_undo.prev_fee_bps = prev.fee_bps;
            tx_undo.sidechain_undo.prev_max_multiplier = prev.max_multiplier;
            tx_undo.sidechain_undo.prev_symbol = prev.token_symbol;
            tx_undo.sidechain_undo.prev_accounting_namespace = prev.accounting_namespace;
            tx_undo.sidechain_undo.prev_accounting_schema_hash = prev.accounting_schema_hash;
            tx_undo.sidechain_undo.prev_committee_m = prev.committee_m;
            tx_undo.sidechain_undo.prev_committee_n = prev.committee_n;
            tx_undo.sidechain_undo.prev_committee_members.clear();
            for (const auto& a : prev.committee_members) {
                tx_undo.sidechain_undo.prev_committee_members.push_back(a.str());
            }
            tx_undo.sidechain_undo.prev_committee_id = prev.committee_id;
            tx_undo.sidechain_undo.prev_checkpoint_policy_id = prev.checkpoint_policy_id;
            tx_undo.sidechain_undo.prev_checkpoint_version = prev.checkpoint_version;
        }
    }
}

bool ChainState::revert_sidechain_undo_(const TxUndo& tx_undo) {
    if (tx_undo.deposit_undo.kind != 0) {
        const std::string id_hex = hash256_to_hex(tx_undo.deposit_undo.sidechain_id);
        if (tx_undo.deposit_undo.kind == 1) {
            sidechain_deposits_.erase(id_hex);
            delete_sidechain_deposit_from_storage(tx_undo.deposit_undo.sidechain_id);
        } else if (tx_undo.deposit_undo.kind == 2 || tx_undo.deposit_undo.kind == 3) {
            SidechainDeposit deposit;
            deposit.owner = Address(tx_undo.deposit_undo.prev_owner);
            deposit.amount = tx_undo.deposit_undo.prev_amount;
            deposit.unlock_height = tx_undo.deposit_undo.prev_unlock_height;
            deposit.checkpoint = tx_undo.deposit_undo.prev_checkpoint;
            deposit.last_checkpoint_height = tx_undo.deposit_undo.prev_last_checkpoint_height;
            deposit.last_sidechain_checkpoint_height =
                tx_undo.deposit_undo.prev_last_sidechain_checkpoint_height;
            sidechain_deposits_[id_hex] = deposit;
            persist_sidechain_deposit_to_storage(tx_undo.deposit_undo.sidechain_id, deposit);
        }
    }

    if (!tx_undo.deposit_undo_batch.empty()) {
        for (const auto& du : tx_undo.deposit_undo_batch) {
            if (du.kind == 0) {
                continue;
            }
            const std::string id_hex = hash256_to_hex(du.sidechain_id);
            if (du.kind == 1) {
                sidechain_deposits_.erase(id_hex);
                delete_sidechain_deposit_from_storage(du.sidechain_id);
            } else if (du.kind == 2 || du.kind == 3) {
                SidechainDeposit deposit;
                deposit.owner = Address(du.prev_owner);
                deposit.amount = du.prev_amount;
                deposit.unlock_height = du.prev_unlock_height;
                deposit.checkpoint = du.prev_checkpoint;
                deposit.last_checkpoint_height = du.prev_last_checkpoint_height;
                deposit.last_sidechain_checkpoint_height = du.prev_last_sidechain_checkpoint_height;
                sidechain_deposits_[id_hex] = deposit;
                persist_sidechain_deposit_to_storage(du.sidechain_id, deposit);
            }
        }
    }

    if (tx_undo.sidechain_undo.kind != 0) {
        const std::string id_hex = hash256_to_hex(tx_undo.sidechain_undo.sidechain_id);
        if (tx_undo.sidechain_undo.kind == 1) {
            sidechain_registry_.erase(id_hex);
            delete_sidechain_registry_from_storage(tx_undo.sidechain_undo.sidechain_id);
        } else if (tx_undo.sidechain_undo.kind == 2) {
            SidechainRegistryEntry entry;
            entry.owner = Address(tx_undo.sidechain_undo.prev_owner);
            entry.issuance_limit = tx_undo.sidechain_undo.prev_limit;
            entry.total_issued = tx_undo.sidechain_undo.prev_issued;
            entry.kind = tx_undo.sidechain_undo.prev_kind;
            entry.status = tx_undo.sidechain_undo.prev_status;
            entry.decommission_height = tx_undo.sidechain_undo.prev_decommission_height;
            entry.multiplier = tx_undo.sidechain_undo.prev_multiplier;
            entry.fee_bps = tx_undo.sidechain_undo.prev_fee_bps;
            entry.max_multiplier = tx_undo.sidechain_undo.prev_max_multiplier;
            entry.token_symbol = tx_undo.sidechain_undo.prev_symbol;
            entry.accounting_namespace = tx_undo.sidechain_undo.prev_accounting_namespace;
            entry.accounting_schema_hash = tx_undo.sidechain_undo.prev_accounting_schema_hash;
            entry.committee_m = tx_undo.sidechain_undo.prev_committee_m;
            entry.committee_n = tx_undo.sidechain_undo.prev_committee_n;
            entry.committee_members.clear();
            for (const auto& s : tx_undo.sidechain_undo.prev_committee_members) {
                entry.committee_members.emplace_back(Address(s));
            }
            entry.committee_id = tx_undo.sidechain_undo.prev_committee_id;
            entry.checkpoint_policy_id = tx_undo.sidechain_undo.prev_checkpoint_policy_id;
            entry.checkpoint_version = tx_undo.sidechain_undo.prev_checkpoint_version;
            sidechain_registry_[id_hex] = entry;
            persist_sidechain_registry_to_storage(tx_undo.sidechain_undo.sidechain_id, entry);
        }
    }
    return true;
}

} // namespace geocoin::state
