#include "geocoin/node/node.hpp"
#include "geocoin/node/chain_coordinator.hpp"
#include "geocoin/node/chain_event_dispatcher.hpp"
#include "geocoin/node/node_config_loader.hpp"
#include "geocoin/node/node_runtime.hpp"
#include "geocoin/utils/activation_manifest.hpp"
#include "geocoin/consensus/chainparams.hpp"
#include "geocoin/utils/logging.hpp"
#include "geocoin/utils/logger.hpp"
#include "geocoin/utils/metrics.hpp"
#include "geocoin/utils/secure_memory.hpp"
#include "geocoin/storage/backend_support.hpp"
#include "geocoin/utils/bytes.hpp"
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#pragma comment(lib, "ws2_32.lib")
#endif

using geocoin::utils::LogPrintf;

namespace geocoin::node {

namespace {
inline bool IsAddrInUseError(int code) {
#ifdef _WIN32
    return code == WSAEADDRINUSE;
#else
    return code == EADDRINUSE;
#endif
}
}

void Node::select_network_and_resolve_data_dir() {
    if (!consensus::SelectParamsByNetworkId(config_.network)) {
        throw std::runtime_error("Invalid network: " + config_.network +
                                 " (valid: degree, degree-test, degree-load)");
    }

    if (config_.p2p_port == 0) {
        config_.p2p_port = consensus::Params().default_port();
    }
    if (config_.rpc_port == 0) {
        config_.rpc_port = consensus::Params().rpc_port();
    }

    LogPrintf("Selected network: %s\n", consensus::Params().network_id().c_str());
    LogPrintf("  Genesis hash: ");
    const Hash256& genesis_hash = consensus::Params().get_genesis_hash();
    for (int i = 0; i < 8; i++) {
        LogPrintf("%02x", genesis_hash[i]);
    }
    LogPrintf("...\n");
    LogPrintf("  P2P port: %u\n", static_cast<unsigned>(config_.p2p_port));
    LogPrintf("  RPC port: %u\n", static_cast<unsigned>(config_.rpc_port));

    const std::filesystem::path base_data_dir =
        resolve_network_data_dir(config_.data_dir, consensus::Params().network_id());
    std::filesystem::create_directories(base_data_dir);
    config_.data_dir = base_data_dir.string();
    LogPrintf("  Data dir: %s\n", config_.data_dir.c_str());
}

storage::StorageOptions Node::load_storage_options_from_config() {
    const std::string config_file = config_.data_dir + "/geocoin.conf";
    write_default_node_config_if_missing(config_file, config_.network);
    apply_storage_config_from_file(config_, config_file);
    apply_runtime_config_from_file(config_, config_file);

    config_.db_backend = "rocksdb";
    const auto storage_type = state::StorageFactory::Type::ROCKSDB;
    auto storage_profile = state::StorageFactory::parse_profile(config_.db_profile);
    if (!storage_profile) {
        throw std::runtime_error(
            "Invalid db-profile: " + config_.db_profile
            + " (valid: safe, fast-sync, unsafe-benchmark)"
        );
    }

    std::string config_error;
    if (!validate_node_config(config_, config_error)) {
        throw std::runtime_error(config_error);
    }

    storage::StorageOptions storage_options;
    storage_options.profile = *storage_profile;
    storage_options.write_buffer_mb = config_.db_write_buffer_mb;
    storage_options.cache_mb = config_.db_cache_mb;
    storage_options.max_background_jobs = config_.db_max_background_jobs;
    storage_options.disable_wal_during_ibd =
        config_.db_unsafe_disable_wal_during_ibd ||
        (*storage_profile == storage::StorageProfile::UNSAFE_BENCHMARK);
    storage_options.network = consensus::Params().network_id();
    storage_options.genesis_hex = utils::hash256_to_hex(consensus::Params().get_genesis_hash());
    storage::backend::configure_shared_resources(storage_options);

    LogPrintf(
        "  DB backend: backend=%s profile=%s write_buffer_mb=%zu cache_mb=%zu max_background_jobs=%d unsafe_wal_ibd=%s\n",
        state::StorageFactory::type_name(storage_type),
        state::StorageFactory::profile_name(*storage_profile),
        storage_options.write_buffer_mb,
        storage_options.cache_mb,
        storage_options.max_background_jobs,
        storage_options.disable_wal_during_ibd ? "yes" : "no"
    );
    if (config_.node_type == NodeType::PRUNED) {
        LogPrintf(
            "  Node type: %s (retaining last %llu blocks, delete_old_block_bodies=%s, delete_old_indexes=%s)\n",
            node_type_name(config_.node_type),
            static_cast<unsigned long long>(config_.pruned_block_retention),
            config_.pruned_delete_old_blocks ? "yes" : "no",
            config_.pruned_delete_old_indexes ? "yes" : "no"
        );
    } else {
        LogPrintf("  Node type: %s\n", node_type_name(config_.node_type));
    }

    return storage_options;
}

void Node::open_storage_components(const storage::StorageOptions& storage_options) {
    const auto storage_type = state::StorageFactory::Type::ROCKSDB;
    const storage::StorageBackend block_backend = storage::StorageBackend::ROCKSDB;

    block_storage_ = std::make_shared<storage::BlockStorage>(
        config_.data_dir,
        block_backend,
        storage_options
    );

    chain_state_ = std::make_shared<state::ChainState>();

    auto storage = state::StorageFactory::create_split(
        storage_type,
        config_.data_dir + "/chaindata/state",
        config_.data_dir + "/addresses/state",
        storage_options
    );
    if (!storage) {
        throw std::runtime_error(
            std::string("Failed to open UTXO storage with backend ")
            + state::StorageFactory::type_name(storage_type)
        );
    }
    chain_state_->set_storage(std::move(storage));

    chain_state_->set_block_storage(block_storage_);
    chain_state_->set_archive_mode(config_.archive);
    if (config_.node_type == NodeType::PRUNED) {
        chain_state_->set_prune_depth(config_.pruned_block_retention);
    }

    blockchain_ = std::make_shared<core::Blockchain>(chain_state_, block_storage_);
}

void Node::create_runtime_components() {
    peer_manager_ = std::make_shared<network::PeerManager>();
    peer_manager_->set_archive_mode(config_.archive);
    peer_manager_->set_chain_state(chain_state_);
    mempool_ = std::make_shared<network::Mempool>(chain_state_);
    mempool_->fee_estimator().configure_storage(config_.data_dir + "/fee-estimates.json",
        utils::hash256_to_hex(consensus::Params().get_genesis_hash()), chain_state_->get_height(), chain_state_->get_tip_hash());
    sync_manager_ = std::make_shared<network::SyncManager>(chain_state_, blockchain_);
    chain_event_dispatcher_ = std::make_unique<ChainEventDispatcher>(
        [this](const services::ChainMutationEvent& event) {
            handle_chain_mutation_event(event);
        }
    );
    chain_coordinator_ = std::make_unique<ChainCoordinator>(
        blockchain_,
        mempool_,
        [this](services::ChainMutationEvent event) {
            return chain_event_dispatcher_ &&
                chain_event_dispatcher_->publish(std::move(event));
        }
    );

    peer_manager_->set_max_peers(config_.max_inbound_peers, config_.max_outbound_peers);
    peer_manager_->set_outbound_connect_timeout(
        std::chrono::milliseconds(config_.outbound_connect_timeout_ms)
    );
    mempool_->set_max_size(config_.mempool_max_size);
    sync_manager_->set_max_parallel_downloads(config_.max_parallel_downloads);
}

void Node::initialize_metrics() {
    GEO_METRIC_SET("node.running", 0);
    GEO_METRIC_SET("node.mining", 0);
    GEO_METRIC_SET("node.height", 0);
    GEO_METRIC_SET("node.connected_peers", 0);
    GEO_METRIC_SET("node.mempool_txs", 0);
    GEO_METRIC_SET("node.hashrate_hs", 0);
}

bool Node::apply_startup_config(std::string& error) {
    if (!validate_node_config(config_, error)) {
        LogPrintf("%s\n", error.c_str());
        return false;
    }

    if (config_.node_type == NodeType::PRUNED) {
        chain_state_->set_prune_depth(config_.pruned_block_retention);
    }

    if (blockchain_) {
        blockchain_->set_policy_checkpoint_options(
            config_.policy_checkpoint_interval,
            config_.policy_checkpoint_confirmations,
            config_.allow_deep_reorg
        );
    }

    return true;
}

void Node::load_activation_manifest() {
    const std::string activation_manifest_path = resolve_activation_manifest_path(config_);
    const auto activation_manifest = utils::ensure_activation_manifest(
        activation_manifest_path,
        consensus::Params().network_id(),
        true
    );
    chain_state_->set_activation_manifest(activation_manifest);
    GEO_LOG_INFO(
        "node",
        "Local admission manifest loaded from " << activation_manifest_path
            << " (profile=" << activation_manifest.profile
            << ", security_gate_open=" << (activation_manifest.is_security_gate_open() ? "yes" : "no")
            << ", external_audit_signed_off=" << (activation_manifest.external_audit_signed_off ? "yes" : "no")
            << ", internal_signoff_signed_off=" << (activation_manifest.internal_signoff_signed_off ? "yes" : "no")
            << ")"
    );
}

Node::BlockchainStartupStatus Node::initialize_blockchain_for_start() {
    LogPrintf("Initializing blockchain...\n");
    if (!blockchain_->initialize()) {
        LogPrintf("Failed to initialize blockchain\n");
        return BlockchainStartupStatus::INITIALIZATION_FAILED;
    }

    // Safety guard: Blockchain::initialize() is responsible for loading or
    // applying the network genesis. An empty active chain after initialize()
    // means startup cannot safely continue.
    if (blockchain_->get_height() == 0 && chain_state_->get_total_supply() == 0) {
        LogPrintf("No genesis block found in storage and initialization left the chain empty.\n");
        LogPrintf("Run mine_genesis and/or bootstrap with a known chain before starting the node.\n");
        return BlockchainStartupStatus::EMPTY_CHAIN;
    }

    prune_old_block_bodies_if_needed();
    return BlockchainStartupStatus::READY;
}

void Node::configure_peer_callbacks() {
    peer_manager_->set_private_key(private_key_);
    peer_manager_->set_local_port(config_.p2p_port);

    peer_manager_->set_message_callback(
        [this](auto peer, auto type, const auto& payload) {
            handle_peer_message(peer, type, payload);
        }
    );

    peer_manager_->set_peer_authenticated_callback(
        [this](auto peer, Height peer_height) {
            handle_peer_authenticated(peer, peer_height);
        }
    );

    peer_manager_->set_get_height_callback(
        [this]() -> Height {
            return blockchain_ ? blockchain_->get_height() : 0;
        }
    );

    sync_manager_->set_block_submitter(
        [this](const auto& block) {
            return process_block(block);
        }
    );

    sync_manager_->set_peer_disconnect_callback(
        [this](auto peer, const std::string& reason) {
            if (peer_manager_) {
                peer_manager_->disconnect_peer(peer, reason);
            } else if (peer) {
                peer->disconnect();
            }
        }
    );
}

bool Node::start_p2p_listener() {
    if (!config_.enable_p2p_listen) {
        LogPrintf("P2P listener disabled, running in RPC-only mode.\n");
        p2p_server_.reset();
        peer_manager_->set_local_port(0);
        return true;
    }

    p2p_server_ = std::make_shared<network::P2PServer>(
        peer_manager_,
        private_key_,
        config_.p2p_port,
        config_.inbound_whitelist_ips
    );

    if (p2p_server_->start()) {
        return true;
    }

    const int start_error = p2p_server_->get_last_start_error();
    if (IsAddrInUseError(start_error)) {
        LogPrintf("P2P listen port %u is already in use. Continuing in outbound-only mode.\n",
                  static_cast<unsigned>(config_.p2p_port));
        p2p_server_.reset();
        peer_manager_->set_local_port(0);
        return true;
    }

    LogPrintf("Failed to start P2P server (error=%d)\n", start_error);
    return false;
}

bool Node::start_background_threads() {
    running_.store(true, std::memory_order_release);
    GEO_METRIC_SET("node.running", 1);

    try {
        update_thread_ = std::thread([this]() {
            // The update loop is the only consumer of P2P messages. If it
            // dies, the node can no longer accept blocks, so mining workers
            // would only burn CPU producing blocks that process_block() rejects
            // with "node-not-running". Close the running gate and the mining
            // flag together; stop_mining()/stop() still join the workers.
            const auto mark_runtime_failed = [this]() {
                running_.store(false, std::memory_order_release);
                connection_thread_running_.store(false, std::memory_order_release);
                mining_.store(false, std::memory_order_release);
                GEO_METRIC_SET("node.running", 0);
                GEO_METRIC_SET("node.mining", 0);
                GEO_METRIC_INC("node.runtime_failure_total", 1);
            };
            try {
                update_loop();
            } catch (const std::exception& error) {
                GEO_LOG_ERROR("node", "Update loop terminated: " << error.what());
                mark_runtime_failed();
            } catch (...) {
                GEO_LOG_ERROR("node", "Update loop terminated with a non-standard exception");
                mark_runtime_failed();
            }
        });

        if (config_.enable_auto_connect) {
            connection_thread_running_.store(true, std::memory_order_release);
            connection_thread_ = std::thread([this]() {
                try {
                    connection_loop();
                } catch (const std::exception& error) {
                    GEO_LOG_ERROR("node", "Connection loop terminated: " << error.what());
                } catch (...) {
                    GEO_LOG_ERROR("node", "Connection loop terminated with a non-standard exception");
                }
                connection_thread_running_.store(false, std::memory_order_release);
            });
            if (detail::node_verbose_enabled()) {
                LogPrintf("Auto-connect enabled\n");
            }
        }
    } catch (const std::exception& error) {
        GEO_LOG_ERROR("node", "Failed to start background thread: " << error.what());
        running_.store(false, std::memory_order_release);
        connection_thread_running_.store(false, std::memory_order_release);
        stop_background_threads();
        GEO_METRIC_SET("node.running", 0);
        return false;
    } catch (...) {
        GEO_LOG_ERROR("node", "Failed to start background thread");
        running_.store(false, std::memory_order_release);
        connection_thread_running_.store(false, std::memory_order_release);
        stop_background_threads();
        GEO_METRIC_SET("node.running", 0);
        return false;
    }

    return true;
}

void Node::stop_background_threads() {
    connection_thread_running_.store(false, std::memory_order_release);
    if (connection_thread_.joinable()) {
        connection_thread_.join();
    }

    if (update_thread_.joinable()) {
        update_thread_.join();
    }
}

void Node::clear_runtime_callbacks() {
    if (sync_manager_) {
        sync_manager_->clear_callbacks();
    }
    if (peer_manager_) {
        peer_manager_->clear_callbacks();
    }
    clear_transaction_callback();
    clear_chain_mutation_event_callback();
}

void Node::shutdown_runtime_components() {
    running_.store(false, std::memory_order_release);
    connection_thread_running_.store(false, std::memory_order_release);

    stop_mining();
    GEO_METRIC_SET("node.running", 0);

    // Close network intake before joining loops which own peer/sync callbacks.
    if (p2p_server_) {
        p2p_server_->stop();
    }
    stop_background_threads();
    // The connection worker reads the listener pointer during initialization.
    // Keep that shared_ptr stable until the worker has been joined.
    p2p_server_.reset();
    if (sync_manager_) {
        sync_manager_->stop_sync();
    }

    // Producers are gone: drain the single writer, then its ordered observers.
    if (chain_coordinator_) {
        chain_coordinator_->stop();
    }
    if (chain_event_dispatcher_) {
        chain_event_dispatcher_->stop();
    }

    clear_runtime_callbacks();
    disconnect_all_peers();
    {
        std::lock_guard<std::mutex> lock(relay_mutex_);
        pending_block_inventory_ = {};
    }
    {
        std::lock_guard<std::mutex> lock(witness_request_mutex_);
        pending_witness_requests_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(transaction_request_mutex_);
        pending_transaction_inventory_.clear();
    }
    if (peer_manager_) {
        peer_manager_->clear_private_key();
    }
    utils::cleanse_container(private_key_);

    if (block_storage_) {
        block_storage_->close();
    }
}

void Node::disconnect_all_peers() {
    if (!peer_manager_) {
        return;
    }

    auto peers = peer_manager_->get_connected_peers();
    for (auto& peer : peers) {
        if (peer) {
            peer_manager_->disconnect_peer(peer, "node-disconnect-all");
        }
    }
}

Node::Node(const NodeConfig& config)
    : config_(config)
    , running_(false)
    , mining_(false)
    , total_hashes_(0)
    , mined_blocks_accepted_(0)
    , last_reported_hashes_(0)
    , current_hashrate_(0.0)
    , connection_thread_running_(false)
{
    select_network_and_resolve_data_dir();
    const auto storage_options = load_storage_options_from_config();
    open_storage_components(storage_options);
    create_runtime_components();
    initialize_metrics();
}

Node::~Node() {
    try {
        stop();
    } catch (...) {
        // Suppress all exceptions in destructor
    }
    utils::cleanse_container(private_key_);
}

void Node::stop() {
    std::lock_guard<std::mutex> stop_lock(stop_control_mutex_);
    std::unique_lock<std::mutex> lifecycle_lock(lifecycle_control_mutex_);
    if (lifecycle_state_ == LifecycleState::STOPPED) {
        return;
    }
    const bool was_started = lifecycle_state_ != LifecycleState::CREATED &&
        lifecycle_state_ != LifecycleState::STOPPING;
    lifecycle_state_ = LifecycleState::STOPPING;
    running_.store(false, std::memory_order_release);
    connection_thread_running_.store(false, std::memory_order_release);
    if (was_started) {
        GEO_METRIC_INC("node.stop_total", 1);
    }
    // Close admission first, then let in-flight observers finish calls such
    // as get_config(), connect_to_peer() or a rejected start(). Holding the
    // lifecycle mutex across join would make shutdown wait on itself.
    // stop_control_mutex_ keeps concurrent stop callers waiting for the drain.
    lifecycle_lock.unlock();
    shutdown_runtime_components();
    lifecycle_lock.lock();
    lifecycle_state_ = LifecycleState::STOPPED;
}

bool Node::start(const bytes& private_key) {
    return start_with_result(private_key).ok();
}

NodeStartResult Node::start_with_result(const bytes& private_key) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_control_mutex_);
    if (lifecycle_state_ != LifecycleState::CREATED) {
        if (lifecycle_state_ == LifecycleState::STOPPED) {
            GEO_LOG_WARN("node", "A stopped Node instance cannot be restarted; construct a new Node");
        }
        return {
            NodeStartFailure::INVALID_LIFECYCLE,
            "node instance is already starting, running, stopping, or stopped",
        };
    }
    lifecycle_state_ = LifecycleState::STARTING;

    private_key_ = private_key;

    const auto fail_start = [this](NodeStartFailure failure, const std::string& reason) {
        GEO_LOG_ERROR("node", "Node startup failed: " << reason);
        shutdown_runtime_components();
        lifecycle_state_ = LifecycleState::STOPPED;
        return NodeStartResult{failure, reason};
    };

    NodeStartFailure exception_context = NodeStartFailure::INVALID_CONFIGURATION;
    try {
        std::string config_error;
        if (!apply_startup_config(config_error)) {
            return fail_start(
                NodeStartFailure::INVALID_CONFIGURATION,
                config_error.empty() ? "invalid runtime configuration" : config_error
            );
        }
        exception_context = NodeStartFailure::ACTIVATION_MANIFEST;
        load_activation_manifest();

        LogPrintf("Starting GeoCoin node...\n");
        LogPrintf("Network: %s\n", consensus::Params().network_id().c_str());

        exception_context = NodeStartFailure::CHAIN_INITIALIZATION;
        const auto blockchain_status = initialize_blockchain_for_start();
        if (blockchain_status == BlockchainStartupStatus::INITIALIZATION_FAILED) {
            return fail_start(
                NodeStartFailure::CHAIN_INITIALIZATION,
                "blockchain initialization failed; chain data was left unchanged"
            );
        }
        if (blockchain_status == BlockchainStartupStatus::EMPTY_CHAIN) {
            return fail_start(
                NodeStartFailure::EMPTY_CHAIN,
                "chain is empty after initialization; create or import the network genesis first"
            );
        }
        exception_context = NodeStartFailure::EVENT_DISPATCHER;
        if (!chain_event_dispatcher_ || !chain_event_dispatcher_->start()) {
            return fail_start(
                NodeStartFailure::EVENT_DISPATCHER,
                "chain event dispatcher did not start"
            );
        }
        exception_context = NodeStartFailure::CHAIN_COORDINATOR;
        if (!chain_coordinator_ || !chain_coordinator_->start()) {
            return fail_start(
                NodeStartFailure::CHAIN_COORDINATOR,
                "chain coordinator did not start"
            );
        }

        exception_context = NodeStartFailure::P2P_LISTENER;
        configure_peer_callbacks();
        if (!start_p2p_listener()) {
            return fail_start(
                NodeStartFailure::P2P_LISTENER,
                "P2P listener did not start"
            );
        }
        exception_context = NodeStartFailure::BACKGROUND_RUNTIME;
        if (!start_background_threads()) {
            return fail_start(
                NodeStartFailure::BACKGROUND_RUNTIME,
                "background runtime did not start"
            );
        }
    } catch (const std::exception& error) {
        return fail_start(exception_context, error.what());
    } catch (...) {
        return fail_start(
            NodeStartFailure::UNEXPECTED_ERROR,
            "non-standard startup exception"
        );
    }

    lifecycle_state_ = LifecycleState::RUNNING;
    GEO_METRIC_INC("node.start_total", 1);

    if (detail::node_verbose_enabled()) {
        LogPrintf("Node started successfully!\n");
        LogPrintf("  Network: %s\n", config_.network.c_str());
        LogPrintf("  P2P Port: %u\n", static_cast<unsigned>(config_.p2p_port));
        LogPrintf("  Height: %llu\n", static_cast<unsigned long long>(blockchain_->get_height()));
        LogPrintf("  Supply: %lld angles\n", static_cast<long long>(chain_state_->get_total_supply()));
    }

    return {NodeStartFailure::NONE, {}};
}

} // namespace geocoin::node
