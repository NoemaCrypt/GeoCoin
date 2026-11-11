#include "geocoin/network/dns_resolver.hpp"
#include "geocoin/network/network_utils.hpp"
#include "geocoin/utils/logger.hpp"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netdb.h>
#include <arpa/inet.h>
#endif
#include <cstring>
#include <algorithm>
#include <memory>

namespace geocoin::network {

namespace {
#ifdef _WIN32
void ensure_winsock_initialized() {
    static const bool initialized = []() {
        WSADATA wsa_data;
        return WSAStartup(MAKEWORD(2, 2), &wsa_data) == 0;
    }();
    (void)initialized;
}
#else
void ensure_winsock_initialized() {}
#endif

std::vector<std::string> resolve_host_with_port(const char* hostname, int port) {
    std::vector<std::string> result;
    if (!hostname || *hostname == '\0' || (port <= 0 || port > 65535)) {
        return result;
    }

    ensure_winsock_initialized();

    struct addrinfo hints = {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* res = nullptr;
    int status = getaddrinfo(hostname, nullptr, &hints, &res);
    if (status != 0) {
        GEO_LOG_WARN("network", "DNS resolve failed for " << hostname << ": " << gai_strerror(status));
        return result;
    }

    const std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> resolved(res, &freeaddrinfo);
    for (const addrinfo* candidate = resolved.get(); candidate != nullptr; candidate = candidate->ai_next) {
        const void* address = nullptr;
        if (candidate->ai_family == AF_INET) {
            address = &reinterpret_cast<const sockaddr_in*>(candidate->ai_addr)->sin_addr;
        } else if (candidate->ai_family == AF_INET6) {
            address = &reinterpret_cast<const sockaddr_in6*>(candidate->ai_addr)->sin6_addr;
        } else {
            continue;
        }
        char host[INET6_ADDRSTRLEN]{};
        if (inet_ntop(candidate->ai_family, address, host, sizeof(host))) {
            const auto endpoint = NetworkUtils::make_endpoint(host, static_cast<uint16_t>(port));
            if (std::find(result.begin(), result.end(), endpoint) == result.end()) result.push_back(endpoint);
        }
    }

    return result;
}
} // namespace

std::vector<std::string> DNSResolver::resolve_seeds_with_port(
    const char* const* seeds,
    int port
) {
    std::vector<std::string> result;
    if (!seeds || (port <= 0 || port > 65535)) {
        return result;
    }

    for (size_t i = 0; seeds[i] != nullptr; ++i) {
        const char* hostname = seeds[i];
        if (!hostname || *hostname == '\0') {
            continue;
        }

        auto addresses = resolve_host_with_port(hostname, port);
        for (const auto& address : addresses) {
            if (std::find(result.begin(), result.end(), address) == result.end()) result.push_back(address);
        }
    }

    return result;
}

std::vector<std::string> DNSResolver::resolve_seeds_with_port(
    const std::vector<std::string>& seeds,
    int port
) {
    std::vector<std::string> result;
    if (seeds.empty() || (port <= 0 || port > 65535)) {
        return result;
    }

    for (const auto& seed : seeds) {
        auto addresses = resolve_host_with_port(seed.c_str(), port);
        for (const auto& address : addresses) {
            if (std::find(result.begin(), result.end(), address) == result.end()) result.push_back(address);
        }
    }

    return result;
}

} // namespace geocoin::network
