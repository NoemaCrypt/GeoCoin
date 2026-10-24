#include "geocoin/rpc/rpc_wallet_helpers.hpp"
#include "geocoin/rpc/rpc_params.hpp"

#include <limits>

namespace geocoin::rpc {

void RPCServer::register_wallet_sidechain_deposit_methods() {

    // createsidechainlockpayload - создать LOCK_DEPOSIT
    register_method({
        .name = "createsidechainlockpayload",
        .method = [this](const json& params) -> json {
            const json& p = required_object_param(params, 0, "Требуется объект с полями sidechain_id, owner_address, amount, unlock_height, inputs, outputs");
            if (!p.contains("amount")) {
                throw RPCException(RPC_INVALID_PARAMETER, "amount обязателен");
            }
            if (!p.contains("unlock_height")) {
                throw RPCException(RPC_INVALID_PARAMETER, "unlock_height обязателен");
            }
            if (!p.contains("inputs") || !p["inputs"].is_array()) {
                throw RPCException(RPC_INVALID_PARAMETER, "inputs обязателен (array)");
            }
            if (!p.contains("outputs") || !p["outputs"].is_array()) {
                throw RPCException(RPC_INVALID_PARAMETER, "outputs обязателен (array)");
            }

            const Amount amount = required_amount_field(p, "amount", "amount обязателен");
            const Height unlock_height = static_cast<Height>(required_u64_field(p, "unlock_height", "unlock_height обязателен"));
            if (amount <= 0) {
                throw RPCException(RPC_INVALID_PARAMETER, "amount должен быть > 0");
            }
            if (unlock_height == 0) {
                throw RPCException(RPC_INVALID_PARAMETER, "unlock_height должен быть > 0");
            }

            core::Transaction tx;
            const auto common = build_sidechain_tx_common(p, tx, "createsidechainlockpayload");
            tx.type = core::TransactionType::LOCK_DEPOSIT;

            tx.sidechain_amount = amount;
            tx.sidechain_unlock_height = unlock_height;
            apply_optional_sidechain_checkpoint_from_rpc(p, tx);

            append_transaction_inputs_from_rpc(p["inputs"], tx.inputs);
            if (tx.inputs.empty()) {
                throw RPCException(RPC_INVALID_PARAMETER, "inputs не должен быть пустым");
            }

            append_transaction_outputs_from_rpc(p["outputs"], tx.outputs);
            if (tx.outputs.empty()) {
                throw RPCException(RPC_INVALID_PARAMETER, "outputs не должен быть пустым");
            }

            const bool validate = get_validate_flag(p, "createsidechainlockpayload");
            if (validate) {
                auto& chain_state = require_chain_state();
                if (chain_state.has_sidechain_deposit_key(common.sidechain_id)) {
                    throw RPCException(RPC_INVALID_PARAMETER, "Депозит уже существует для sidechain_id");
                }
            }

            bytes payload = core::build_sidechain_payload_for_signing(tx);

            json out;
            out["tx_hex"] = bytes_to_hex_string(tx.serialize());
            out["payload_to_sign_hex"] = bytes_to_hex_string(payload);
            out["sidechain_id"] = hash256_to_hex_string(common.sidechain_id);
            out["owner_address"] = common.owner_address;
            out["amount"] = amount;
            out["unlock_height"] = unlock_height;
            out["timestamp"] = tx.timestamp;
            return out;
        },
        .category = "sidechain",
        .help = "createsidechainlockpayload {\"sidechain_id\":\"hex32\"|\"genesis_hash\":\"hex32\",\"owner_address\":\"GEO...\",\"amount\":n,\"unlock_height\":n,\"inputs\":[{\"txid\":\"hex32\",\"vout\":n,\"sequence\":n,\"script_sig_hex\":\"..\"}],\"outputs\":[{\"address\":\"GEO...\",\"value\":n}],\"checkpoint\":\"hex32\",\"validate\":true|false}\n\nСоздает LOCK_DEPOSIT и возвращает payload для подписи сайдчейн-владельца. validate по умолчанию = true.\n",
        .requires_wallet = false,
        .param_names = {"request"}
    });

    register_method({
        .name = "finalizesidechainlock",
        .method = [this](const json& params) -> json {
            return finalize_sidechain_tx(params, core::TransactionType::LOCK_DEPOSIT, "finalizesidechainlock");
        },
        .category = "sidechain",
        .help = "finalizesidechainlock \"tx_hex\" \"signature_hex\"\n\nВозвращает финальный tx_hex с подписью для LOCK_DEPOSIT.\n",
        .requires_wallet = false,
        .param_names = {"tx_hex", "signature_hex"}
    });

    register_method({
        .name = "createsidechaintopuppayload",
        .method = [this](const json& params) -> json {
            const json& p = required_object_param(
                params,
                0,
                "Требуется объект с sidechain_id, owner_address, amount, inputs и outputs"
            );
            const Amount amount = required_amount_field(p, "amount", "amount обязателен");
            if (amount <= 0 || !p.contains("inputs") || !p["inputs"].is_array() ||
                !p.contains("outputs") || !p["outputs"].is_array()) {
                throw RPCException(RPC_INVALID_PARAMETER, "amount > 0, inputs и outputs обязательны");
            }

            core::Transaction tx;
            const auto common = build_sidechain_tx_common(p, tx, "createsidechaintopuppayload");
            tx.type = core::TransactionType::TOP_UP_SIDECHAIN_DEPOSIT;
            tx.sidechain_amount = amount;
            tx.sidechain_unlock_height = 0;
            tx.sidechain_checkpoint = {};
            append_transaction_inputs_from_rpc(p["inputs"], tx.inputs);
            append_transaction_outputs_from_rpc(p["outputs"], tx.outputs);
            if (tx.inputs.empty()) {
                throw RPCException(RPC_INVALID_PARAMETER, "inputs не должен быть пустым");
            }

            if (get_validate_flag(p, "createsidechaintopuppayload")) {
                auto& chain_state = require_chain_state();
                state::SidechainDeposit deposit{};
                if (!chain_state.get_sidechain_deposit(common.sidechain_id, deposit)) {
                    throw RPCException(RPC_INVALID_PARAMETER, "Депозит не найден для sidechain_id");
                }
                if (deposit.owner.str() != common.owner_address) {
                    throw RPCException(RPC_INVALID_PARAMETER, "owner_address не совпадает с владельцем депозита");
                }
                state::SidechainRegistryEntry entry{};
                if (chain_state.get_sidechain_registry(common.sidechain_id, entry) &&
                    entry.status != state::SidechainStatus::ACTIVE) {
                    throw RPCException(RPC_INVALID_PARAMETER, "Нельзя пополнять депозит закрываемого sidechain");
                }
            }

            const bytes payload = core::build_sidechain_payload_for_signing(tx);
            return json{
                {"tx_hex", bytes_to_hex_string(tx.serialize())},
                {"payload_to_sign_hex", bytes_to_hex_string(payload)},
                {"sidechain_id", hash256_to_hex_string(common.sidechain_id)},
                {"owner_address", common.owner_address},
                {"amount", amount},
                {"timestamp", tx.timestamp}
            };
        },
        .category = "sidechain",
        .help = "createsidechaintopuppayload {\"sidechain_id\":\"hex32\"|\"genesis_hash\":\"hex32\",\"owner_address\":\"GEO...\",\"amount\":n,\"inputs\":[...],\"outputs\":[...],\"validate\":true|false}\n\nПополняет существующий депозит.\n",
        .requires_wallet = false,
        .param_names = {"request"}
    });

    register_method({
        .name = "finalizesidechaintopup",
        .method = [this](const json& params) -> json {
            return finalize_sidechain_tx(
                params,
                core::TransactionType::TOP_UP_SIDECHAIN_DEPOSIT,
                "finalizesidechaintopup"
            );
        },
        .category = "sidechain",
        .help = "finalizesidechaintopup \"tx_hex\" \"signature_hex\"\n",
        .requires_wallet = false,
        .param_names = {"tx_hex", "signature_hex"}
    });

    // createsidechainspendpayload - создать SPEND_DEPOSIT
    register_method({
        .name = "createsidechainspendpayload",
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
            const auto common = build_sidechain_tx_common(p, tx, "createsidechainspendpayload");
            tx.type = core::TransactionType::SPEND_DEPOSIT;

            tx.sidechain_amount = amount;
            apply_optional_sidechain_checkpoint_from_rpc(p, tx);

            const Amount total_output = append_transaction_outputs_from_rpc(p["outputs"], tx.outputs);

            if (tx.outputs.empty()) {
                throw RPCException(RPC_INVALID_PARAMETER, "outputs не должен быть пустым");
            }
            if (total_output != amount) {
                throw RPCException(RPC_INVALID_PARAMETER, "Сумма outputs должна равняться amount");
            }

            const bool validate = get_validate_flag(p, "createsidechainspendpayload");
            if (validate) {
                auto& chain_state = require_chain_state();
                state::SidechainDeposit deposit{};
                if (!chain_state.get_sidechain_deposit(common.sidechain_id, deposit)) {
                    throw RPCException(RPC_INVALID_PARAMETER, "Депозит не найден для sidechain_id");
                }
                if (deposit.owner.str() != common.owner_address) {
                    throw RPCException(RPC_INVALID_PARAMETER, "owner_address не совпадает с владельцем депозита");
                }
                const Height effective_height = chain_state.get_height() + 1;
                if (effective_height < deposit.unlock_height) {
                    throw RPCException(RPC_INVALID_PARAMETER, "unlock_height еще не достигнут");
                }
                if (chain_state.has_sidechain_registry_key(common.sidechain_id)) {
                    throw RPCException(RPC_INVALID_PARAMETER, "SPEND_DEPOSIT запрещен после регистрации сайдчейна; используйте UNLOCK_DEPOSIT после закрытия");
                }
                if (amount > deposit.amount) {
                    throw RPCException(RPC_INVALID_PARAMETER, "Сумма SPEND превышает депозит");
                }
            }

            bytes payload = core::build_sidechain_payload_for_signing(tx);

            json out;
            out["tx_hex"] = bytes_to_hex_string(tx.serialize());
            out["payload_to_sign_hex"] = bytes_to_hex_string(payload);
            out["sidechain_id"] = hash256_to_hex_string(common.sidechain_id);
            out["owner_address"] = common.owner_address;
            out["amount"] = amount;
            out["timestamp"] = tx.timestamp;
            return out;
        },
        .category = "sidechain",
        .help = "createsidechainspendpayload {\"sidechain_id\":\"hex32\"|\"genesis_hash\":\"hex32\",\"owner_address\":\"GEO...\",\"amount\":n,\"outputs\":[{\"address\":\"GEO...\",\"value\":n}],\"checkpoint\":\"hex32\",\"validate\":true|false}\n\nСоздает SPEND_DEPOSIT для частичного вывода незарегистрированного депозита после unlock_height и возвращает payload для подписи. validate по умолчанию = true.\n",
        .requires_wallet = false,
        .param_names = {"request"}
    });

    register_method({
        .name = "finalizesidechainspend",
        .method = [this](const json& params) -> json {
            return finalize_sidechain_tx(params, core::TransactionType::SPEND_DEPOSIT, "finalizesidechainspend");
        },
        .category = "sidechain",
        .help = "finalizesidechainspend \"tx_hex\" \"signature_hex\"\n\nВозвращает финальный tx_hex с подписью для SPEND_DEPOSIT.\n",
        .requires_wallet = false,
        .param_names = {"tx_hex", "signature_hex"}
    });

    // createsidechainunlockpayload - создать UNLOCK_DEPOSIT
    register_method({
        .name = "createsidechainunlockpayload",
        .method = [this](const json& params) -> json {
            const json& p = required_object_param(params, 0, "Требуется объект с полями sidechain_id, owner_address, outputs");
            if (!p.contains("outputs") || !p["outputs"].is_array()) {
                throw RPCException(RPC_INVALID_PARAMETER, "outputs обязателен (array)");
            }

            core::Transaction tx;
            const auto common = build_sidechain_tx_common(p, tx, "createsidechainunlockpayload");
            tx.type = core::TransactionType::UNLOCK_DEPOSIT;

            tx.sidechain_amount = 0;
            apply_optional_sidechain_checkpoint_from_rpc(p, tx);

            const Amount total_output = append_transaction_outputs_from_rpc(p["outputs"], tx.outputs);

            if (tx.outputs.empty()) {
                throw RPCException(RPC_INVALID_PARAMETER, "outputs не должен быть пустым");
            }

            const bool validate = get_validate_flag(p, "createsidechainunlockpayload");
            if (validate) {
                auto& chain_state = require_chain_state();
                state::SidechainDeposit deposit{};
                if (!chain_state.get_sidechain_deposit(common.sidechain_id, deposit)) {
                    throw RPCException(RPC_INVALID_PARAMETER, "Депозит не найден для sidechain_id");
                }
                if (deposit.owner.str() != common.owner_address) {
                    throw RPCException(RPC_INVALID_PARAMETER, "owner_address не совпадает с владельцем депозита");
                }
                const Height effective_height = chain_state.get_height() + 1;
                if (effective_height < deposit.unlock_height) {
                    throw RPCException(RPC_INVALID_PARAMETER, "unlock_height еще не достигнут");
                }
                state::SidechainRegistryEntry entry{};
                if (chain_state.get_sidechain_registry(common.sidechain_id, entry)) {
                    if (entry.status != state::SidechainStatus::DECOMMISSIONING) {
                        throw RPCException(RPC_INVALID_PARAMETER, "Сначала выполните DECOMMISSION_SIDECHAIN");
                    }
                    if (entry.decommission_height >
                        std::numeric_limits<Height>::max() -
                            consensus::params::SIDECHAIN_DECOMMISSION_CHALLENGE_BLOCKS) {
                        throw RPCException(RPC_INVALID_PARAMETER, "Некорректная высота декомиссии");
                    }
                    const Height challenge_end = entry.decommission_height +
                        consensus::params::SIDECHAIN_DECOMMISSION_CHALLENGE_BLOCKS;
                    if (effective_height < challenge_end) {
                        throw RPCException(
                            RPC_INVALID_PARAMETER,
                            "Challenge-период еще не завершен; unlock доступен с высоты " +
                                std::to_string(challenge_end)
                        );
                    }
                }
                if (total_output != deposit.amount) {
                    throw RPCException(RPC_INVALID_PARAMETER, "Сумма outputs должна равняться сумме депозита");
                }
            }

            bytes payload = core::build_sidechain_payload_for_signing(tx);

            json out;
            out["tx_hex"] = bytes_to_hex_string(tx.serialize());
            out["payload_to_sign_hex"] = bytes_to_hex_string(payload);
            out["sidechain_id"] = hash256_to_hex_string(common.sidechain_id);
            out["owner_address"] = common.owner_address;
            out["timestamp"] = tx.timestamp;
            return out;
        },
        .category = "sidechain",
        .help = "createsidechainunlockpayload {\"sidechain_id\":\"hex32\"|\"genesis_hash\":\"hex32\",\"owner_address\":\"GEO...\",\"outputs\":[{\"address\":\"GEO...\",\"value\":n}],\"checkpoint\":\"hex32\",\"validate\":true|false}\n\nСоздает UNLOCK_DEPOSIT и возвращает payload для подписи. validate по умолчанию = true.\n",
        .requires_wallet = false,
        .param_names = {"request"}
    });

    register_method({
        .name = "finalizesidechainunlock",
        .method = [this](const json& params) -> json {
            return finalize_sidechain_tx(params, core::TransactionType::UNLOCK_DEPOSIT, "finalizesidechainunlock");
        },
        .category = "sidechain",
        .help = "finalizesidechainunlock \"tx_hex\" \"signature_hex\"\n\nВозвращает финальный tx_hex с подписью для UNLOCK_DEPOSIT.\n",
        .requires_wallet = false,
        .param_names = {"tx_hex", "signature_hex"}
    });

}

} // namespace geocoin::rpc
