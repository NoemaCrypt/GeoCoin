#include "rpc_blockchain_helpers.hpp"

#include "geocoin/core/blockchain.hpp"
#include <sstream>

namespace geocoin::rpc {

void require_height_available_for_rpc(const node::NodeInfoService* node_info, Height height) {
    if (node_info && node_info->is_height_pruned_for_api(height)) {
        throw RPCException(RPC_MISC_ERROR, node_info->describe_pruned_api_error(height));
    }
}

void require_block_available_for_rpc(
    const core::Blockchain* blockchain,
    const node::NodeInfoService* node_info,
    const Hash256& hash
) {
    if (!node_info || !blockchain || !node_info->is_pruned_mode()) {
        return;
    }
    Height height = 0;
    if (node_info->is_block_pruned_for_api(hash, &height)) {
        throw RPCException(RPC_MISC_ERROR, node_info->describe_pruned_api_error(height));
    }
}

bool pruned_node_has_inaccessible_history(const node::NodeInfoService* node_info) {
    if (!node_info || !node_info->is_pruned_mode()) {
        return false;
    }
    return node_info->get_pruned_data_info().first_available_height > 0;
}

std::string describe_pruned_tx_lookup_error(
    const node::NodeInfoService* node_info,
    const std::string& txid
) {
    const auto info = node_info->get_pruned_data_info();
    std::ostringstream oss;
    oss << "Транзакция " << txid << " не найдена в доступном окне Pruned-ноды. "
        << "Эта нода обслуживает только последние " << info.retention
        << " блоков (доступно с высоты " << info.first_available_height
        << ", текущая высота " << info.tip_height
        << "). Старые транзакции запрашивайте у FullNode или архивной ноды.";
    return oss.str();
}

} // namespace geocoin::rpc
