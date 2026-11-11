#include "geocoin/rpc/rpc_wallet_helpers.hpp"
#include "geocoin/rpc/rpc_params.hpp"
#include "geocoin/utils/bytes.hpp"

namespace geocoin::rpc {

void RPCServer::register_wallet_sidechain_asset_slash_methods() {

    // createsidechainslashpayload - создать SLASH_DEPOSIT
    register_method({
        .name = "createsidechainslashpayload",
        .method = [this](const json& params) -> json {
            const json& p = required_object_param(params, 0, "Требуется объект с полями sidechain_id, owner_address, amount");
            if (!p.contains("amount")) {
                throw RPCException(RPC_INVALID_PARAMETER, "amount обязателен");
            }

            const Amount amount = required_amount_field(p, "amount", "amount обязателен");
            if (amount <= 0) {
                throw RPCException(RPC_INVALID_PARAMETER, "amount должен быть > 0");
            }

            core::Transaction tx;
            const auto common = build_sidechain_tx_common(p, tx, "createsidechainslashpayload");
            tx.type = core::TransactionType::SLASH_DEPOSIT;

            tx.sidechain_amount = amount;
            apply_optional_sidechain_checkpoint_from_rpc(p, tx);
            apply_optional_sidechain_committee_id_from_rpc(p, tx);

            bool ref_a_index_provided = false;
            bool ref_b_index_provided = false;

            const bool has_ref_obj_a = p.contains("fraud_ref_a");
            const bool has_ref_obj_b = p.contains("fraud_ref_b");
            const bool has_legacy_ref =
                p.contains("fraud_checkpoint_a") || p.contains("fraud_checkpoint_b") ||
                p.contains("fraud_checkpoint_index_a") || p.contains("fraud_checkpoint_index_b");

            if ((has_ref_obj_a || has_ref_obj_b) && has_legacy_ref) {
                throw RPCException(RPC_INVALID_PARAMETER, "используйте либо fraud_ref_a/fraud_ref_b, либо legacy fraud_checkpoint_* поля");
            }

            if (has_ref_obj_a || has_ref_obj_b) {
                if (!has_ref_obj_a || !has_ref_obj_b || !p["fraud_ref_a"].is_object() || !p["fraud_ref_b"].is_object()) {
                    throw RPCException(RPC_INVALID_PARAMETER, "fraud_ref_a и fraud_ref_b обязательны и должны быть объектами");
                }
                const auto parse_ref = [](const json& ref_obj, Hash256& out_txid, uint16_t& out_index, bool& index_provided, const char* label) {
                    if (!ref_obj.contains("txid") || !ref_obj["txid"].is_string()) {
                        throw RPCException(RPC_INVALID_PARAMETER, std::string(label) + ".txid обязателен");
                    }
                    if (!hex_to_hash256_checked(ref_obj["txid"].get<std::string>(), out_txid)) {
                        throw RPCException(RPC_INVALID_PARAMETER, std::string(label) + ".txid должен быть 32-байт hex");
                    }
                    if (ref_obj.contains("index")) {
                        const uint64_t idx = required_u64_field(ref_obj, "index", std::string(label) + ".index вне диапазона uint16");
                        if (idx > 0xFFFFu) {
                            throw RPCException(RPC_INVALID_PARAMETER, std::string(label) + ".index вне диапазона uint16");
                        }
                        out_index = static_cast<uint16_t>(idx);
                        index_provided = true;
                    }
                };
                parse_ref(p["fraud_ref_a"], tx.sidechain_fraud_checkpoint_a, tx.sidechain_fraud_checkpoint_index_a, ref_a_index_provided, "fraud_ref_a");
                parse_ref(p["fraud_ref_b"], tx.sidechain_fraud_checkpoint_b, tx.sidechain_fraud_checkpoint_index_b, ref_b_index_provided, "fraud_ref_b");
                if (!chain_state_ && (!ref_a_index_provided || !ref_b_index_provided)) {
                    throw RPCException(RPC_INVALID_PARAMETER, "без ChainState укажите fraud_ref_a.index и fraud_ref_b.index явно");
                }
            } else {
                if (p.contains("fraud_checkpoint_a") && p["fraud_checkpoint_a"].is_string()) {
                    if (!hex_to_hash256_checked(p["fraud_checkpoint_a"].get<std::string>(), tx.sidechain_fraud_checkpoint_a)) {
                        throw RPCException(RPC_INVALID_PARAMETER, "fraud_checkpoint_a должен быть 32-байт hex");
                    }
                }
                if (p.contains("fraud_checkpoint_b") && p["fraud_checkpoint_b"].is_string()) {
                    if (!hex_to_hash256_checked(p["fraud_checkpoint_b"].get<std::string>(), tx.sidechain_fraud_checkpoint_b)) {
                        throw RPCException(RPC_INVALID_PARAMETER, "fraud_checkpoint_b должен быть 32-байт hex");
                    }
                }
                if (p.contains("fraud_checkpoint_index_a")) {
                    const uint64_t idx = required_u64_field(p, "fraud_checkpoint_index_a", "fraud_checkpoint_index_a вне диапазона uint16");
                    if (idx > 0xFFFFu) {
                        throw RPCException(RPC_INVALID_PARAMETER, "fraud_checkpoint_index_a вне диапазона uint16");
                    }
                    tx.sidechain_fraud_checkpoint_index_a = static_cast<uint16_t>(idx);
                    ref_a_index_provided = true;
                }
                if (p.contains("fraud_checkpoint_index_b")) {
                    const uint64_t idx = required_u64_field(p, "fraud_checkpoint_index_b", "fraud_checkpoint_index_b вне диапазона uint16");
                    if (idx > 0xFFFFu) {
                        throw RPCException(RPC_INVALID_PARAMETER, "fraud_checkpoint_index_b вне диапазона uint16");
                    }
                    tx.sidechain_fraud_checkpoint_index_b = static_cast<uint16_t>(idx);
                    ref_b_index_provided = true;
                }
            }

            // Авто-резолв индексов даже при validate=false, если есть chain_state:
            // это упрощает GUI и снижает ошибки при формировании SLASH.
            if (chain_state_) {
                if (!utils::is_null_hash256(tx.sidechain_fraud_checkpoint_a)) {
                    ResolvedFraudRef ref{};
                    std::string err;
                    if (!resolve_fraud_checkpoint_ref(
                            chain_state_.get(),
                            common.sidechain_id,
                            tx.sidechain_fraud_checkpoint_a,
                            ref_a_index_provided,
                            tx.sidechain_fraud_checkpoint_index_a,
                            ref,
                            err)) {
                        throw RPCException(RPC_INVALID_PARAMETER, "fraud_ref_a: " + err);
                    }
                    tx.sidechain_fraud_checkpoint_index_a = ref.index;
                }
                if (!utils::is_null_hash256(tx.sidechain_fraud_checkpoint_b)) {
                    ResolvedFraudRef ref{};
                    std::string err;
                    if (!resolve_fraud_checkpoint_ref(
                            chain_state_.get(),
                            common.sidechain_id,
                            tx.sidechain_fraud_checkpoint_b,
                            ref_b_index_provided,
                            tx.sidechain_fraud_checkpoint_index_b,
                            ref,
                            err)) {
                        throw RPCException(RPC_INVALID_PARAMETER, "fraud_ref_b: " + err);
                    }
                    tx.sidechain_fraud_checkpoint_index_b = ref.index;
                }
            }

            Amount total_output = 0;
            if (p.contains("outputs")) {
                if (!p["outputs"].is_array()) {
                    throw RPCException(RPC_INVALID_PARAMETER, "outputs должен быть массивом");
                }
                total_output = append_transaction_outputs_from_rpc(p["outputs"], tx.outputs);
            }
            if (total_output > amount) {
                throw RPCException(RPC_INVALID_PARAMETER, "Сумма outputs не должна превышать amount");
            }

            const bool validate = get_validate_flag(p, "createsidechainslashpayload");
            if (validate) {
                auto& chain_state = require_chain_state();
                state::SidechainRegistryEntry entry{};
                if (!chain_state.get_sidechain_registry(common.sidechain_id, entry)) {
                    throw RPCException(RPC_INVALID_PARAMETER, "Sidechain не зарегистрирован");
                }
                state::SidechainDeposit deposit{};
                if (!chain_state.get_sidechain_deposit(common.sidechain_id, deposit)) {
                    throw RPCException(RPC_INVALID_PARAMETER, "Депозит не найден для sidechain_id");
                }
                if (deposit.amount < amount) {
                    throw RPCException(RPC_INVALID_PARAMETER, "Недостаточно средств в депозите для SLASH");
                }
                if (tx.sidechain_fraud_checkpoint_a == Hash256{} ||
                    tx.sidechain_fraud_checkpoint_b == Hash256{} ||
                    (tx.sidechain_fraud_checkpoint_a == tx.sidechain_fraud_checkpoint_b &&
                     tx.sidechain_fraud_checkpoint_index_a == tx.sidechain_fraud_checkpoint_index_b)) {
                    throw RPCException(RPC_INVALID_PARAMETER, "Нужны две разные ссылки fraud proof");
                }

                ResolvedFraudRef a_ref{};
                ResolvedFraudRef b_ref{};
                std::string err_a;
                std::string err_b;
                if (!resolve_fraud_checkpoint_ref(
                        &chain_state,
                        common.sidechain_id,
                        tx.sidechain_fraud_checkpoint_a,
                        true,
                        tx.sidechain_fraud_checkpoint_index_a,
                        a_ref,
                        err_a) ||
                    !resolve_fraud_checkpoint_ref(
                        &chain_state,
                        common.sidechain_id,
                        tx.sidechain_fraud_checkpoint_b,
                        true,
                        tx.sidechain_fraud_checkpoint_index_b,
                        b_ref,
                        err_b) ||
                    a_ref.sidechain_id != common.sidechain_id ||
                    b_ref.sidechain_id != common.sidechain_id ||
                    a_ref.checkpoint_height == 0 ||
                    b_ref.checkpoint_height == 0 ||
                    a_ref.checkpoint_height != b_ref.checkpoint_height ||
                    a_ref.checkpoint == b_ref.checkpoint) {
                    throw RPCException(
                        RPC_INVALID_PARAMETER,
                        std::string("fraud proof невалиден")
                            + (err_a.empty() ? "" : (": A=" + err_a))
                            + (err_b.empty() ? "" : (", B=" + err_b))
                    );
                }
                apply_active_sidechain_committee_id_or_throw(tx, entry);
            } else if (tx.sidechain_committee_id == Hash256{} && chain_state_) {
                fill_active_sidechain_committee_id_if_available(
                    tx,
                    chain_state_.get(),
                    common.sidechain_id
                );
            }

            bytes payload = core::build_sidechain_payload_for_signing(tx);

            json out;
            out["tx_hex"] = bytes_to_hex_string(tx.serialize());
            out["payload_to_sign_hex"] = bytes_to_hex_string(payload);
            out["sidechain_id"] = hash256_to_hex_string(common.sidechain_id);
            out["owner_address"] = common.owner_address;
            out["amount"] = amount;
            out["fraud_checkpoint_a"] = hash256_to_hex_string(tx.sidechain_fraud_checkpoint_a);
            out["fraud_checkpoint_b"] = hash256_to_hex_string(tx.sidechain_fraud_checkpoint_b);
            out["fraud_checkpoint_index_a"] = tx.sidechain_fraud_checkpoint_index_a;
            out["fraud_checkpoint_index_b"] = tx.sidechain_fraud_checkpoint_index_b;
            out["committee_id"] = hash256_to_hex_string(tx.sidechain_committee_id);
            out["timestamp"] = tx.timestamp;
            return out;
        },
        .category = "sidechain",
        .help = "createsidechainslashpayload {\"sidechain_id\":\"hex32\"|\"genesis_hash\":\"hex32\",\"owner_address\":\"GEO...\",\"amount\":n,\"outputs\":[{\"address\":\"GEO...\",\"value\":n}]?,\"fraud_ref_a\":{\"txid\":\"hex32\",\"index\":n?},\"fraud_ref_b\":{\"txid\":\"hex32\",\"index\":n?},\"fraud_checkpoint_a\":\"hex32\",\"fraud_checkpoint_b\":\"hex32\",\"fraud_checkpoint_index_a\":n?,\"fraud_checkpoint_index_b\":n?,\"checkpoint\":\"hex32\"?,\"committee_id\":\"hex32\"?,\"validate\":true|false}\n\nСоздает SLASH_DEPOSIT и возвращает payload для подписи комитета. Рекомендуется использовать fraud_ref_*: индекс для SINGLE будет выставлен автоматически в 65535, для BATCH можно указать index (или он будет выбран автоматически, если элемент для sidechain только один).\n",
        .requires_wallet = false,
        .param_names = {"request"}
    });

    register_method({
        .name = "finalizesidechainslash",
        .method = [this](const json& params) -> json {
            return finalize_committee_signed_transaction(
                params,
                core::TransactionType::SLASH_DEPOSIT,
                "finalizesidechainslash: неверный tx_hex",
                "finalizesidechainslash: tx имеет неверный тип"
            );
        },
        .category = "sidechain",
        .help = "finalizesidechainslash \"tx_hex\" [\"sig1_hex\",\"sig2_hex\"]\n\nВозвращает финальный tx_hex с подписями комитета для SLASH_DEPOSIT.\n",
        .requires_wallet = false,
        .param_names = {"tx_hex", "signature_hex"}
    });

    register_method({
        .name = "finalizesidechainslashandsubmit",
        .method = [this](const json& params) -> json {
            json finalized = finalize_committee_signed_transaction(
                params,
                core::TransactionType::SLASH_DEPOSIT,
                "finalizesidechainslashandsubmit: неверный tx_hex",
                "finalizesidechainslashandsubmit: tx имеет неверный тип"
            );
            const std::string tx_hex = finalized["tx_hex"].get<std::string>();
            const std::string txid = submit_raw_transaction_hex_to_mempool(
                tx_hex,
                chain_state_,
                "finalizesidechainslashandsubmit",
                true,
                true,
                chain_mutations_
            );
            finalized["txid"] = txid;
            return finalized;
        },
        .category = "sidechain",
        .help = "finalizesidechainslashandsubmit \"tx_hex\" [\"sig1_hex\",\"sig2_hex\"]\n\nФинализирует SLASH_DEPOSIT подписями и отправляет в mempool. Возвращает tx_hex и txid.\n",
        .requires_wallet = false,
        .param_names = {"tx_hex", "signature_hex"},
        .mutates = true,
        .side_effects = "submits a finalized sidechain slash transaction to the local mempool"
    });

}

} // namespace geocoin::rpc
