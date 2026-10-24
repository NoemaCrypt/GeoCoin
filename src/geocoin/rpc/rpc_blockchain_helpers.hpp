#pragma once

#include "geocoin/node/node_services.hpp"
#include "geocoin/rpc/rpc_types.hpp"

namespace geocoin::core { class Blockchain; }

namespace geocoin::rpc {

void require_height_available_for_rpc(const node::NodeInfoService* node_info, Height height);
void require_block_available_for_rpc(
    const core::Blockchain* blockchain,
    const node::NodeInfoService* node_info,
    const Hash256& hash
);
bool pruned_node_has_inaccessible_history(const node::NodeInfoService* node_info);
std::string describe_pruned_tx_lookup_error(
    const node::NodeInfoService* node_info,
    const std::string& txid
);

} // namespace geocoin::rpc
