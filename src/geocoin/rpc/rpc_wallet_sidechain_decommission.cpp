#include "geocoin/rpc/rpc_wallet_helpers.hpp"
#include "geocoin/rpc/rpc_params.hpp"

#include <limits>

namespace geocoin::rpc {
namespace {

json finalize_sidechain_decommission(const json& params, const char* context_label) {
    require_min_params(
        params,
        3,
        std::string(context_label) + ": требуется tx_hex, signature_hex и подписи комитета"
    );
    core::Transaction tx = deserialize_transaction_hex_or_throw(
        params[0].get<std::string>(),
        std::string(context_label) + ": неверный tx_hex",
        "tx_hex"
    );
    if (tx.type != core::TransactionType::DECOMMISSION_SIDECHAIN) {
        throw RPCException(RPC_INVALID_PARAMETER, std::string(context_label) + ": tx имеет неверный тип");
    }
    bytes sig;
    if (!hex_to_bytes_checked(params[1].get<std::string>(), sig) || sig.size() < 10 || sig.size() > 4096) {
        throw RPCException(
            RPC_INVALID_PARAMETER,
            std::string(context_label) + ": signature_hex имеет неверный размер (ожидается Falcon payload)"
        );
    }
    tx.sidechain_signature = std::move(sig);
    set_committee_signatures_from_rpc(tx.sidechain_committee_signatures, params[2]);
    return transaction_hex_result(tx);
}

} // namespace

void RPCServer::register_wallet_sidechain_decommission_methods() {
    register_method({
        .name = "createsidechaindecommissionpayload",
        .method = [this](const json& params) -> json {
            const json& p = required_object_param(params, 0, "Требуется объект с полями sidechain_id/genesis_hash, owner_address");
            core::Transaction tx;
            const auto common = build_sidechain_tx_common(p, tx, "createsidechaindecommissionpayload");
            tx.type = core::TransactionType::DECOMMISSION_SIDECHAIN;
            tx.sidechain_amount = 0;
            tx.sidechain_unlock_height = 0;
            tx.sidechain_checkpoint = Hash256{};
            apply_optional_sidechain_committee_id_from_rpc(p, tx);
            fill_active_sidechain_committee_id_if_available(tx, chain_state_.get(), common.sidechain_id);
            const bool validate = get_validate_flag(p, "createsidechaindecommissionpayload");
            Height challenge_end_height = 0;
            if (validate) {
                auto& chain_state = require_chain_state();
                state::SidechainRegistryEntry entry{};
                if (!chain_state.get_sidechain_registry(common.sidechain_id, entry)) {
                    throw RPCException(RPC_INVALID_PARAMETER, "Sidechain не найден");
                }
                state::SidechainDeposit deposit{};
                if (!chain_state.get_sidechain_deposit(common.sidechain_id, deposit)) {
                    throw RPCException(RPC_INVALID_PARAMETER, "Депозит не найден для sidechain_id");
                }
                if (deposit.owner.str() != common.owner_address) {
                    throw RPCException(RPC_INVALID_PARAMETER, "owner_address не совпадает с владельцем депозита");
                }
                if (entry.total_issued != 0 && deposit.amount != 0) {
                    throw RPCException(RPC_INVALID_PARAMETER, "total_issued должен быть 0 для декомиссии");
                }
                if (entry.status != state::SidechainStatus::ACTIVE) {
                    throw RPCException(RPC_INVALID_PARAMETER, "Sidechain уже закрывается");
                }
                apply_active_sidechain_committee_id_or_throw(tx, entry);
                const Height current_height = chain_state.get_height();
                if (current_height > std::numeric_limits<Height>::max() - 1 -
                        consensus::params::SIDECHAIN_DECOMMISSION_CHALLENGE_BLOCKS) {
                    throw RPCException(RPC_INVALID_PARAMETER, "Высота цепи вне допустимого диапазона");
                }
                challenge_end_height = current_height + 1 +
                    consensus::params::SIDECHAIN_DECOMMISSION_CHALLENGE_BLOCKS;
            }
            bytes payload = core::build_sidechain_payload_for_signing(tx);
            json out;
            out["tx_hex"] = bytes_to_hex_string(tx.serialize());
            out["payload_to_sign_hex"] = bytes_to_hex_string(payload);
            out["sidechain_id"] = hash256_to_hex_string(common.sidechain_id);
            out["committee_id"] = hash256_to_hex_string(tx.sidechain_committee_id);
            if (common.has_genesis) {
                out["genesis_hash"] = hash256_to_hex_string(common.genesis_hash);
            }
            out["owner_address"] = common.owner_address;
            out["status_after_confirmation"] = "decommissioning";
            out["challenge_blocks"] =
                consensus::params::SIDECHAIN_DECOMMISSION_CHALLENGE_BLOCKS;
            if (challenge_end_height != 0) {
                out["challenge_end_height"] = challenge_end_height;
            }
            out["timestamp"] = tx.timestamp;
            return out;
        },
        .category = "sidechain",
        .help = "createsidechaindecommissionpayload {\"sidechain_id\":\"hex32\"|\"genesis_hash\":\"hex32\",\"owner_address\":\"GEO...\",\"validate\":true|false}\n\nСоздает DECOMMISSION_SIDECHAIN и возвращает payload для подписи владельца и комитета. validate по умолчанию = true.\n",
        .requires_wallet = false,
        .param_names = {"request"}
    });
    register_method({
        .name = "finalizesidechaindecommission",
        .method = [this](const json& params) -> json {
            return finalize_sidechain_decommission(params, "finalizesidechaindecommission");
        },
        .category = "sidechain",
        .help = "finalizesidechaindecommission \"tx_hex\" \"owner_signature_hex\" [\"committee_sig_hex\",...]\n\nВозвращает финальный tx_hex с подписями владельца и комитета для DECOMMISSION_SIDECHAIN.\n",
        .requires_wallet = false,
        .param_names = {"tx_hex", "signature_hex", "committee_signatures"}
    });
    register_method({
        .name = "finalizesidechaindecommissionandsubmit",
        .method = [this](const json& params) -> json {
            json finalized = finalize_sidechain_decommission(params, "finalizesidechaindecommissionandsubmit");
            const std::string tx_hex = finalized["tx_hex"].get<std::string>();
            const std::string txid = submit_raw_transaction_hex_to_mempool(
                tx_hex,
                chain_state_,
                "finalizesidechaindecommissionandsubmit",
                true,
                true,
                chain_mutations_
            );
            finalized["txid"] = txid;
            return finalized;
        },
        .category = "sidechain",
        .help = "finalizesidechaindecommissionandsubmit \"tx_hex\" \"owner_signature_hex\" [\"committee_sig_hex\",...]\n\nФинализирует DECOMMISSION_SIDECHAIN подписями владельца и комитета и отправляет транзакцию в mempool. Возвращает tx_hex и txid.\n",
        .requires_wallet = false,
        .param_names = {"tx_hex", "signature_hex", "committee_signatures"},
        .mutates = true,
        .side_effects = "submits a finalized sidechain decommission transaction to the local mempool"
    });
}

} // namespace geocoin::rpc
