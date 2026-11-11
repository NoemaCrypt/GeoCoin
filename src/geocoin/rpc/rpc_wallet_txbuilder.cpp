#include "geocoin/rpc/rpc_server.hpp"
#include "geocoin/rpc/rpc_amount.hpp"
#include "geocoin/rpc/rpc_encoding.hpp"
#include "geocoin/rpc/rpc_params.hpp"
#include "geocoin/rpc/rpc_wallet_helpers.hpp"
#include "geocoin/consensus/consensus.hpp"
#include "geocoin/crypto/addresses.hpp"
#include "geocoin/utils/secure_memory.hpp"
#include "rpc_blockchain_format.hpp"

#include <chrono>
#include <map>
#include <optional>
#include <set>

namespace geocoin::rpc {
namespace {

Amount amount_from_output_object(const json& output) {
    if (output.contains("value_angles")) {
        return rpc_angles_to_positive_amount(output["value_angles"], "value_angles должен быть положительным integer");
    }
    if (output.contains("amount")) {
        return rpc_coins_to_positive_amount(output["amount"], "amount должен быть положительным числом GC");
    }
    if (output.contains("value")) {
        return rpc_coins_to_positive_amount(output["value"], "value должен быть положительным числом GC");
    }
    throw RPCException(RPC_INVALID_PARAMETER, "output должен содержать amount, value или value_angles");
}

std::vector<core::TxInput> parse_raw_tx_inputs(const json& inputs_json, uint32_t default_sequence) {
    if (!inputs_json.is_array() || inputs_json.empty()) {
        throw RPCException(RPC_INVALID_PARAMETER, "inputs должен быть непустым массивом");
    }
    if (inputs_json.size() > consensus::params::MAX_TX_INPUTS) {
        throw RPCException(RPC_INVALID_PARAMETER, "inputs превышает consensus limit");
    }

    std::vector<core::TxInput> inputs;
    inputs.reserve(inputs_json.size());
    for (const auto& item : inputs_json) {
        if (!item.is_object()) {
            throw RPCException(RPC_INVALID_PARAMETER, "input должен быть объектом");
        }

        const std::string txid_str = required_string_field(item, "txid", "input.txid обязателен").get<std::string>();
        Hash256 txid;
        if (!hex_to_hash256_checked(txid_str, txid)) {
            throw RPCException(RPC_INVALID_PARAMETER, "input.txid неверный");
        }

        core::TxInput input;
        input.prev_tx_hash = txid;
        input.vout = u64_to_u32_checked(
            required_u64_field(item, "vout", "input.vout обязателен"),
            "input.vout вне диапазона"
        );
        input.sequence = optional_u32_field(item, "sequence", default_sequence, "input.sequence вне диапазона");
        inputs.push_back(std::move(input));
    }
    return inputs;
}

std::vector<core::TxOutput> parse_raw_tx_outputs(const json& outputs_json) {
    std::vector<core::TxOutput> outputs;

    if (outputs_json.is_object()) {
        if (outputs_json.size() > consensus::params::MAX_TX_OUTPUTS) {
            throw RPCException(RPC_INVALID_PARAMETER, "outputs превышает consensus limit");
        }
        outputs.reserve(outputs_json.size());
        for (const auto& [address, value] : outputs_json.items()) {
            core::TxOutput output;
            output.address = Address(address);
            if (!crypto::verify_address(output.address)) {
                throw RPCException(RPC_INVALID_ADDRESS_OR_KEY, "output address неверный");
            }
            output.value = rpc_coins_to_positive_amount(value, "output amount должен быть положительным числом GC");
            outputs.push_back(std::move(output));
        }
    } else if (outputs_json.is_array()) {
        if (outputs_json.size() > consensus::params::MAX_TX_OUTPUTS) {
            throw RPCException(RPC_INVALID_PARAMETER, "outputs превышает consensus limit");
        }
        outputs.reserve(outputs_json.size());
        for (const auto& item : outputs_json) {
            if (!item.is_object()) {
                throw RPCException(RPC_INVALID_PARAMETER, "output должен быть объектом");
            }
            const std::string address = required_string_field(item, "address", "output.address обязателен").get<std::string>();
            core::TxOutput output;
            output.address = Address(address);
            if (!crypto::verify_address(output.address)) {
                throw RPCException(RPC_INVALID_ADDRESS_OR_KEY, "output.address неверный");
            }
            output.value = amount_from_output_object(item);
            outputs.push_back(std::move(output));
        }
    } else {
        throw RPCException(RPC_INVALID_PARAMETER, "outputs должен быть объектом или массивом");
    }

    if (outputs.empty()) {
        throw RPCException(RPC_INVALID_PARAMETER, "outputs должен быть непустым");
    }
    return outputs;
}

json sign_raw_transaction_with_wallet(
    const core::Transaction& raw_tx,
    wallet::Wallet& wallet,
    state::ChainState& chain_state
) {
    core::Transaction tx = raw_tx;
    json errors = json::array();

    if (tx.inputs.empty()) {
        json error;
        error["error"] = "transaction has no inputs";
        errors.push_back(std::move(error));
    }
    if (tx.type != core::TransactionType::STANDARD) {
        json error;
        error["error"] = "signrawtransactionwithwallet supports STANDARD transactions only";
        error["type"] = static_cast<int>(tx.type);
        errors.push_back(std::move(error));
    }

    std::optional<Address> signing_address;
    bytes signing_key;
    utils::ScopedContainerCleanser<bytes> signing_key_cleanser(signing_key);
    std::set<std::string> missing_inputs;

    for (size_t i = 0; i < tx.inputs.size(); ++i) {
        const auto& input = tx.inputs[i];
        state::UTXO utxo;
        if (!chain_state.get_utxo(input.prev_tx_hash, input.vout, utxo)) {
            json error;
            error["txid"] = hash256_to_hex_string(input.prev_tx_hash);
            error["vout"] = input.vout;
            error["error"] = "input UTXO not found or already spent";
            errors.push_back(std::move(error));
            missing_inputs.insert(hash256_to_hex_string(input.prev_tx_hash) + ":" + std::to_string(input.vout));
            continue;
        }

        bytes private_key;
        utils::ScopedContainerCleanser<bytes> private_key_cleanser(private_key);
        if (!wallet.get_private_key(utxo.address, private_key)) {
            json error;
            error["txid"] = hash256_to_hex_string(input.prev_tx_hash);
            error["vout"] = input.vout;
            error["address"] = utxo.address.str();
            error["error"] = "private key not found in wallet";
            errors.push_back(std::move(error));
            continue;
        }

        if (!signing_address.has_value()) {
            signing_address = utxo.address;
            signing_key = std::move(private_key);
        } else if (signing_address.value() != utxo.address) {
            json error;
            error["txid"] = hash256_to_hex_string(input.prev_tx_hash);
            error["vout"] = input.vout;
            error["address"] = utxo.address.str();
            error["error"] = "multi-key signing is not supported by this transaction format";
            errors.push_back(std::move(error));
        }
    }

    const bool can_sign = errors.empty() && signing_address.has_value() && !signing_key.empty();
    if (can_sign) {
        tx.sign(signing_key);
    }

    const bytes serialized = tx.serialize();
    bool complete = can_sign && tx.verify();
    if (serialized.size() > consensus::params::MAX_TX_SIZE) {
        json error;
        error["error"] = "signed transaction exceeds consensus size limit";
        error["size"] = serialized.size();
        error["max_size"] = consensus::params::MAX_TX_SIZE;
        errors.push_back(std::move(error));
        complete = false;
    }

    json out;
    out["hex"] = bytes_to_hex_string(serialized);
    out["txid"] = transaction_txid_hex(tx);
    out["complete"] = complete;
    out["errors"] = std::move(errors);
    out["missing_inputs"] = missing_inputs.size();
    return out;
}

} // namespace

void RPCServer::register_wallet_txbuilder_methods() {
    register_method({
        .name = "createrawtransaction",
        .method = [](const json& params) -> json {
            require_min_params(params, 2, "Нужно inputs и outputs");

            core::Transaction tx;
            tx.version = 2;
            tx.type = core::TransactionType::STANDARD;
            tx.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()
            ).count();
            tx.lock_time = u64_to_u32_checked(
                optional_u64_param(
                    params,
                    2,
                    0,
                    "locktime вне диапазона"
                ),
                "locktime вне диапазона"
            );
            tx.inputs = parse_raw_tx_inputs(params[0], tx.lock_time == 0 ?
                core::TxInput::SEQUENCE_FINAL : core::TxInput::SEQUENCE_LOCKTIME);
            tx.outputs = parse_raw_tx_outputs(params[1]);

            const bytes serialized = tx.serialize();
            if (serialized.size() > consensus::params::MAX_TX_SIZE) {
                throw RPCException(RPC_INVALID_PARAMETER, "transaction exceeds consensus size limit");
            }
            return bytes_to_hex_string(serialized);
        },
        .category = "txbuilder",
        .help = "createrawtransaction inputs outputs ( locktime )\n\nBuilds an unsigned STANDARD raw transaction. outputs may be {\"address\": amount_gc} or [{\"address\":\"...\",\"amount\":amount_gc}]. Use value_angles for exact integer angles. locktime: 0 disables; below 500000000 is a height, otherwise block Unix time. The threshold must be strictly passed. With nonzero locktime, omitted input.sequence defaults to 4294967294; all inputs set to 4294967295 disable the lock. Other sequence values enable only the absolute lock, with no relative delay.\n",
        .requires_wallet = false,
        .required_role = RPCAuthRole::READONLY,
        .param_names = {"inputs", "outputs", "locktime"},
        .capabilities = {"readonly", "tx_builder"}
    });

    register_method({
        .name = "decoderawtransaction",
        .method = [this](const json& params) -> json {
            auto& chain_state = require_chain_state();
            const std::string hex_str = required_param(params, 0, "Требуется hex-строка транзакции").get<std::string>();
            const core::Transaction tx = deserialize_transaction_hex_or_throw(
                hex_str,
                "Неверная hex-строка транзакции",
                "tx_hex"
            );
            return transaction_to_verbose_json(tx, tx.get_hash(), transaction_txid_hex(tx), chain_state);
        },
        .category = "txbuilder",
        .help = "decoderawtransaction \"hexstring\"\n\nDecodes a raw transaction hex string into JSON.\n",
        .requires_wallet = false,
        .required_role = RPCAuthRole::READONLY,
        .param_names = {"hexstring"},
        .capabilities = {"readonly", "tx_builder"}
    });

    register_method({
        .name = "signrawtransactionwithwallet",
        .method = [this](const json& params) -> json {
            auto& wallet = require_wallet();
            auto& chain_state = require_chain_state();
            const std::string hex_str = required_param(params, 0, "Требуется hex-строка транзакции").get<std::string>();
            std::string password = optional_param<std::string>(params, 1, "");
            utils::ScopedContainerCleanser<std::string> passwordCleanser(password);
            require_wallet_password(
                wallet,
                password,
                "Кошелек защищен паролем: укажите password вторым аргументом"
            );
            const core::Transaction tx = deserialize_transaction_hex_or_throw(
                hex_str,
                "Неверная hex-строка транзакции",
                "tx_hex"
            );
            return sign_raw_transaction_with_wallet(tx, wallet, chain_state);
        },
        .category = "txbuilder",
        .help = "signrawtransactionwithwallet \"hexstring\" ( \"password\" )\n\nSigns a raw STANDARD transaction with wallet keys when all inputs belong to one local address.\n",
        .requires_wallet = true,
        .required_role = RPCAuthRole::ADMIN,
        .param_names = {"hexstring", "password"},
        .capabilities = {"wallet"}
    });
}

} // namespace geocoin::rpc
