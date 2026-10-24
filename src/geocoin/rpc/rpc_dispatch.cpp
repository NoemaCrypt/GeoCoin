#include "geocoin/rpc/rpc_dispatch.hpp"

#include "geocoin/rpc/rpc_security.hpp"
#include "geocoin/utils/logger.hpp"
#include "geocoin/storage/read_status.hpp"
#include "rpc_sensitive_json.hpp"

#include <algorithm>
#include <array>
#include <set>

namespace geocoin::rpc {

namespace {
constexpr size_t kMaxRpcBatchItems = 10;
constexpr size_t kMaxRpcIdStringBytes = 256;
constexpr size_t kMaxRpcJsonDepth = 64;
constexpr size_t kMaxRpcJsonNodes = 50'000;
constexpr size_t kMaxRpcJsonKeyBytes = 256;

void preflight_rpc_json_shape(const std::string& payload) {
    size_t depth = 0;
    size_t structural_items = 0;
    bool in_string = false;
    bool escaped = false;

    for (const unsigned char c : payload) {
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }

        if (c == '"') {
            in_string = true;
            continue;
        }
        if (c == '{' || c == '[') {
            ++structural_items;
            if (++depth > kMaxRpcJsonDepth) {
                throw RPCException(
                    RPC_INVALID_REQUEST,
                    "Invalid Request: JSON nesting is too deep"
                );
            }
        } else if (c == '}' || c == ']') {
            if (depth > 0) {
                --depth;
            }
        } else if (c == ',') {
            ++structural_items;
        }

        if (structural_items > kMaxRpcJsonNodes) {
            throw RPCException(
                RPC_INVALID_REQUEST,
                "Invalid Request: JSON payload is too complex"
            );
        }
    }
}
}

json make_json_rpc_error(RPCErrorCode code, const std::string& message, const json& id) {
    return {
        {"jsonrpc", "2.0"},
        {"error", {
            {"code", code},
            {"message", message}
        }},
        {"id", id}
    };
}

json make_json_rpc_invalid_request(const std::string& message, const json& id) {
    return make_json_rpc_error(RPC_INVALID_REQUEST, message, id);
}

json make_json_rpc_success(const json& result, const json& id) {
    return {
        {"result", result},
        {"jsonrpc", "2.0"},
        {"id", id}
    };
}

json parse_json_rpc_payload(const std::string& payload) {
    // Reject allocation-amplifying shapes before constructing a DOM. The
    // scanner is deliberately only a bound check; the JSON parser remains the
    // authority for syntax and string escaping.
    preflight_rpc_json_shape(payload);

    bool duplicate_key = false;
    bool excessive_depth = false;
    bool excessive_complexity = false;
    bool excessive_key_length = false;
    size_t node_count = 0;
    std::array<std::set<std::string, std::less<>>, kMaxRpcJsonDepth + 1> object_keys;

    const auto callback = [&](int depth, json::parse_event_t event, json& parsed) {
        const bool counts_as_node =
            event == json::parse_event_t::object_start ||
            event == json::parse_event_t::array_start ||
            event == json::parse_event_t::key ||
            event == json::parse_event_t::value;
        if (counts_as_node && ++node_count > kMaxRpcJsonNodes) {
            excessive_complexity = true;
        }

        if (event == json::parse_event_t::object_start ||
            event == json::parse_event_t::array_start) {
            if (depth < 0 || static_cast<size_t>(depth) >= kMaxRpcJsonDepth) {
                excessive_depth = true;
            } else if (event == json::parse_event_t::object_start) {
                object_keys[static_cast<size_t>(depth) + 1].clear();
            }
        } else if (event == json::parse_event_t::key &&
                   depth > 0 && static_cast<size_t>(depth) <= kMaxRpcJsonDepth) {
            const auto& key = parsed.get_ref<const std::string&>();
            if (key.size() > kMaxRpcJsonKeyBytes) {
                excessive_key_length = true;
            } else if (!object_keys[static_cast<size_t>(depth)].insert(key).second) {
                duplicate_key = true;
            }
        } else if (event == json::parse_event_t::object_end &&
                   depth >= 0 && static_cast<size_t>(depth) < kMaxRpcJsonDepth) {
            object_keys[static_cast<size_t>(depth) + 1].clear();
        }

        return !duplicate_key && !excessive_depth &&
            !excessive_complexity && !excessive_key_length;
    };

    json parsed;
    try {
        parsed = json::parse(payload, callback);
    } catch (const json::exception&) {
        if (duplicate_key) {
            throw RPCException(RPC_INVALID_REQUEST, "Invalid Request: duplicate JSON object key");
        }
        if (excessive_depth) {
            throw RPCException(RPC_INVALID_REQUEST, "Invalid Request: JSON nesting is too deep");
        }
        if (excessive_complexity) {
            throw RPCException(RPC_INVALID_REQUEST, "Invalid Request: JSON payload is too complex");
        }
        if (excessive_key_length) {
            throw RPCException(RPC_INVALID_REQUEST, "Invalid Request: JSON object key is too long");
        }
        throw RPCException(RPC_PARSE_ERROR, "Parse error");
    }

    if (duplicate_key || excessive_depth || excessive_complexity || excessive_key_length) {
        detail::cleanse_json_strings(parsed);
        if (duplicate_key) {
            throw RPCException(RPC_INVALID_REQUEST, "Invalid Request: duplicate JSON object key");
        }
        if (excessive_depth) {
            throw RPCException(RPC_INVALID_REQUEST, "Invalid Request: JSON nesting is too deep");
        }
        if (excessive_complexity) {
            throw RPCException(RPC_INVALID_REQUEST, "Invalid Request: JSON payload is too complex");
        }
        throw RPCException(RPC_INVALID_REQUEST, "Invalid Request: JSON object key is too long");
    }

    return parsed;
}

json normalize_rpc_params(const RPCCommand& command, const json& params) {
    if (params.is_null()) {
        return json::array();
    }
    if (params.is_array()) {
        if (!command.variadic_params && !command.param_names.empty() && params.size() > command.param_names.size()) {
            throw RPCException(
                RPC_INVALID_PARAMS,
                "Too many positional params for method: " + command.name
            );
        }
        if (command.param_names.empty() && !command.variadic_params && !params.empty()) {
            throw RPCException(
                RPC_INVALID_PARAMS,
                "Method takes no positional params: " + command.name
            );
        }
        return params;
    }
    if (!params.is_object()) {
        throw RPCException(RPC_INVALID_PARAMS, "params must be an array, object, or null");
    }
    if (command.param_names.empty()) {
        if (params.empty()) {
            return json::array();
        }
        throw RPCException(
            RPC_INVALID_PARAMS,
            "Named params are not supported for method: " + command.name
        );
    }

    if (params.size() > command.param_names.size()) {
        throw RPCException(
            RPC_INVALID_PARAMS,
            "Too many named params for method: " + command.name
        );
    }

    for (auto it = params.begin(); it != params.end(); ++it) {
        if (std::find(command.param_names.begin(), command.param_names.end(), it.key()) ==
            command.param_names.end()) {
            throw RPCException(
                RPC_INVALID_PARAMS,
                "Unknown named param for method: " + command.name
            );
        }
    }

    json out = json::array();
    size_t missing_previous = 0;
    for (const auto& name : command.param_names) {
        auto it = params.find(name);
        if (it != params.end()) {
            while (missing_previous > 0) {
                out.push_back(nullptr);
                --missing_previous;
            }
            out.push_back(*it);
        } else {
            ++missing_previous;
        }
    }
    return out;
}

json dispatch_json_rpc_payload(
    const json& payload,
    RPCAuthRole auth_role,
    const RPCRequestHandler& request_handler
) {
    if (payload.is_array()) {
        if (payload.empty()) {
            return make_json_rpc_invalid_request("Invalid Request: empty batch");
        }
        if (payload.size() > kMaxRpcBatchItems) {
            return make_json_rpc_invalid_request("Invalid Request: batch too large");
        }

        json response = json::array();
        for (const auto& item : payload) {
            json item_response = request_handler(item, auth_role);
            if (!item_response.is_null()) {
                response.push_back(std::move(item_response));
            }
        }
        return response.empty() ? json(nullptr) : response;
    }

    return request_handler(payload, auth_role);
}

json dispatch_json_rpc_request(
    const RPCCommandRegistry& registry,
    const json& request,
    RPCAuthRole auth_role,
    bool wallet_loaded,
    const std::string* warmup_status
) {
    if (!request.is_object()) {
        return make_json_rpc_invalid_request("Invalid Request: request must be an object");
    }

    json id = nullptr;
    if (request.contains("id")) {
        const auto& candidate = request["id"];
        const bool valid_type = candidate.is_null() || candidate.is_string() ||
            candidate.is_number_integer() || candidate.is_number_unsigned() ||
            candidate.is_number_float();
        if (!valid_type ||
            (candidate.is_string() &&
             candidate.get_ref<const std::string&>().size() > kMaxRpcIdStringBytes)) {
            return make_json_rpc_invalid_request("Invalid Request: id has an invalid type or length");
        }
        id = candidate;
    }
    if (request.contains("jsonrpc") &&
        (!request["jsonrpc"].is_string() || request["jsonrpc"] != "2.0")) {
        return make_json_rpc_invalid_request("Invalid Request: jsonrpc must be \"2.0\"", id);
    }
    if (!request.contains("method") || !request["method"].is_string()) {
        return make_json_rpc_invalid_request("Invalid Request: method must be a string", id);
    }
    if (request.contains("params") &&
        !request["params"].is_null() &&
        !request["params"].is_array() &&
        !request["params"].is_object()) {
        return make_json_rpc_error(
            RPC_INVALID_PARAMS,
            "params must be an array, object, or null",
            id
        );
    }

    const std::string method = request["method"].get<std::string>();
    if (!is_valid_rpc_method_name(method)) {
        return make_json_rpc_invalid_request("Invalid Request: method name is invalid", id);
    }

    const auto execute = [&]() -> json {
        if (warmup_status) {
            return make_json_rpc_error(
                RPC_IN_WARMUP, "RPC server is in warmup: " + *warmup_status, id);
        }
        GEO_LOG_INFO(
            "audit",
            "rpc_method method=" << sanitize_rpc_log_value(method)
                << " wallet_loaded=" << (wallet_loaded ? "yes" : "no")
                << " profile=" << rpc_command_profile_name(registry.profile())
        );

        const RPCCommand* command = registry.find(method);
        if (!command) {
            GEO_LOG_WARN("rpc", "Method not found: " << sanitize_rpc_log_value(method));
            return make_json_rpc_error(RPC_METHOD_NOT_FOUND, "Method not found: " + method, id);
        }

        if (!is_rpc_command_authorized(auth_role, *command)) {
            GEO_LOG_WARN(
                "audit",
                "rpc_authz_denied method=" << sanitize_rpc_log_value(method)
                    << " required_role=" << rpc_role_name(command->required_role)
                    << " auth_role=" << rpc_role_name(auth_role)
            );
            return make_json_rpc_error(
                RPC_FORBIDDEN_BY_SAFE_MODE,
                "RPC method is not permitted for this credential role",
                id
            );
        }

        if (command->requires_wallet && !wallet_loaded) {
            GEO_LOG_WARN("rpc", "Wallet required but not loaded");
            return make_json_rpc_error(
                RPC_WALLET_ERROR,
                "Method requires wallet but wallet is not loaded",
                id
            );
        }

        try {
            json params = normalize_rpc_params(
                *command,
                request.contains("params") ? request["params"] : json::array()
            );
            detail::ScopedJsonStringCleanser params_cleanser(params);
            const ScopedRpcRequestAuthRole request_role(auth_role);
            return make_json_rpc_success(command->method(params), id);
        } catch (const RPCException& error) {
            GEO_LOG_WARN(
                "rpc",
                "RPCException code=" << error.code()
                    << " message=" << sanitize_rpc_log_value(error.what(), 256)
            );
            return make_json_rpc_error(error.code(), error.what(), id);
        } catch (const json::type_error& error) {
            GEO_LOG_WARN(
                "rpc",
                "Invalid RPC parameter type: " << sanitize_rpc_log_value(error.what(), 256)
            );
            return make_json_rpc_error(
                RPC_INVALID_PARAMS,
                "Invalid parameter type",
                id
            );
        } catch (const json::out_of_range& error) {
            GEO_LOG_WARN(
                "rpc",
                "Missing RPC parameter field: " << sanitize_rpc_log_value(error.what(), 256)
            );
            return make_json_rpc_error(
                RPC_INVALID_PARAMS,
                "Missing or invalid parameter field",
                id
            );
        } catch (const std::invalid_argument& error) {
            GEO_LOG_WARN(
                "rpc",
                "Invalid RPC parameter: " << sanitize_rpc_log_value(error.what(), 256)
            );
            return make_json_rpc_error(RPC_INVALID_PARAMETER, "Invalid parameter", id);
        } catch (const std::out_of_range& error) {
            GEO_LOG_WARN(
                "rpc",
                "RPC parameter out of range: " << sanitize_rpc_log_value(error.what(), 256)
            );
            return make_json_rpc_error(RPC_INVALID_PARAMETER, "Parameter out of range", id);
        } catch (const storage::ReadError& error) {
            GEO_LOG_ERROR("rpc", "Database read failed: " << sanitize_rpc_log_value(error.what(), 256));
            return make_json_rpc_error(RPC_DATABASE_ERROR, "Database read failed", id);
        } catch (const std::exception& error) {
            GEO_LOG_ERROR(
                "rpc",
                "Exception: " << sanitize_rpc_log_value(error.what(), 256)
            );
            return make_json_rpc_error(RPC_INTERNAL_ERROR, "Internal RPC error", id);
        }
    };
    json response = execute();
    if (!request.contains("id")) {
        detail::cleanse_json_strings(response);
        return nullptr;
    }
    return response;
}

}  // namespace geocoin::rpc
