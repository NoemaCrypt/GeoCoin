#include "geocoin/rpc/rpc_server.hpp"
#include "geocoin/utils/secure_memory.hpp"

#include "geocoin/rpc/rpc_encoding.hpp"
#include "geocoin/rpc/rpc_params.hpp"
#include "geocoin/rpc/rpc_wallet_helpers.hpp"
#include "geocoin/consensus/consensus.hpp"
#include "geocoin/crypto/addresses.hpp"
#include "geocoin/crypto/falcon512.hpp"
#include "geocoin/crypto/geo_area.hpp"
#include "geocoin/crypto/signatures.hpp"
#include "geocoin/wallet/wallet.hpp"
#include <cstring>
#include <utility>

namespace geocoin::rpc {
namespace {

constexpr size_t kMaxSignedMessageBytes = 64 * 1024;

bytes build_prefixed_message_payload(const std::string& message) {
    static constexpr const char* kPrefix = "GeoCoin Signed Message:\n";

    bytes payload;
    payload.reserve(std::strlen(kPrefix) + 4 + message.size());
    payload.insert(payload.end(), kPrefix, kPrefix + std::strlen(kPrefix));

    const uint32_t len = static_cast<uint32_t>(message.size());
    payload.push_back(static_cast<byte>(len & 0xFF));
    payload.push_back(static_cast<byte>((len >> 8) & 0xFF));
    payload.push_back(static_cast<byte>((len >> 16) & 0xFF));
    payload.push_back(static_cast<byte>((len >> 24) & 0xFF));

    payload.insert(payload.end(), message.begin(), message.end());
    return payload;
}

json validate_address_result(const std::string& address_str) {
    json result;
    result["isvalid"] = crypto::verify_address(address_str);
    result["address"] = address_str;
    if (result["isvalid"].get<bool>()) {
        result["isgeocoin"] = address_str.substr(0, 3) == "GEO";
    }
    return result;
}

} // namespace

void RPCServer::register_misc_signing_methods() {
    register_method({
        .name = "validateaddress",
        .method = [](const json& params) -> json {
            const std::string address_str =
                required_param(params, 0, "Отсутствует параметр address").get<std::string>();
            return validate_address_result(address_str);
        },
        .category = "util",
        .help = "validateaddress \"address\"\n\nПроверяет формат адреса без доступа к данным локального кошелька.\n\nАргументы:\n1. \"address\"    (string, required) Адрес geocoin для проверки\n\nРезультат:\n{\n  \"isvalid\" : true|false,    (boolean) Валиден ли адрес\n  \"address\" : \"address\",    (string) Проверенный адрес geocoin\n  \"isgeocoin\" : true|false   (boolean) Является ли адрес адресом geocoin\n}\n",
        .requires_wallet = false,
        .required_role = RPCAuthRole::READONLY,
        .param_names = {"address"}
    });

    register_method({
        .name = "getwalletaddressinfo",
        .method = [this](const json& params) -> json {
            const std::string address_str =
                required_param(params, 0, "Отсутствует параметр address").get<std::string>();
            json result = validate_address_result(address_str);
            result["ismine"] = result["isvalid"].get<bool>() &&
                require_wallet().has_key(Address(address_str));
            return result;
        },
        .category = "wallet",
        .help = "getwalletaddressinfo \"address\"\n\nПроверяет адрес и сообщает, принадлежит ли он загруженному локальному кошельку. Требует admin/wallet capability.\n",
        .requires_wallet = true,
        .param_names = {"address"}
    });

    register_method({
        .name = "verifysignature",
        .method = [this](const json& params) -> json {
            (void)this;
            require_min_params(params, 3, "Отсутствуют параметры: address, signature, message");

            std::string address_str = params[0].get<std::string>();
            std::string signature_hex = params[1].get<std::string>();
            std::string message = params[2].get<std::string>();
            if (message.size() > kMaxSignedMessageBytes) {
                throw RPCException(RPC_INVALID_PARAMETER, "Сообщение слишком длинное");
            }

            if (!crypto::verify_address(address_str)) {
                throw RPCException(RPC_INVALID_ADDRESS_OR_KEY, "Неверный адрес");
            }

            Signature signature;
            if (!hex_to_bytes_checked(signature_hex, signature) ||
                signature.empty() ||
                signature.size() > consensus::params::MAX_SIGNATURE_SIZE) {
                throw RPCException(RPC_INVALID_PARAMETER, "Неверный формат подписи");
            }

            Hash256 msg32 = crypto::sha256(build_prefixed_message_payload(message));
            Address recovered;
            if (!crypto::recover_signer_address_from_signed_message(msg32, signature, recovered)) {
                return false;
            }
            return recovered.str() == address_str;
        },
        .category = "geocoin",
        .help = "verifysignature \"address\" \"signature\" \"message\"\n\nПроверить подпись Falcon-512.\n\nАргументы:\n1. \"address\"     (string, required) Адрес geocoin\n2. \"signature\"   (string, required) Подпись в hex формате\n3. \"message\"     (string, required) Сообщение которое было подписано\n\nРезультат:\ntrue|false    (boolean) Валидна ли подпись\n",
        .requires_wallet = false,
        .required_role = RPCAuthRole::READONLY,
        .param_names = {"address", "signature", "message"}
    });

    register_method({
        .name = "signmessage",
        .method = [this](const json& params) -> json {
            auto& wallet = require_wallet();

            require_min_params(params, 2, "Отсутствуют параметры: address, message");

            std::string address_str = params[0].get<std::string>();
            std::string message = params[1].get<std::string>();
            std::string password = optional_param<std::string>(params, 2, "");
            utils::ScopedContainerCleanser<std::string> passwordCleanser(password);
            if (message.size() > kMaxSignedMessageBytes) {
                throw RPCException(RPC_INVALID_PARAMETER, "Сообщение слишком длинное");
            }
            require_wallet_password(
                wallet,
                password,
                "Кошелек защищен паролем: укажите password третьим аргументом"
            );

            Address address(address_str);

            if (!wallet.has_key(address)) {
                throw RPCException(RPC_WALLET_ERROR, "Приватный ключ недоступен для этого адреса");
            }

            bytes private_key;
            utils::ScopedContainerCleanser<bytes> privateKeyCleanser(private_key);

            if (!wallet.get_private_key(address, private_key)) {
                throw RPCException(RPC_WALLET_ERROR, "Не удалось получить приватный ключ");
            }

            Hash256 msg32 = crypto::sha256(build_prefixed_message_payload(message));
            Signature signature = crypto::sign_recoverable_message(msg32, private_key);

            json result;
            result["signature"] = crypto::bytes_to_hex(signature);
            result["crypto"] = "FALCON-512";

            return result;
        },
        .category = "geocoin",
        .help = "signmessage \"address\" \"message\" ( \"password\" )\n\nПодписать сообщение приватным ключом адреса используя Falcon-512.\n\nАргументы:\n1. \"address\"    (string, required) Адрес geocoin для подписи\n2. \"message\"    (string, required) Сообщение для создания подписи\n3. \"password\"   (string, optional) Пароль кошелька (обязателен для зашифрованного кошелька)\n\nРезультат:\n{\n  \"signature\" : \"signature\",      (string) Подпись в hex формате\n  \"crypto\" : \"FALCON-512\"         (string) Криптосистема\n}\n",
        .requires_wallet = true,
        .param_names = {"address", "message", "password"}
    });

    register_method({
        .name = "getfalconinfo",
        .method = [this](const json& params) -> json {
            (void)this;
            (void)params;
            const bool real_falcon_backend = crypto::falcon512::available();
            json result;

            result["name"] = "FALCON-512";
            result["description"] = "Signature payload format F512 with embedded public key payload";
            result["signature_size"] = 1600;
            result["address_prefix"] = "GEO";
            result["uses_geometric_area"] = false;
            result["area_based_verification"] = false;
            result["quantum_resistant"] = real_falcon_backend;

            return result;
        },
        .category = "geocoin",
        .help = "getfalconinfo\n\nПолучить информацию о текущей криптосистеме подписи.\n\nРезультат:\n{\n  \"name\" : \"FALCON-512\",               (string) Название криптосистемы\n  \"description\" : \"...\",               (string) Описание\n  \"signature_size\" : 1600,              (numeric) Типовой размер подписи в байтах\n  \"address_prefix\" : \"GEO\",            (string) Префикс адреса\n  \"uses_geometric_area\" : false,        (boolean)\n  \"area_based_verification\" : false,    (boolean)\n  \"quantum_resistant\" : true|false      (boolean)\n}\n",
        .requires_wallet = false,
        .required_role = RPCAuthRole::READONLY
    });
}

} // namespace geocoin::rpc
