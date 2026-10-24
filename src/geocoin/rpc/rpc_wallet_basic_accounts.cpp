#include "geocoin/rpc/rpc_amount.hpp"
#include "geocoin/crypto/geo_area.hpp"
#include "geocoin/utils/secure_memory.hpp"
#include "geocoin/rpc/rpc_encoding.hpp"
#include "geocoin/rpc/rpc_wallet_helpers.hpp"
#include "geocoin/rpc/rpc_params.hpp"

#include <cstdlib>
#include <exception>
#include <string_view>

namespace {

bool allow_plaintext_wallet_for_tests()
{
    const char* value = std::getenv("GEOCOIN_ALLOW_PLAINTEXT_WALLET");
    return value != nullptr && std::string_view(value) == "1";
}

const char* wallet_scan_status_name(geocoin::wallet::WalletScanStatus status)
{
    using geocoin::wallet::WalletScanStatus;
    switch (status) {
        case WalletScanStatus::COMPLETE: return "complete";
        case WalletScanStatus::READ_ERROR: return "read-error";
        case WalletScanStatus::HISTORY_PRUNED: return "history-pruned";
        case WalletScanStatus::CHAIN_CHANGED: return "chain-changed";
        case WalletScanStatus::NO_ADDRESSES: return "no-addresses";
    }
    return "unknown";
}

} // namespace

namespace geocoin::rpc {

void RPCServer::register_wallet_basic_account_methods() {

    // createwallet - Создать новый кошелек
    register_method({
        .name = "createwallet",
        .method = [this](const json& params) -> json {
            auto& wallet = require_wallet();
            std::string password = optional_param<std::string>(params, 0, "");
            utils::ScopedContainerCleanser<std::string> passwordCleanser(password);
            if (password.empty() && !allow_plaintext_wallet_for_tests()) {
                throw RPCException(
                    RPC_INVALID_PARAMETER,
                    "Пароль обязателен: создание незашифрованного кошелька отключено"
                );
            }

            // Генерируем случайный приватный ключ
            bytes private_key;
            utils::ScopedContainerCleanser<bytes> privateKeyCleanser(private_key);

            if (!crypto::generate_private_key(private_key)) {
                throw RPCException(RPC_INTERNAL_ERROR, "Не удалось сгенерировать случайный ключ");
            }

            // Инициализируем кошелек
            if (!wallet.initialize(private_key)) {
                throw RPCException(RPC_WALLET_ERROR, "Не удалось инициализировать кошелек");
            }

            // Сохраняем кошелек
            if (!wallet.save(password)) {
                throw RPCException(RPC_WALLET_ERROR, "Не удалось сохранить кошелек");
            }

            json result;
            result["address"] = wallet.get_address().str();
            result["encrypted"] = !password.empty();

            return result;
        },
        .category = "wallet",
        .help = "createwallet \"password\"\n\nСоздает новый зашифрованный кошелек со случайным приватным ключом.\n\nАргументы:\n1. \"password\"    (string, required) Пароль для шифрования кошелька\n\nРезультат:\n{\n  \"address\": \"GEO...\",    (string) Адрес кошелька\n  \"encrypted\": true        (boolean) Зашифрован ли кошелек\n}\n\nПустой пароль разрешается только для изолированных тестов через GEOCOIN_ALLOW_PLAINTEXT_WALLET=1.\n",
        .requires_wallet = true,
        .param_names = {"password"},
        .mutates = true,
        .side_effects = "initializes and saves the local wallet"
    });

    // getbalance - Возвращает общий доступный баланс
    register_method({
        .name = "getbalance",
        .method = [this](const json& params) -> json {
            auto& wallet = require_wallet();
            auto& chain_state = require_chain_state();

            auto addresses = wallet.get_all_addresses();
            Amount total_balance = 0;

            for (const auto& addr : addresses) {
                const Amount balance = chain_state.get_balance(addr);
                total_balance = checked_add_amount_or_throw(
                    total_balance,
                    balance,
                    "Переполнение баланса кошелька",
                    RPC_INTERNAL_ERROR
                );
            }

            double balance_gc = static_cast<double>(total_balance) / COIN;

            return balance_gc;
        },
        .category = "wallet",
        .help = "getbalance\n\nВозвращает общий доступный баланс.\n\nРезультат:\namount    (numeric) Общая сумма в GC полученная этим кошельком.\n",
        .requires_wallet = true
    });

    // listaddressbalances - Показывает баланс каждого адреса
    register_method({
        .name = "listaddressbalances",
        .method = [this](const json& params) -> json {
            auto& wallet = require_wallet();
            auto& chain_state = require_chain_state();

            constexpr size_t kDefaultAddressPage = 1000;
            constexpr size_t kMaxAddressPage = 5000;
            const size_t offset = u64_to_size_checked(
                optional_u64_param(params, 0, 0, "offset must be a non-negative integer"),
                "offset is too large");
            const size_t limit = optional_bounded_size_param(
                params, 1, kDefaultAddressPage, 1, kMaxAddressPage, "limit");

            const auto addresses = wallet.get_all_addresses();
            json result = json::object();
            size_t skipped = 0;

            for (const auto& addr : addresses) {
                Amount balance = chain_state.get_balance(addr);
                if (balance <= 0) {
                    continue;
                }
                if (skipped < offset) {
                    ++skipped;
                    continue;
                }
                result[addr.str()] = static_cast<double>(balance) / COIN;
                if (result.size() >= limit) {
                    break;
                }
            }

            return result;
        },
        .category = "wallet",
        .help = "listaddressbalances ( offset limit )\n\nПоказывает bounded страницу балансов адресов кошелька (только с балансом > 0; default 1000, max 5000).\n\nАргументы:\n1. offset    (numeric, optional, default=0) Пропустить столько адресов с ненулевым балансом\n2. limit     (numeric, optional, default=1000, max=5000) Максимум записей в ответе\n\nРезультат:\n{\n  \"address\": amount,\n  ...\n}\n",
        .requires_wallet = true,
        .param_names = {"offset", "limit"}
    });

    // getnewaddress - Возвращает новый адрес GeoCoin для получения платежей
    register_method({
        .name = "getnewaddress",
        .method = [this](const json& params) -> json {
            auto& wallet = require_wallet();

            std::string label = optional_param<std::string>(params, 0, "");
            std::string password = optional_param<std::string>(params, 1, "");
            utils::ScopedContainerCleanser<std::string> passwordCleanser(password);

            require_wallet_password(
                wallet,
                password,
                "Кошелек защищен паролем: укажите password вторым аргументом"
            );
            Address addr = wallet.create_new_key(label);

            if (addr.empty()) {
                throw RPCException(RPC_WALLET_ERROR, "Не удалось сгенерировать новый адрес");
            }

            // Сохраняем кошелек
            if (!wallet.save(password)) {
                throw RPCException(RPC_WALLET_ERROR, "Не удалось сохранить кошелек");
            }

            return addr.str();
        },
        .category = "wallet",
        .help = "getnewaddress ( \"label\" \"password\" )\n\nВозвращает новый адрес GeoCoin для получения платежей.\n\nАргументы:\n1. \"label\"      (string, optional) Метка для адреса\n2. \"password\"   (string, optional) Пароль кошелька (обязателен для зашифрованного кошелька)\n\nРезультат:\n\"address\"    (string) Новый адрес geocoin\n",
        .requires_wallet = true,
        .param_names = {"label", "password"},
        .mutates = true,
        .side_effects = "creates and saves a new wallet key"
    });

    // importprivkey - Импортирует приватный ключ в кошелек
    register_method({
        .name = "importprivkey",
        .method = [this](const json& params) -> json {
            auto& wallet = require_wallet();

            std::string privkey_hex = required_param(
                params,
                0,
                "Отсутствует обязательный параметр: privkey"
            ).get<std::string>();
            utils::ScopedContainerCleanser<std::string> privateKeyHexCleanser(privkey_hex);
            std::string label = optional_param<std::string>(params, 1, "");
            std::string password = optional_param<std::string>(params, 2, "");
            utils::ScopedContainerCleanser<std::string> passwordCleanser(password);
            const bool rescan = optional_param<bool>(params, 3, true);
            const Height rescan_from = optional_param<Height>(params, 4, 0);

            require_wallet_password(
                wallet,
                password,
                "Кошелек защищен паролем: укажите password третьим аргументом"
            );

            // Конвертируем hex в bytes
            if (privkey_hex.length() != 64) {
                throw RPCException(RPC_INVALID_PARAMETER, "Неверная длина приватного ключа (должно быть 64 hex символа)");
            }

            bytes private_key(32);
            utils::ScopedContainerCleanser<bytes> privateKeyCleanser(private_key);

            if (!hex_to_bytes_checked(privkey_hex, private_key) || private_key.size() != 32) {
                throw RPCException(RPC_INVALID_PARAMETER, "Приватный ключ должен быть strict hex");
            }

            // Получаем адрес из приватного ключа
            Address addr;
            if (!crypto::compute_address_from_private_key(private_key, addr)) {
                throw RPCException(RPC_WALLET_ERROR, "Не удалось вычислить адрес из приватного ключа");
            }

            // Re-importing the same key is intentionally idempotent. This lets
            // callers resume an incomplete history scan from next_height after
            // temporarily unavailable or pruned block data has been restored.
            wallet::KeyInfo existing_key;
            bool already_imported = wallet.get_key_info(addr, existing_key);
            if (!already_imported && !wallet.import_private_key(private_key, label)) {
                // A concurrent request may have inserted the same key between
                // the lookup and import. Re-read it before reporting failure.
                already_imported = wallet.get_key_info(addr, existing_key);
            }
            if (already_imported) {
                if (existing_key.private_key.size() != private_key.size() ||
                    CRYPTO_memcmp(
                        existing_key.private_key.data(),
                        private_key.data(),
                        private_key.size()
                    ) != 0) {
                    throw RPCException(
                        RPC_WALLET_ERROR,
                        "Адрес уже присутствует с другим приватным ключом"
                    );
                }
            } else if (!wallet.get_key_info(addr, existing_key)) {
                throw RPCException(RPC_WALLET_ERROR, "Не удалось импортировать приватный ключ");
            }

            wallet::WalletScanResult scan_result;
            bool rescan_attempted = false;
            if (rescan && chain_state_) {
                rescan_attempted = true;
                try {
                    scan_result = wallet.reconcile_blockchain(chain_state_, rescan_from);
                } catch (const std::exception& error) {
                    scan_result.status = wallet::WalletScanStatus::READ_ERROR;
                    scan_result.next_height = rescan_from;
                    scan_result.error = error.what();
                } catch (...) {
                    scan_result.status = wallet::WalletScanStatus::READ_ERROR;
                    scan_result.next_height = rescan_from;
                    scan_result.error = "unknown wallet rescan failure";
                }
            }

            // Persist the imported key together with any history and anchored
            // scan progress that was recovered above.
            if (!wallet.save(password)) {
                throw RPCException(RPC_WALLET_ERROR, "Не удалось сохранить кошелек");
            }

            json result;
            result["address"] = addr.str();
            result["label"] = already_imported ? existing_key.label : label;
            result["rescan_requested"] = rescan;
            result["rescan_complete"] = !rescan ||
                (rescan_attempted && scan_result.complete());
            result["rescan_status"] = !rescan
                ? "disabled"
                : (rescan_attempted ? wallet_scan_status_name(scan_result.status) : "unavailable");
            result["next_height"] = scan_result.next_height;
            if (!scan_result.error.empty()) {
                result["rescan_error"] = scan_result.error;
            }

            return result;
        },
        .category = "wallet",
        .help = "importprivkey \"privkey\" ( \"label\" \"password\" rescan start_height )\n\nИмпортирует приватный ключ и по умолчанию пересканирует активную цепь.\n\nАргументы:\n1. \"privkey\"    (string, required) Приватный ключ в hex формате (64 символа)\n2. \"label\"      (string, optional) Метка для адреса\n3. \"password\"   (string, optional) Пароль кошелька (обязателен для зашифрованного кошелька)\n4. rescan         (boolean, optional, default=true) Восстановить историю ключа\n5. start_height   (numeric, optional, default=0) Высота начала пересканирования\n\nРезультат содержит address, label, rescan_complete, rescan_status и next_height.\n",
        .requires_wallet = true,
        .param_names = {"privkey", "label", "password", "rescan", "start_height"},
        .mutates = true,
        .side_effects = "imports a private key, optionally reconciles chain history, and saves the wallet"
    });

    // dumpprivkey - Экспортирует приватный ключ для адреса
    register_method({
        .name = "dumpprivkey",
        .method = [this](const json& params) -> json {
            auto& wallet = require_wallet();

            std::string address_str = required_param(
                params,
                0,
                "Требуется адрес",
                RPC_INVALID_PARAMETER
            ).get<std::string>();
            std::string password = optional_param<std::string>(params, 1, "");
            utils::ScopedContainerCleanser<std::string> passwordCleanser(password);

            require_wallet_password(
                wallet,
                password,
                "Кошелек защищен паролем: укажите password вторым аргументом"
            );

            Address address(address_str);

            bytes private_key;
            utils::ScopedContainerCleanser<bytes> privateKeyCleanser(private_key);

            if (!wallet.get_private_key(address, private_key)) {
                throw RPCException(RPC_WALLET_ERROR, "Адрес не найден в кошельке");
            }

            return bytes_to_hex_string(private_key);
        },
        .category = "wallet",
        .help = "dumpprivkey \"address\" ( \"password\" )\n\nЭкспортирует приватный ключ для адреса.\n\nАргументы:\n1. \"address\"    (string, required) Адрес GEO\n2. \"password\"   (string, optional) Пароль кошелька (обязателен для зашифрованного кошелька)\n\nРезультат:\n\"key\"    (string) Приватный ключ в hex формате\n",
        .requires_wallet = true,
        .param_names = {"address", "password"}
    });

}

} // namespace geocoin::rpc
