#include "node_connection_plan.hpp"

#include "geocoin/consensus/chainparams.hpp"
#include "geocoin/network/addrman.hpp"
#include "geocoin/network/dns_seeds.h"
#include "geocoin/network/network_utils.hpp"
#include "geocoin/node/node_runtime.hpp"
#include "geocoin/utils/logging.hpp"

#include <set>

using geocoin::utils::LogPrintf;

namespace geocoin::node::detail {
namespace {

constexpr uint16_t kLocalFallbackP2pPort = 4040;

std::vector<std::string> local_seed_endpoints(uint16_t network_port) {
    std::vector<std::string> endpoints;
    std::set<uint16_t> seen_ports;

    auto add_port = [&](uint16_t port) {
        if (port == 0 || !seen_ports.insert(port).second) {
            return;
        }
        endpoints.push_back(std::string("127.0.0.1:") + std::to_string(port));
    };

    add_port(network_port);
    add_port(kLocalFallbackP2pPort);
    return endpoints;
}

void append_unique(
    std::vector<std::string>& out,
    std::set<std::string>& seen,
    const std::string& endpoint
) {
    if (seen.insert(endpoint).second) {
        out.push_back(endpoint);
    }
}

std::vector<std::string> build_base_priority_targets(
    const NodeConfig& config,
    bool connect_only_mode,
    const std::vector<std::string>& default_seed_nodes,
    const std::vector<std::string>& local_seed_nodes
) {
    std::vector<std::string> targets;
    std::set<std::string> seen;

    if (connect_only_mode) {
        for (const auto& target : config.connect_nodes) {
            append_unique(targets, seen, target);
        }
        return targets;
    }

    for (const auto& seed : config.seed_nodes) {
        append_unique(targets, seen, seed);
    }
    for (const auto& seed : default_seed_nodes) {
        append_unique(targets, seen, seed);
    }
    for (const auto& endpoint : local_seed_nodes) {
        append_unique(targets, seen, endpoint);
    }
    return targets;
}

}  // namespace

bool is_self_endpoint(const std::string& endpoint, int self_check_port) {
    return self_check_port > 0 &&
           network::NetworkUtils::is_self_address(endpoint, self_check_port);
}

PeerBootstrapPlan build_peer_bootstrap_plan(
    const NodeConfig& config,
    bool connect_only_mode,
    bool use_testnet
) {
    PeerBootstrapPlan plan;
    if (!connect_only_mode) {
        // LOADNET has only explicit peers and local fallback endpoints. The
        // fast bootstrap path must follow the same rule as DNS seeding.
        if (consensus::Params().get_network() != consensus::Network::LOADNET) {
            plan.default_seed_nodes = network::get_hardcoded_seed_nodes(use_testnet);
        }
        plan.local_seed_nodes = local_seed_endpoints(consensus::Params().default_port());
    }
    plan.base_priority_targets = build_base_priority_targets(
        config,
        connect_only_mode,
        plan.default_seed_nodes,
        plan.local_seed_nodes
    );
    return plan;
}

size_t add_bootstrap_endpoints(
    network::AddrMan& addrman,
    const std::vector<std::string>& endpoints,
    const std::string& source,
    int self_check_port,
    const char* self_skip_log_prefix
) {
    size_t added = 0;
    for (const auto& endpoint : endpoints) {
        if (is_self_endpoint(endpoint, self_check_port)) {
            if (self_skip_log_prefix != nullptr && node_verbose_enabled()) {
                LogPrintf("  %s: %s\n", self_skip_log_prefix, endpoint.c_str());
            }
            continue;
        }
        if (addrman.add(endpoint, source)) {
            ++added;
        }
    }
    return added;
}

void append_runtime_priority_targets(
    std::vector<std::string>& targets,
    const std::vector<std::string>& added_nodes,
    const std::set<std::string>& reconnect_targets
) {
    std::set<std::string> seen(targets.begin(), targets.end());
    for (const auto& addr : added_nodes) {
        append_unique(targets, seen, addr);
    }
    for (const auto& addr : reconnect_targets) {
        append_unique(targets, seen, addr);
    }
}

}  // namespace geocoin::node::detail
