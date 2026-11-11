#include "geocoin/rpc/rpc_wallet_helpers.hpp"
#include "geocoin/rpc/rpc_params.hpp"

namespace geocoin::rpc {

void RPCServer::register_wallet_sidechain_checkpoint_batch_methods() {

    // createbatchsidechaincheckpointpayload - создать батч чекпоинтов
    register_method({
        .name = "createbatchsidechaincheckpointpayload",
        .method = [this](const json& params) -> json {
            const json& p = required_object_param(params, 0, "Требуется объект с полем items");
            if (!p.contains("items") || !p["items"].is_array()) {
                throw RPCException(RPC_INVALID_PARAMETER, "items обязателен (array)");
            }
            if (p["items"].empty() || p["items"].size() > consensus::params::MAX_BATCH_SIDECHAIN_CHECKPOINTS) {
                throw RPCException(RPC_INVALID_PARAMETER, "items must contain 1..MAX_BATCH_SIDECHAIN_CHECKPOINTS entries");
            }

            core::Transaction tx;
            tx.version = 2;
            tx.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()
            ).count();
            tx.type = core::TransactionType::BATCH_SIDECHAIN_CHECKPOINT;

            std::vector<json> payloads;
            const bool validate = get_validate_flag(p, "createbatchsidechaincheckpointpayload");
            state::ChainState* chain_state = validate
                ? &require_chain_state()
                : chain_state_.get();

            const Height effective_height = chain_state ? (chain_state->get_height() + 1) : 0;

            for (const auto& v : p["items"]) {
                if (!v.is_object()) {
                    throw RPCException(RPC_INVALID_PARAMETER, "items должен быть массивом объектов");
                }
                core::BatchCheckpointItem item{};

                if (!v.contains("sidechain_id") && !v.contains("genesis_hash")) {
                    throw RPCException(RPC_INVALID_PARAMETER, "item.sidechain_id или genesis_hash обязателен");
                }
                if (v.contains("sidechain_id")) {
                    if (!v["sidechain_id"].is_string()) {
                        throw RPCException(RPC_INVALID_PARAMETER, "sidechain_id must be a string");
                    }
                    if (!hex_to_hash256_checked(v["sidechain_id"].get<std::string>(), item.sidechain_id)) {
                        throw RPCException(RPC_INVALID_PARAMETER, "item.sidechain_id должен быть 32-байт hex");
                    }
                }
                if (v.contains("genesis_hash")) {
                    if (!v["genesis_hash"].is_string()) {
                        throw RPCException(RPC_INVALID_PARAMETER, "genesis_hash must be a string");
                    }
                    Hash256 gh{};
                    if (!hex_to_hash256_checked(v["genesis_hash"].get<std::string>(), gh)) {
                        throw RPCException(RPC_INVALID_PARAMETER, "item.genesis_hash должен быть 32-байт hex");
                    }
                    if (item.sidechain_id != Hash256{} && item.sidechain_id != gh) {
                        throw RPCException(RPC_INVALID_PARAMETER, "item.sidechain_id должен совпадать с genesis_hash");
                    }
                    item.sidechain_id = gh;
                }
                if (!v.contains("checkpoint") || !v["checkpoint"].is_string()) {
                    throw RPCException(RPC_INVALID_PARAMETER, "item.checkpoint обязателен");
                }
                if (!hex_to_hash256_checked(v["checkpoint"].get<std::string>(), item.checkpoint)) {
                    throw RPCException(RPC_INVALID_PARAMETER, "item.checkpoint должен быть 32-байт hex");
                }
                if (!v.contains("checkpoint_height")) {
                    throw RPCException(RPC_INVALID_PARAMETER, "item.checkpoint_height обязателен");
                }
                item.checkpoint_height = static_cast<Height>(required_u64_field(v, "checkpoint_height", "item.checkpoint_height обязателен"));
                if (item.checkpoint_height == 0 ||
                    item.checkpoint_height > consensus::params::MAX_SIDECHAIN_CHECKPOINT_HEIGHT) {
                    throw RPCException(RPC_INVALID_PARAMETER, "item.checkpoint_height вне допустимого диапазона");
                }
                if (!v.contains("fee")) {
                    throw RPCException(RPC_INVALID_PARAMETER, "item.fee обязателен");
                }
                item.fee = required_amount_field(v, "fee", "item.fee обязателен");
                const bool version_provided = v.contains("version");
                if (version_provided) {
                    item.version = optional_u8_field(v, "version", item.version, "Неподдерживаемая версия checkpoint");
                }
                if (v.contains("committee_id")) {
                    if (!v["committee_id"].is_string()) {
                        throw RPCException(RPC_INVALID_PARAMETER, "committee_id must be a string");
                    }
                    if (!hex_to_hash256_checked(v["committee_id"].get<std::string>(), item.committee_id)) {
                        throw RPCException(RPC_INVALID_PARAMETER, "item.committee_id должен быть 32-байт hex");
                    }
                }
                if (v.contains("proof_hex")) {
                    if (!v["proof_hex"].is_string()) {
                        throw RPCException(RPC_INVALID_PARAMETER, "proof_hex must be a string");
                    }
                    bytes proof;
                    if (!hex_to_bytes_checked(v["proof_hex"].get<std::string>(), proof)) {
                        throw RPCException(RPC_INVALID_PARAMETER, "item.proof_hex неверный hex");
                    }
                    item.proof = std::move(proof);
                }

                if (validate) {
                    state::SidechainRegistryEntry entry{};
                    if (!chain_state->get_sidechain_registry(item.sidechain_id, entry)) {
                        throw RPCException(RPC_INVALID_PARAMETER, "Sidechain не зарегистрирован");
                    }
                    state::SidechainDeposit deposit{};
                    if (!chain_state->get_sidechain_deposit(item.sidechain_id, deposit)) {
                        throw RPCException(RPC_INVALID_PARAMETER, "Депозит не найден для sidechain_id");
                    }
                    if (!consensus::is_supported_checkpoint_policy_version(entry.checkpoint_policy_id, entry.checkpoint_version)) {
                        throw RPCException(RPC_INVALID_PARAMETER, "Неподдерживаемая policy/version для sidechain");
                    }
                    if (entry.status != state::SidechainStatus::ACTIVE &&
                        entry.status != state::SidechainStatus::DECOMMISSIONING) {
                        throw RPCException(RPC_INVALID_PARAMETER, "Sidechain недоступен для checkpoint");
                    }
                    if (!version_provided) {
                        item.version = entry.checkpoint_version;
                    }
                    if (item.version != entry.checkpoint_version) {
                        throw RPCException(RPC_INVALID_PARAMETER, "item.version должен совпадать с версией sidechain");
                    }
                    if (entry.kind == core::SidechainKind::FINANCIAL &&
                        (entry.checkpoint_policy_id != consensus::params::FINANCIAL_CHECKPOINT_POLICY_ID ||
                         item.version != consensus::params::FINANCIAL_CHECKPOINT_VERSION)) {
                        throw RPCException(RPC_INVALID_PARAMETER, "Financial sidechain требует checkpoint policy 2/version 2");
                    }
                    if (entry.kind == core::SidechainKind::ACCOUNTING &&
                        (entry.checkpoint_policy_id != consensus::params::ACCOUNTING_CHECKPOINT_POLICY_ID ||
                         item.version != consensus::params::ACCOUNTING_CHECKPOINT_VERSION)) {
                        throw RPCException(RPC_INVALID_PARAMETER, "Accounting sidechain требует checkpoint policy 3/version 1");
                    }
                    if (item.checkpoint_height == 0 ||
                        item.checkpoint_height <= deposit.last_sidechain_checkpoint_height) {
                        throw RPCException(RPC_INVALID_PARAMETER, "checkpoint_height должен расти");
                    }
                    if (deposit.amount < item.fee) {
                        throw RPCException(RPC_INVALID_PARAMETER, "Недостаточно средств в депозите для fee");
                    }
                    if (deposit.checkpoint == item.checkpoint) {
                        throw RPCException(RPC_INVALID_PARAMETER, "checkpoint уже использован");
                    }
                    if (deposit.last_checkpoint_height > 0 &&
                        effective_height < deposit.last_checkpoint_height + consensus::params::MIN_SIDECHAIN_CHECKPOINT_INTERVAL) {
                        throw RPCException(RPC_INVALID_PARAMETER, "слишком частый checkpoint");
                    }
                    const Amount min_fee = consensus::calculate_sidechain_checkpoint_min_fee(
                        effective_height,
                        deposit.last_checkpoint_height
                    );
                    if (item.fee < min_fee) {
                        throw RPCException(RPC_INVALID_PARAMETER, "fee ниже минимального");
                    }
                    apply_active_sidechain_committee_id_or_throw(item.committee_id, entry);
                }

                if (item.version != consensus::params::FINANCIAL_CHECKPOINT_VERSION &&
                    item.version != consensus::params::ACCOUNTING_CHECKPOINT_VERSION) {
                    throw RPCException(RPC_INVALID_PARAMETER, "Неподдерживаемая версия checkpoint");
                }
                if (item.proof.empty()) {
                    throw RPCException(RPC_INVALID_PARAMETER, "proof_hex обязателен для checkpoint");
                }
                if (item.proof.size() > consensus::params::MAX_BATCH_CHECKPOINT_PROOF) {
                    throw RPCException(RPC_INVALID_PARAMETER, "proof_hex слишком большой");
                }

                bytes payload = core::build_sidechain_batch_checkpoint_payload_for_signing(item);
                json payload_json;
                payload_json["sidechain_id"] = hash256_to_hex_string(item.sidechain_id);
                payload_json["committee_id"] = hash256_to_hex_string(item.committee_id);
                payload_json["version"] = item.version;
                payload_json["payload_to_sign_hex"] = bytes_to_hex_string(payload);
                payloads.push_back(payload_json);

                tx.sidechain_batch_checkpoints.push_back(std::move(item));
            }

            json out;
            out["tx_hex"] = bytes_to_hex_string(tx.serialize());
            out["payloads"] = payloads;
            out["timestamp"] = tx.timestamp;
            return out;
        },
        .category = "sidechain",
        .help = "createbatchsidechaincheckpointpayload {\"items\":[{\"sidechain_id\":\"hex32\"|\"genesis_hash\":\"hex32\",\"checkpoint\":\"hex32\",\"checkpoint_height\":n,\"fee\":n,\"committee_id\":\"hex32\"?,\"version\":1|2?,\"proof_hex\":\"hex\"}],\"validate\":true|false}\n\nСоздает BATCH_SIDECHAIN_CHECKPOINT и возвращает payload для подписи по каждому элементу. Версия берется из registry, если validate=true и version не указана; proof_hex обязателен для financial v2 и accounting v1.\n",
        .requires_wallet = false,
        .param_names = {"request"}
    });

    register_method({
        .name = "finalizebatchsidechaincheckpoint",
        .method = [this](const json& params) -> json {
            require_min_params(params, 2, "Нужно tx_hex и массив подписей");
            core::Transaction tx = deserialize_transaction_hex_or_throw(
                params[0].get<std::string>(),
                "finalizebatchsidechaincheckpoint: неверный tx_hex",
                "tx_hex"
            );
            if (tx.type != core::TransactionType::BATCH_SIDECHAIN_CHECKPOINT) {
                throw RPCException(RPC_INVALID_PARAMETER, "finalizebatchsidechaincheckpoint: tx имеет неверный тип");
            }
            set_batch_checkpoint_signatures_from_rpc(tx, params[1]);
            return transaction_hex_result(tx);
        },
        .category = "sidechain",
        .help = "finalizebatchsidechaincheckpoint \"tx_hex\" [[\"sig1_hex\",\"sig2_hex\"], ...]\n\nВозвращает финальный tx_hex с подписями комитета для BATCH_SIDECHAIN_CHECKPOINT.\n",
        .requires_wallet = false,
        .param_names = {"tx_hex", "signatures"}
    });

    register_method({
        .name = "finalizebatchsidechaincheckpointandsubmit",
        .method = [this](const json& params) -> json {
            require_min_params(params, 2, "Нужно tx_hex и массив подписей");
            core::Transaction tx = deserialize_transaction_hex_or_throw(
                params[0].get<std::string>(),
                "finalizebatchsidechaincheckpointandsubmit: неверный tx_hex",
                "tx_hex"
            );
            if (tx.type != core::TransactionType::BATCH_SIDECHAIN_CHECKPOINT) {
                throw RPCException(RPC_INVALID_PARAMETER, "finalizebatchsidechaincheckpointandsubmit: tx имеет неверный тип");
            }
            set_batch_checkpoint_signatures_from_rpc(tx, params[1]);
            const std::string tx_hex = bytes_to_hex_string(tx.serialize());
            const std::string txid = submit_raw_transaction_hex_to_mempool(
                tx_hex,
                chain_state_,
                "finalizebatchsidechaincheckpointandsubmit",
                true,
                true,
                chain_mutations_
            );
            json out;
            out["tx_hex"] = tx_hex;
            out["txid"] = txid;
            return out;
        },
        .category = "sidechain",
        .help = "finalizebatchsidechaincheckpointandsubmit \"tx_hex\" [[\"sig1_hex\",\"sig2_hex\"], ...]\n\nФинализирует BATCH_SIDECHAIN_CHECKPOINT подписями и отправляет в mempool. Возвращает tx_hex и txid.\n",
        .requires_wallet = false,
        .param_names = {"tx_hex", "signatures"},
        .mutates = true,
        .side_effects = "submits a finalized batch sidechain checkpoint transaction to the local mempool"
    });

}
} // namespace geocoin::rpc
