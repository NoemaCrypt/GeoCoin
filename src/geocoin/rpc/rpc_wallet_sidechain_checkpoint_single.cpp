#include "geocoin/rpc/rpc_wallet_helpers.hpp"
#include "geocoin/rpc/rpc_params.hpp"

namespace geocoin::rpc {

void RPCServer::register_wallet_sidechain_checkpoint_single_methods() {

    // createsidechaincheckpointpayload - создать SIDECHAIN_CHECKPOINT с автосчетом комиссии
    register_method({
        .name = "createsidechaincheckpointpayload",
        .method = [this](const json& params) -> json {
            const json& p = required_object_param(params, 0, "Требуется объект с полями sidechain_id/genesis_hash, owner_address, checkpoint и fee или total_weight_bytes");

            if (!p.contains("checkpoint") || !p["checkpoint"].is_string()) {
                throw RPCException(RPC_INVALID_PARAMETER, "checkpoint обязателен (hex 32 bytes)");
            }
            if (!p.contains("checkpoint_height")) {
                throw RPCException(RPC_INVALID_PARAMETER, "checkpoint_height обязателен");
            }

            Hash256 checkpoint{};
            if (!hex_to_hash256_checked(p["checkpoint"].get<std::string>(), checkpoint)) {
                throw RPCException(RPC_INVALID_PARAMETER, "checkpoint должен быть 32-байт hex");
            }
            const Height checkpoint_height = static_cast<Height>(required_u64_field(p, "checkpoint_height", "checkpoint_height обязателен"));
            if (checkpoint_height == 0 ||
                checkpoint_height > consensus::params::MAX_SIDECHAIN_CHECKPOINT_HEIGHT) {
                throw RPCException(RPC_INVALID_PARAMETER, "checkpoint_height вне допустимого диапазона");
            }

            Amount fee = 0;
            if (p.contains("fee")) {
                fee = required_amount_field(p, "fee", "fee должен быть > 0");
            } else if (p.contains("total_weight_bytes")) {
                const uint64_t weight = required_u64_field(p, "total_weight_bytes", "total_weight_bytes должен быть числом");
                fee = consensus::calculate_sidechain_checkpoint_fee(weight);
            } else {
                throw RPCException(RPC_INVALID_PARAMETER, "Нужно указать fee или total_weight_bytes");
            }
            if (fee <= 0) {
                throw RPCException(RPC_INVALID_PARAMETER, "fee должен быть > 0");
            }

            core::Transaction tx;
            const auto common = build_sidechain_tx_common(p, tx, "createsidechaincheckpointpayload");
            tx.type = core::TransactionType::SIDECHAIN_CHECKPOINT;
            tx.sidechain_amount = fee;
            tx.sidechain_checkpoint = checkpoint;
            tx.sidechain_checkpoint_height = checkpoint_height;

            if (p.contains("committee_id")) {

                if (!p["committee_id"].is_string()) {

                    throw RPCException(RPC_INVALID_PARAMETER, "committee_id must be a string");

                }
                if (!hex_to_hash256_checked(p["committee_id"].get<std::string>(), tx.sidechain_committee_id)) {
                    throw RPCException(RPC_INVALID_PARAMETER, "committee_id должен быть 32-байт hex");
                }
            } else if (chain_state_) {
                state::SidechainRegistryEntry entry{};
                if (!chain_state_->get_sidechain_registry(common.sidechain_id, entry)) {
                    throw RPCException(RPC_INVALID_PARAMETER, "Sidechain не найден");
                }
                tx.sidechain_committee_id = consensus::compute_sidechain_committee_id(
                    entry.committee_m,
                    entry.committee_n,
                    entry.committee_members
                );
            } else {
                throw RPCException(RPC_INVALID_PARAMETER, "committee_id обязателен, если ChainState недоступен");
            }

            const bool validate = get_validate_flag(p, "createsidechaincheckpointpayload");
            if (validate) {
                auto& chain_state = require_chain_state();
                state::SidechainDeposit deposit{};
                if (!chain_state.get_sidechain_deposit(common.sidechain_id, deposit)) {
                    throw RPCException(RPC_INVALID_PARAMETER, "Депозит не найден для sidechain_id");
                }
                if (deposit.owner.str() != common.owner_address) {
                    throw RPCException(RPC_INVALID_PARAMETER, "owner_address не совпадает с владельцем депозита");
                }
                if (deposit.checkpoint == checkpoint) {
                    throw RPCException(RPC_INVALID_PARAMETER, "checkpoint уже установлен");
                }
                if (fee > deposit.amount) {
                    throw RPCException(RPC_INVALID_PARAMETER, "Недостаточно средств в депозите для CHECKPOINT");
                }
                const Height effective_height = chain_state.get_height() + 1;
                const Amount min_fee = consensus::calculate_sidechain_checkpoint_min_fee(
                    effective_height,
                    deposit.last_checkpoint_height
                );
                if (fee < min_fee) {
                    throw RPCException(RPC_INVALID_PARAMETER, "fee ниже минимального порога для CHECKPOINT");
                }
                state::SidechainRegistryEntry entry{};
                if (!chain_state.get_sidechain_registry(common.sidechain_id, entry)) {
                    throw RPCException(RPC_INVALID_PARAMETER, "Sidechain не найден");
                }
                const Hash256 expected_id = consensus::compute_sidechain_committee_id(
                    entry.committee_m,
                    entry.committee_n,
                    entry.committee_members
                );
                tx.sidechain_committee_id = expected_id;
            }

            bytes payload = core::build_sidechain_checkpoint_payload_for_signing(tx);

            json out;
            out["tx_hex"] = bytes_to_hex_string(tx.serialize());
            out["payload_to_sign_hex"] = bytes_to_hex_string(payload);
            out["sidechain_id"] = hash256_to_hex_string(common.sidechain_id);
            if (common.has_genesis) {
                out["genesis_hash"] = hash256_to_hex_string(common.genesis_hash);
            }
            out["owner_address"] = common.owner_address;
            out["fee"] = fee;
            out["checkpoint"] = hash256_to_hex_string(checkpoint);
            out["checkpoint_height"] = checkpoint_height;
            out["committee_id"] = hash256_to_hex_string(tx.sidechain_committee_id);
            out["timestamp"] = tx.timestamp;
            return out;
        },
        .category = "sidechain",
        .help = "createsidechaincheckpointpayload {\"sidechain_id\":\"hex32\"|\"genesis_hash\":\"hex32\",\"owner_address\":\"GEO...\",\"checkpoint\":\"hex32\",\"checkpoint_height\":n,\"committee_id\":\"hex32\"?,\"fee\":n|\"total_weight_bytes\":n,\"validate\":true|false}\n\nСоздает SIDECHAIN_CHECKPOINT и возвращает payload для подписи комитета. validate по умолчанию = true.\n",
        .requires_wallet = false,
        .param_names = {"request"}
    });

    // finalizesidechaincheckpoint - подставить подпись в SIDECHAIN_CHECKPOINT
    register_method({
        .name = "finalizesidechaincheckpoint",
        .method = [this](const json& params) -> json {
            return finalize_committee_signed_transaction(
                params,
                core::TransactionType::SIDECHAIN_CHECKPOINT,
                "Неверная hex-строка транзакции",
                "tx_hex не является SIDECHAIN_CHECKPOINT"
            );
        },
        .category = "sidechain",
        .help = "finalizesidechaincheckpoint \"tx_hex\" [\"sig1_hex\",\"sig2_hex\"]\n\nВозвращает финальный tx_hex с подписями комитета для SIDECHAIN_CHECKPOINT.\n",
        .requires_wallet = false,
        .param_names = {"tx_hex", "signature_hex"}
    });

}

} // namespace geocoin::rpc
