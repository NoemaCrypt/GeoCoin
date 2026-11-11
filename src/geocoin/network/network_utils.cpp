#include "geocoin/network/network_utils.hpp"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif
#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <iostream>
#include <limits>

namespace geocoin::network {
namespace {

constexpr size_t kMaxEndpointLength = 512;
constexpr size_t kMaxHostLength = 255;

bool contains_control_or_space(const std::string& value) {
    return std::any_of(value.begin(), value.end(), [](unsigned char c) {
        return c <= 0x20 || c == 0x7f;
    });
}

bool parse_port_strict(const std::string& raw, uint16_t& port) {
    port = 0;
    if (raw.empty() || raw.size() > 5) {
        return false;
    }

    uint32_t parsed = 0;
    for (unsigned char c : raw) {
        if (!std::isdigit(c)) {
            return false;
        }
        parsed = parsed * 10U + static_cast<uint32_t>(c - '0');
        if (parsed > (std::numeric_limits<uint16_t>::max)()) {
            return false;
        }
    }

    if (parsed == 0) {
        return false;
    }
    port = static_cast<uint16_t>(parsed);
    return true;
}

bool normalize_host_part(const std::string& raw_host, std::string& host) {
    host.clear();
    if (raw_host.empty() || raw_host.size() > kMaxHostLength || contains_control_or_space(raw_host)) {
        return false;
    }

    host = NetworkUtils::normalize_ip_address(raw_host);
    return !host.empty() && host.size() <= kMaxHostLength && !contains_control_or_space(host);
}

std::string strip_ip_brackets(const std::string& value) {
    if (value.size() >= 2 && value.front() == '[' && value.back() == ']') {
        return value.substr(1, value.size() - 2);
    }
    return value;
}

bool is_ipv4_mapped(const std::array<uint8_t, 16>& bytes) {
    for (size_t i = 0; i < 10; ++i) {
        if (bytes[i] != 0) {
            return false;
        }
    }
    return bytes[10] == 0xff && bytes[11] == 0xff;
}

bool parse_ipv4_bytes(const std::string& ip, std::array<uint8_t, 4>& bytes) {
    struct in_addr address {};
    if (inet_pton(AF_INET, ip.c_str(), &address) != 1) {
        return false;
    }
    std::memcpy(bytes.data(), &address, bytes.size());
    return true;
}

bool parse_ipv6_bytes(const std::string& ip, std::array<uint8_t, 16>& bytes) {
    struct in6_addr address {};
    if (inet_pton(AF_INET6, ip.c_str(), &address) != 1) {
        return false;
    }
    std::memcpy(bytes.data(), &address, bytes.size());
    return true;
}

}  // namespace

bool NetworkUtils::split_host_port(const std::string& endpoint, std::string& host, uint16_t& port) {
    host.clear();
    port = 0;

    if (endpoint.empty() ||
        endpoint.size() > kMaxEndpointLength ||
        contains_control_or_space(endpoint)) {
        return false;
    }

    // [ipv6]:port
    if (endpoint.front() == '[') {
        const size_t close = endpoint.find(']');
        if (close == std::string::npos || close + 1 >= endpoint.size() || endpoint[close + 1] != ':') {
            return false;
        }
        if (!normalize_host_part(endpoint.substr(1, close - 1), host)) {
            return false;
        }
        return parse_port_strict(endpoint.substr(close + 2), port);
    }

    const size_t last_colon = endpoint.rfind(':');
    if (last_colon == std::string::npos) {
        return false;
    }
    // Небракетный IPv6 адрес неоднозначен.
    if (endpoint.find(':') != last_colon) {
        return false;
    }

    if (!normalize_host_part(endpoint.substr(0, last_colon), host)) {
        return false;
    }

    return parse_port_strict(endpoint.substr(last_colon + 1), port);
}

bool NetworkUtils::parse_peer_address(
    const std::string& address,
    std::string& host,
    uint16_t& port,
    uint16_t default_port
) {
    host.clear();
    port = 0;
    if (address.find(':') != std::string::npos) {
        return split_host_port(address, host, port);
    }
    if (default_port == 0 || !normalize_host_part(address, host)) {
        return false;
    }
    port = default_port;
    return true;
}

std::string NetworkUtils::normalize_ip_address(const std::string& ip) {
    if (ip.empty()) {
        return ip;
    }

    const std::string out = strip_ip_brackets(ip);

    std::string numeric;
    if (canonicalize_numeric_ip(out, numeric)) {
        return numeric;
    }

    std::string lowered = out;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    return lowered;
}

bool NetworkUtils::canonicalize_numeric_ip(const std::string& ip, std::string& canonical) {
    canonical.clear();
    const std::string raw = strip_ip_brackets(ip);
    if (raw.empty() || raw.size() > kMaxHostLength || contains_control_or_space(raw)) {
        return false;
    }

    struct in_addr address4 {};
    if (inet_pton(AF_INET, raw.c_str(), &address4) == 1) {
        char text[INET_ADDRSTRLEN] = {};
        if (inet_ntop(AF_INET, &address4, text, sizeof(text)) == nullptr) {
            return false;
        }
        canonical = text;
        return true;
    }

    struct in6_addr address6 {};
    if (inet_pton(AF_INET6, raw.c_str(), &address6) != 1) {
        return false;
    }

    std::array<uint8_t, 16> bytes {};
    std::memcpy(bytes.data(), &address6, bytes.size());
    if (is_ipv4_mapped(bytes)) {
        struct in_addr mapped4 {};
        std::memcpy(&mapped4, bytes.data() + 12, sizeof(mapped4));
        char text[INET_ADDRSTRLEN] = {};
        if (inet_ntop(AF_INET, &mapped4, text, sizeof(text)) == nullptr) {
            return false;
        }
        canonical = text;
        return true;
    }

    char text[INET6_ADDRSTRLEN] = {};
    if (inet_ntop(AF_INET6, &address6, text, sizeof(text)) == nullptr) {
        return false;
    }
    canonical = text;
    std::transform(canonical.begin(), canonical.end(), canonical.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return true;
}

bool NetworkUtils::is_global_unicast_ip(const std::string& ip) {
    std::string canonical;
    if (!canonicalize_numeric_ip(ip, canonical)) {
        return false;
    }

    std::array<uint8_t, 4> ipv4 {};
    if (parse_ipv4_bytes(canonical, ipv4)) {
        const uint8_t a = ipv4[0];
        const uint8_t b = ipv4[1];
        const uint8_t c = ipv4[2];
        if (a == 0 || a == 10 || a == 127 || a >= 224) {
            return false;
        }
        if (a == 100 && (b & 0xc0U) == 0x40U) {  // 100.64.0.0/10 CGNAT
            return false;
        }
        if (a == 169 && b == 254) {
            return false;
        }
        if (a == 172 && b >= 16 && b <= 31) {
            return false;
        }
        if (a == 192 && (b == 168 || (b == 0 && c == 0) || (b == 0 && c == 2) ||
                         (b == 88 && c == 99))) {
            return false;
        }
        if (a == 198 && (b == 18 || b == 19 || (b == 51 && c == 100))) {
            return false;
        }
        if (a == 203 && b == 0 && c == 113) {
            return false;
        }
        return true;
    }

    std::array<uint8_t, 16> ipv6 {};
    if (!parse_ipv6_bytes(canonical, ipv6)) {
        return false;
    }

    // Current globally routable IPv6 unicast space is 2000::/3.
    if ((ipv6[0] & 0xe0U) != 0x20U) {
        return false;
    }
    // Documentation, Teredo, benchmarking and ORCHID ranges are not peers.
    if (ipv6[0] == 0x20 && ipv6[1] == 0x01) {
        if ((ipv6[2] == 0x0d && ipv6[3] == 0xb8) ||  // 2001:db8::/32
            (ipv6[2] == 0x00 && ipv6[3] == 0x00) ||  // 2001::/32
            (ipv6[2] == 0x00 && ipv6[3] == 0x02) ||  // 2001:2::/48
            (ipv6[2] == 0x00 && (ipv6[3] & 0xf0U) == 0x10U) ||
            (ipv6[2] == 0x00 && (ipv6[3] & 0xf0U) == 0x20U)) {
            return false;
        }
    }
    return true;
}

std::string NetworkUtils::network_group(const std::string& host) {
    std::string canonical;
    if (!canonicalize_numeric_ip(host, canonical)) {
        const std::string normalized = normalize_ip_address(host);
        return normalized.empty() ? std::string{} : "name:" + normalized;
    }

    std::array<uint8_t, 4> ipv4 {};
    if (parse_ipv4_bytes(canonical, ipv4)) {
        return std::to_string(ipv4[0]) + "." + std::to_string(ipv4[1]) + "." +
               std::to_string(ipv4[2]) + ".0/24";
    }

    std::array<uint8_t, 16> ipv6 {};
    if (!parse_ipv6_bytes(canonical, ipv6)) {
        return {};
    }
    std::fill(ipv6.begin() + 8, ipv6.end(), 0);
    struct in6_addr grouped {};
    std::memcpy(&grouped, ipv6.data(), ipv6.size());
    char text[INET6_ADDRSTRLEN] = {};
    if (inet_ntop(AF_INET6, &grouped, text, sizeof(text)) == nullptr) {
        return {};
    }
    std::string result = text;
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return result + "/64";
}

std::string NetworkUtils::abuse_key(const std::string& host) {
    std::string canonical;
    if (!canonicalize_numeric_ip(host, canonical)) {
        return {};
    }

    std::array<uint8_t, 4> ipv4 {};
    if (parse_ipv4_bytes(canonical, ipv4)) {
        return canonical;
    }
    return network_group(canonical);
}

std::string NetworkUtils::make_endpoint(const std::string& host, uint16_t port) {
    const std::string normalized = normalize_ip_address(host);
    if (normalized.find(':') != std::string::npos) {
        return "[" + normalized + "]:" + std::to_string(port);
    }
    return normalized + ":" + std::to_string(port);
}

std::vector<std::string> NetworkUtils::get_local_addresses() {
    std::vector<std::string> addresses;

#ifdef _WIN32
    static const bool winsock_ready = []() {
        WSADATA wsa_data;
        return WSAStartup(MAKEWORD(2, 2), &wsa_data) == 0;
    }();
    if (!winsock_ready) {
        return addresses;
    }

    char hostname[256] = {};
    if (gethostname(hostname, sizeof(hostname)) != 0) {
        return addresses;
    }

    struct addrinfo hints = {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* info = nullptr;
    if (getaddrinfo(hostname, nullptr, &hints, &info) != 0) {
        return addresses;
    }

    for (struct addrinfo* it = info; it != nullptr; it = it->ai_next) {
        if (it->ai_family == AF_INET) {
            const auto* addr = reinterpret_cast<struct sockaddr_in*>(it->ai_addr);
            char ip_str[INET_ADDRSTRLEN] = {};
            if (inet_ntop(AF_INET, &(addr->sin_addr), ip_str, sizeof(ip_str))) {
                addresses.push_back(normalize_ip_address(ip_str));
            }
        } else if (it->ai_family == AF_INET6) {
            const auto* addr = reinterpret_cast<struct sockaddr_in6*>(it->ai_addr);
            char ip_str[INET6_ADDRSTRLEN] = {};
            if (inet_ntop(AF_INET6, &(addr->sin6_addr), ip_str, sizeof(ip_str))) {
                addresses.push_back(normalize_ip_address(ip_str));
            }
        }
    }

    freeaddrinfo(info);
    return addresses;
#else
    struct ifaddrs* ifaddr;
    if (getifaddrs(&ifaddr) == -1) {
        return addresses;
    }

    // Проходим по всем интерфейсам
    for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == nullptr) {
            continue;
        }

        if (ifa->ifa_addr->sa_family == AF_INET) {
            struct sockaddr_in* addr = (struct sockaddr_in*)ifa->ifa_addr;
            char ip_str[INET_ADDRSTRLEN];

            if (inet_ntop(AF_INET, &addr->sin_addr, ip_str, INET_ADDRSTRLEN)) {
                addresses.push_back(normalize_ip_address(ip_str));
            }
        } else if (ifa->ifa_addr->sa_family == AF_INET6) {
            struct sockaddr_in6* addr = (struct sockaddr_in6*)ifa->ifa_addr;
            char ip_str[INET6_ADDRSTRLEN];

            if (inet_ntop(AF_INET6, &addr->sin6_addr, ip_str, INET6_ADDRSTRLEN)) {
                addresses.push_back(normalize_ip_address(ip_str));
            }
        }
    }

    freeifaddrs(ifaddr);

    return addresses;
#endif
}

bool NetworkUtils::is_local_address(const std::string& ip) {
    const std::string normalized = normalize_ip_address(ip);

    // Проверяем очевидные локальные адреса
    if (normalized == "127.0.0.1" ||
        normalized == "localhost" ||
        normalized == "::1" ||
        normalized == "0.0.0.0" ||
        normalized == "::") {
        return true;
    }

    // Получаем все локальные адреса
    auto local_addrs = get_local_addresses();

    for (const auto& local_ip : local_addrs) {
        if (normalize_ip_address(local_ip) == normalized) {
            return true;
        }
    }

    return false;
}

bool NetworkUtils::is_self_address(const std::string& address, int our_port) {
    std::string ip;
    uint16_t port = 0;
    if (!split_host_port(address, ip, port)) {
        return false;
    }

    // Проверяем порт
    if (port != our_port) {
        return false;
    }

    // Проверяем IP
    return is_local_address(ip);
}

} // namespace geocoin::network
