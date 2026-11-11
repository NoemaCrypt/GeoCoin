#include "geocoin/rpc/rpc_server.hpp"

#include "geocoin/rpc/rpc_params.hpp"
#include "geocoin/wallet/wallet.hpp"
#include <string>

namespace geocoin::rpc {

void RPCServer::register_mining_control_methods() {
    register_method({
        .name = "setgenerate",
        .method = [this](const json& params) -> json {
            bool generate = required_param(params, 0, "Отсутствует параметр generate").get<bool>();
            int threads = optional_param<int>(params, 1, 1);

            if (threads < 1) {
                threads = 1;
            }
            const size_t max_threads = node::cpu_mining_thread_limit();
            if (static_cast<size_t>(threads) > max_threads) {
                throw RPCException(
                    RPC_INVALID_PARAMETER,
                    "genproclimit exceeds safe CPU thread limit (" + std::to_string(max_threads) + ")"
                );
            }

            auto& mining = require_mining_control();

            if (generate) {
                auto& wallet = require_wallet();
                if (wallet.get_key_count() == 0) {
                    throw RPCException(RPC_WALLET_ERROR, "Кошелек пуст. Создайте адрес через getnewaddress");
                }

                Address mining_address = wallet.get_address();
                if (!mining.start_mining(threads, mining_address)) {
                    throw RPCException(RPC_INTERNAL_ERROR, "Не удалось запустить майнинг");
                }
            } else {
                mining.stop_mining();
            }

            return nullptr;
        },
        .category = "mining",
        .help = "setgenerate generate ( genproclimit )\n\nВключить или выключить майнинг.\n\nАргументы:\n1. generate         (boolean, required) true для включения, false для выключения\n2. genproclimit     (numeric, optional) Количество потоков (по умолчанию 1)\n\nПримеры:\n> geocoin-cli setgenerate true 4\n> geocoin-cli setgenerate false\n",
        .requires_wallet = true,
        .param_names = {"generate", "genproclimit"},
        .mutates = true,
        .side_effects = "starts or stops node CPU mining"
    });
}

} // namespace geocoin::rpc
