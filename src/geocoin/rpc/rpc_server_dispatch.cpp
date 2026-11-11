#include "geocoin/rpc/rpc_server.hpp"

#include "geocoin/rpc/rpc_dispatch.hpp"
#include "geocoin/rpc/rpc_security.hpp"
#include "geocoin/utils/logger.hpp"
#include "geocoin/utils/rpc_bind_policy.hpp"
#include "rpc_sensitive_json.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <limits>
#include <utility>

namespace geocoin::rpc {
namespace {
constexpr size_t kRpcRateLimitBodyQuantum = 256 * 1024;

bool sync_debug_enabled() {
    static const bool enabled = (std::getenv("GEOCOIN_SYNC_DEBUG") != nullptr);
    return enabled;
}

std::string trim_ascii_whitespace(std::string value) {
    const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char c) {
        return std::isspace(c) != 0;
    });
    const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char c) {
        return std::isspace(c) != 0;
    }).base();
    if (first >= last) {
        return {};
    }
    return std::string(first, last);
}

bool has_json_content_type(const HTTPRequest& request) {
    const auto it = request.headers.find("content-type");
    if (it == request.headers.end()) {
        return false;
    }

    const size_t parameter_separator = it->second.find(';');
    std::string media_type = trim_ascii_whitespace(
        it->second.substr(0, parameter_separator)
    );
    std::transform(media_type.begin(), media_type.end(), media_type.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return media_type == "application/json";
}

std::optional<HTTPResponse> reject_invalid_rpc_host(
    const HTTPRequest& request,
    const std::string& bind_host,
    uint16_t bind_port
) {
    if (bind_host.empty()) {
        return std::nullopt;
    }
    const auto host = request.headers.find("host");
    if (host == request.headers.end() ||
        !utils::is_allowed_rpc_host_header(host->second, bind_host, bind_port)) {
        GEO_LOG_WARN("audit", "rpc_host_rejected client=" <<
            sanitize_rpc_log_value(rpc_client_network_key(request.remote_addr)));
        HTTPResponse response;
        response.status_code = 421;
        response.status_text = "Misdirected Request";
        response.headers["Content-Type"] = "text/plain";
        response.headers["X-Content-Type-Options"] = "nosniff";
        response.body = "Invalid RPC Host header";
        return response;
    }
    return std::nullopt;
}

uint32_t rpc_body_rate_limit_cost(size_t body_bytes) {
    return static_cast<uint32_t>(body_bytes / kRpcRateLimitBodyQuantum);
}

HTTPResponse make_plain_http_error(
    int status_code,
    std::string status_text,
    std::string body
) {
    HTTPResponse response;
    response.status_code = status_code;
    response.status_text = std::move(status_text);
    response.headers["Content-Type"] = "text/plain";
    response.headers["X-Content-Type-Options"] = "nosniff";
    response.body = std::move(body);
    return response;
}

}  // namespace

std::optional<HTTPResponse> RPCServer::preflight_http_request(
    const HTTPRequest& request
) {
    if (request.method != "POST") {
        HTTPResponse response;
        response.status_code = 405;
        response.status_text = "Method Not Allowed";
        response.headers["Allow"] = "POST";
        return response;
    }

    if (request.uri != "/") {
        return make_plain_http_error(404, "Not Found", "RPC endpoint not found");
    }

    if (const auto host_rejection =
            reject_invalid_rpc_host(request, rpc_bind_host_, rpc_bind_port_)) {
        return host_rejection;
    }

    if (!has_json_content_type(request)) {
        return make_plain_http_error(
            415,
            "Unsupported Media Type",
            "JSON Content-Type required"
        );
    }

    const std::string client_key = auth_client_key(request);
    uint32_t retry_after_sec = 0;
    if (is_auth_failure_rate_limited(client_key, retry_after_sec)) {
        HTTPResponse response = create_unauthorized_response();
        response.headers["Retry-After"] = std::to_string(retry_after_sec);
        return response;
    }

    const std::string auth_key = client_key + "\x1f" + request.auth_user;
    if (is_auth_temporarily_blocked(auth_key)) {
        return create_unauthorized_response();
    }

    if (authenticate(request) == RPCAuthRole::NONE) {
        record_auth_failure(auth_key);
        record_auth_failure_rate(client_key);
        GEO_LOG_WARN(
            "audit",
            "rpc_auth_failed_pre_body user=" << sanitize_for_log(request.auth_user)
                << " client=" << sanitize_for_log(client_key)
        );
        return create_unauthorized_response();
    }

    size_t content_length = 0;
    const auto content_length_header = request.headers.find("content-length");
    if (content_length_header != request.headers.end() &&
        !parse_rpc_content_length(content_length_header->second, content_length)) {
        return make_plain_http_error(400, "Bad Request", "Invalid Content-Length");
    }

    const uint32_t preflight_cost = 1u + rpc_body_rate_limit_cost(content_length);
    if (!consume_rate_limit(client_key, preflight_cost, retry_after_sec)) {
        GEO_LOG_WARN(
            "audit",
            "rpc_rate_limited_pre_body client=" << sanitize_for_log(client_key)
                << " content_length=" << content_length
                << " retry_after=" << retry_after_sec
        );
        return create_rate_limited_response(retry_after_sec);
    }
    request.rate_limit_charged = true;

    return std::nullopt;
}

HTTPResponse RPCServer::handle_http_request(const HTTPRequest& request) {
    if (sync_debug_enabled()) {
        GEO_LOG_DEBUG("rpc", "Received " << request.method << " request to " << request.uri);
    }

    if (request.method != "POST") {
        HTTPResponse response;
        response.status_code = 405;
        response.status_text = "Method Not Allowed";
        response.headers["Allow"] = "POST";
        return response;
    }

    if (request.uri != "/") {
        return make_plain_http_error(404, "Not Found", "RPC endpoint not found");
    }

    if (const auto host_rejection =
            reject_invalid_rpc_host(request, rpc_bind_host_, rpc_bind_port_)) {
        return *host_rejection;
    }

    // Requiring a non-simple browser content type forces cross-origin callers
    // through CORS preflight (which this private RPC service never permits) and
    // prevents HTML form POSTs from invoking authenticated methods.
    if (!has_json_content_type(request)) {
        return make_plain_http_error(
            415,
            "Unsupported Media Type",
            "JSON Content-Type required"
        );
    }

    const std::string client_key = auth_client_key(request);
    uint32_t auth_retry_after_sec = 0;
    if (is_auth_failure_rate_limited(client_key, auth_retry_after_sec)) {
        GEO_LOG_WARN(
            "audit",
            "rpc_auth_rate_limited client=" << sanitize_for_log(client_key)
                << " retry_after=" << auth_retry_after_sec
        );
        HTTPResponse response = create_unauthorized_response();
        response.headers["Retry-After"] = std::to_string(auth_retry_after_sec);
        return response;
    }
    const std::string auth_key = client_key + "\x1f" + request.auth_user;
    if (is_auth_temporarily_blocked(auth_key)) {
        GEO_LOG_WARN(
            "audit",
            "rpc_auth_throttled user=" << sanitize_for_log(request.auth_user)
                << " client=" << sanitize_for_log(client_key)
        );
        return create_unauthorized_response();
    }

    const RPCAuthRole auth_role = authenticate(request);
    if (auth_role == RPCAuthRole::NONE) {
        record_auth_failure(auth_key);
        record_auth_failure_rate(client_key);
        GEO_LOG_WARN("rpc", "Auth failed (user=" << sanitize_for_log(request.auth_user) << ")");
        GEO_LOG_WARN(
            "audit",
            "rpc_auth_failed user=" << sanitize_for_log(request.auth_user)
                << " client=" << sanitize_for_log(client_key)
                << " uri=" << sanitize_for_log(request.uri)
        );
        return create_unauthorized_response();
    }
    record_auth_success(auth_key);

    uint32_t retry_after_sec = 0;
    if (!request.rate_limit_charged) {
        if (!consume_rate_limit(client_key, 1, retry_after_sec)) {
            GEO_LOG_WARN(
                "audit",
                "rpc_rate_limited client=" << sanitize_for_log(client_key)
                    << " retry_after=" << retry_after_sec
            );
            return create_rate_limited_response(retry_after_sec);
        }

        const uint32_t body_cost = rpc_body_rate_limit_cost(request.body.size());
        if (body_cost > 0 &&
            !consume_rate_limit(client_key, body_cost, retry_after_sec)) {
            GEO_LOG_WARN(
                "audit",
                "rpc_body_rate_limited client=" << sanitize_for_log(client_key)
                    << " body_bytes=" << request.body.size()
                    << " retry_after=" << retry_after_sec
            );
            return create_rate_limited_response(retry_after_sec);
        }
    }

    if (sync_debug_enabled()) {
        GEO_LOG_DEBUG("rpc", "Auth OK, processing request");
    }

    try {
        json request_json = parse_json_rpc_payload(request.body);
        detail::ScopedJsonStringCleanser request_cleanser(request_json);
        if (request_json.is_array() && request_json.size() > 1) {
            const uint32_t additional_cost = static_cast<uint32_t>(std::min<size_t>(
                request_json.size() - 1,
                static_cast<size_t>(std::numeric_limits<uint32_t>::max())
            ));
            if (!consume_rate_limit(client_key, additional_cost, retry_after_sec)) {
                GEO_LOG_WARN(
                    "audit",
                    "rpc_batch_rate_limited client=" << sanitize_for_log(client_key)
                        << " batch_items=" << request_json.size()
                        << " retry_after=" << retry_after_sec
                );
                return create_rate_limited_response(retry_after_sec);
            }
        }
        if (sync_debug_enabled()) {
            GEO_LOG_DEBUG("rpc", "Parsed JSON, calling method");
        }
        json response_json = dispatch_json_rpc_payload(
            request_json,
            auth_role,
            [this](const json& item, RPCAuthRole role) {
                if (in_warmup_) {
                    return dispatch_json_rpc_request(
                        command_registry_, item, role, static_cast<bool>(wallet_), &warmup_status_);
                }
                return handle_rpc_call(item, role);
            }
        );
        detail::ScopedJsonStringCleanser response_cleanser(response_json);
        if (response_json.is_null()) {
            HTTPResponse response;
            response.status_code = 204;
            response.status_text = "No Content";
            return response;
        }
        if (sync_debug_enabled()) {
            GEO_LOG_DEBUG("rpc", "Method executed, sending response");
        }
        return create_json_response(response_json);
    } catch (const RPCException& error) {
        GEO_LOG_WARN("rpc", "Rejected RPC JSON payload with code " << error.code());
        return create_json_response(make_json_rpc_error(
            error.code(),
            error.what(),
            nullptr
        ));
    } catch (const std::exception& error) {
        GEO_LOG_ERROR(
            "rpc",
            "Exception: " << sanitize_rpc_log_value(error.what(), 256)
        );
        return create_error_response("Internal RPC error");
    }
}

json RPCServer::handle_rpc_call(const json& request_json, RPCAuthRole auth_role) {
    return dispatch_json_rpc_request(
        command_registry_,
        request_json,
        auth_role,
        static_cast<bool>(wallet_)
    );
}

}  // namespace geocoin::rpc
