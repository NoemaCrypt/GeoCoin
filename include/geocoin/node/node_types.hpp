#pragma once

#include "geocoin/types.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace geocoin::node {

inline constexpr size_t MAX_CPU_MINING_THREADS = 256;

inline size_t cpu_mining_thread_limit() {
    const unsigned int hw_threads = std::thread::hardware_concurrency();
    const size_t detected = hw_threads == 0 ? 1 : static_cast<size_t>(hw_threads);
    return std::max<size_t>(1, std::min<size_t>(MAX_CPU_MINING_THREADS, detected));
}

inline size_t clamp_cpu_mining_threads(size_t threads) {
    if (threads == 0) {
        return 1;
    }
    return std::min(threads, cpu_mining_thread_limit());
}

enum class NodeType {
    FULL,
    PRUNED
};

/** Stable startup failure categories for CLI/GUI adapters and operators. */
enum class NodeStartFailure {
    NONE,
    INVALID_LIFECYCLE,
    INVALID_CONFIGURATION,
    ACTIVATION_MANIFEST,
    CHAIN_INITIALIZATION,
    EMPTY_CHAIN,
    EVENT_DISPATCHER,
    CHAIN_COORDINATOR,
    P2P_LISTENER,
    BACKGROUND_RUNTIME,
    UNEXPECTED_ERROR,
};

inline const char* node_start_failure_name(NodeStartFailure failure) noexcept {
    switch (failure) {
        case NodeStartFailure::NONE: return "none";
        case NodeStartFailure::INVALID_LIFECYCLE: return "invalid-lifecycle";
        case NodeStartFailure::INVALID_CONFIGURATION: return "invalid-configuration";
        case NodeStartFailure::ACTIVATION_MANIFEST: return "activation-manifest";
        case NodeStartFailure::CHAIN_INITIALIZATION: return "chain-initialization";
        case NodeStartFailure::EMPTY_CHAIN: return "empty-chain";
        case NodeStartFailure::EVENT_DISPATCHER: return "event-dispatcher";
        case NodeStartFailure::CHAIN_COORDINATOR: return "chain-coordinator";
        case NodeStartFailure::P2P_LISTENER: return "p2p-listener";
        case NodeStartFailure::BACKGROUND_RUNTIME: return "background-runtime";
        case NodeStartFailure::UNEXPECTED_ERROR: return "unexpected-error";
    }
    return "unknown";
}

struct NodeStartResult {
    NodeStartFailure failure{NodeStartFailure::NONE};
    std::string message;

    [[nodiscard]] bool ok() const noexcept {
        return failure == NodeStartFailure::NONE;
    }

    /** A failed chain open merits operator inspection, never an implicit reset. */
    [[nodiscard]] bool chain_data_may_need_attention() const noexcept {
        return failure == NodeStartFailure::CHAIN_INITIALIZATION;
    }

    /** Chain data must never be moved or deleted merely because startup failed. */
    [[nodiscard]] constexpr bool automatic_chain_data_recovery_allowed() const noexcept {
        return false;
    }
};

inline constexpr Height DEFAULT_PRUNED_BLOCK_RETENTION = 1000;

struct PrunedDataInfo {
    bool enabled = false;
    Height retention = DEFAULT_PRUNED_BLOCK_RETENTION;
    Height tip_height = 0;
    Height first_available_height = 0;
};

/** Validated runtime configuration shared by CLI, GUI and node adapters. */
struct NodeConfig {
    uint16_t p2p_port = 0;
    uint16_t rpc_port = 0;
    size_t max_inbound_peers = 125;
    size_t max_outbound_peers = 125;

    bool enable_auto_connect = true;
    std::vector<std::string> seed_nodes;
    std::vector<std::string> connect_nodes;
    uint32_t dns_seed_delay_ms = 11000;
    uint32_t outbound_connect_timeout_ms = 5000;
    std::vector<std::string> inbound_whitelist_ips;
    bool enable_p2p_listen = true;

    size_t mempool_max_size = 300 * 1024 * 1024;
    size_t max_parallel_downloads = 16;

    bool enable_mining = false;
    Address mining_address;
    size_t mining_threads = 1;
    uint32_t mining_cooperative_pause_us = 1000;
    uint64_t mining_cooperative_batch_iterations = 65536;
    std::string mining_sha256_backend = "auto";

    std::string data_dir = "./data";
    std::string db_backend = "rocksdb";
    std::string db_profile = "safe";
    size_t db_write_buffer_mb = 0;
    size_t db_cache_mb = 0;
    int db_max_background_jobs = 0;
    bool db_unsafe_disable_wal_during_ibd = false;
    bool db_profile_explicit = false;
    bool db_write_buffer_explicit = false;
    bool db_cache_explicit = false;
    bool db_max_background_jobs_explicit = false;
    bool db_unsafe_wal_explicit = false;

    std::string network = "degree";
    bool archive = false;

    NodeType node_type = NodeType::FULL;
    Height pruned_block_retention = DEFAULT_PRUNED_BLOCK_RETENTION;
    bool pruned_delete_old_blocks = true;
    bool pruned_delete_old_indexes = true;
    bool node_type_explicit = false;
    bool pruned_block_retention_explicit = false;
    bool pruned_delete_old_blocks_explicit = false;
    bool pruned_delete_old_indexes_explicit = false;

    std::string activation_manifest_path;

    Height policy_checkpoint_interval = 100;
    Height policy_checkpoint_confirmations = 144;
    bool allow_deep_reorg = false;

    // 0 disables the corresponding RPC cap so local/load tests are not throttled.
    uint32_t rpc_rate_limit_max_requests_per_window = 0;
    size_t rpc_max_connections = 0;
    size_t rpc_max_connections_per_ip = 0;
    bool rpc_rate_limit_explicit = false;
    bool rpc_max_connections_explicit = false;
    bool rpc_max_connections_per_ip_explicit = false;
};

struct NodeStatus {
    bool running = false;
    bool syncing = false;
    Height current_height = 0;
    Height target_height = 0;
    Height best_peer_height = 0;
    bool initial_sync_complete = true;
    bool mining_blocked_by_sync = false;
    std::string mining_block_reason;
    size_t connected_peers = 0;
    size_t authenticated_peers = 0;
    size_t mempool_size = 0;
    uint64_t total_supply = 0;
    bool mining = false;
    size_t mining_threads = 0;
    double hashrate = 0.0;
    uint64_t mined_blocks_accepted = 0;
};

} // namespace geocoin::node
