#include "geocoin/rpc/rpc_wallet_helpers.hpp"
#include "geocoin/rpc/rpc_params.hpp"
#include "geocoin/consensus/sidechain_rules.hpp"

namespace geocoin::rpc {

void RPCServer::register_wallet_sidechain_asset_mint_methods() {

    // createsidechainmintpayload - создать MINT_SIDECHAIN
    register_method({
        .name = "createsidechainmintpayload",
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
            const auto common = build_sidechain_tx_common(p, tx, "createsidechainmintpayload");
            tx.type = core::TransactionType::MINT_SIDECHAIN;

            tx.sidechain_amount = amount;
            apply_optional_sidechain_committee_id_from_rpc(p, tx);

            const bool validate = get_validate_flag(p, "createsidechainmintpayload");
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
                    throw RPCException(RPC_INVALID_PARAMETER, "MINT доступен только для financial sidechain");
                }
                if (!consensus::mint_fits_issuance_capacity(entry.issuance_limit, entry.total_issued, amount)) {
                    throw RPCException(RPC_INVALID_PARAMETER, "Превышен лимит эмиссии");
                }
                state::SidechainDeposit deposit{};
                if (!chain_state.get_sidechain_deposit(common.sidechain_id, deposit)) {
                    throw RPCException(RPC_INVALID_PARAMETER, "Депозит не найден для sidechain_id");
                }
                const Amount fee = calc_sidechain_fee_rpc(amount, entry.fee_bps);
                if (deposit.amount < fee) {
                    throw RPCException(RPC_INVALID_PARAMETER, "Недостаточно средств в депозите для MINT fee");
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
        .help = "createsidechainmintpayload {\"sidechain_id\":\"hex32\"|\"genesis_hash\":\"hex32\",\"owner_address\":\"GEO...\",\"amount\":n,\"checkpoint\":\"hex32\",\"committee_id\":\"hex32\"?,\"validate\":true|false}\n\nСоздает MINT_SIDECHAIN и возвращает payload для подписи комитета. validate по умолчанию = true.\n",
        .requires_wallet = false,
        .param_names = {"request"}
    });

    register_method({
        .name = "finalizesidechainmint",
        .method = [this](const json& params) -> json {
            return finalize_committee_signed_transaction(
                params,
                core::TransactionType::MINT_SIDECHAIN,
                "finalizesidechainmint: неверный tx_hex",
                "finalizesidechainmint: tx имеет неверный тип"
            );
        },
        .category = "sidechain",
        .help = "finalizesidechainmint \"tx_hex\" [\"sig1_hex\",\"sig2_hex\"]\n\nВозвращает финальный tx_hex с подписями комитета для MINT_SIDECHAIN.\n",
        .requires_wallet = false,
        .param_names = {"tx_hex", "signature_hex"}
    });

    register_method({
        .name = "finalizesidechainmintandsubmit",
        .method = [this](const json& params) -> json {
            json finalized = finalize_committee_signed_transaction(
                params,
                core::TransactionType::MINT_SIDECHAIN,
                "finalizesidechainmintandsubmit: неверный tx_hex",
                "finalizesidechainmintandsubmit: tx имеет неверный тип"
            );
            const std::string tx_hex = finalized["tx_hex"].get<std::string>();
            const std::string txid = submit_raw_transaction_hex_to_mempool(
                tx_hex,
                chain_state_,
                "finalizesidechainmintandsubmit",
                true,
                true,
                chain_mutations_
            );
            finalized["txid"] = txid;
            return finalized;
        },
        .category = "sidechain",
        .help = "finalizesidechainmintandsubmit \"tx_hex\" [\"sig1_hex\",\"sig2_hex\"]\n\nФинализирует MINT_SIDECHAIN подписями и отправляет в mempool. Возвращает tx_hex и txid.\n",
        .requires_wallet = false,
        .param_names = {"tx_hex", "signature_hex"},
        .mutates = true,
        .side_effects = "submits a finalized sidechain mint transaction to the local mempool"
    });

}

} // namespace geocoin::rpc
