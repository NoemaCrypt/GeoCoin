#pragma once

#include "geocoin/rpc/rpc_types.hpp"
#include "geocoin/rpc/rpc_command_registry.hpp"

#include <functional>
#include <string>

namespace geocoin::rpc {

using RPCRequestHandler = std::function<json(const json& request, RPCAuthRole auth_role)>;

json make_json_rpc_error(RPCErrorCode code, const std::string& message, const json& id);
json make_json_rpc_invalid_request(const std::string& message, const json& id = nullptr);
json make_json_rpc_success(const json& result, const json& id);

/** Parse an RPC JSON body with duplicate-key, depth and complexity guards. */
json parse_json_rpc_payload(const std::string& payload);

json normalize_rpc_params(const RPCCommand& command, const json& params);
json dispatch_json_rpc_payload(
    const json& payload,
    RPCAuthRole auth_role,
    const RPCRequestHandler& request_handler
);

/** Validate, authorize and invoke one request; null means a notification with no reply. */
json dispatch_json_rpc_request(
    const RPCCommandRegistry& registry,
    const json& request,
    RPCAuthRole auth_role,
    bool wallet_loaded,
    const std::string* warmup_status = nullptr
);

}  // namespace geocoin::rpc
