#pragma once

#include "geocoin/types.hpp"
#include "nlohmann_json.hpp"
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
#include <functional>

namespace geocoin::rpc {

using json = nlohmann::json;

/**
 * @brief RPC credential role.
 */
enum class RPCAuthRole {
    NONE = 0,
    READONLY,
    ADMIN,
    MINER
};

/**
 * @brief RPC error codes (Bitcoin compatible)
 */
enum RPCErrorCode {
    // Standard JSON-RPC 2.0 errors
    RPC_INVALID_REQUEST = -32600,
    RPC_METHOD_NOT_FOUND = -32601,
    RPC_INVALID_PARAMS = -32602,
    RPC_INTERNAL_ERROR = -32603,
    RPC_PARSE_ERROR = -32700,

    // General application defined errors
    RPC_MISC_ERROR = -1,
    RPC_FORBIDDEN_BY_SAFE_MODE = -2,
    RPC_TYPE_ERROR = -3,
    RPC_INVALID_ADDRESS_OR_KEY = -5,
    RPC_OUT_OF_MEMORY = -7,
    RPC_INVALID_PARAMETER = -8,
    RPC_DATABASE_ERROR = -20,
    RPC_DESERIALIZATION_ERROR = -22,
    RPC_VERIFY_ERROR = -25,
    RPC_VERIFY_REJECTED = -26,
    RPC_VERIFY_ALREADY_IN_CHAIN = -27,
    RPC_IN_WARMUP = -28,

    // Client errors
    RPC_CLIENT_NOT_CONNECTED = -9,
    RPC_CLIENT_IN_INITIAL_DOWNLOAD = -10,
    RPC_CLIENT_NODE_ALREADY_ADDED = -23,
    RPC_CLIENT_NODE_NOT_ADDED = -24,
    RPC_CLIENT_NODE_NOT_CONNECTED = -29,

    // Wallet errors
    RPC_WALLET_ERROR = -4,
    RPC_WALLET_INSUFFICIENT_FUNDS = -6,
    RPC_WALLET_INVALID_ACCOUNT_NAME = -11,
    RPC_WALLET_KEYPOOL_RAN_OUT = -12,
    RPC_WALLET_UNLOCK_NEEDED = -13,
    RPC_WALLET_PASSPHRASE_INCORRECT = -14,
    RPC_WALLET_WRONG_ENC_STATE = -15,
    RPC_WALLET_ENCRYPTION_FAILED = -16,
    RPC_WALLET_ALREADY_UNLOCKED = -17,
};

/**
 * @brief RPC exception
 */
class RPCException : public std::runtime_error {
public:
    RPCException(RPCErrorCode code, const std::string& message)
        : std::runtime_error(message), code_(code) {}

    RPCErrorCode code() const { return code_; }

private:
    RPCErrorCode code_;
};

/**
 * @brief RPC method handler
 *
 * @param params JSON array of parameters
 * @return JSON result
 * @throws RPCException on error
 */
using RPCMethod = std::function<json(const json& params)>;

/**
 * @brief RPC command info
 */
struct RPCCommand {
    std::string name;
    RPCMethod method;
    std::string category;
    std::string help;
    bool requires_wallet = false;
    bool safe_mode = true;
    RPCAuthRole required_role = RPCAuthRole::ADMIN;
    std::vector<std::string> param_names;
    bool variadic_params = false;
    bool mutates = false;
    std::string stability = "stable";
    std::string since = "0.1.0";
    std::vector<std::string> capabilities;
    std::string deprecation;
    std::string side_effects;
};

/**
 * @brief HTTP request
 */
struct HTTPRequest {
    std::string method;  // GET, POST, etc.
    std::string uri;
    std::string version; // HTTP/1.1
    std::map<std::string, std::string> headers;
    std::string body;
    std::string remote_addr; // client ip:port (best effort)

    // Auth
    std::string auth_user;
    std::string auth_password;

    // Set by header preflight after it charges Content-Length against the
    // per-client budget so handle_http_request does not double-count.
    mutable bool rate_limit_charged = false;
};

/**
 * @brief HTTP response
 */
struct HTTPResponse {
    int status_code = 200;
    std::string status_text = "OK";
    std::map<std::string, std::string> headers;
    std::string body;

    std::string to_string() const;
};

} // namespace geocoin::rpc
