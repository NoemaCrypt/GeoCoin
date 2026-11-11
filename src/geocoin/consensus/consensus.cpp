#include "geocoin/consensus/consensus.hpp"
#include "geocoin/consensus/consensus_internal.hpp"
#include <exception>

namespace geocoin::consensus {

bool check_transaction(
    const core::Transaction& tx,
    ValidationState& state
) {
    // Prod: сеть запускается с единственным форматом транзакций.
    // Любая версия, отличная от 2, считается недопустимой.
    if (tx.version != 2) {
        return state.invalid(RejectCode::INVALID, "bad-tx-version", "transaction version must be 2");
    }
    if (!core::is_known_transaction_type(tx.type)) {
        return state.invalid(RejectCode::INVALID, "bad-tx-type", "unknown transaction type");
    }
    if (tx.is_coinbase() && tx.type != core::TransactionType::STANDARD) {
        return state.invalid(RejectCode::INVALID, "bad-cb-type", "coinbase must be a standard transaction");
    }
    if (tx.witness_version != params::WITNESS_FORMAT_VERSION_CURRENT) {
        return state.invalid(RejectCode::INVALID, "bad-witness-version", "unsupported witness version");
    }

    // Проверяем количество inputs/outputs до сериализации (защита от DoS).
    if (tx.inputs.size() > params::MAX_TX_INPUTS) {
        return state.invalid(RejectCode::INVALID, "bad-txns-too-many-inputs",
            "too many inputs");
    }

    if (tx.outputs.size() > params::MAX_TX_OUTPUTS) {
        return state.invalid(RejectCode::INVALID, "bad-txns-too-many-outputs",
            "too many outputs");
    }

    // Match the wire reader before serialization, allocation or typed early
    // returns. Otherwise a locally accepted object may be unreadable on disk
    // or by another node. Missing witness slots serialize as empty records.
    if (tx.witnesses.size() > tx.inputs.size()) {
        return state.invalid(RejectCode::INVALID, "bad-txns-witness-count", "too many witnesses for inputs");
    }
    for (const auto& input : tx.inputs) {
        if (input.script_sig.size() > params::MAX_SCRIPT_SIG_SIZE) {
            return state.invalid(RejectCode::INVALID, "bad-txns-script-sig-size", "input script too large");
        }
    }
    for (const auto& output : tx.outputs) {
        if (output.address.str().size() > params::MAX_ADDRESS_SIZE) {
            return state.invalid(RejectCode::INVALID, "bad-txns-address-size", "output address too long");
        }
        if (output.script_pubkey.size() > params::MAX_SCRIPT_PUBKEY_SIZE) {
            return state.invalid(RejectCode::INVALID, "bad-txns-script-pubkey-size", "output script too large");
        }
    }
    for (const auto& witness : tx.witnesses) {
        if (!witness.public_key.empty() && witness.public_key.size() != params::WITNESS_PUBKEY_SIZE) {
            return state.invalid(RejectCode::INVALID, "bad-txns-witness-pubkey-size", "invalid witness public key size");
        }
        if (witness.signature.size() > params::MAX_WITNESS_SIG_SIZE) {
            return state.invalid(RejectCode::INVALID, "bad-txns-witness-signature-size", "witness signature too large");
        }
    }

    // Payload presence must be checked before serialize(): serialize() is allowed
    // to throw for internally malformed typed transactions, but consensus
    // validation must turn malformed input into a reject instead of an exception.
    if (const char* field = tx.unused_payload_field()) {
        return state.invalid(RejectCode::INVALID, "bad-txns-unused-field", field);
    }
    if (tx.type == core::TransactionType::NFT_CREATE && !tx.nft_create.has_value()) {
        return state.invalid(RejectCode::INVALID, "bad-nft-create", "NFT_CREATE missing payload");
    }
    if (tx.type == core::TransactionType::NFT_TRANSFER && !tx.nft_transfer.has_value()) {
        return state.invalid(RejectCode::INVALID, "bad-nft-transfer", "NFT_TRANSFER missing payload");
    }
    if (tx.type == core::TransactionType::NFT_BURN && !tx.nft_burn.has_value()) {
        return state.invalid(RejectCode::INVALID, "bad-nft-burn", "NFT_BURN missing payload");
    }

    // Проверяем размер
    size_t tx_size = 0;
    try {
        tx_size = tx.serialize().size();
    } catch (const std::exception& e) {
        return state.invalid(RejectCode::INVALID, "bad-txns-serialization", e.what());
    } catch (...) {
        return state.invalid(RejectCode::INVALID, "bad-txns-serialization", "transaction serialization failed");
    }
    if (tx_size > params::MAX_TX_SIZE) {
        return state.invalid(RejectCode::INVALID, "bad-txns-oversize", "transaction too large");
    }

    if (tx.is_coinbase()) {
        return check_common_utxo_transaction(tx, state);
    }

    // === Проверка в зависимости от типа транзакции ===

    {
        bool multisig_complete = false;
        if (!check_multisig_transaction(tx, state, multisig_complete)) {
            return false;
        }
        if (multisig_complete) {
            return true;
        }
    }

    if (core::is_nft_transition_type(tx.type) && !check_nft_transaction(tx, state)) {
        return false;
    }

    if (!check_cadastral_transaction(tx, state)) {
        return false;
    }
    if (core::is_card_transition_type(tx.type) && !check_card_transaction(tx, state)) {
        return false;
    }

    if (!check_foundation_transaction(tx, state)) {
        return false;
    }

    if (core::is_sidechain_transition_type(tx.type)) {
        bool sidechain_complete = false;
        if (!check_sidechain_transaction(tx, state, sidechain_complete)) {
            return false;
        }
        if (sidechain_complete) {
            return true;
        }
    }

    // === Обычные проверки для STANDARD и MULTISIG_SPEND ===
    return check_common_utxo_transaction(tx, state);
}

bool contextual_check_transaction(
    const core::Transaction& tx,
    ValidationState& state,
    const ContextualTxLookup& lookup
) {
    if (!lookup.signatures_verified) {
        if (!tx.verify()) {
            return state.invalid(RejectCode::INVALID, "bad-txns-signature", "transaction signature failed");
        }
    }

    if (!contextual_check_nft_transaction(tx, state, lookup)) {
        return false;
    }

    // Пропускаем coinbase транзакции
    if (tx.is_coinbase()) {
        return true;
    }

    if (lookup.get_transaction_by_txid) {
        core::Transaction existing_tx;
        if (lookup.get_transaction_by_txid(tx.get_hash(), existing_tx)) {
            return state.invalid(RejectCode::INVALID, "bad-txns-duplicate",
                "transaction already exists in active chain or mempool");
        }
    }

    if (!contextual_check_multisig_transaction(tx, state, lookup)) {
        return false;
    }

    return true;
}

} // namespace geocoin::consensus
