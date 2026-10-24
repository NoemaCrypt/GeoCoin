#include "geocoin/rpc/rpc_server.hpp"

#include "geocoin/rpc/rpc_encoding.hpp"
#include "geocoin/rpc/rpc_params.hpp"
#include "geocoin/core/blockchain.hpp"
#include "geocoin/state/state.hpp"
#include "geocoin/storage/block_storage.hpp"
#include <algorithm>

namespace geocoin::rpc {

void RPCServer::register_blockchain_pruning_methods() {
    register_method({
        .name = "prunenow",
        .method = [this](const json& params) -> json {
            auto& blockchain = require_blockchain();
            auto& chain_state = require_chain_state();
            if (chain_state.is_archive_mode()) {
                throw RPCException(RPC_INVALID_REQUEST, "Archive mode: pruning disabled");
            }
            Height tip = blockchain.get_height();
            Height depth = chain_state.get_prune_depth();
            if (depth == 0 || tip <= depth) {
                json out;
                out["result"] = "nothing_to_prune";
                return out;
            }
            Height target = tip - depth;
            if (has_param(params, 0)) {
                target = std::min(target, static_cast<Height>(required_u64_value(params[0], "height должен быть числом")));
            }
            size_t max_blocks = 0;
            if (has_param(params, 1)) {
                max_blocks = static_cast<size_t>(required_u64_value(params[1], "max_blocks должен быть числом"));
            }
            if (!chain_state.prune_witness_up_to(target, max_blocks)) {
                throw RPCException(RPC_INTERNAL_ERROR, "prune failed");
            }
            Height pruned_height = 0;
            chain_state.get_witness_pruned_height(pruned_height);
            storage::BlockStorage::PruningStats stats;
            chain_state.get_pruning_stats(stats);
            json out;
            out["result"] = "ok";
            out["target_height"] = target;
            out["pruned_height"] = pruned_height;
            out["pruned_blocks"] = stats.pruned_blocks;
            out["body_bytes_reduced"] = stats.body_bytes_reduced;
            return out;
        },
        .category = "blockchain",
        .help = "prunenow [height] [max_blocks]\n\nПринудительно prunes witness до указанной высоты.\n\nАргументы:\n1. height      (numeric, optional) максимальная высота для pruning\n2. max_blocks  (numeric, optional) лимит блоков за вызов\n",
        .requires_wallet = false,
        .param_names = {"height", "max_blocks"},
        .mutates = true,
        .side_effects = "moves witness out of block bodies while retaining the payload for reconstruction"
    });

    register_method({
        .name = "unprune",
        .method = [this](const json& params) -> json {
            auto& blockchain = require_blockchain();
            auto& network = require_network_control();
            const json& query = required_param(params, 0, "Требуется hash или height");
            Hash256 hash{};
            if (query.is_string()) {
                std::string hex = query.get<std::string>();
                if (!hex_to_hash256_checked(hex, hash)) {
                    throw RPCException(RPC_INVALID_PARAMETER, "Неверный hash");
                }
            } else {
                Height h = static_cast<Height>(required_u64_value(query, "height должен быть числом"));
                if (!blockchain.get_active_block_hash_by_height(h, hash)) {
                    throw RPCException(RPC_INVALID_PARAMETER, "Высота вне активной цепи");
                }
            }
            bool sent = network.request_witness(hash);
            json out;
            out["result"] = sent ? "requested" : "no_peers";
            out["blockhash"] = hash256_to_hex_string(hash);
            return out;
        },
        .category = "blockchain",
        .help = "unprune <hash|height>\n\nЗапрашивает witness у пиров и восстанавливает блок при получении.\n",
        .requires_wallet = false,
        .param_names = {"hash_or_height"},
        .mutates = true,
        .side_effects = "requests missing witness data from peers"
    });
}

} // namespace geocoin::rpc
