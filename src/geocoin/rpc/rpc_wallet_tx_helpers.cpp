#include "geocoin/rpc/rpc_amount.hpp"
#include "geocoin/rpc/rpc_wallet_helpers.hpp"
#include "geocoin/rpc/rpc_params.hpp"
#include "geocoin/consensus/consensus.hpp"

namespace geocoin::rpc {

namespace {
std::string labeled_error(const std::string& label, const std::string& message) {
    if (label.empty()) {
        return message;
    }
    return "[" + label + "] " + message;
}
}

core::Transaction deserialize_transaction_exact(const bytes& tx_data, const std::string& context_label) {
    if (tx_data.size() > consensus::params::MAX_TX_SIZE) {
        throw RPCException(RPC_INVALID_PARAMETER, context_label + ": transaction exceeds consensus size limit");
    }
    try {
        return core::Transaction::deserialize_canonical(tx_data);
    } catch (const std::exception& e) {
        throw RPCException(RPC_INVALID_PARAMETER, context_label + ": " + e.what());
    }
}

core::Transaction deserialize_transaction_hex_or_throw(
    const std::string& hex,
    const std::string& invalid_hex_message,
    const std::string& context_label
) {
    return deserialize_transaction_exact(parse_hex_bytes_or_throw(hex, invalid_hex_message), context_label);
}

std::string transaction_txid_hex(const core::Transaction& tx) {
    return hash256_to_hex_string(tx.get_hash());
}

bool requires_basic_signature_verification(core::TransactionType type) {
    return type != core::TransactionType::MULTISIG_SPEND &&
           type != core::TransactionType::DELEGATION_PROOF &&
           type != core::TransactionType::REVOKE_DELEGATION &&
           type != core::TransactionType::STANDARD;
}

RawTransactionPreflight validate_raw_transaction_for_mempool(
    const core::Transaction& tx,
    state::ChainState& chain_state,
    network::Mempool& mempool
) {
    RawTransactionPreflight out;
    out.txid = transaction_txid_hex(tx);
    out.type = static_cast<int>(tx.type);

    out.verified = true;
    if (requires_basic_signature_verification(tx.type)) {
        out.verified = tx.verify();
    }
    if (!out.verified) {
        out.reject_reason = "tx.verify() failed";
        return out;
    }

    out.valid = chain_state.validate_transaction(tx);
    if (!out.valid) {
        out.reject_reason = "chainstate validation failed";
        return out;
    }

    consensus::ValidationState st;
    consensus::ContextualTxLookup lookup;
    lookup.get_transaction_by_txid = [&chain_state, &mempool](const Hash256& hash, core::Transaction& out_tx) -> bool {
        auto tx_ptr = chain_state.get_transaction(hash);
        if (tx_ptr) {
            out_tx = *tx_ptr;
            return true;
        }
        return mempool.get_transaction(hash, out_tx);
    };
    lookup.has_angle_commitment_key = [&chain_state](const Hash256& key) -> bool {
        return chain_state.has_angle_commitment_key(key);
    };
    lookup.has_multisig_spend_key = [&chain_state](const Hash256& key) -> bool {
        return chain_state.has_multisig_spend_key(key);
    };
    lookup.has_hierarchical_spend_key = [&chain_state](const Hash256& key) -> bool {
        return chain_state.has_hierarchical_spend_key(key);
    };
    lookup.has_delegation_proof_key = [&chain_state](const Hash256& key) -> bool {
        return chain_state.has_delegation_proof_key(key);
    };
    lookup.has_revoke_delegation_key = [&chain_state](const Hash256& key) -> bool {
        return chain_state.has_revoke_delegation_key(key);
    };
    lookup.has_nft_id_key = [&chain_state](const Hash256& key) -> bool {
        return chain_state.has_nft_id_key(key);
    };
    lookup.has_sidechain_deposit_key = [&chain_state](const Hash256& key) -> bool {
        return chain_state.has_sidechain_deposit_key(key);
    };
    lookup.has_sidechain_registry_key = [&chain_state](const Hash256& key) -> bool {
        return chain_state.has_sidechain_registry_key(key);
    };
    lookup.signatures_verified = true;

    out.contextual = consensus::contextual_check_transaction(tx, st, lookup);
    if (!out.contextual) {
        out.reject_reason = st.get_reject_reason();
        return out;
    }

    std::string mempool_reject_reason;
    if (!mempool.test_accept_transaction(tx, &mempool_reject_reason)) {
        out.contextual = false;
        out.reject_reason = mempool_reject_reason.empty()
            ? "mempool admission rejected"
            : mempool_reject_reason;
    }
    return out;
}

json raw_transaction_preflight_to_json(
    const RawTransactionPreflight& preflight,
    bool include_type
) {
    json result;
    result["txid"] = preflight.txid;
    result["allowed"] = preflight.allowed();
    if (include_type) {
        result["type"] = preflight.type;
    }
    result["verified"] = preflight.verified;
    result["valid"] = preflight.valid;
    result["contextual"] = preflight.contextual;
    if (!preflight.reject_reason.empty()) {
        result["reject_reason"] = preflight.reject_reason;
        result["reject-reason"] = preflight.reject_reason;
    }
    return result;
}

std::string submit_transaction_to_mempool(
    const core::Transaction& tx,
    const std::shared_ptr<state::ChainState>& chain_state,
    const std::string& label,
    bool verify_signature,
    bool include_type_in_validation_error,
    const std::shared_ptr<services::ChainMutationService>& chain_mutations
) {
    if (verify_signature && !tx.verify()) {
        throw RPCException(RPC_VERIFY_ERROR, labeled_error(label, "Верификация транзакции не прошла"));
    }
    if (!chain_state) {
        throw RPCException(RPC_INTERNAL_ERROR, "ChainState не инициализирован");
    }
    if (!chain_state->validate_transaction(tx)) {
        std::string message = "Валидация транзакции не прошла";
        if (include_type_in_validation_error) {
            std::ostringstream oss;
            oss << message << " (type=" << static_cast<int>(tx.type) << ")";
            message = oss.str();
        }
        throw RPCException(RPC_VERIFY_ERROR, labeled_error(label, message));
    }
    if (!chain_mutations) {
        throw RPCException(RPC_INTERNAL_ERROR, "ChainMutationService не инициализирован");
    }
    const auto admission = chain_mutations->admit_transaction(tx);
    if (!admission.is_accepted()) {
        const std::string reason = admission.reject_reason.empty()
            ? "Не удалось добавить транзакцию в mempool"
            : admission.reject_reason;
        const RPCErrorCode error_code =
            admission.outcome == services::SubmissionOutcome::REJECTED
                ? RPC_VERIFY_ERROR
                : RPC_INTERNAL_ERROR;
        throw RPCException(error_code, labeled_error(label, reason));
    }
    return transaction_txid_hex(tx);
}

std::string submit_wallet_transaction_to_mempool(
    wallet::Wallet& wallet,
    const core::Transaction& tx,
    const std::shared_ptr<state::ChainState>& chain_state,
    const std::string& label,
    bool verify_signature,
    bool include_type_in_validation_error,
    const std::shared_ptr<services::ChainMutationService>& chain_mutations
) {
    try {
        return submit_transaction_to_mempool(
            tx,
            chain_state,
            label,
            verify_signature,
            include_type_in_validation_error,
            chain_mutations
        );
    } catch (...) {
        wallet.discard_unconfirmed_transaction(tx.get_hash());
        throw;
    }
}

std::vector<core::Transaction> pending_mempool_transactions(const network::Mempool* mempool) {
    return mempool ? mempool->get_all_transactions() : std::vector<core::Transaction>{};
}

std::string submit_raw_transaction_hex_to_mempool(
    const std::string& hex,
    const std::shared_ptr<state::ChainState>& chain_state,
    const std::string& label,
    bool verify_signature,
    bool include_type_in_validation_error,
    const std::shared_ptr<services::ChainMutationService>& chain_mutations
) {
    const std::string invalid_hex_message =
        label.empty() ? "Неверная hex-строка транзакции" : "Неверная hex-строка транзакции (" + label + ")";
    core::Transaction tx = deserialize_transaction_hex_or_throw(
        hex,
        invalid_hex_message,
        label.empty() ? "tx_hex" : std::string("tx_hex (") + label + ")"
    );
    return submit_transaction_to_mempool(
        tx,
        chain_state,
        label,
        verify_signature,
        include_type_in_validation_error,
        chain_mutations
    );
}

json submit_commitment_package_from_rpc(
    const json& payload,
    const std::string& final_tx_field,
    const std::string& final_result_field,
    const std::shared_ptr<state::ChainState>& chain_state,
    bool include_type_in_validation_error,
    const std::shared_ptr<services::ChainMutationService>& chain_mutations
) {
    json result;
    result["commitment_txids"] = json::array();
    for (size_t idx = 0; idx < payload["commitments"].size(); ++idx) {
        const auto& commitment_hex = payload["commitments"][idx];
        if (!commitment_hex.is_string()) {
            throw RPCException(RPC_INVALID_PARAMETER, "commitments должен содержать только hex-строки");
        }
        const std::string label = "commitment[" + std::to_string(idx) + "]";
        result["commitment_txids"].push_back(
            submit_raw_transaction_hex_to_mempool(
                commitment_hex.get<std::string>(),
                chain_state,
                label,
                true,
                include_type_in_validation_error,
                chain_mutations
            )
        );
    }

    result[final_result_field] = submit_raw_transaction_hex_to_mempool(
        payload[final_tx_field].get<std::string>(),
        chain_state,
        final_tx_field,
        true,
        include_type_in_validation_error,
        chain_mutations
    );
    return result;
}

json transaction_hex_result(const core::Transaction& tx) {
    json out;
    out["tx_hex"] = bytes_to_hex_string(tx.serialize());
    out["txid"] = transaction_txid_hex(tx);
    return out;
}

void set_committee_signatures_from_rpc(
    std::vector<bytes>& signatures,
    const json& signatures_json,
    const std::string& invalid_shape_message
) {
    signatures.clear();
    if (signatures_json.is_array()) {
        for (const auto& s : signatures_json) {
            if (!s.is_string()) {
                throw RPCException(RPC_INVALID_PARAMETER, "signature должен быть hex строкой");
            }
            signatures.push_back(parse_hex_bytes_or_throw(s.get<std::string>(), "signature должен быть hex строкой"));
        }
    } else if (signatures_json.is_string()) {
        signatures.push_back(parse_hex_bytes_or_throw(signatures_json.get<std::string>(), "signature должен быть hex строкой"));
    } else {
        throw RPCException(RPC_INVALID_PARAMETER, invalid_shape_message);
    }
}

void set_batch_checkpoint_signatures_from_rpc(core::Transaction& tx, const json& signatures_json) {
    if (!signatures_json.is_array()) {
        throw RPCException(RPC_INVALID_PARAMETER, "signatures должен быть массивом");
    }
    if (signatures_json.size() != tx.sidechain_batch_checkpoints.size()) {
        throw RPCException(RPC_INVALID_PARAMETER, "signatures size не совпадает с items");
    }
    for (size_t i = 0; i < tx.sidechain_batch_checkpoints.size(); i++) {
        set_committee_signatures_from_rpc(
            tx.sidechain_batch_checkpoints[i].committee_signatures,
            signatures_json[i],
            "signatures[i] должен быть массивом или строкой"
        );
    }
}

json finalize_committee_signed_transaction(
    const json& params,
    core::TransactionType expected_type,
    const std::string& invalid_hex_message,
    const std::string& wrong_type_message
) {
    require_min_params(params, 2, "Нужно tx_hex и массив подписей");
    core::Transaction tx = deserialize_transaction_hex_or_throw(
        params[0].get<std::string>(),
        invalid_hex_message,
        "tx_hex"
    );
    if (tx.type != expected_type) {
        throw RPCException(RPC_INVALID_PARAMETER, wrong_type_message);
    }
    set_committee_signatures_from_rpc(tx.sidechain_committee_signatures, params[1]);
    return transaction_hex_result(tx);
}

json finalize_nft_transaction_signature(
    const json& params,
    core::TransactionType expected_type,
    const std::string& expected_type_name
) {
    require_min_params(params, 2, "Требуется tx_hex и signature_hex");

    core::Transaction tx = deserialize_transaction_hex_or_throw(
        params[0].get<std::string>(),
        "Неверный tx_hex",
        "tx_hex"
    );

    const bytes sig = parse_hex_bytes_or_throw(
        params[1].get<std::string>(),
        "signature_hex имеет неверный размер (ожидается Falcon payload)"
    );
    if (sig.size() < 10 || sig.size() > 4096) {
        throw RPCException(RPC_INVALID_PARAMETER, "signature_hex имеет неверный размер (ожидается Falcon payload)");
    }

    if (tx.type != expected_type) {
        throw RPCException(RPC_INVALID_PARAMETER, "tx не является " + expected_type_name);
    }
    if (expected_type == core::TransactionType::NFT_CREATE && tx.nft_create.has_value()) {
        tx.nft_create->signature = sig;
    } else if (expected_type == core::TransactionType::NFT_TRANSFER && tx.nft_transfer.has_value()) {
        tx.nft_transfer->signature = sig;
    } else if (expected_type == core::TransactionType::NFT_BURN && tx.nft_burn.has_value()) {
        tx.nft_burn->signature = sig;
    } else {
        throw RPCException(RPC_INVALID_PARAMETER, "tx не является " + expected_type_name);
    }

    return transaction_hex_result(tx);
}

uint64_t compute_recommended_fee_rate_per_kb(const network::Mempool* mempool) {
    return network::fee_policy::recommended_rate_per_kb(mempool);
}

Amount compute_dynamic_wallet_fee(
    wallet::Wallet* wallet,
    std::shared_ptr<state::ChainState> chain_state,
    Amount amount,
    const network::Mempool* mempool,
    std::span<const core::Transaction> pending
) {
    if (!wallet) {
        return consensus::params::MIN_TX_FEE;
    }

    const Amount base_fee = wallet->estimate_required_fee(amount, chain_state, pending);
    if (base_fee <= 0) {
        return consensus::params::MIN_TX_FEE;
    }

    const uint64_t recommended_rate = compute_recommended_fee_rate_per_kb(mempool);
    return network::fee_policy::scale_fee_to_rate(base_fee, recommended_rate);
}

} // namespace geocoin::rpc
