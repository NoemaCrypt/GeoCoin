#include "geocoin/rpc/rpc_server.hpp"
#include "geocoin/rpc/rpc_security.hpp"
#include "geocoin/core/blockchain.hpp"
#include "geocoin/wallet/wallet.hpp"
#include "geocoin/state/state.hpp"
#include "geocoin/network/mempool.hpp"
#include "geocoin/utils/logger.hpp"
#include "geocoin/utils/rpc_bind_policy.hpp"
#include <stdexcept>
#include <thread>

namespace geocoin::rpc {

RPCServer::RPCServer(RPCCommandProfile profile)
    : command_registry_(profile) {
    register_builtin_modules();
}

RPCServer::~RPCServer() {
    stop();
}

bool RPCServer::start(
    const std::string& host,
    uint16_t port,
    const std::string& rpc_user,
    const std::string& rpc_password,
    const std::string& miner_rpc_user,
    const std::string& miner_rpc_password,
    const std::string& readonly_rpc_user,
    const std::string& readonly_rpc_password
) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    std::lock_guard<std::mutex> lock(mutex_);

    if (http_server_ && http_server_->is_running()) {
        return false;
    }

    if (!utils::is_loopback_rpc_bind_host(host) &&
        !utils::allow_public_rpc_bind_from_env()) {
        GEO_LOG_ERROR(
            "rpc",
            "Refusing non-loopback RPC bind without "
                "GEOCOIN_ALLOW_PUBLIC_RPC_BIND=1: " << host << ':' << port
        );
        return false;
    }
    if (!utils::is_loopback_rpc_bind_host(host) &&
        !utils::public_rpc_wildcard_bind_has_host_allowlist(host)) {
        GEO_LOG_ERROR(
            "rpc",
            "Refusing wildcard RPC bind without GEOCOIN_RPC_ALLOWED_HOSTS: "
                << host << ':' << port
        );
        return false;
    }
    if (!utils::is_loopback_rpc_bind_host(host)) {
        GEO_LOG_WARN(
            "audit",
            "RPC public-bind override enabled for " << host << ':' << port
                << "; require Host in GEOCOIN_RPC_ALLOWED_HOSTS or the bind address; "
                << "use an audited private firewall and TLS proxy"
        );
    }

    const bool admin_configured = !rpc_user.empty() && !rpc_password.empty();
    const bool miner_user_configured = !miner_rpc_user.empty();
    const bool miner_password_configured = !miner_rpc_password.empty();
    const bool readonly_user_configured = !readonly_rpc_user.empty();
    const bool readonly_password_configured = !readonly_rpc_password.empty();
    if (!admin_configured ||
        miner_user_configured != miner_password_configured ||
        readonly_user_configured != readonly_password_configured) {
        GEO_LOG_ERROR("rpc", "Invalid RPC credential configuration");
        return false;
    }
    if (!is_valid_rpc_credential_pair(rpc_user, rpc_password) ||
        (miner_user_configured &&
         !is_valid_rpc_credential_pair(miner_rpc_user, miner_rpc_password)) ||
        (readonly_user_configured &&
         !is_valid_rpc_credential_pair(readonly_rpc_user, readonly_rpc_password))) {
        GEO_LOG_ERROR("rpc", "RPC credentials contain invalid characters or exceed protocol limits");
        return false;
    }
    if (miner_user_configured &&
        (secure_string_equals(rpc_user, miner_rpc_user) ||
         secure_string_equals(rpc_password, miner_rpc_password))) {
        GEO_LOG_ERROR("rpc", "Admin and miner RPC usernames and passwords must each be different");
        return false;
    }
    if (readonly_user_configured &&
        (secure_string_equals(rpc_user, readonly_rpc_user) ||
         secure_string_equals(rpc_password, readonly_rpc_password))) {
        GEO_LOG_ERROR("rpc", "Admin and read-only RPC usernames and passwords must each be different");
        return false;
    }
    if (miner_user_configured && readonly_user_configured &&
        (secure_string_equals(miner_rpc_user, readonly_rpc_user) ||
         secure_string_equals(miner_rpc_password, readonly_rpc_password))) {
        GEO_LOG_ERROR("rpc", "Miner and read-only RPC usernames and passwords must each be different");
        return false;
    }

    rpc_user_ = rpc_user;
    rpc_password_ = rpc_password;
    miner_rpc_user_ = miner_rpc_user;
    miner_rpc_password_ = miner_rpc_password;
    readonly_rpc_user_ = readonly_rpc_user;
    readonly_rpc_password_ = readonly_rpc_password;
    rpc_bind_host_ = host;
    rpc_bind_port_ = port;

    http_server_ = std::make_unique<HTTPServer>(host, port);
    http_server_->set_connection_limits(rpc_max_connections_, rpc_max_connections_per_ip_);

    http_server_->set_preflight_handler([this](const HTTPRequest& request) {
        return preflight_http_request(request);
    });
    http_server_->set_handler([this](const HTTPRequest& request) {
        return handle_http_request(request);
    });

    bool started = http_server_->start();

    if (started) {
        in_warmup_ = false;
    } else {
        http_server_.reset();
        cleanse_rpc_secret(rpc_password_);
        cleanse_rpc_secret(miner_rpc_password_);
        cleanse_rpc_secret(readonly_rpc_password_);
        rpc_user_.clear();
        miner_rpc_user_.clear();
        readonly_rpc_user_.clear();
        rpc_bind_host_.clear();
        rpc_bind_port_ = 0;
    }

    return started;
}

void RPCServer::stop() {
    {
        std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
        std::unique_ptr<HTTPServer> server;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            server = std::move(http_server_);
        }

        if (server) {
            server->stop();
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            cleanse_rpc_secret(rpc_password_);
            cleanse_rpc_secret(miner_rpc_password_);
            cleanse_rpc_secret(readonly_rpc_password_);
            rpc_user_.clear();
            miner_rpc_user_.clear();
            readonly_rpc_user_.clear();
            rpc_bind_host_.clear();
            rpc_bind_port_ = 0;
            in_warmup_.store(true, std::memory_order_release);
            warmup_status_.clear();
        }
        {
            std::lock_guard<std::mutex> lock(auth_failures_mutex_);
            auth_failures_.clear();
            auth_failure_rates_.clear();
        }
        {
            std::lock_guard<std::mutex> lock(rpc_rate_limit_mutex_);
            rpc_rate_limits_.clear();
        }
    }

    // A completed std::thread remains joinable. Reap it only after releasing
    // the lifecycle mutex so a shutdown callback may itself call stop().
    reap_shutdown_thread();
}

bool RPCServer::is_running() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return http_server_ && http_server_->is_running();
}

bool RPCServer::can_update_context_unlocked() const {
    return !http_server_ || !http_server_->is_running();
}

bool RPCServer::set_context(RPCServerContext context) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    std::lock_guard<std::mutex> lock(mutex_);
    if (!can_update_context_unlocked()) {
        GEO_LOG_WARN("rpc", "Ignoring context replacement while RPC server is running");
        return false;
    }

    if (context.chain_mutations && context.node_services.mutations &&
        context.chain_mutations.get() != context.node_services.mutations.get()) {
        GEO_LOG_ERROR(
            "rpc",
            "Rejecting context with conflicting ChainMutationService instances"
        );
        return false;
    }
    if (!context.chain_mutations) {
        context.chain_mutations = context.node_services.mutations;
    }

    blockchain_ = std::move(context.blockchain);
    wallet_ = std::move(context.wallet);
    chain_state_ = std::move(context.chain_state);
    mempool_ = std::move(context.mempool);
    peer_manager_ = std::move(context.peer_manager);
    chain_mutations_ = std::move(context.chain_mutations);
    node_services_ = std::move(context.node_services);
    rpc_rate_limit_max_requests_per_window_ = context.rpc_rate_limit_max_requests_per_window;
    rpc_max_connections_ = context.rpc_max_connections;
    rpc_max_connections_per_ip_ = context.rpc_max_connections_per_ip;
    return true;
}

void RPCServer::set_shutdown_callback(std::function<void()> callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    shutdown_callback_ = std::move(callback);
}

core::Blockchain& RPCServer::require_blockchain() const {
    if (!blockchain_) {
        throw RPCException(RPC_INTERNAL_ERROR, "Blockchain не инициализирован");
    }
    return *blockchain_;
}

wallet::Wallet& RPCServer::require_wallet() const {
    if (!wallet_) {
        throw RPCException(RPC_WALLET_ERROR, "Кошелек не инициализирован");
    }
    return *wallet_;
}

state::ChainState& RPCServer::require_chain_state() const {
    if (!chain_state_) {
        throw RPCException(RPC_INTERNAL_ERROR, "ChainState не инициализирован");
    }
    return *chain_state_;
}

network::Mempool& RPCServer::require_mempool() const {
    if (!mempool_) {
        throw RPCException(RPC_INTERNAL_ERROR, "Mempool не инициализирован");
    }
    return *mempool_;
}

network::PeerManager& RPCServer::require_peer_manager() const {
    if (!peer_manager_) {
        throw RPCException(RPC_INTERNAL_ERROR, "PeerManager не инициализирован");
    }
    return *peer_manager_;
}

services::ChainMutationService& RPCServer::require_chain_mutations() const {
    if (!chain_mutations_) {
        throw RPCException(RPC_INTERNAL_ERROR, "ChainMutationService не инициализирован");
    }
    return *chain_mutations_;
}

node::NodeInfoService& RPCServer::require_node_info() const {
    if (!node_services_.info) {
        throw RPCException(RPC_INTERNAL_ERROR, "NodeInfoService не инициализирован");
    }
    return *node_services_.info;
}

node::MiningControlService& RPCServer::require_mining_control() const {
    if (!node_services_.mining) {
        throw RPCException(RPC_INTERNAL_ERROR, "MiningControlService не инициализирован");
    }
    return *node_services_.mining;
}

node::NetworkControlService& RPCServer::require_network_control() const {
    if (!node_services_.network) {
        throw RPCException(RPC_INTERNAL_ERROR, "NetworkControlService не инициализирован");
    }
    return *node_services_.network;
}

void RPCServer::request_shutdown() {
    std::function<void()> callback;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        callback = shutdown_callback_;
    }
    if (!callback) {
        return;
    }

    std::lock_guard<std::mutex> thread_lock(shutdown_thread_mutex_);
    if (shutdown_thread_.joinable()) {
        return;
    }
    shutdown_thread_ = std::thread([callback = std::move(callback)]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        callback();
    });
}

void RPCServer::reap_shutdown_thread() noexcept {
    std::thread completed;
    try {
        {
            std::lock_guard<std::mutex> thread_lock(shutdown_thread_mutex_);
            if (!shutdown_thread_.joinable()) {
                return;
            }
            if (shutdown_thread_.get_id() == std::this_thread::get_id()) {
                shutdown_thread_.detach();
                return;
            }
            completed = std::move(shutdown_thread_);
        }
        completed.join();
    } catch (...) {
        if (completed.joinable()) {
            try {
                completed.detach();
            } catch (...) {
            }
        }
    }
}

void RPCServer::register_method(const RPCCommand& command) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    std::lock_guard<std::mutex> lock(mutex_);
    if (!can_update_context_unlocked()) {
        throw std::logic_error("RPC methods cannot be registered while the server is running");
    }
    (void)command_registry_.add(command);
}

} // namespace geocoin::rpc
