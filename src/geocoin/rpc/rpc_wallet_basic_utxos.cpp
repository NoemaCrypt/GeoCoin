#include "geocoin/rpc/rpc_wallet_helpers.hpp"
#include "geocoin/rpc/rpc_params.hpp"

#include <algorithm>

namespace geocoin::rpc {
namespace {
constexpr size_t kDefaultUtxoPage = 1000;
constexpr size_t kMaxUtxoPage = 5000;
constexpr size_t kUtxoScanChunk = 512;
}

void RPCServer::register_wallet_basic_utxo_methods() {

    // listunspent - Возвращает массив неизрасходованных выходов транзакций
    register_method({
        .name = "listunspent",
        .method = [this](const json& params) -> json {
            auto& wallet = require_wallet();
            auto& chain_state = require_chain_state();

            int minconf = optional_param<int>(params, 0, 1);
            int maxconf = optional_param<int>(params, 1, 9999999);
            if (minconf < 0 || maxconf < minconf) {
                throw RPCException(
                    RPC_INVALID_PARAMETER,
                    "minconf и maxconf должны быть неотрицательными, maxconf >= minconf"
                );
            }
            const size_t offset = u64_to_size_checked(
                optional_u64_param(params, 2, 0, "offset must be a non-negative integer"),
                "offset is too large");
            const size_t limit = optional_bounded_size_param(
                params, 3, kDefaultUtxoPage, 1, kMaxUtxoPage, "limit");

            auto addresses = wallet.get_all_addresses();
            json result = json::array();
            Height current_height = chain_state.get_height();
            size_t matched = 0;

            for (const auto& addr : addresses) {
                size_t address_offset = 0;
                for (;;) {
                    const auto utxos = chain_state.get_utxos_for_address_page(
                        addr,
                        address_offset,
                        kUtxoScanChunk
                    );
                    if (utxos.empty()) {
                        break;
                    }

                    for (const auto& utxo : utxos) {
                    // Вычисляем реальное количество подтверждений
                    Height confirmations = 0;
                    if (utxo.height != 0 && current_height >= utxo.height) {
                        confirmations = current_height - utxo.height + 1;
                    }

                    // Проверяем фильтры minconf/maxconf
                    if (confirmations < static_cast<Height>(minconf) ||
                        confirmations > static_cast<Height>(maxconf)) {
                        continue;
                    }
                    if (matched < offset) {
                        ++matched;
                        continue;
                    }
                    if (result.size() >= limit) {
                        return result;
                    }
                    ++matched;

                    // Проверяем созревание coinbase
                    bool spendable = true;
                    if (utxo.is_coinbase) {
                        spendable = (confirmations >= consensus::params::COINBASE_MATURITY);
                    }

                    double amount_gc = static_cast<double>(utxo.value) / COIN;

                    json utxo_json;
                    utxo_json["txid"] = hash256_to_hex_string(utxo.tx_hash);
                    utxo_json["vout"] = utxo.vout;
                    utxo_json["address"] = addr.str();
                    utxo_json["amount"] = amount_gc;
                    utxo_json["confirmations"] = confirmations;
                    utxo_json["spendable"] = spendable;

                    result.push_back(utxo_json);
                    }

                    address_offset += utxos.size();
                    if (utxos.size() < kUtxoScanChunk) {
                        break;
                    }
                }
            }

            return result;
        },
        .category = "wallet",
        .help = "listunspent ( minconf maxconf offset limit )\n\nВозвращает bounded страницу неизрасходованных выходов кошелька (default 1000, maximum 5000).\n\nАргументы:\n1. minconf    (numeric, optional, default=1) Минимум подтверждений\n2. maxconf    (numeric, optional, default=9999999) Максимум подтверждений\n3. offset     (numeric, optional, default=0) Пропустить столько подходящих UTXO\n4. limit      (numeric, optional, default=1000, max=5000) Максимум записей в ответе\n\nРезультат:\n[                   (array of json object)\n  {\n    \"txid\" : \"txid\",    (string) ID транзакции\n    \"vout\" : n,         (numeric) индекс выхода\n    \"address\" : \"address\",  (string) адрес geocoin\n    \"amount\" : x.xxx,   (numeric) сумма транзакции в GC\n    ...\n  }\n  ,...\n]\n",
        .requires_wallet = true,
        .param_names = {"minconf", "maxconf", "offset", "limit"}
    });

    // getaddressutxos - Возвращает UTXO для произвольного адреса
    register_method({
        .name = "getaddressutxos",
        .method = [this](const json& params) -> json {
            auto& chain_state = require_chain_state();

            std::string address_str = required_param(params, 0, "Отсутствует параметр address").get<std::string>();
            Address address(address_str);

            const size_t offset = u64_to_size_checked(
                optional_u64_param(params, 1, 0, "offset must be a non-negative integer"),
                "offset is too large");
            const size_t limit = optional_bounded_size_param(
                params, 2, kDefaultUtxoPage, 1, kMaxUtxoPage, "limit");

            const auto utxos = chain_state.get_utxos_for_address_page(address, offset, limit);

            json result = json::array();
            for (const auto& utxo : utxos) {
                json utxo_json;
                utxo_json["txid"] = hash256_to_hex_string(utxo.tx_hash);
                utxo_json["vout"] = utxo.vout;
                utxo_json["amount"] = static_cast<double>(utxo.value) / COIN;
                utxo_json["value"] = utxo.value;
                utxo_json["height"] = utxo.height;
                utxo_json["coinbase"] = utxo.is_coinbase;
                result.push_back(utxo_json);
            }

            return result;
        },
        .category = "blockchain",
        .help = "getaddressutxos \"address\" ( offset limit )\n\nВозвращает bounded UTXO page для произвольного адреса (default 1000, maximum 5000).\n",
        .requires_wallet = false,
        .required_role = RPCAuthRole::READONLY,
        .param_names = {"address", "offset", "limit"}
    });

}

} // namespace geocoin::rpc
