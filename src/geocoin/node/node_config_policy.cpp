#include "geocoin/node/node_config_loader.hpp"

#include <filesystem>
#include <fstream>

namespace geocoin::node {
namespace {

bool is_relative_activation_manifest_path(const std::string& path) {
    return !path.empty() &&
           path.find(':') == std::string::npos &&
           path.front() != '/' &&
           path.front() != '\\';
}

}  // namespace

const char* node_type_name(NodeType type) {
    switch (type) {
        case NodeType::FULL:
            return "full";
        case NodeType::PRUNED:
            return "pruned";
    }
    return "unknown";
}

void write_default_node_config_if_missing(const std::string& config_file) {
    if (std::filesystem::exists(config_file)) {
        return;
    }

    std::ofstream cfg(config_file, std::ios::out | std::ios::trunc);
    if (!cfg.good()) {
        return;
    }

    cfg << "# GeoCoin configuration\n";
    cfg << "config-version=1\n";
    cfg << "# Storage backend is RocksDB-only.\n";
    cfg << "# Storage profile: safe, fast-sync, unsafe-benchmark.\n";
    cfg << "# db-profile=safe\n";
    cfg << "# Optional storage tuning in MiB / jobs (0 = backend default).\n";
    cfg << "# db-write-buffer-mb=0\n";
    cfg << "# db-cache-mb=0\n";
    cfg << "# db-max-background-jobs=0\n";
    cfg << "# Benchmark only: may lose recent IBD writes after crash.\n";
    cfg << "# db-unsafe-disable-wal-during-ibd=0\n";
    cfg << "# Add persistent peers:\n";
    cfg << "# addnode=1.2.3.4:3030\n";
    cfg << "# addnode=5.6.7.8:3030\n";
    cfg << "# Optional: disable auto-connect logic\n";
    cfg << "# no-auto-connect=1\n";
    cfg << "# Optional: archive mode (store witness, disable pruning)\n";
    cfg << "# archive=1\n";
    cfg << "# Node type: full or pruned. Pruned RPC serves only the recent block window.\n";
    cfg << "# node-type=full\n";
    cfg << "# Number of recent blocks available through historical RPC on pruned nodes.\n";
    cfg << "# pruned-blocks=1000\n";
    cfg << "# PrunedNode deletes old block bodies by default. Set 0 to keep them on disk.\n";
    cfg << "# pruned-delete-old-blocks=1\n";
    cfg << "# PrunedNode deletes old block indexes by default after body pruning.\n";
    cfg << "# pruned-delete-old-indexes=1\n";
    cfg << "# Optional: local admission manifest for staged risky feature rollout (non-consensus)\n";
    cfg << "# activation-manifest=activation-manifest.json\n";
    cfg << "# Optional: local policy-checkpoint settings (non-consensus)\n";
    cfg << "# policy-checkpoint-interval=100\n";
    cfg << "# policy-checkpoint-confirmations=144\n";
    cfg << "# Emergency local-policy override; does not bypass hardened network checkpoints.\n";
    cfg << "# allow-deep-reorg=0\n";
    cfg << "# RPC request budget per client per 60s window. 0 disables the limiter.\n";
    cfg << "rpc-rate-limit=0\n";
    cfg << "# Concurrent RPC TCP connections. 0 disables the cap.\n";
    cfg << "rpc-max-connections=0\n";
    cfg << "# Concurrent RPC connections per client IP (IPv6 grouped /64). 0 disables the cap.\n";
    cfg << "rpc-max-connections-per-ip=0\n";
    cfg << "# SECURITY: keep wallet/admin RPC on loopback and publish only through TLS reverse proxy.\n";
    cfg.flush();
}

bool validate_node_config(NodeConfig& config, std::string& error) {
    if (config.node_type != NodeType::PRUNED) {
        return true;
    }

    if (config.pruned_block_retention == 0) {
        error = "Invalid pruned-blocks: value must be greater than zero";
        return false;
    }

    if (config.archive) {
        error = "Invalid node configuration: pruned node cannot run with archive=1";
        return false;
    }

    if (!config.pruned_delete_old_blocks && config.pruned_delete_old_indexes) {
        config.pruned_delete_old_indexes = false;
    }

    return true;
}

std::filesystem::path resolve_network_data_dir(
    const std::string& configured_data_dir,
    const std::string& network_id
) {
    std::filesystem::path base = configured_data_dir.empty()
        ? std::filesystem::path("./data")
        : std::filesystem::path(configured_data_dir);
    base = base.lexically_normal();

    // lexically_normal() keeps a trailing separator ("<dir>/degree/"), whose
    // filename() is empty and never equals the network id. Drop it so the
    // comparison below sees the real last component.
    if (!base.has_filename() && base.has_parent_path() && base.parent_path() != base) {
        base = base.parent_path();
    }

    if (base.filename() != network_id) {
        base /= network_id;
    }
    return base;
}

std::string resolve_activation_manifest_path(const NodeConfig& config) {
    if (config.activation_manifest_path.empty()) {
        return config.data_dir + "/activation-manifest.json";
    }
    if (is_relative_activation_manifest_path(config.activation_manifest_path)) {
        return config.data_dir + "/" + config.activation_manifest_path;
    }
    return config.activation_manifest_path;
}

}  // namespace geocoin::node
