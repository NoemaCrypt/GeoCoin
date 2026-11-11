#pragma once

#include "geocoin/node/node_services.hpp"
#include "geocoin/rpc/rpc_types.hpp"

#include <optional>

namespace geocoin::core {
class Block;
class Blockchain;
}
namespace geocoin::network { class Mempool; }
namespace geocoin::state { class ChainState; }

namespace geocoin::rpc {

double bits_to_difficulty(uint32_t bits);
void require_height_available_for_mining_rpc(
    const node::NodeInfoService* node_info,
    Height height
);

core::Block build_block_template(
    core::Blockchain* blockchain,
    state::ChainState* chain_state,
    network::Mempool* mempool,
    const Address& mining_address,
    std::optional<Timestamp> requested_timestamp = std::nullopt
);

} // namespace geocoin::rpc
