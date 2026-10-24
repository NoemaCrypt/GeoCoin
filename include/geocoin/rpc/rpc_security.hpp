#pragma once

#include "geocoin/rpc/rpc_types.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace geocoin::rpc {

inline constexpr size_t MAX_RPC_METHOD_NAME_BYTES = 128;
inline constexpr size_t MAX_RPC_BASIC_AUTH_ENCODED_BYTES = 4096;
inline constexpr size_t MAX_RPC_BASIC_AUTH_DECODED_BYTES = 3072;
inline constexpr size_t MAX_RPC_BASIC_AUTH_USERNAME_BYTES = 128;

bool secure_string_equals(const std::string& a, const std::string& b);
void cleanse_rpc_secret(std::string& value) noexcept;

bool credentials_equal(
    const std::string& user_a,
    const std::string& pass_a,
    const std::string& user_b,
    const std::string& pass_b
);
bool is_valid_rpc_credential_pair(
    const std::string& user,
    const std::string& password
) noexcept;

bool has_required_rpc_role(RPCAuthRole auth_role, RPCAuthRole required_role);
const char* rpc_role_name(RPCAuthRole role);
std::vector<std::string> rpc_role_capabilities(RPCAuthRole role);
std::vector<std::string> rpc_command_capabilities(const RPCCommand& command);
bool rpc_role_has_capability(RPCAuthRole role, const std::string& capability);
bool is_rpc_command_authorized(RPCAuthRole auth_role, const RPCCommand& command);
bool is_valid_rpc_method_name(const std::string& method) noexcept;

/** Role of the JSON-RPC request currently executing on this thread. */
RPCAuthRole rpc_request_auth_role() noexcept;

class ScopedRpcRequestAuthRole {
public:
    explicit ScopedRpcRequestAuthRole(RPCAuthRole role) noexcept;
    ~ScopedRpcRequestAuthRole();

    ScopedRpcRequestAuthRole(const ScopedRpcRequestAuthRole&) = delete;
    ScopedRpcRequestAuthRole& operator=(const ScopedRpcRequestAuthRole&) = delete;

private:
    RPCAuthRole previous_;
};

// Env override for the per-client request budget. 0 keeps the limiter off.
uint32_t rpc_rate_limit_max_requests_per_window(uint32_t default_value);
bool parse_rpc_content_length(const std::string& value, size_t& out) noexcept;
std::string rpc_client_network_key(const std::string& remote_addr);
std::string sanitize_rpc_log_value(const std::string& value, size_t max_len = 64);

}  // namespace geocoin::rpc
