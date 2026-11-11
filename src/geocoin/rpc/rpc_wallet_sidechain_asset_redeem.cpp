#include "geocoin/rpc/rpc_amount.hpp"
#include "geocoin/rpc/rpc_wallet_helpers.hpp"
#include "geocoin/rpc/rpc_params.hpp"

namespace geocoin::rpc {

void RPCServer::register_wallet_sidechain_asset_redeem_methods() {

    // createsidechainredeempayload - создать REDEEM_SIDECHAIN
    register_method({
        .name = "createsidechainredeempayload",
        .method = [this](const json& params) -> json {
            const json& p = required_object_param(params, 0, "Требуется объект с полями sidechain_id, owner_address, amount, outputs");
            if (!p.contains("amount")) {
                throw RPCException(RPC_INVALID_PARAMETER, "amount обязателен");
            }
            if (!p.contains("outputs") || !p["outputs"].is_array()) {
                throw RPCException(RPC_INVALID_PARAMETER, "outputs обязателен (array)");
            }

            const Amount amount = required_amount_field(p, "amount", "amount обязателен");
            if (amount <= 0) {
                throw RPCException(RPC_INVALID_PARAMETER, "amount должен быть > 0");
            }

            core::Transaction tx;
            const auto common = build_sidechain_tx_common(p, tx, "createsidechainredeempayload");
            tx.type = core::TransactionType::REDEEM_SIDECHAIN;

            tx.sidechain_amount = amount;
            apply_optional_sidechain_checkpoint_from_rpc(p, tx);
            apply_optional_sidechain_committee_id_from_rpc(p, tx);

            const Amount total_output = append_transaction_outputs_from_rpc(p["outputs"], tx.outputs);

            if (tx.outputs.empty()) {
                throw RPCException(RPC_INVALID_PARAMETER, "outputs не должен быть пустым");
            }
            if (total_output != amount) {
                throw RPCException(RPC_INVALID_PARAMETER, "Сумма outputs должна равняться amount");
            }

            const bool validate = get_validate_flag(p, "createsidechainredeempayload");
            if (validate) {
                auto& chain_state = require_chain_state();
                state::SidechainRegistryEntry entry{};
                if (!chain_state.get_sidechain_registry(common.sidechain_id, entry)) {
                    throw RPCException(RPC_INVALID_PARAMETER, "Sidechain не зарегистрирован");
                }
                if (entry.status != state::SidechainStatus::ACTIVE) {
                    throw RPCException(RPC_INVALID_PARAMETER, "Sidechain находится в процессе decommission");
                }
                if (entry.kind != core::SidechainKind::FINANCIAL) {
                    throw RPCException(RPC_INVALID_PARAMETER, "REDEEM доступен только для financial sidechain");
                }
                if (entry.total_issued < amount) {
                    throw RPCException(RPC_INVALID_PARAMETER, "Недостаточно выпущенных токенов для REDEEM");
                }
                state::SidechainDeposit deposit{};
                if (!chain_state.get_sidechain_deposit(common.sidechain_id, deposit)) {
                    throw RPCException(RPC_INVALID_PARAMETER, "Депозит не найден для sidechain_id");
                }
                const Amount fee = calc_sidechain_fee_rpc(amount, entry.fee_bps);
                const Amount required_deposit = checked_add_amount_or_throw(
                    amount,
                    fee,
                    "REDEEM + fee превышает допустимый диапазон"
                );
                if (deposit.amount < required_deposit) {
                    throw RPCException(RPC_INVALID_PARAMETER, "Недостаточно средств в депозите для REDEEM + fee");
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
            out["committee_id"] = hash256_to_hex_string(tx.sidechain_committee_id);
            out["timestamp"] = tx.timestamp;
            return out;
        },
        .category = "sidechain",
        .help = "createsidechainredeempayload {\"sidechain_id\":\"hex32\"|\"genesis_hash\":\"hex32\",\"owner_address\":\"GEO...\",\"amount\":n,\"outputs\":[{\"address\":\"GEO...\",\"value\":n}],\"checkpoint\":\"hex32\",\"committee_id\":\"hex32\"?,\"validate\":true|false}\n\nСоздает REDEEM_SIDECHAIN и возвращает payload для подписи комитета. validate по умолчанию = true.\n",
        .requires_wallet = false,
        .param_names = {"request"}
    });

    register_method({
        .name = "finalizesidechainredeem",
        .method = [this](const json& params) -> json {
            return finalize_committee_signed_transaction(
                params,
                core::TransactionType::REDEEM_SIDECHAIN,
                "finalizesidechainredeem: неверный tx_hex",
                "finalizesidechainredeem: tx имеет неверный тип"
            );
        },
        .category = "sidechain",
        .help = "finalizesidechainredeem \"tx_hex\" [\"sig1_hex\",\"sig2_hex\"]\n\nВозвращает финальный tx_hex с подписями комитета для REDEEM_SIDECHAIN.\n",
        .requires_wallet = false,
        .param_names = {"tx_hex", "signature_hex"}
    });

    register_method({
        .name = "finalizesidechainredeemandsubmit",
        .method = [this](const json& params) -> json {
            json finalized = finalize_committee_signed_transaction(
                params,
                core::TransactionType::REDEEM_SIDECHAIN,
                "finalizesidechainredeemandsubmit: неверный tx_hex",
                "finalizesidechainredeemandsubmit: tx имеет неверный тип"
            );
            const std::string tx_hex = finalized["tx_hex"].get<std::string>();
            const std::string txid = submit_raw_transaction_hex_to_mempool(
                tx_hex,
                chain_state_,
                "finalizesidechainredeemandsubmit",
                true,
                true,
                chain_mutations_
            );
            finalized["txid"] = txid;
            return finalized;
        },
        .category = "sidechain",
        .help = "finalizesidechainredeemandsubmit \"tx_hex\" [\"sig1_hex\",\"sig2_hex\"]\n\nФинализирует REDEEM_SIDECHAIN подписями и отправляет в mempool. Возвращает tx_hex и txid.\n",
        .requires_wallet = false,
        .param_names = {"tx_hex", "signature_hex"},
        .mutates = true,
        .side_effects = "submits a finalized sidechain redeem transaction to the local mempool"
    });

}

} // namespace geocoin::rpc
