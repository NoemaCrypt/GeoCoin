#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace geocoin::rpc::socket_detail {

#ifdef _WIN32
using RpcSocket = SOCKET;
inline constexpr RpcSocket kInvalidSocket = INVALID_SOCKET;
#else
using RpcSocket = int;
inline constexpr RpcSocket kInvalidSocket = -1;
#endif

enum class IoStatus { Ok, Timeout, Closed, Error };

inline std::intptr_t to_fd(RpcSocket socket) noexcept {
    return static_cast<std::intptr_t>(socket);
}

inline RpcSocket from_fd(std::intptr_t fd) noexcept {
    return static_cast<RpcSocket>(fd);
}

inline bool valid(RpcSocket socket) noexcept {
    return socket != kInvalidSocket;
}

inline bool ensure_network() {
#ifdef _WIN32
    static const bool started = [] {
        WSADATA data{};
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return started;
#else
    return true;
#endif
}

inline void close_socket(RpcSocket socket) noexcept {
    if (!valid(socket)) {
        return;
    }
#ifdef _WIN32
    ::closesocket(socket);
#else
    ::close(socket);
#endif
}

inline void shutdown_both(RpcSocket socket) noexcept {
    if (!valid(socket)) {
        return;
    }
#ifdef _WIN32
    ::shutdown(socket, SD_BOTH);
#else
    ::shutdown(socket, SHUT_RDWR);
#endif
}

inline bool set_nonblocking(RpcSocket socket) {
#ifdef _WIN32
    u_long mode = 1;
    return ::ioctlsocket(socket, FIONBIO, &mode) == 0;
#else
    const int flags = ::fcntl(socket, F_GETFL, 0);
    return flags >= 0 && ::fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

inline bool set_reuseaddr(RpcSocket socket) {
    const int enabled = 1;
    return ::setsockopt(
               socket,
               SOL_SOCKET,
               SO_REUSEADDR,
               reinterpret_cast<const char*>(&enabled),
               sizeof(enabled)) == 0;
}

inline int last_error() {
#ifdef _WIN32
    return ::WSAGetLastError();
#else
    return errno;
#endif
}

inline bool interrupted(int error) {
#ifdef _WIN32
    return error == WSAEINTR;
#else
    return error == EINTR;
#endif
}

inline bool would_block(int error) {
#ifdef _WIN32
    return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS;
#else
    return error == EAGAIN || error == EWOULDBLOCK || error == EINPROGRESS;
#endif
}

inline int wait_for(RpcSocket socket, short events, std::chrono::steady_clock::time_point deadline) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
        return 0;
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    int timeout_ms = static_cast<int>(std::min<std::chrono::milliseconds::rep>(remaining.count(), 1000));
    if (timeout_ms < 0) {
        timeout_ms = 0;
    }
#ifdef _WIN32
    WSAPOLLFD poll_fd{};
    poll_fd.fd = socket;
    poll_fd.events = events;
    const int ready = ::WSAPoll(&poll_fd, 1, timeout_ms);
#else
    pollfd poll_fd{};
    poll_fd.fd = socket;
    poll_fd.events = events;
    const int ready = ::poll(&poll_fd, 1, timeout_ms);
#endif
    if (ready > 0 && (poll_fd.revents & (POLLERR | POLLNVAL)) != 0 &&
        (poll_fd.revents & (POLLIN | POLLOUT)) == 0) {
        return -1;
    }
    return ready;
}

inline IoStatus read_some(
    RpcSocket socket,
    char* buffer,
    size_t capacity,
    size_t& got,
    std::chrono::steady_clock::time_point deadline) {
    got = 0;
    while (true) {
        const int ready = wait_for(socket, POLLIN, deadline);
        if (ready == 0) {
            if (std::chrono::steady_clock::now() >= deadline) {
                return IoStatus::Timeout;
            }
            continue;
        }
        if (ready < 0) {
            if (interrupted(last_error())) {
                continue;
            }
            return IoStatus::Error;
        }
#ifdef _WIN32
        const int received = ::recv(socket, buffer, static_cast<int>(capacity), 0);
        if (received == SOCKET_ERROR) {
#else
        const ssize_t received = ::recv(socket, buffer, capacity, 0);
        if (received < 0) {
#endif
            if (would_block(last_error()) || interrupted(last_error())) {
                continue;
            }
            return IoStatus::Closed;
        }
        if (received == 0) {
            return IoStatus::Closed;
        }
        got = static_cast<size_t>(received);
        return IoStatus::Ok;
    }
}

inline IoStatus write_all(
    RpcSocket socket,
    const char* data,
    size_t size,
    std::chrono::steady_clock::time_point deadline) {
    size_t sent_total = 0;
    while (sent_total < size) {
        const int ready = wait_for(socket, POLLOUT, deadline);
        if (ready == 0) {
            if (std::chrono::steady_clock::now() >= deadline) {
                return IoStatus::Timeout;
            }
            continue;
        }
        if (ready < 0) {
            if (interrupted(last_error())) {
                continue;
            }
            return IoStatus::Error;
        }
#ifdef _WIN32
        const int sent = ::send(
            socket,
            data + sent_total,
            static_cast<int>(size - sent_total),
            0);
        if (sent == SOCKET_ERROR) {
#else
        const ssize_t sent = ::send(
            socket,
            data + sent_total,
            size - sent_total,
            MSG_NOSIGNAL);
        if (sent < 0) {
#endif
            if (would_block(last_error()) || interrupted(last_error())) {
                continue;
            }
            return IoStatus::Error;
        }
        if (sent == 0) {
            return IoStatus::Error;
        }
        sent_total += static_cast<size_t>(sent);
    }
    return IoStatus::Ok;
}

inline std::string peer_address(RpcSocket socket) {
    sockaddr_storage address{};
    socklen_t length = sizeof(address);
    if (::getpeername(socket, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
        return "unknown";
    }
    char host[NI_MAXHOST] = {};
    if (::getnameinfo(
            reinterpret_cast<sockaddr*>(&address),
            length,
            host,
            sizeof(host),
            nullptr,
            0,
            NI_NUMERICHOST) != 0) {
        return "unknown";
    }
    return host;
}

inline std::intptr_t listen_tcp(const std::string& host, uint16_t port) {
    if (!ensure_network()) {
        throw std::runtime_error("socket subsystem is unavailable");
    }

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;
    addrinfo* result = nullptr;
    const std::string service = std::to_string(port);
    const int lookup = ::getaddrinfo(host.c_str(), service.c_str(), &hints, &result);
    if (lookup != 0) {
        throw std::runtime_error(std::string("invalid RPC bind address: ") + gai_strerror(lookup));
    }

    std::intptr_t bound = -1;
    std::string failure = "failed to bind RPC listen socket";
    for (addrinfo* info = result; info != nullptr; info = info->ai_next) {
        const RpcSocket socket = ::socket(info->ai_family, info->ai_socktype, info->ai_protocol);
        if (!valid(socket)) {
            continue;
        }
        set_reuseaddr(socket);
        if (::bind(socket, info->ai_addr, info->ai_addrlen) != 0 ||
            ::listen(socket, SOMAXCONN) != 0) {
            failure = "failed to bind RPC listen socket";
            close_socket(socket);
            continue;
        }
        bound = to_fd(socket);
        break;
    }
    ::freeaddrinfo(result);
    if (bound < 0) {
        throw std::runtime_error(failure);
    }
    return bound;
}

} // namespace geocoin::rpc::socket_detail
