#include "geocoin/rpc/rpc_security.hpp"
#include "geocoin/utils/secure_memory.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <limits>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#endif

namespace geocoin::rpc {

bool secure_string_equals(const std::string& a, const std::string& b) {
    const size_t max_len = std::max(a.size(), b.size());
    size_t diff = a.size() ^ b.size();
    for (size_t i = 0; i < max_len; ++i) {
        const unsigned char ac = (i < a.size()) ? static_cast<unsigned char>(a[i]) : 0;
        const unsigned char bc = (i < b.size()) ? static_cast<unsigned char>(b[i]) : 0;
        diff |= static_cast<size_t>(ac ^ bc);
    }
    return diff == 0;
}

void cleanse_rpc_secret(std::string& value) noexcept {
    utils::cleanse_container(value);
}

bool credentials_equal(
    const std::string& user_a,
    const std::string& pass_a,
    const std::string& user_b,
    const std::string& pass_b
) {
    const bool users_equal = secure_string_equals(user_a, user_b);
    const bool passwords_equal = secure_string_equals(pass_a, pass_b);
    return users_equal & passwords_equal;
}

bool is_valid_rpc_credential_pair(
    const std::string& user,
    const std::string& password
) noexcept {
    if (user.empty() || password.empty() ||
        user.find(':') != std::string::npos ||
        user.size() > MAX_RPC_BASIC_AUTH_USERNAME_BYTES ||
        user.size() + 1 > MAX_RPC_BASIC_AUTH_DECODED_BYTES ||
        password.size() > MAX_RPC_BASIC_AUTH_DECODED_BYTES - user.size() - 1) {
        return false;
    }

    const auto has_control = [](const std::string& value) {
        return std::any_of(value.begin(), value.end(), [](unsigned char c) {
            return c < 0x20 || c == 0x7f;
        });
    };
    return !has_control(user) && !has_control(password);
}

bool has_required_rpc_role(RPCAuthRole auth_role, RPCAuthRole required_role) {
    if (auth_role == RPCAuthRole::ADMIN) {
        return required_role == RPCAuthRole::ADMIN || required_role == RPCAuthRole::READONLY;
    }
    if (required_role == RPCAuthRole::READONLY) {
        return auth_role == RPCAuthRole::READONLY;
    }
    return auth_role == required_role;
}

const char* rpc_role_name(RPCAuthRole role) {
    switch (role) {
        case RPCAuthRole::NONE:
            return "none";
        case RPCAuthRole::READONLY:
            return "readonly";
        case RPCAuthRole::ADMIN:
            return "admin";
        case RPCAuthRole::MINER:
            return "miner";
    }
    return "unknown";
}

std::vector<std::string> rpc_role_capabilities(RPCAuthRole role) {
    switch (role) {
        case RPCAuthRole::ADMIN:
            return {
                "readonly",
                "wallet",
                "tx_submit",
                "tx_builder",
                "network_admin",
                "chain_admin",
                "diagnostics",
                "admin"
            };
        case RPCAuthRole::READONLY:
            return {"readonly"};
        case RPCAuthRole::MINER:
            return {"miner"};
        case RPCAuthRole::NONE:
            return {};
    }
    return {};
}

std::vector<std::string> rpc_command_capabilities(const RPCCommand& command) {
    if (!command.capabilities.empty()) {
        return command.capabilities;
    }

    if (command.required_role == RPCAuthRole::MINER) {
        return {"miner"};
    }
    if (command.required_role == RPCAuthRole::READONLY) {
        return {"readonly"};
    }

    if (command.category == "network" && command.mutates) {
        return {"network_admin"};
    }
    if (command.category == "blockchain" && command.mutates) {
        return {"chain_admin"};
    }
    if (command.category == "mempool" && command.mutates) {
        return {"tx_submit"};
    }
    if (command.category == "wallet") {
        return {"wallet"};
    }
    if (command.category == "txbuilder") {
        return {"tx_builder"};
    }
    if (command.category == "nft" ||
        command.category == "sidechain" ||
        command.category == "multisig") {
        return command.mutates ? std::vector<std::string>{"tx_submit"}
                               : std::vector<std::string>{"tx_builder"};
    }
    if (command.category == "control" && command.mutates) {
        return {"admin"};
    }
    if (command.category == "util" ||
        command.category == "geocoin" ||
        command.category == "control") {
        if (command.required_role == RPCAuthRole::ADMIN) {
            return {"admin"};
        }
        if (command.required_role == RPCAuthRole::MINER) {
            return {"miner"};
        }
        return {"readonly"};
    }

    // An unknown or newly introduced category must fail closed. A typo in a
    // category must never turn the default ADMIN role into read-only access.
    return command.required_role == RPCAuthRole::MINER
        ? std::vector<std::string>{"miner"}
        : command.required_role == RPCAuthRole::READONLY
            ? std::vector<std::string>{"readonly"}
            : std::vector<std::string>{"admin"};
}

bool rpc_role_has_capability(RPCAuthRole role, const std::string& capability) {
    const auto capabilities = rpc_role_capabilities(role);
    return std::find(capabilities.begin(), capabilities.end(), capability) != capabilities.end();
}

bool is_rpc_command_authorized(RPCAuthRole auth_role, const RPCCommand& command) {
    if (auth_role == RPCAuthRole::NONE || command.required_role == RPCAuthRole::NONE) {
        return false;
    }

    const auto capabilities = rpc_command_capabilities(command);
    if (capabilities.empty()) {
        return has_required_rpc_role(auth_role, command.required_role);
    }

    for (const auto& capability : capabilities) {
        if (rpc_role_has_capability(auth_role, capability)) {
            return true;
        }
    }
    return false;
}

bool is_valid_rpc_method_name(const std::string& method) noexcept {
    return !method.empty() &&
        method.size() <= MAX_RPC_METHOD_NAME_BYTES &&
        std::all_of(method.begin(), method.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') ||
                (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') ||
                c == '_' || c == '-' || c == '.';
        });
}

namespace {
thread_local RPCAuthRole t_rpc_request_auth_role = RPCAuthRole::NONE;
}

RPCAuthRole rpc_request_auth_role() noexcept {
    return t_rpc_request_auth_role;
}

ScopedRpcRequestAuthRole::ScopedRpcRequestAuthRole(RPCAuthRole role) noexcept
    : previous_(t_rpc_request_auth_role) {
    t_rpc_request_auth_role = role;
}

ScopedRpcRequestAuthRole::~ScopedRpcRequestAuthRole() {
    t_rpc_request_auth_role = previous_;
}

uint32_t rpc_rate_limit_max_requests_per_window(uint32_t default_value) {
    const char* raw = std::getenv("GEOCOIN_RPC_RATE_LIMIT_MAX_REQUESTS_PER_WINDOW");
    if (raw == nullptr || *raw == '\0') {
        raw = std::getenv("GEOCOIN_RPC_RATE_LIMIT_MAX_REQUESTS");
    }
    if (raw == nullptr || *raw == '\0') {
        return default_value;
    }

    errno = 0;
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(raw, &end, 10);
    if (errno != 0 || end == raw || *end != '\0' ||
        parsed > std::numeric_limits<uint32_t>::max()) {
        return default_value;
    }
    return static_cast<uint32_t>(parsed);
}

bool parse_rpc_content_length(const std::string& value, size_t& out) noexcept {
    if (value.empty()) {
        return false;
    }

    size_t parsed = 0;
    bool saw_digit = false;
    bool trailing_space = false;
    for (unsigned char c : value) {
        if (c == ' ' || c == '\t') {
            if (saw_digit) {
                trailing_space = true;
            }
            continue;
        }
        if (trailing_space || c < '0' || c > '9') {
            return false;
        }
        saw_digit = true;
        const size_t digit = static_cast<size_t>(c - '0');
        if (parsed > (std::numeric_limits<size_t>::max() - digit) / 10) {
            return false;
        }
        parsed = parsed * 10 + digit;
    }
    if (!saw_digit) {
        return false;
    }
    out = parsed;
    return true;
}

std::string rpc_client_network_key(const std::string& remote_addr) {
    if (remote_addr.empty()) {
        return "unknown";
    }

    std::string address = remote_addr;
    const auto zone = address.find('%');
    if (zone != std::string::npos) {
        address.resize(zone);
    }

    in_addr ipv4{};
    if (::inet_pton(AF_INET, address.c_str(), &ipv4) == 1) {
        char text[INET_ADDRSTRLEN] = {};
        if (::inet_ntop(AF_INET, &ipv4, text, sizeof(text)) == nullptr) {
            return remote_addr;
        }
        return text;
    }

    in6_addr ipv6{};
    if (::inet_pton(AF_INET6, address.c_str(), &ipv6) != 1) {
        return remote_addr;
    }

    const bool is_v4_mapped =
        std::all_of(ipv6.s6_addr, ipv6.s6_addr + 10, [](unsigned char value) { return value == 0; }) &&
        ipv6.s6_addr[10] == 0xff && ipv6.s6_addr[11] == 0xff;
    if (is_v4_mapped) {
        in_addr mapped{};
        std::memcpy(&mapped, ipv6.s6_addr + 12, sizeof(mapped));
        char text[INET_ADDRSTRLEN] = {};
        if (::inet_ntop(AF_INET, &mapped, text, sizeof(text)) == nullptr) {
            return remote_addr;
        }
        return text;
    }

    // A single routed IPv6 prefix can cheaply rotate interface identifiers.
    // Grouping by /64 prevents bypassing both connection and request limits.
    std::fill(ipv6.s6_addr + 8, ipv6.s6_addr + 16, 0);
    char text[INET6_ADDRSTRLEN] = {};
    if (::inet_ntop(AF_INET6, &ipv6, text, sizeof(text)) == nullptr) {
        return remote_addr;
    }
    return std::string(text) + "/64";
}

std::string sanitize_rpc_log_value(const std::string& value, size_t max_len) {
    std::string out;
    out.reserve(std::min(value.size(), max_len));
    for (char c : value) {
        if (out.size() >= max_len) {
            break;
        }
        const unsigned char uc = static_cast<unsigned char>(c);
        if (uc >= 32 && uc < 127) {
            out.push_back(static_cast<char>(uc));
        } else {
            out.push_back('?');
        }
    }
    return out;
}

}  // namespace geocoin::rpc
