#pragma once

#include "geocoin/node/node.hpp"

#include <cstddef>
#include <set>
#include <string>
#include <vector>

namespace geocoin::network {
class AddrMan;
}

namespace geocoin::node::detail {

struct PeerBootstrapPlan {
    std::vector<std::string> default_seed_nodes;
    std::vector<std::string> local_seed_nodes;
    std::vector<std::string> base_priority_targets;
};

bool is_self_endpoint(const std::string& endpoint, int self_check_port);

PeerBootstrapPlan build_peer_bootstrap_plan(
    const NodeConfig& config,
    bool connect_only_mode,
    bool use_testnet
);

size_t add_bootstrap_endpoints(
    network::AddrMan& addrman,
    const std::vector<std::string>& endpoints,
    const std::string& source,
    int self_check_port,
    const char* self_skip_log_prefix
);

void append_runtime_priority_targets(
    std::vector<std::string>& targets,
    const std::vector<std::string>& added_nodes,
    const std::set<std::string>& reconnect_targets
);

}  // namespace geocoin::node::detail
