#include "geocoin/rpc/rpc_server.hpp"

#include "geocoin/rpc/rpc_dispatch.hpp"
#include "geocoin/rpc/rpc_security.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>

namespace geocoin::rpc {

namespace {
constexpr uint32_t kRpcRateLimitWindowSec = 60;
constexpr size_t kRpcRateLimitMaxTrackedClients = 4096;
constexpr auto kRpcAuthFailureRetention = std::chrono::minutes(15);
constexpr auto kRpcAuthFailureRateWindow = std::chrono::minutes(1);
constexpr uint32_t kRpcAuthFailuresPerWindow = 60;
} // namespace

RPCAuthRole RPCServer::authenticate(const HTTPRequest& request) const {
    // Evaluate every configured role and both credential fields before
    // branching. This keeps username/role discovery out of the comparison
    // timing and always requires authentication.
    const bool admin_configured = !rpc_user_.empty() && !rpc_password_.empty();
    const bool miner_configured = !miner_rpc_user_.empty() && !miner_rpc_password_.empty();
    const bool readonly_configured =
        !readonly_rpc_user_.empty() && !readonly_rpc_password_.empty();
    const bool admin_match = credentials_equal(
        request.auth_user, request.auth_password, rpc_user_, rpc_password_);
    const bool miner_match = credentials_equal(
        request.auth_user, request.auth_password, miner_rpc_user_, miner_rpc_password_);
    const bool readonly_match = credentials_equal(
        request.auth_user, request.auth_password,
        readonly_rpc_user_, readonly_rpc_password_);

    if (admin_configured & admin_match) {
        return RPCAuthRole::ADMIN;
    }
    if (miner_configured & miner_match) {
        return RPCAuthRole::MINER;
    }
    if (readonly_configured & readonly_match) {
        return RPCAuthRole::READONLY;
    }

    return RPCAuthRole::NONE;
}

std::string RPCServer::sanitize_for_log(const std::string& value) const {
    return sanitize_rpc_log_value(value);
}

std::string RPCServer::auth_client_key(const HTTPRequest& request) const {
    return rpc_client_network_key(request.remote_addr);
}

bool RPCServer::is_auth_temporarily_blocked(const std::string& key) const {
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(auth_failures_mutex_);
    auto it = auth_failures_.find(key);
    if (it == auth_failures_.end()) {
        return false;
    }
    if (now < it->second.blocked_until) {
        return true;
    }
    // Keep the failure count after a short block expires. Erasing it here
    // reduced every brute-force penalty back to one second.
    if (it->second.last_failure.time_since_epoch().count() != 0 &&
        now - it->second.last_failure >= kRpcAuthFailureRetention) {
        auth_failures_.erase(it);
    }
    return false;
}

void RPCServer::record_auth_failure(const std::string& key) {
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(auth_failures_mutex_);
    auto it = auth_failures_.find(key);
    if (it == auth_failures_.end()) {
        for (auto stale = auth_failures_.begin(); stale != auth_failures_.end();) {
            if (stale->second.last_failure.time_since_epoch().count() != 0 &&
                now - stale->second.last_failure >= kRpcAuthFailureRetention) {
                stale = auth_failures_.erase(stale);
            } else {
                ++stale;
            }
        }
        if (auth_failures_.size() >= kRpcRateLimitMaxTrackedClients) {
            const auto oldest = std::min_element(
                auth_failures_.begin(),
                auth_failures_.end(),
                [](const auto& lhs, const auto& rhs) {
                    return lhs.second.blocked_until < rhs.second.blocked_until;
                }
            );
            if (oldest != auth_failures_.end()) {
                auth_failures_.erase(oldest);
            }
        }
        it = auth_failures_.emplace(key, AuthFailureState{}).first;
    } else if (it->second.last_failure.time_since_epoch().count() != 0 &&
               now - it->second.last_failure >= kRpcAuthFailureRetention) {
        it->second = AuthFailureState{};
    }
    auto& state = it->second;
    if (state.failures < 20) {
        ++state.failures;
    }
    state.last_failure = now;

    // Экспоненциальный backoff: 1,2,4,...,30 секунд
    const uint32_t shift = std::min<uint32_t>(state.failures - 1, 5);
    const uint32_t penalty_sec = std::min<uint32_t>(1u << shift, 30u);
    state.blocked_until = now + std::chrono::seconds(penalty_sec);

}

void RPCServer::record_auth_success(const std::string& key) {
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(auth_failures_mutex_);
    const auto it = auth_failures_.find(key);
    if (it != auth_failures_.end() &&
        it->second.last_failure.time_since_epoch().count() != 0 &&
        now - it->second.last_failure >= kRpcAuthFailureRetention) {
        auth_failures_.erase(it);
    }
    // A valid read-only/miner credential must not erase attempts against the
    // admin credential from the same network. Failure history decays only
    // after a quiet retention period.
}

bool RPCServer::is_auth_failure_rate_limited(
    const std::string& client_key,
    uint32_t& retry_after_sec
) const {
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(auth_failures_mutex_);
    const auto it = auth_failure_rates_.find(client_key);
    if (it == auth_failure_rates_.end() ||
        it->second.window_start.time_since_epoch().count() == 0 ||
        now - it->second.window_start >= kRpcAuthFailureRateWindow ||
        it->second.failures < kRpcAuthFailuresPerWindow) {
        retry_after_sec = 0;
        return false;
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        now - it->second.window_start).count();
    const int64_t remaining = 60 - elapsed;
    retry_after_sec = remaining > 0 ? static_cast<uint32_t>(remaining) : 1;
    return true;
}

void RPCServer::record_auth_failure_rate(const std::string& client_key) {
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(auth_failures_mutex_);

    auto it = auth_failure_rates_.find(client_key);
    if (it == auth_failure_rates_.end()) {
        for (auto stale = auth_failure_rates_.begin(); stale != auth_failure_rates_.end();) {
            if (stale->second.window_start.time_since_epoch().count() == 0 ||
                now - stale->second.window_start >= kRpcAuthFailureRateWindow) {
                stale = auth_failure_rates_.erase(stale);
            } else {
                ++stale;
            }
        }
        if (auth_failure_rates_.size() >= kRpcRateLimitMaxTrackedClients) {
            const auto oldest = std::min_element(
                auth_failure_rates_.begin(),
                auth_failure_rates_.end(),
                [](const auto& lhs, const auto& rhs) {
                    return lhs.second.window_start < rhs.second.window_start;
                });
            if (oldest != auth_failure_rates_.end()) {
                auth_failure_rates_.erase(oldest);
            }
        }
        it = auth_failure_rates_.emplace(client_key, AuthFailureRateState{}).first;
    }

    auto& state = it->second;
    if (state.window_start.time_since_epoch().count() == 0 ||
        now - state.window_start >= kRpcAuthFailureRateWindow) {
        state.window_start = now;
        state.failures = 0;
    }
    if (state.failures < kRpcAuthFailuresPerWindow) {
        ++state.failures;
    }
}

bool RPCServer::consume_rate_limit(const std::string& key, uint32_t cost, uint32_t& retry_after_sec) {
    const uint32_t max_requests_per_window =
        rpc_rate_limit_max_requests_per_window(rpc_rate_limit_max_requests_per_window_);
    if (max_requests_per_window == 0 || cost == 0) {
        retry_after_sec = 0;
        return true;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto window = std::chrono::seconds(kRpcRateLimitWindowSec);

    std::lock_guard<std::mutex> lock(rpc_rate_limit_mutex_);
    auto it = rpc_rate_limits_.find(key);
    if (it == rpc_rate_limits_.end()) {
        for (auto stale = rpc_rate_limits_.begin(); stale != rpc_rate_limits_.end();) {
            if (now - stale->second.window_start >= window) {
                stale = rpc_rate_limits_.erase(stale);
            } else {
                ++stale;
            }
        }
        if (rpc_rate_limits_.size() >= kRpcRateLimitMaxTrackedClients) {
            const auto oldest = std::min_element(
                rpc_rate_limits_.begin(),
                rpc_rate_limits_.end(),
                [](const auto& lhs, const auto& rhs) {
                    return lhs.second.window_start < rhs.second.window_start;
                }
            );
            if (oldest != rpc_rate_limits_.end()) {
                rpc_rate_limits_.erase(oldest);
            }
        }
        it = rpc_rate_limits_.emplace(key, RpcRateLimitState{}).first;
    }
    auto& state = it->second;

    if (state.window_start.time_since_epoch().count() == 0 ||
        now - state.window_start >= window) {
        state.window_start = now;
        state.requests = 0;
    }

    if (cost > max_requests_per_window || state.requests > max_requests_per_window - cost) {
        const auto elapsed_sec = std::chrono::duration_cast<std::chrono::seconds>(
            now - state.window_start
        ).count();
        const int64_t remaining = static_cast<int64_t>(kRpcRateLimitWindowSec) - elapsed_sec;
        retry_after_sec = (remaining > 0) ? static_cast<uint32_t>(remaining) : 1;
        return false;
    }

    state.requests += cost;
    retry_after_sec = 0;

    return true;
}

HTTPResponse RPCServer::create_rate_limited_response(uint32_t retry_after_sec) const {
    HTTPResponse response;
    response.status_code = 429;
    response.status_text = "Too Many Requests";
    response.headers["Content-Type"] = "application/json";
    response.headers["Retry-After"] = std::to_string(std::max<uint32_t>(retry_after_sec, 1));

    response.body = make_json_rpc_error(
        RPC_MISC_ERROR,
        "Too many RPC requests from this client",
        nullptr
    ).dump();
    return response;
}


} // namespace geocoin::rpc
