#include "geocoin/rpc/rpc_server.hpp"
#include "rpc_blockchain_format.hpp"
#include "rpc_blockchain_helpers.hpp"

#include "geocoin/rpc/rpc_encoding.hpp"
#include "geocoin/rpc/rpc_params.hpp"
#include "geocoin/core/transaction.hpp"
#include "geocoin/state/state.hpp"
#include "geocoin/network/mempool.hpp"

namespace geocoin::rpc {
namespace {

constexpr size_t kDefaultHistoryPage = 1000;
constexpr size_t kMaxHistoryPage = 5000;
constexpr size_t kDefaultVerboseHistoryPage = 100;
constexpr size_t kMaxVerboseHistoryPage = 250;

size_t history_offset(const json& params, size_t index) {
    return u64_to_size_checked(
        optional_u64_param(params, index, 0, "offset must be a non-negative integer"),
        "offset is too large");
}

size_t history_limit(const json& params, size_t index, bool verbose) {
    return optional_bounded_size_param(
        params,
        index,
        verbose ? kDefaultVerboseHistoryPage : kDefaultHistoryPage,
        1,
        verbose ? kMaxVerboseHistoryPage : kMaxHistoryPage,
        "limit");
}

json indexed_tx_ref_to_json(
    const state::IndexedTxRef& ref,
    const state::ChainState& chain_state,
    bool verbose
) {
    const std::string txid_hex = hash256_to_hex_string(ref.txid);
    json item;
    item["txid"] = txid_hex;
    item["height"] = ref.height;

    if (verbose) {
        auto tx = chain_state.get_transaction(ref.txid);
        if (tx) {
            item["tx"] = transaction_to_verbose_json(*tx, ref.txid, txid_hex, chain_state);
        }
    }
    return item;
}

json indexed_tx_refs_to_json(
    const std::vector<state::IndexedTxRef>& refs,
    const state::ChainState& chain_state,
    bool verbose
) {
    json out = json::array();
    for (const auto& ref : refs) {
        out.push_back(indexed_tx_ref_to_json(ref, chain_state, verbose));
    }
    return out;
}

} // namespace

void RPCServer::register_blockchain_transaction_methods() {
    register_method({
        .name = "getrawtransaction",
        .method = [this](const json& params) -> json {
            auto& chain_state = require_chain_state();

            std::string txid_str = required_param(params, 0, "Отсутствует параметр txid").get<std::string>();
            bool verbose = optional_param<int>(params, 1, 0) != 0;

            Hash256 txid;
            if (!hex_to_hash256_checked(txid_str, txid)) {
                throw RPCException(RPC_INVALID_PARAMETER, "Неверный txid");
            }

            auto tx = chain_state.get_transaction(txid);
            if (!tx && mempool_) {
                core::Transaction pending;
                if (mempool_->get_transaction(txid, pending)) {
                    tx = std::make_unique<core::Transaction>(std::move(pending));
                }
            }
            if (!tx) {
                if (pruned_node_has_inaccessible_history(node_services_.info.get())) {
                    throw RPCException(
                        RPC_MISC_ERROR,
                        describe_pruned_tx_lookup_error(node_services_.info.get(), txid_str)
                    );
                }
                throw RPCException(RPC_INVALID_ADDRESS_OR_KEY, "Транзакция не найдена");
            }

            if (!verbose) {
                return bytes_to_hex_string(tx->serialize());
            }

            return transaction_to_verbose_json(*tx, txid, txid_str, chain_state);
        },
        .category = "blockchain",
        .help = "getrawtransaction \"txid\" ( verbose )\n\nВозвращает сырые данные транзакции.\n\nАргументы:\n1. \"txid\"    (string, required) ID транзакции\n2. verbose    (numeric, optional, default=0) 0 для hex, 1 для JSON объекта\n\nРезультат (для verbose=0):\n\"data\"      (string) Сериализованная транзакция в hex\n\nРезультат (для verbose=1):\n{\n  \"txid\" : \"id\",        (string) ID транзакции\n  \"version\" : n,          (numeric) версия\n  \"type\" : n,             (numeric) тип транзакции\n  ...\n}\n",
        .requires_wallet = false,
        .required_role = RPCAuthRole::READONLY,
        .param_names = {"txid", "verbose"}
    });

    register_method({
        .name = "getdelegationproof",
        .method = [this](const json& params) -> json {
            auto& chain_state = require_chain_state();

            std::string txid_str = required_param(params, 0, "Отсутствует параметр txid").get<std::string>();

            Hash256 txid;
            if (!hex_to_hash256_checked(txid_str, txid)) {
                throw RPCException(RPC_INVALID_PARAMETER, "Неверный txid");
            }

            auto tx = chain_state.get_transaction(txid);
            if (!tx) {
                if (pruned_node_has_inaccessible_history(node_services_.info.get())) {
                    throw RPCException(
                        RPC_MISC_ERROR,
                        describe_pruned_tx_lookup_error(node_services_.info.get(), txid_str)
                    );
                }
                throw RPCException(RPC_INVALID_ADDRESS_OR_KEY, "Delegation proof не найден");
            }

            if (tx->type != core::TransactionType::DELEGATION_PROOF) {
                throw RPCException(RPC_INVALID_PARAMETER, "Транзакция не является delegation proof");
            }

            return delegation_proof_to_json(*tx, txid, txid_str, chain_state);
        },
        .category = "blockchain",
        .help = "getdelegationproof \"txid\"\n\nВозвращает информацию о delegation proof.\n\nАргументы:\n1. \"txid\"    (string, required) ID delegation proof транзакции\n\nРезультат:\n{\n  \"txid\" : \"id\",                    (string) ID транзакции\n  \"primary_multisig_address\" : \"addr\",  (string) Основной мультисиг адрес\n  \"delegated_multisig_address\" : \"addr\", (string) Делегированный мультисиг адрес\n  \"primary_m\" : n,                    (numeric) M для primary multisig\n  \"primary_n\" : n,                    (numeric) N для primary multisig\n  \"delegated_m\" : n,                  (numeric) M для delegated multisig\n  \"delegated_n\" : n,                  (numeric) N для delegated multisig\n  \"valid_from\" : timestamp,           (numeric) Начало действия\n  \"valid_until\" : timestamp,          (numeric) Окончание действия\n  \"max_total_delegated\" : amount,     (numeric) Максимум всего делегировано\n  \"max_per_transaction\" : amount,     (numeric) Максимум за одну транзакцию\n  \"total_spent\" : amount,             (numeric) Уже потрачено\n  \"is_valid\" : true|false,            (boolean) Действителен ли proof\n  \"is_expired\" : true|false           (boolean) Истек ли proof\n}\n",
        .requires_wallet = false,
        .required_role = RPCAuthRole::READONLY,
        .param_names = {"txid"}
    });

    register_method({
        .name = "gettxout",
        .method = [this](const json& params) -> json {
            auto& chain_state = require_chain_state();

            const std::string txid_str = required_param(params, 0, "Отсутствует параметр txid").get<std::string>();
            const uint32_t vout = required_integer_value<uint32_t>(required_param(params, 1, "Отсутствует параметр vout"), "vout must be a uint32 integer");

            Hash256 txid;
            if (!hex_to_hash256_checked(txid_str, txid)) {
                throw RPCException(RPC_INVALID_PARAMETER, "Неверный txid");
            }

            state::UTXO utxo;
            if (!chain_state.get_utxo(txid, vout, utxo)) {
                return nullptr;
            }

            const Height current_height = chain_state.get_height();
            uint64_t confirmations = 0;
            if (current_height >= utxo.height) {
                confirmations = current_height - utxo.height + 1;
            }

            json result;
            result["bestblock"] = hash256_to_hex_string(chain_state.get_tip_hash());
            result["confirmations"] = confirmations;
            result["txid"] = txid_str;
            result["vout"] = vout;
            result["height"] = utxo.height;
            result["value"] = utxo.value / static_cast<double>(COIN);
            result["value_angles"] = utxo.value;
            result["address"] = utxo.address.str();
            result["coinbase"] = utxo.is_coinbase;
            return result;
        },
        .category = "blockchain",
        .help = "gettxout \"txid\" vout\n\nReturns the current unspent output, or null if the output is spent or unknown.\n",
        .requires_wallet = false,
        .required_role = RPCAuthRole::READONLY,
        .param_names = {"txid", "vout"}
    });

    register_method({
        .name = "getaddressbalance",
        .method = [this](const json& params) -> json {
            auto& chain_state = require_chain_state();

            std::string address_str = required_param(params, 0, "Отсутствует параметр address").get<std::string>();
            Address address(address_str);

            Amount balance = chain_state.get_balance(address);

            json result;
            result["address"] = address_str;
            result["balance"] = balance / static_cast<double>(COIN);
            result["balance_angles"] = balance;

            return result;
        },
        .category = "blockchain",
        .help = "getaddressbalance \"address\"\n\nВозвращает баланс любого адреса.\n\nАргументы:\n1. \"address\"    (string, required) Адрес для проверки баланса\n\nРезультат:\n{\n  \"address\" : \"addr\",    (string) Адрес\n  \"balance\" : amount,     (numeric) Баланс в GC\n  \"balance_angles\" : n    (numeric) Баланс в angles\n}\n",
        .requires_wallet = false,
        .required_role = RPCAuthRole::READONLY,
        .param_names = {"address"}
    });

    register_method({
        .name = "getaddresstxids",
        .method = [this](const json& params) -> json {
            auto& chain_state = require_chain_state();
            const std::string address_str = required_param(params, 0, "Отсутствует параметр address").get<std::string>();
            const size_t offset = history_offset(params, 1);
            const size_t limit = history_limit(params, 2, false);
            const auto refs = chain_state.get_address_txids_page(
                Address(address_str), offset, limit);

            json out = json::array();
            for (const auto& ref : refs) {
                out.push_back(hash256_to_hex_string(ref.txid));
            }
            return out;
        },
        .category = "blockchain",
        .help = "getaddresstxids \"address\" ( offset limit )\n\nReturns a bounded active-chain transaction-id page for an address (default 1000, maximum 5000).\n",
        .requires_wallet = false,
        .required_role = RPCAuthRole::READONLY,
        .param_names = {"address", "offset", "limit"}
    });

    register_method({
        .name = "getaddresshistory",
        .method = [this](const json& params) -> json {
            auto& chain_state = require_chain_state();
            const std::string address_str = required_param(params, 0, "Отсутствует параметр address").get<std::string>();
            const bool verbose = optional_param<bool>(params, 1, false);
            const size_t offset = history_offset(params, 2);
            const size_t limit = history_limit(params, 3, verbose);
            return indexed_tx_refs_to_json(
                chain_state.get_address_txids_page(Address(address_str), offset, limit),
                chain_state,
                verbose
            );
        },
        .category = "blockchain",
        .help = "getaddresshistory \"address\" ( verbose offset limit )\n\nReturns a bounded active-chain transaction page for an address (maximum 5000 refs or 250 verbose transactions).\n",
        .requires_wallet = false,
        .required_role = RPCAuthRole::READONLY,
        .param_names = {"address", "verbose", "offset", "limit"}
    });

    register_method({
        .name = "getnftshistory",
        .method = [this](const json& params) -> json {
            auto& chain_state = require_chain_state();
            const std::string nft_id_str = required_param(params, 0, "Отсутствует параметр nft_id").get<std::string>();
            const bool verbose = optional_param<bool>(params, 1, false);
            const size_t offset = history_offset(params, 2);
            const size_t limit = history_limit(params, 3, verbose);

            Hash256 nft_id;
            if (!hex_to_hash256_checked(nft_id_str, nft_id)) {
                throw RPCException(RPC_INVALID_PARAMETER, "Неверный nft_id");
            }
            return indexed_tx_refs_to_json(
                chain_state.get_nft_txids_page(nft_id, offset, limit),
                chain_state,
                verbose);
        },
        .category = "nft",
        .help = "getnftshistory \"nft_id\" ( verbose offset limit )\n\nReturns a bounded active-chain NFT history page (maximum 5000 refs or 250 verbose transactions).\n",
        .requires_wallet = false,
        .required_role = RPCAuthRole::READONLY,
        .param_names = {"nft_id", "verbose", "offset", "limit"}
    });
}

} // namespace geocoin::rpc
