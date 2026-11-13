#pragma once

#include "geocoin/rpc/rpc_types.hpp"
#include "geocoin/rpc/rpc_protocol.hpp"
#include "geocoin/rpc/rpc_command_registry.hpp"
#include "geocoin/node/node_services.hpp"
#include "geocoin/services/chain_mutation.hpp"
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <chrono>
#include <thread>

namespace geocoin {

// Forward declarations
namespace core { class Blockchain; }
namespace wallet { class Wallet; }
namespace state { class ChainState; }
namespace network {
    class Mempool;
    class PeerManager;
}

namespace rpc {

/**
 * Owning services exposed by an RPC server. Configure the context before
 * start(); runtime replacement is rejected to keep request handlers free of
 * pointer races and every dependency remains alive until stop() completes.
 */
struct RPCServerContext {
    std::shared_ptr<core::Blockchain> blockchain;
    std::shared_ptr<wallet::Wallet> wallet;
    std::shared_ptr<state::ChainState> chain_state;
    std::shared_ptr<network::Mempool> mempool;
    std::shared_ptr<network::PeerManager> peer_manager;
    std::shared_ptr<services::ChainMutationService> chain_mutations;
    node::NodeServiceHandle node_services{};
    // 0 disables the corresponding RPC cap (request budget / TCP connections).
    uint32_t rpc_rate_limit_max_requests_per_window = 0;
    size_t rpc_max_connections = 0;
    size_t rpc_max_connections_per_ip = 0;
};

/**
 * @brief JSON-RPC server
 *
 * Implements Bitcoin-compatible JSON-RPC interface over HTTP
 */
class RPCServer {
#ifdef GEOCOIN_ENABLE_RPC_SERVER_TEST_ACCESS
    friend struct RPCServerTestAccess;
#endif
public:
    explicit RPCServer(RPCCommandProfile profile = RPCCommandProfile::FULL);
    ~RPCServer();

    /**
     * @brief Start RPC server
     *
     * @param host Host to bind (default: 127.0.0.1)
     * @param port Port to bind (default: 8332)
     * @param rpc_user Admin RPC username
     * @param rpc_password Admin RPC password
     * @param miner_rpc_user External miner RPC username
     * @param miner_rpc_password External miner RPC password
     * @param readonly_rpc_user Read-only RPC username
     * @param readonly_rpc_password Read-only RPC password
     * @return true if started successfully
     */
    bool start(
        const std::string& host = "127.0.0.1",
        uint16_t port = 8332,
        const std::string& rpc_user = "",
        const std::string& rpc_password = "",
        const std::string& miner_rpc_user = "",
        const std::string& miner_rpc_password = "",
        const std::string& readonly_rpc_user = "",
        const std::string& readonly_rpc_password = ""
    );

    /**
     * @brief Stop RPC server
     */
    void stop();

    /**
     * @brief Check if server is running
     */
    bool is_running() const;

    /** Command surface retained by this listener. */
    RPCCommandProfile command_profile() const noexcept {
        return command_registry_.profile();
    }

    /**
     * Atomically configure every dependency used by RPC handlers.
     * Partial setter APIs are intentionally absent: a listener can observe
     * either the previous complete context or the replacement, never a mix.
     */
    bool set_context(RPCServerContext context);

    /**
     * @brief Set process shutdown callback used by the stop RPC command.
     */
    void set_shutdown_callback(std::function<void()> callback);

    /**
     * @brief Register RPC method
     */
    void register_method(const RPCCommand& command);

    /**
     * @brief Get help for method
     */
    std::string get_help(const std::string& method = "") const;

private:
    bool can_update_context_unlocked() const;
    std::optional<HTTPResponse> preflight_http_request(const HTTPRequest& request);
    HTTPResponse handle_http_request(const HTTPRequest& request);
    json handle_rpc_call(const json& request_json, RPCAuthRole auth_role);

    RPCAuthRole authenticate(const HTTPRequest& request) const;
    std::string sanitize_for_log(const std::string& value) const;
    std::string auth_client_key(const HTTPRequest& request) const;
    bool is_auth_temporarily_blocked(const std::string& key) const;
    void record_auth_failure(const std::string& key);
    void record_auth_success(const std::string& key);
    bool is_auth_failure_rate_limited(
        const std::string& client_key,
        uint32_t& retry_after_sec
    ) const;
    void record_auth_failure_rate(const std::string& client_key);
    bool consume_rate_limit(const std::string& key, uint32_t cost, uint32_t& retry_after_sec);
    HTTPResponse create_rate_limited_response(uint32_t retry_after_sec) const;

    void register_blockchain_methods();
    void register_wallet_methods();
    void register_wallet_basic_methods();
    void register_wallet_basic_account_methods();
    void register_wallet_basic_utxo_methods();
    void register_wallet_basic_transaction_methods();
    void register_wallet_exchange_methods();
    void synchronize_wallet();
    void register_wallet_txbuilder_methods();
    void register_wallet_nft_methods();
    void register_wallet_nft_query_methods();
    void register_wallet_nft_payload_methods();
    void register_wallet_nft_finalize_methods();
    void register_wallet_nft_transaction_methods();
    void register_wallet_sidechain_methods();
    void register_wallet_sidechain_asset_methods();
    void register_wallet_sidechain_asset_issue_methods();
    void register_wallet_sidechain_asset_mint_methods();
    void register_wallet_sidechain_asset_redeem_methods();
    void register_wallet_sidechain_asset_slash_methods();
    void register_wallet_sidechain_deposit_methods();
    void register_wallet_sidechain_checkpoint_methods();
    void register_wallet_sidechain_checkpoint_single_methods();
    void register_wallet_sidechain_checkpoint_proof_methods();
    void register_wallet_sidechain_checkpoint_batch_methods();
    void register_wallet_sidechain_committee_methods();
    void register_wallet_sidechain_decommission_methods();
    void register_wallet_multisig_methods();
    void register_wallet_multisig_package_methods();
    void register_wallet_multisig_delegation_methods();
    void register_wallet_multisig_legacy_methods();
    void register_mempool_methods();
    void register_mining_methods();
    void register_network_methods();
    void register_misc_methods();
    void register_blockchain_core_methods();
    void register_blockchain_availability_methods();
    void register_blockchain_sidechain_methods();
    void register_blockchain_block_methods();
    void register_blockchain_pruning_methods();
    void register_blockchain_transaction_methods();
    void register_misc_control_methods();
    void register_misc_diagnostics_methods();
    void register_misc_signing_methods();
    void register_builtin_modules();
    void register_mining_info_methods();
    void register_mining_control_methods();
    void register_mining_template_methods();
    void request_shutdown();
    void reap_shutdown_thread() noexcept;

    core::Blockchain& require_blockchain() const;
    wallet::Wallet& require_wallet() const;
    state::ChainState& require_chain_state() const;
    network::Mempool& require_mempool() const;
    network::PeerManager& require_peer_manager() const;
    services::ChainMutationService& require_chain_mutations() const;
    node::NodeInfoService& require_node_info() const;
    node::MiningControlService& require_mining_control() const;
    node::NetworkControlService& require_network_control() const;

    RPCCommandRegistry command_registry_;
    std::unique_ptr<HTTPServer> http_server_;

    // Auth
    std::string rpc_user_;
    std::string rpc_password_;
    std::string miner_rpc_user_;
    std::string miner_rpc_password_;
    std::string readonly_rpc_user_;
    std::string readonly_rpc_password_;
    std::string rpc_bind_host_;
    uint16_t rpc_bind_port_{0};
    uint32_t rpc_rate_limit_max_requests_per_window_{0};
    size_t rpc_max_connections_{0};
    size_t rpc_max_connections_per_ip_{0};

    // Components
    std::shared_ptr<core::Blockchain> blockchain_;
    std::shared_ptr<wallet::Wallet> wallet_;
    std::shared_ptr<state::ChainState> chain_state_;
    std::shared_ptr<network::Mempool> mempool_;
    std::shared_ptr<network::PeerManager> peer_manager_;
    std::shared_ptr<services::ChainMutationService> chain_mutations_;
    node::NodeServiceHandle node_services_{};
    std::function<void()> shutdown_callback_;
    std::mutex shutdown_thread_mutex_;
    std::thread shutdown_thread_;

    // Thread safety
    mutable std::mutex lifecycle_mutex_;
    mutable std::mutex mutex_;
    mutable std::mutex auth_failures_mutex_;
    struct AuthFailureState {
        uint32_t failures = 0;
        std::chrono::steady_clock::time_point blocked_until{};
        std::chrono::steady_clock::time_point last_failure{};
    };
    mutable std::unordered_map<std::string, AuthFailureState> auth_failures_;
    struct AuthFailureRateState {
        uint32_t failures = 0;
        std::chrono::steady_clock::time_point window_start{};
    };
    mutable std::unordered_map<std::string, AuthFailureRateState> auth_failure_rates_;
    mutable std::mutex rpc_rate_limit_mutex_;
    struct RpcRateLimitState {
        uint32_t requests = 0;
        std::chrono::steady_clock::time_point window_start{};
    };
    mutable std::unordered_map<std::string, RpcRateLimitState> rpc_rate_limits_;

    // Warmup
    std::atomic<bool> in_warmup_{true};
    std::string warmup_status_;
};

} // namespace rpc
} // namespace geocoin
