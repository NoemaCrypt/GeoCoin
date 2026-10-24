#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace geocoin::utils {

inline std::string ascii_lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

inline std::string trim_ascii_copy(std::string value) {
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

inline bool is_loopback_rpc_bind_host(std::string_view host) {
    return host == "127.0.0.1" ||
           host == "localhost" ||
           host == "::1" ||
           host == "[::1]";
}

inline bool is_wildcard_rpc_bind_host(std::string_view host) {
    return host.empty() ||
           host == "0.0.0.0" ||
           host == "*" ||
           host == "::" ||
           host == "[::]";
}

inline bool allow_public_rpc_bind_from_env() {
    const char* raw = std::getenv("GEOCOIN_ALLOW_PUBLIC_RPC_BIND");
    if (raw == nullptr) {
        return false;
    }

    const std::string value = ascii_lower_copy(raw);
    return value == "1" || value == "true" || value == "yes" || value == "on";
}

inline std::vector<std::string> rpc_allowed_hosts_from_env() {
    const char* raw = std::getenv("GEOCOIN_RPC_ALLOWED_HOSTS");
    if (raw == nullptr || *raw == '\0') {
        return {};
    }

    std::vector<std::string> hosts;
    std::string current;
    const auto flush = [&]() {
        const std::string host = ascii_lower_copy(trim_ascii_copy(current));
        current.clear();
        if (!host.empty()) {
            hosts.push_back(host);
        }
    };
    for (char c : std::string(raw)) {
        if (c == ',') {
            flush();
        } else {
            current.push_back(c);
        }
    }
    flush();
    return hosts;
}

inline bool host_header_matches_candidate(
    std::string_view raw_host,
    std::string_view candidate,
    uint16_t port
) {
    const std::string host = ascii_lower_copy(std::string(raw_host));
    const std::string cand = ascii_lower_copy(std::string(candidate));
    if (host.empty() || cand.empty()) {
        return false;
    }

    const std::string port_suffix = ":" + std::to_string(port);
    const auto matches = [&](const std::string& value) {
        return host == value || host == value + port_suffix;
    };
    if (matches(cand)) {
        return true;
    }
    if (cand.front() != '[' && cand.find(':') != std::string::npos) {
        const std::string bracketed = "[" + cand + "]";
        if (matches(bracketed)) {
            return true;
        }
    }
    return false;
}

inline bool is_allowed_rpc_host_header(
    std::string_view raw_host,
    std::string_view bind_host,
    uint16_t port
) {
    if (raw_host.empty()) {
        return false;
    }
    if (is_loopback_rpc_bind_host(bind_host)) {
        return host_header_matches_candidate(raw_host, "127.0.0.1", port) ||
               host_header_matches_candidate(raw_host, "localhost", port) ||
               host_header_matches_candidate(raw_host, "[::1]", port);
    }

    if (!is_wildcard_rpc_bind_host(bind_host) &&
        host_header_matches_candidate(raw_host, bind_host, port)) {
        return true;
    }

    for (const auto& allowed : rpc_allowed_hosts_from_env()) {
        if (host_header_matches_candidate(raw_host, allowed, port)) {
            return true;
        }
    }
    return false;
}

inline bool public_rpc_wildcard_bind_has_host_allowlist(std::string_view bind_host) {
    return !is_wildcard_rpc_bind_host(bind_host) || !rpc_allowed_hosts_from_env().empty();
}

} // namespace geocoin::utils
