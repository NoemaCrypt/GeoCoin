#pragma once

#include "geocoin/utils/base64.hpp"

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace geocoin::tools {

struct HttpResponse {
    int status = 0;
    std::string body;
};

inline HttpResponse http_post(
    const std::string& url,
    const std::string& username,
    const std::string& password,
    const std::string& content_type,
    const std::string& body,
    int timeout_seconds
) {
    if (url.rfind("http://", 0) != 0) {
        throw std::runtime_error("only http:// RPC URLs are supported");
    }
    const std::string rest = url.substr(7);
    const auto slash = rest.find('/');
    const std::string host_port = slash == std::string::npos ? rest : rest.substr(0, slash);
    const std::string path = slash == std::string::npos || slash + 1 == rest.size()
        ? "/"
        : rest.substr(slash);
    const auto colon = host_port.rfind(':');
    const std::string host = colon == std::string::npos ? host_port : host_port.substr(0, colon);
    const std::string port = colon == std::string::npos ? "80" : host_port.substr(colon + 1);
    if (host.empty() || port.empty()) {
        throw std::runtime_error("invalid RPC URL");
    }

    const std::string credential = geocoin::utils::base64_encode(username + ":" + password);
    std::ostringstream request;
    request << "POST " << path << " HTTP/1.1\r\n"
            << "Host: " << host << "\r\n"
            << "Authorization: Basic " << credential << "\r\n"
            << "Content-Type: " << content_type << "\r\n"
            << "Content-Length: " << body.size() << "\r\n"
            << "Connection: close\r\n\r\n"
            << body;
    const std::string bytes = request.str();

#if defined(_WIN32)
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        throw std::runtime_error("WSAStartup failed");
    }
    struct WsaGuard { ~WsaGuard() { WSACleanup(); } } wsa_guard;
    using Socket = SOCKET;
    constexpr Socket kInvalid = INVALID_SOCKET;
    auto last_error = [] { return WSAGetLastError(); };
    auto close_socket = [](Socket socket) { closesocket(socket); };
#else
    using Socket = int;
    constexpr Socket kInvalid = -1;
    auto last_error = [] { return errno; };
    auto close_socket = [](Socket socket) { ::close(socket); };
#endif

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* resolved = nullptr;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &resolved) != 0 || resolved == nullptr) {
        throw std::runtime_error("RPC host lookup failed");
    }
    Socket socket = kInvalid;
    for (addrinfo* it = resolved; it != nullptr && socket == kInvalid; it = it->ai_next) {
        socket = ::socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (socket == kInvalid) continue;
        if (::connect(socket, it->ai_addr, static_cast<int>(it->ai_addrlen)) != 0) {
            close_socket(socket);
            socket = kInvalid;
        }
    }
    freeaddrinfo(resolved);
    if (socket == kInvalid) {
        throw std::runtime_error("RPC connect failed: " + std::to_string(last_error()));
    }
    struct SocketGuard {
        Socket socket;
        void (*close_socket)(Socket);
        ~SocketGuard() { close_socket(socket); }
    } guard{socket, close_socket};

    auto wait_for = [&](bool write) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(socket, &fds);
        timeval tv{};
        tv.tv_sec = timeout_seconds;
        const int ready = ::select(
#if defined(_WIN32)
            0,
#else
            static_cast<int>(socket) + 1,
#endif
            write ? nullptr : &fds,
            write ? &fds : nullptr,
            nullptr,
            &tv
        );
        if (ready <= 0) {
            throw std::runtime_error("RPC request timed out");
        }
    };

    size_t sent = 0;
    while (sent < bytes.size()) {
        wait_for(true);
        const auto n = ::send(socket, bytes.data() + sent, static_cast<int>(bytes.size() - sent), 0);
        if (n <= 0) throw std::runtime_error("RPC send failed");
        sent += static_cast<size_t>(n);
    }

    std::string raw;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
    char chunk[4096];
    while (std::chrono::steady_clock::now() < deadline) {
        wait_for(false);
        const auto n = ::recv(socket, chunk, sizeof(chunk), 0);
        if (n == 0) break;
        if (n < 0) throw std::runtime_error("RPC receive failed");
        raw.append(chunk, static_cast<size_t>(n));
    }

    const auto header_end = raw.find("\r\n\r\n");
    if (header_end == std::string::npos || raw.size() < 12 || raw.compare(0, 5, "HTTP/") != 0) {
        throw std::runtime_error("RPC response is not HTTP");
    }
    const auto status_start = raw.find(' ');
    const auto status_end = status_start == std::string::npos ? std::string::npos : raw.find(' ', status_start + 1);
    if (status_start == std::string::npos || status_end == std::string::npos) {
        throw std::runtime_error("RPC response has no status");
    }
    HttpResponse response;
    response.status = std::stoi(raw.substr(status_start + 1, status_end - status_start - 1));
    response.body = raw.substr(header_end + 4);
    return response;
}

} // namespace geocoin::tools
