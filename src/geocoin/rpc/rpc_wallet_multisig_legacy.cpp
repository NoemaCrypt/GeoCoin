#include "geocoin/rpc/rpc_amount.hpp"
#include "geocoin/rpc/rpc_wallet_helpers.hpp"
#include "geocoin/rpc/rpc_params.hpp"
#include "geocoin/wallet/wallet_application_service.hpp"

#include <algorithm>

namespace geocoin::rpc {

void RPCServer::register_wallet_multisig_legacy_methods() {

    // getaddressesbyaccount - Возвращает список адресов для указанного аккаунта
    register_method({
        .name = "getaddressesbyaccount",
        .method = [this](const json& params) -> json {
            auto& wallet = require_wallet();

            const json& account = required_param(
                params, 0, "Требуется account", RPC_INVALID_PARAMS);
            if (!account.is_string()) {
                throw RPCException(RPC_INVALID_PARAMS, "account должен быть строкой");
            }

            constexpr size_t kDefaultAddressPage = 1000;
            constexpr size_t kMaxAddressPage = 5000;
            const size_t offset = u64_to_size_checked(
                optional_u64_param(params, 1, 0, "offset must be a non-negative integer"),
                "offset is too large");
            const size_t limit = optional_bounded_size_param(
                params, 2, kDefaultAddressPage, 1, kMaxAddressPage, "limit");

            const auto addresses = wallet.get_all_addresses();
            json result = json::array();
            if (offset < addresses.size()) {
                const size_t end = std::min(addresses.size(), offset + limit);
                for (size_t i = offset; i < end; ++i) {
                    result.push_back(addresses[i].str());
                }
            }

            return result;
        },
        .category = "wallet",
        .help = "getaddressesbyaccount \"account\" ( offset limit )\n\nВозвращает bounded страницу адресов кошелька (default 1000, maximum 5000).\n\nАргументы:\n1. \"account\"    (string, required) Имя аккаунта (legacy; список не фильтруется по имени)\n2. offset        (numeric, optional, default=0) Пропустить столько адресов\n3. limit         (numeric, optional, default=1000, max=5000) Максимум адресов в ответе\n\nРезультат:\n[                     (json array of string)\n  \"address\"    (string) адрес geocoin\n  ,...\n]\n",
        .requires_wallet = true,
        .param_names = {"account", "offset", "limit"},
        .deprecation = "Legacy account-style wallet endpoint; accounts are not a first-class API for new clients."
    });

    // getfundbalance - Возвращает баланс дивидендного фонда (10% эмиссии)
    register_method({
        .name = "getfundbalance",
        .method = [this](const json& params) -> json {
            auto& chain_state = require_chain_state();

            // Адрес дивидендного фонда (hardcoded в consensus)
            // double: 0.41650486
            // Address: (see consensus::params::FOUNDATION_ADDRESS)
            Address fund_address = chain_state.get_foundation_address();

            Amount fund_balance = chain_state.get_balance(fund_address);
            double balance_gc = static_cast<double>(fund_balance) / COIN;

            json result;
            result["address"] = fund_address.str();
            result["balance"] = balance_gc;
            result["balance_angles"] = fund_balance;
            result["percentage"] = 10.0;  // 10% от эмиссии

            // Дополнительная информация
            Height current_height = chain_state.get_height();
            result["current_height"] = current_height;

            // Ожидаемый баланс фонда (10% от текущей эмиссии)
            // Примерный расчет: каждый блок добавляет 5 монет в фонд (10% от 50)
            Amount expected_balance = 0;
            constexpr Height kHalvingInterval = 210000;
            for (Height halvings = 0; halvings < 64; ++halvings) {
                const Height epoch_start = halvings * kHalvingInterval;
                if (epoch_start > current_height) {
                    break;
                }
                Amount block_reward = 50 * COIN;
                block_reward >>= halvings;
                const Amount fund_reward = block_reward / 10;
                const Height distance = current_height - epoch_start;
                const Height blocks = distance >= kHalvingInterval - 1
                    ? kHalvingInterval
                    : distance + 1;
                if (fund_reward != 0 &&
                    blocks > static_cast<Height>((std::numeric_limits<Amount>::max)() / fund_reward)) {
                    throw RPCException(RPC_INTERNAL_ERROR, "Переполнение ожидаемого баланса фонда");
                }
                const Amount epoch_reward = fund_reward * static_cast<Amount>(blocks);
                expected_balance = checked_add_amount_or_throw(
                    expected_balance,
                    epoch_reward,
                    "Переполнение ожидаемого баланса фонда",
                    RPC_INTERNAL_ERROR
                );
            }

            double expected_gc = static_cast<double>(expected_balance) / COIN;
            result["expected_balance"] = expected_gc;
            result["expected_balance_angles"] = expected_balance;

            return result;
        },
        .category = "blockchain",
        .help = "getfundbalance\n\nВозвращает баланс дивидендного фонда Proof-of-Equity.\n\nДивидендный фонд получает 10% от каждой награды за блок.\nЭти монеты используются для выплаты дивидендов акционерам.\n\nРезультат:\n{\n  \"address\": \"GEOM...\",              (string) Адрес фонда\n  \"balance\": x.xxx,                   (numeric) Баланс в GC\n  \"balance_angles\": n,                (numeric) Баланс в angles\n  \"percentage\": 10.0,                 (numeric) Процент от эмиссии\n  \"current_height\": n,                (numeric) Текущая высота блокчейна\n  \"expected_balance\": x.xxx,          (numeric) Ожидаемый баланс в GC\n  \"expected_balance_angles\": n        (numeric) Ожидаемый баланс в angles\n}\n\nПример:\n> geocoin-cli getfundbalance\n",
        .requires_wallet = false,
        .required_role = RPCAuthRole::READONLY
    });

    // registermultisig - Регистрирует конфигурацию мультиподписи
    register_method({
        .name = "registermultisig",
        .method = [this](const json& params) -> json {
            require_min_params(params, 3, "Требуется: registermultisig <address> <m> <n> [<participant1> <participant2> ...]");

            std::string multisig_addr_str = params[0].get<std::string>();
            int m = required_integer_value<int>(params[1], "m must be an integer in range");
            int n = required_integer_value<int>(params[2], "n must be an integer in range");

            if (m < 1 || n < 1 || m > n ||
                n > static_cast<int>(consensus::params::MAX_MULTISIG_PARTICIPANTS)) {
                throw RPCException(RPC_INVALID_PARAMETER, "Неверные параметры m и n");
            }

            require_min_params(params, 3 + static_cast<std::size_t>(n), "Требуется " + std::to_string(n) + " адресов участников");
            if (params.size() != 3 + static_cast<std::size_t>(n)) {
                throw RPCException(RPC_INVALID_PARAMS, "Количество участников не совпадает с n");
            }

            wallet::MultisigConfig config;
            config.m = m;
            config.n = n;
            config.address = Address(multisig_addr_str);

            for (int i = 0; i < n; i++) {
                std::string participant_addr = params[3 + i].get<std::string>();
                wallet::MultisigParticipant participant;
                participant.address = Address(participant_addr);
                config.participants.push_back(participant);
            }

            wallet::WalletApplicationService use_cases(
                wallet_, chain_state_, chain_mutations_);
            std::string registration_error;
            if (!use_cases.remember_multisig_config(config, registration_error)) {
                throw RPCException(
                    RPC_INVALID_PARAMETER,
                    registration_error.empty()
                        ? "Конфигурация не соответствует multisig адресу или содержит неверные адреса"
                        : registration_error
                );
            }

            json result;
            result["address"] = multisig_addr_str;
            result["m"] = m;
            result["n"] = n;
            result["registered"] = true;

            return result;
        },
        .category = "multisig",
        .help = "registermultisig <address> <m> <n> <participant1> <participant2> ...\n\nСохраняет доверенную multisig-конфигурацию в локальном кошельке для наблюдения и подготовки транзакций. Консенсус проверяет участников из самой транзакции.\n",
        .requires_wallet = true,
        .variadic_params = true,
        .mutates = true,
        .side_effects = "stores wallet multisig metadata"
    });

}

} // namespace geocoin::rpc
