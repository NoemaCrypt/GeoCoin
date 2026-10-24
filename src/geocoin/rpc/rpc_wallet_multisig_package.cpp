#include "geocoin/rpc/rpc_wallet_helpers.hpp"
#include "geocoin/rpc/rpc_params.hpp"

namespace geocoin::rpc {

void RPCServer::register_wallet_multisig_package_methods() {

    // sendmultisigpackage - Последовательно отправляет angle commitments и multisig spend
    register_method({
        .name = "sendmultisigpackage",
        .method = [this](const json& params) -> json {
            auto& chain_state = require_chain_state();
            const json& payload = required_object_param(params, 0, "Требуется объект с полями commitments и spend");
            if (!payload.contains("commitments") || !payload["commitments"].is_array()) {
                throw RPCException(RPC_INVALID_PARAMETER, "commitments должен быть массивом hex-строк");
            }
            if (!payload.contains("spend") || !payload["spend"].is_string()) {
                throw RPCException(RPC_INVALID_PARAMETER, "spend должен быть hex-строкой");
            }
            return submit_commitment_package_from_rpc(
                payload,
                "spend",
                "spend_txid",
                chain_state_,
                true,
                chain_mutations_
            );
        },
        .category = "multisig",
        .help = "sendmultisigpackage {\"commitments\":[\"hex1\",\"hex2\",...],\"spend\":\"hex\"}\n\nПоследовательно публикует все angle commitments, затем multisig spend.\nВозвращает txid каждой транзакции.\n",
        .requires_wallet = false,
        .param_names = {"request"},
        .mutates = true,
        .side_effects = "submits a commitment package to the local mempool"
    });

}

} // namespace geocoin::rpc
