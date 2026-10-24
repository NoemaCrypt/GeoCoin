#include "geocoin/rpc/rpc_amount.hpp"
#include "geocoin/rpc/rpc_wallet_helpers.hpp"
#include "geocoin/rpc/rpc_params.hpp"
#include "geocoin/crypto/addresses.hpp"
#include "geocoin/utils/secure_memory.hpp"

#include <algorithm>

namespace geocoin::rpc {

void RPCServer::register_wallet_basic_transaction_methods() {

    // sendtoaddress - Отправить сумму на указанный адрес
    register_method({
        .name = "getfeerate",
        .method = [this](const json& params) -> json {
            uint64_t recommended = compute_recommended_fee_rate_per_kb(mempool_.get());
            json result;
            const uint64_t base_rate =
                static_cast<uint64_t>(consensus::params::FEE_RATE_PER_KB);
            result["recommended_fee_rate_per_kb"] = recommended;
            result["recommended_fee_rate_per_byte"] = recommended / 1000.0;
            result["base_fee_rate_per_kb"] = base_rate;
            result["min_tx_fee"] = consensus::params::MIN_TX_FEE;
            result["recommended_fee_rate_per_kb_angles"] = recommended;
            result["recommended_fee_rate_per_kb_gc"] =
                recommended / static_cast<double>(COIN);
            result["base_fee_rate_per_kb_angles"] = base_rate;
            result["base_fee_rate_per_kb_gc"] =
                base_rate / static_cast<double>(COIN);
            result["min_tx_fee_angles"] = consensus::params::MIN_TX_FEE;
            result["min_tx_fee_gc"] =
                consensus::params::MIN_TX_FEE / static_cast<double>(COIN);
            result["congestion_multiplier"] = base_rate > 0 ? recommended / base_rate : 1;
            result["max_congestion_multiplier"] =
                network::fee_policy::MAX_CONGESTION_MULTIPLIER;
            result["fee_rate_unit"] = "angles_per_kb";
            result["fee_amount_unit"] = "angles";

            if (mempool_) {
                auto stats = mempool_->get_stats();
                result["mempool_bytes"] = stats.total_size;
                result["mempool_max_bytes"] = mempool_->max_size_limit();
                result["mempool_txs"] = stats.tx_count;
                result["mempool_avg_fee_per_byte"] = stats.avg_fee_per_byte;
                result["mempool_max_fee_per_byte"] = stats.max_fee_per_byte;
            } else {
                result["mempool_bytes"] = 0;
                result["mempool_max_bytes"] = 0;
                result["mempool_txs"] = 0;
                result["mempool_avg_fee_per_byte"] = 0;
                result["mempool_max_fee_per_byte"] = 0;
            }
            return result;
        },
        .category = "mempool",
        .help = "getfeerate\n\nВозвращает ограниченную динамическую ставку комиссии. Поля без суффикса _gc выражены в angles.\n\nРезультат:\n{\n  \"recommended_fee_rate_per_kb\": n,\n  \"recommended_fee_rate_per_kb_gc\": x.xxx,\n  \"base_fee_rate_per_kb\": n,\n  \"min_tx_fee\": n,\n  \"min_tx_fee_gc\": x.xxx,\n  \"congestion_multiplier\": n,\n  \"max_congestion_multiplier\": 16,\n  \"mempool_bytes\": n,\n  \"mempool_max_bytes\": n,\n  \"mempool_txs\": n\n}\n",
        .requires_wallet = false,
        .required_role = RPCAuthRole::READONLY
    });

    // sendtoaddress - Отправить сумму на указанный адрес
    register_method({
        .name = "sendtoaddress",
        .method = [this](const json& params) -> json {
            auto& wallet = require_wallet();
            auto& chain_state = require_chain_state();

            require_min_params(params, 2, "Отсутствуют обязательные параметры: address и amount");

            std::string to_address_str = params[0].get<std::string>();

            Address to_address(to_address_str);
            if (!crypto::verify_address(to_address)) {
                throw RPCException(RPC_INVALID_ADDRESS_OR_KEY, "Неверный адрес получателя");
            }
            Amount amount = rpc_coins_to_positive_amount(params[1], "Неверная сумма");

            if (SyncDebugEnabled()) {
                GEO_LOG_DEBUG("rpc", "sendtoaddress: to_address=" << to_address_str);
                GEO_LOG_DEBUG("rpc", "sendtoaddress: amount(angles)=" << amount);
            }

            if (amount <= 0) {
                throw RPCException(RPC_INVALID_PARAMETER, "Неверная сумма");
            }

            std::string password = optional_param<std::string>(params, 3, "");
            utils::ScopedContainerCleanser<std::string> passwordCleanser(password);
            require_wallet_password(
                wallet,
                password,
                "Кошелек защищен паролем: укажите password четвертым аргументом"
            );

            std::shared_ptr<state::ChainState> chain_state_ptr(&chain_state, [](state::ChainState*){});

            Amount fee = consensus::params::MIN_TX_FEE;
            if (has_param(params, 2)) {
                fee = rpc_coins_to_positive_amount(params[2], "Неверная комиссия");
                const Amount required_fee = wallet.estimate_required_fee(amount, chain_state_ptr);
                if (required_fee <= 0) {
                    throw RPCException(RPC_WALLET_ERROR, "Не удалось рассчитать минимальную комиссию");
                }
                fee = std::max(fee, required_fee);
            } else {
                fee = compute_dynamic_wallet_fee(&wallet, chain_state_ptr, amount, mempool_.get());
            }

            if (SyncDebugEnabled()) {
                GEO_LOG_DEBUG("rpc", "sendtoaddress: fee=" << fee);
            }

            core::Transaction tx;

            try {
                if (!wallet.create_transaction(to_address, amount, fee, chain_state_ptr, tx)) {
                    throw RPCException(RPC_WALLET_ERROR, "Не удалось создать транзакцию");
                }
            } catch (const RPCException&) {
                throw;
            } catch (const std::exception& e) {
                GEO_LOG_ERROR(
                    "rpc",
                    "wallet transaction creation failed: " << sanitize_for_log(e.what())
                );
                throw RPCException(RPC_WALLET_ERROR, "Не удалось создать транзакцию");
            }

            return submit_transaction_to_mempool(
                tx,
                chain_state_,
                "sendtoaddress",
                false,
                false,
                chain_mutations_
            );
        },
        .category = "wallet",
        .help = "sendtoaddress \"address\" amount ( fee password )\n\nОтправить сумму на указанный адрес.\n\nАргументы:\n1. \"address\"    (string, required) Адрес geocoin получателя\n2. amount        (numeric, required) Сумма в GC для отправки\n3. fee           (numeric, optional) Комиссия в GC. Если не указана, используется динамический расчет.\n4. \"password\"   (string, optional) Пароль кошелька (обязателен для зашифрованного кошелька)\n\nРезультат:\n\"txid\"    (string) ID транзакции\n",
        .requires_wallet = true,
        .param_names = {"address", "amount", "fee", "password"},
        .mutates = true,
        .side_effects = "creates a wallet transaction and submits it to the local mempool"
    });

    // sendrawtransaction - Отправить сырую транзакцию в сеть
    register_method({
        .name = "sendrawtransaction",
        .method = [this](const json& params) -> json {
            const std::string hex_str = required_param(
                params,
                0,
                "Требуется hex-строка транзакции"
            ).get<std::string>();
            const core::Transaction tx = deserialize_transaction_hex_or_throw(
                hex_str,
                "Неверная hex-строка транзакции",
                "tx_hex"
            );
            return submit_transaction_to_mempool(
                tx,
                chain_state_,
                "",
                requires_basic_signature_verification(tx.type),
                false,
                chain_mutations_
            );
        },
        .category = "mempool",
        .help = "sendrawtransaction \"hexstring\"\n\nОтправить сырую транзакцию в сеть.\n\nАргументы:\n1. \"hexstring\"    (string, required) Hex-строка транзакции\n\nРезультат:\n\"txid\"    (string) ID транзакции\n",
        .requires_wallet = false,
        .param_names = {"hexstring"},
        .mutates = true,
        .side_effects = "submits a transaction to the local mempool"
    });

    // verifyrawtransaction - Проверить сырую транзакцию (без добавления в mempool)
    register_method({
        .name = "verifyrawtransaction",
        .method = [this](const json& params) -> json {
            auto& chain_state = require_chain_state();
            auto& mempool = require_mempool();

            const std::string hex_str = required_param(
                params,
                0,
                "Требуется hex-строка транзакции"
            ).get<std::string>();
            core::Transaction tx = deserialize_transaction_hex_or_throw(
                hex_str,
                "Неверная hex-строка транзакции",
                "tx_hex"
            );

            return raw_transaction_preflight_to_json(
                validate_raw_transaction_for_mempool(tx, chain_state, mempool)
            );
        },
        .category = "mempool",
        .help = "verifyrawtransaction \"hexstring\"\n\nПроверить сырую транзакцию (deserialize/verify/validate/contextual_check) без добавления в mempool.\n\nАргументы:\n1. \"hexstring\"    (string, required) Hex-строка транзакции\n\nРезультат:\n{\n  \"txid\": \"...\",\n  \"type\": n,\n  \"verified\": true|false,\n  \"valid\": true|false,\n  \"contextual\": true|false,\n  \"reject_reason\": \"...\"\n}\n",
        .requires_wallet = false,
        .required_role = RPCAuthRole::READONLY,
        .param_names = {"hexstring"}
    });

}

} // namespace geocoin::rpc
