#include "geocoin/state/state.hpp"

#include "geocoin/consensus/chainparams.hpp"
#include "geocoin/consensus/consensus.hpp"
#include "geocoin/crypto/addresses.hpp"
#include "geocoin/crypto/signatures.hpp"
#include "geocoin/nft/nft_validator.hpp"
#include "geocoin/utils/bytes.hpp"
#include "geocoin/utils/logger.hpp"

#include <exception>
#include <limits>
#include <set>
#include <utility>

namespace geocoin::state {

bool ChainState::apply_nft_transaction_(const core::Transaction& tx, Height height) {
    const Hash256 tx_hash = tx.get_hash();

    const auto poison_transition = [this] {
        derived_state_storage_valid_ = false;
        state_transition_poisoned_ = true;
    };
    const auto delete_rejected_transaction = [this, &tx_hash]() {
        if (!storage_) {
            return true;
        }
        try {
            return storage_->delete_transaction(tx_hash);
        } catch (const std::exception& error) {
            GEO_LOG_ERROR(
                "chainstate",
                "Failed to delete rejected NFT transaction: " << error.what()
            );
        } catch (...) {
            GEO_LOG_ERROR("chainstate", "Failed to delete rejected NFT transaction");
        }
        return false;
    };
    const auto restore_persistence = [this](
        const Hash256& nft_id,
        const nft::NFTMetadata& metadata,
        const Address& owner
    ) {
        return persist_nft_metadata_to_storage(nft_id, metadata) &&
               persist_nft_owner_to_storage(nft_id, owner);
    };
    const auto restore_registry = [this](
        const Hash256& nft_id,
        const nft::NFTMetadata& metadata,
        const Address& owner
    ) {
        nft::NFTCreationTransaction restore;
        restore.nft_id = nft_id;
        restore.metadata = metadata;
        restore.creator_address = owner;
        return nft_registry_.register_nft(restore);
    };

    if (tx.type == core::TransactionType::NFT_CREATE) {
        if (!tx.nft_create ||
            !nft::NFTValidator::validate_nft_creation(*tx.nft_create, nft_registry_) ||
            !process_standard_utxo_effects_(tx, height, false, true)) {
            return false;
        }

        const Hash256& nft_id = tx.nft_create->nft_id;
        const std::string key = hash256_to_hex(nft_id);
        const auto cleanup_persistence = [this, &nft_id, &key, &delete_rejected_transaction] {
            if (!storage_) {
                return true;
            }

            const bool owner_and_metadata = delete_nft_from_storage(nft_id);
            bool index_ok = false;
            try {
                index_ok = storage_->delete_metadata(idx_meta_key("nft", key));
            } catch (const std::exception& error) {
                GEO_LOG_ERROR(
                    "chainstate",
                    "Failed to delete rejected NFT uniqueness index: " << error.what()
                );
            } catch (...) {
                GEO_LOG_ERROR(
                    "chainstate",
                    "Failed to delete rejected NFT uniqueness index"
                );
            }
            const bool transaction_ok = delete_rejected_transaction();
            return owner_and_metadata && index_ok && transaction_ok;
        };

        bool persistence_ok = true;
        try {
            if (storage_) {
                persistence_ok =
                    persist_nft_metadata_to_storage(nft_id, tx.nft_create->metadata) &&
                    persist_nft_owner_to_storage(nft_id, tx.nft_create->creator_address) &&
                    storage_->put_metadata(idx_meta_key("nft", key), bytes{1}) &&
                    storage_->put_transaction(tx_hash, tx);
            }
        } catch (const std::exception& error) {
            GEO_LOG_ERROR(
                "chainstate",
                "Failed to prepare NFT create persistence: " << error.what()
            );
            persistence_ok = false;
        } catch (...) {
            GEO_LOG_ERROR("chainstate", "Failed to prepare NFT create persistence");
            persistence_ok = false;
        }

        if (!persistence_ok || !nft_registry_.register_nft(*tx.nft_create)) {
            if (!cleanup_persistence()) {
                poison_transition();
            }
            return false;
        }
        if (!process_standard_utxo_effects_(tx, height, true, true)) {
            if (!nft_registry_.burn_nft(nft_id) || !cleanup_persistence()) {
                poison_transition();
            }
            return false;
        }

        nft_id_keys_.insert(key);
        return true;
    }

    const auto rollback_removed_nft = [
        &restore_persistence,
        &restore_registry,
        &delete_rejected_transaction,
        &poison_transition
    ](
        const Hash256& nft_id,
        const nft::NFTMetadata& metadata,
        const Address& owner
    ) {
        const bool registry_ok = restore_registry(nft_id, metadata, owner);
        const bool persistence_ok = restore_persistence(nft_id, metadata, owner);
        const bool transaction_ok = delete_rejected_transaction();
        if (!registry_ok || !persistence_ok || !transaction_ok) {
            poison_transition();
        }
    };

    if (tx.type == core::TransactionType::NFT_TRANSFER) {
        if (!tx.nft_transfer ||
            !nft::NFTValidator::validate_nft_transfer(*tx.nft_transfer, nft_registry_) ||
            !process_standard_utxo_effects_(tx, height, false, true)) {
            return false;
        }

        const Hash256& nft_id = tx.nft_transfer->nft_id;
        const auto metadata = nft_registry_.get_metadata(nft_id);
        const Address previous_owner = nft_registry_.get_owner(nft_id);
        if (nft::NFTValidator::transfer_burns_nft(metadata)) {
            GEO_LOG_INFO("nft", "Cadastral NFT transfer -> auto burn");

            bool persistence_ok = true;
            try {
                persistence_ok = !storage_ ||
                    (storage_->put_transaction(tx_hash, tx) &&
                     delete_nft_from_storage(nft_id));
            } catch (...) {
                persistence_ok = false;
            }
            if (!persistence_ok) {
                const bool restored =
                    restore_persistence(nft_id, metadata, previous_owner);
                const bool transaction_ok = delete_rejected_transaction();
                if (!restored || !transaction_ok) {
                    poison_transition();
                }
                return false;
            }

            if (!nft_registry_.burn_nft(nft_id)) {
                (void)restore_persistence(nft_id, metadata, previous_owner);
                (void)delete_rejected_transaction();
                return false;
            }
            if (!process_standard_utxo_effects_(tx, height, true, true)) {
                rollback_removed_nft(nft_id, metadata, previous_owner);
                return false;
            }
            return true;
        }

        bool persistence_ok = true;
        try {
            persistence_ok = !storage_ ||
                (storage_->put_transaction(tx_hash, tx) &&
                 persist_nft_owner_to_storage(nft_id, tx.nft_transfer->to));
        } catch (...) {
            persistence_ok = false;
        }
        if (!persistence_ok) {
            const bool owner_ok = persist_nft_owner_to_storage(nft_id, previous_owner);
            const bool transaction_ok = delete_rejected_transaction();
            if (!owner_ok || !transaction_ok) {
                poison_transition();
            }
            return false;
        }
        if (!nft_registry_.transfer_nft(nft_id, tx.nft_transfer->to)) {
            (void)persist_nft_owner_to_storage(nft_id, previous_owner);
            (void)delete_rejected_transaction();
            return false;
        }
        if (!process_standard_utxo_effects_(tx, height, true, true)) {
            const bool registry_ok = nft_registry_.transfer_nft(nft_id, previous_owner);
            const bool owner_ok = persist_nft_owner_to_storage(nft_id, previous_owner);
            const bool transaction_ok = delete_rejected_transaction();
            if (!registry_ok || !owner_ok || !transaction_ok) {
                poison_transition();
            }
            return false;
        }
        return true;
    }

    if (tx.type == core::TransactionType::NFT_BURN) {
        if (!tx.nft_burn ||
            !nft::NFTValidator::validate_nft_burn(*tx.nft_burn, nft_registry_) ||
            !process_standard_utxo_effects_(tx, height, false, true)) {
            return false;
        }

        const Hash256& nft_id = tx.nft_burn->nft_id;
        const auto metadata = nft_registry_.get_metadata(nft_id);
        const Address previous_owner = nft_registry_.get_owner(nft_id);
        bool persistence_ok = true;
        try {
            persistence_ok = !storage_ ||
                (storage_->put_transaction(tx_hash, tx) &&
                 delete_nft_from_storage(nft_id));
        } catch (...) {
            persistence_ok = false;
        }
        if (!persistence_ok) {
            const bool restored = restore_persistence(nft_id, metadata, previous_owner);
            const bool transaction_ok = delete_rejected_transaction();
            if (!restored || !transaction_ok) {
                poison_transition();
            }
            return false;
        }

        if (!nft_registry_.burn_nft(nft_id)) {
            (void)restore_persistence(nft_id, metadata, previous_owner);
            (void)delete_rejected_transaction();
            return false;
        }
        if (!process_standard_utxo_effects_(tx, height, true, true)) {
            rollback_removed_nft(nft_id, metadata, previous_owner);
            return false;
        }
        return true;
    }

    return false;
}

bool ChainState::validate_nft_transaction_(const core::Transaction& tx) const {
    // The NFT payload signature authorizes from/to/fee, but it is not bound to
    // the outer inputs. A copied signature must not move or burn an NFT using
    // somebody else's coins, including after the asset returns to the signer.
    const auto spends_owner_output = [this](const core::Transaction& candidate, const Address& owner) {
        if (owner.str().empty()) {
            return false;
        }
        for (const auto& input : candidate.inputs) {
            const auto* utxo = find_utxo_unlocked_(input.prev_tx_hash, input.vout);
            if (utxo && utxo->address.str() == owner.str()) {
                return true;
            }
        }
        return false;
    };

    if (tx.type == core::TransactionType::NFT_CREATE) {
        if (!tx.nft_create.has_value()) {
            GEO_LOG_ERROR("chainstate", "NFT_CREATE missing payload");
            return false;
        }
        return nft::NFTValidator::validate_nft_creation(*tx.nft_create, nft_registry_) &&
               verify_owned_utxo_inputs_(tx);
    }
    if (tx.type == core::TransactionType::NFT_TRANSFER) {
        if (!tx.nft_transfer.has_value()) {
            GEO_LOG_ERROR("chainstate", "NFT_TRANSFER missing payload");
            return false;
        }
        Amount actual_fee = 0;
        return nft::NFTValidator::validate_nft_transfer(*tx.nft_transfer, nft_registry_) &&
               verify_owned_utxo_inputs_(tx, &actual_fee) &&
               tx.nft_transfer->fee == actual_fee &&
               spends_owner_output(tx, tx.nft_transfer->from);
    }
    if (tx.type == core::TransactionType::NFT_BURN) {
        if (!tx.nft_burn.has_value()) {
            GEO_LOG_ERROR("chainstate", "NFT_BURN missing payload");
            return false;
        }
        Amount actual_fee = 0;
        return nft::NFTValidator::validate_nft_burn(*tx.nft_burn, nft_registry_) &&
               verify_owned_utxo_inputs_(tx, &actual_fee) &&
               tx.nft_burn->fee == actual_fee &&
               spends_owner_output(tx, tx.nft_burn->owner);
    }
    return false;
}

void ChainState::prepare_nft_undo_(const core::Transaction& tx, TxUndo& tx_undo) const {
    if (tx.type == core::TransactionType::NFT_CREATE && tx.nft_create.has_value()) {
        tx_undo.nft_undo.kind = 1;
        tx_undo.nft_undo.nft_id = tx.nft_create->nft_id;
    } else if (tx.type == core::TransactionType::NFT_TRANSFER && tx.nft_transfer.has_value()) {
        const auto meta = nft_registry_.get_metadata(tx.nft_transfer->nft_id);
        if (nft::NFTValidator::transfer_burns_nft(meta)) {
            tx_undo.nft_undo.kind = 3;
            tx_undo.nft_undo.nft_id = tx.nft_transfer->nft_id;
            Address prev_owner = nft_registry_.get_owner(tx.nft_transfer->nft_id);
            tx_undo.nft_undo.prev_owner = prev_owner.str();
            tx_undo.nft_undo.prev_metadata = meta.serialize();
        } else {
            tx_undo.nft_undo.kind = 2;
            tx_undo.nft_undo.nft_id = tx.nft_transfer->nft_id;
            Address prev_owner = nft_registry_.get_owner(tx.nft_transfer->nft_id);
            tx_undo.nft_undo.prev_owner = prev_owner.str();
        }
    } else if (tx.type == core::TransactionType::NFT_BURN && tx.nft_burn.has_value()) {
        tx_undo.nft_undo.kind = 3;
        tx_undo.nft_undo.nft_id = tx.nft_burn->nft_id;
        Address prev_owner = nft_registry_.get_owner(tx.nft_burn->nft_id);
        tx_undo.nft_undo.prev_owner = prev_owner.str();
        tx_undo.nft_undo.prev_metadata = nft_registry_.get_metadata(tx.nft_burn->nft_id).serialize();
    }
}

bool ChainState::revert_nft_undo_(const TxUndo& tx_undo) {
    if (tx_undo.nft_undo.kind == 0) {
        return true;
    }
    const std::string id_hex = hash256_to_hex(tx_undo.nft_undo.nft_id);
    if (tx_undo.nft_undo.kind == 1) {
        // NFT_CREATE -> удалить созданный NFT
        if (!nft_registry_.burn_nft(tx_undo.nft_undo.nft_id)) {
            GEO_LOG_ERROR("chainstate", "NFT create undo references a missing NFT");
            return false;
        }
        nft_id_keys_.erase(id_hex);
        const bool index_deleted = !storage_ ||
            storage_->delete_metadata(idx_meta_key("nft", id_hex));
        if (!index_deleted || !delete_nft_from_storage(tx_undo.nft_undo.nft_id)) {
            GEO_LOG_ERROR("chainstate", "Failed to persist NFT create undo");
            return false;
        }
    } else if (tx_undo.nft_undo.kind == 2) {
        // NFT_TRANSFER -> восстановить прежнего владельца
        if (tx_undo.nft_undo.prev_owner.empty()) {
            GEO_LOG_ERROR("chainstate", "NFT transfer undo is incomplete or references a missing NFT");
            return false;
        }
        const Address previous_owner(tx_undo.nft_undo.prev_owner);
        if (!persist_nft_owner_to_storage(tx_undo.nft_undo.nft_id, previous_owner) ||
            !nft_registry_.transfer_nft(tx_undo.nft_undo.nft_id, previous_owner)) {
            GEO_LOG_ERROR("chainstate", "Failed to restore NFT transfer state");
            return false;
        }
    } else if (tx_undo.nft_undo.kind == 3) {
        // NFT_BURN -> восстановить NFT через повторную регистрацию метаданных и владельца
        if (tx_undo.nft_undo.prev_metadata.empty() ||
            tx_undo.nft_undo.prev_owner.empty()) {
            GEO_LOG_ERROR("chainstate", "NFT burn undo is missing metadata or owner");
            return false;
        }

        nft::NFTCreationTransaction create_tx;
        create_tx.nft_id = tx_undo.nft_undo.nft_id;
        create_tx.creator_address = Address(tx_undo.nft_undo.prev_owner);
        create_tx.royalty_percent = 0;
        try {
            create_tx.metadata = nft::NFTMetadata::deserialize(tx_undo.nft_undo.prev_metadata);
        } catch (const std::exception&) {
            GEO_LOG_ERROR("chainstate", "NFT burn undo contains malformed metadata");
            return false;
        }
        if (create_tx.metadata.calculate_hash() != create_tx.nft_id) {
            GEO_LOG_ERROR("chainstate", "NFT burn undo metadata does not match its NFT id");
            return false;
        }

        // Сначала восстановить обе половины persistent-записи, затем
        // публиковать NFT в памяти. Так ошибка хранилища не создаёт объект,
        // который исчезнет после перезапуска.
        const Address previous_owner(tx_undo.nft_undo.prev_owner);
        if (!persist_nft_metadata_to_storage(tx_undo.nft_undo.nft_id, create_tx.metadata) ||
            !persist_nft_owner_to_storage(tx_undo.nft_undo.nft_id, previous_owner)) {
            (void)delete_nft_from_storage(tx_undo.nft_undo.nft_id);
            GEO_LOG_ERROR("chainstate", "Failed to persist NFT burn undo");
            return false;
        }
        if (!nft_registry_.register_nft(create_tx)) {
            (void)delete_nft_from_storage(tx_undo.nft_undo.nft_id);
            GEO_LOG_ERROR("chainstate", "Failed to restore NFT burn registry state");
            return false;
        }
    }
    return true;
}

} // namespace geocoin::state
