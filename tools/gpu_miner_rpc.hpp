#pragma once

// GPU-local transport: all socket waits are bounded and cancellation is checked during DNS/I/O.
#include "http_post.hpp"
#include "gpu_miner_runtime.hpp"

#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http.hpp>

#include <atomic>
#include <memory>
#include <mutex>

#ifndef _WIN32
#include <poll.h>
#endif

namespace geocoin::tools::gpu_miner {

inline HttpResponse cancellable_http_post(const std::string& url, const std::string& user,
    const std::string& pass, const std::string& body, const std::function<bool()>& cancel)
{
    if (url.rfind("http://", 0) != 0)
        throw FailureError(Failure::Configuration, "only http:// RPC URLs are supported");
    const auto slash = url.find('/', 7);
    const std::string authority = url.substr(7, slash == std::string::npos ? slash : slash - 7);
    std::string host = authority;
    std::string port = "80";
    if (!authority.empty() && authority.front() == '[') {
        const auto end = authority.find(']');
        if (end == std::string::npos || (end + 1 < authority.size() && authority[end + 1] != ':'))
            throw FailureError(Failure::Configuration, "invalid RPC URL host");
        host = authority.substr(1, end - 1);
        if (end + 1 < authority.size()) port = authority.substr(end + 2);
    } else {
        const auto colon = authority.find(':');
        if (colon != authority.rfind(':')) throw FailureError(Failure::Configuration, "IPv6 RPC hosts require brackets");
        if (colon != std::string::npos) { host = authority.substr(0, colon); port = authority.substr(colon + 1); }
    }
    const std::string path = slash == std::string::npos ? "/" : url.substr(slash);
    if (host.empty() || port.empty() || port.size() > 5 ||
        !std::all_of(port.begin(), port.end(), [](char c) { return c >= '0' && c <= '9'; }) ||
        std::stoul(port) == 0 || std::stoul(port) > 65535 ||
        authority.find_first_of("\r\n\t@ ?#") != std::string::npos || path.find_first_of("\r\n\t ") != std::string::npos)
        throw FailureError(Failure::Configuration, "invalid RPC URL");
#ifdef _WIN32
    struct Network {
        Network() { WSADATA data{}; if (WSAStartup(MAKEWORD(2, 2), &data)) throw FailureError(Failure::Configuration, "WSAStartup failed"); }
        ~Network() { WSACleanup(); }
    };
    auto network = std::make_shared<Network>();
    using Socket = SOCKET;
    constexpr Socket invalid = INVALID_SOCKET;
    auto close_socket = [](Socket s) { closesocket(s); };
    auto would_block = [] { const int e = WSAGetLastError(); return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS; };
#else
    auto network = std::make_shared<int>(0);
    using Socket = int;
    constexpr Socket invalid = -1;
    auto close_socket = [](Socket s) { ::close(s); };
    auto would_block = [] { return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS || errno == EINTR; };
#endif
    const auto deadline = Clock::now() + std::chrono::seconds(30);
    auto check = [&] {
        if (cancel && cancel()) throw Cancelled();
        if (Clock::now() >= deadline) throw FailureError(Failure::Transient, "RPC request timed out");
    };
    struct Resolution {
        std::shared_ptr<void> network;
        std::string host, port;
        std::atomic<bool> done{false};
        int error = 0;
        addrinfo* addresses = nullptr;
        ~Resolution() { if (addresses) freeaddrinfo(addresses); }
    };
    check();
    auto resolution = std::make_shared<Resolution>();
    resolution->network = network;
    resolution->host = host;
    resolution->port = port;
    addrinfo hints{};
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;
    // Numeric GUI/local URLs never start a DNS worker.
    const int numeric = getaddrinfo(host.c_str(), port.c_str(), &hints, &resolution->addresses);
    if (numeric == 0) resolution->done.store(true, std::memory_order_release);
    else {
        // A timed-out lookup remains the sole outstanding worker and is reused on retries.
        static std::mutex resolver_mutex;
        static std::shared_ptr<Resolution> pending;
        for (;;) {
            check();
            std::lock_guard<std::mutex> lock(resolver_mutex);
            if (!pending || pending->done.load(std::memory_order_acquire)) {
                pending = resolution;
                std::thread([resolution] {
                    addrinfo dns_hints{};
                    dns_hints.ai_socktype = SOCK_STREAM;
                    dns_hints.ai_family = AF_UNSPEC;
                    dns_hints.ai_flags = AI_NUMERICSERV;
                    resolution->error = getaddrinfo(resolution->host.c_str(), resolution->port.c_str(),
                        &dns_hints, &resolution->addresses);
                    resolution->done.store(true, std::memory_order_release);
                }).detach();
                break;
            }
            if (pending->host == host && pending->port == port) { resolution = pending; break; }
            // No second lookup is started for a different host until this one finishes.
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    while (!resolution->done.load(std::memory_order_acquire)) {
        check();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    check();
    if (resolution->error || !resolution->addresses)
        throw FailureError(Failure::Transient, "RPC host lookup failed");
    auto wait = [&](Socket s, bool write) {
        for (;;) {
            check();
#ifdef _WIN32
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(s, &fds);
            fd_set errors = fds;
            timeval timeout{0, 50000};
            const int ready = ::select(0, write ? nullptr : &fds, write ? &fds : nullptr, &errors, &timeout);
#else
            pollfd descriptor{s, static_cast<short>(write ? POLLOUT : POLLIN), 0};
            const int ready = ::poll(&descriptor, 1, 50);
#endif
            if (ready > 0) return;
            if (ready < 0) {
#ifndef _WIN32
                if (errno == EINTR) continue;
#endif
                throw FailureError(Failure::Transient, "RPC socket wait failed");
            }
        }
    };
    struct SocketGuard {
        Socket socket;
        void (*close)(Socket);
        ~SocketGuard() { if (socket != invalid) close(socket); }
    } guard{invalid, close_socket};
    for (const auto* address = resolution->addresses; address; address = address->ai_next) {
        check();
        Socket s = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (s == invalid) continue;
        guard.socket = s;
#ifdef _WIN32
        u_long nonblocking = 1;
        const bool configured = ioctlsocket(s, FIONBIO, &nonblocking) == 0;
#else
        const bool configured = fcntl(s, F_SETFL, O_NONBLOCK) == 0;
#endif
        if (configured) {
            const int rc = ::connect(s, address->ai_addr, static_cast<int>(address->ai_addrlen));
            if (rc == 0) break;
            if (would_block()) {
                wait(s, true);
                int error = 0;
#ifdef _WIN32
                int size = sizeof(error);
#else
                socklen_t size = sizeof(error);
#endif
                if (getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &size) == 0 && error == 0) break;
            }
        }
        close_socket(s);
        guard.socket = invalid;
    }
    if (guard.socket == invalid) throw FailureError(Failure::Transient, "RPC connect failed");
    const Socket s = guard.socket;
    const std::string request = "POST " + path + " HTTP/1.1\r\nHost: " + authority +
        "\r\nAuthorization: Basic " + geocoin::utils::base64_encode(user + ":" + pass) +
        "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) +
        "\r\nConnection: close\r\n\r\n" + body;
    size_t sent = 0;
    while (sent < request.size()) {
        wait(s, true);
#ifdef MSG_NOSIGNAL
        constexpr int flags = MSG_NOSIGNAL;
#else
        constexpr int flags = 0;
#endif
        const int n = static_cast<int>(::send(s, request.data() + sent,
            static_cast<int>(std::min<size_t>(request.size() - sent, 65536)), flags));
        if (n < 0 && would_block()) continue;
        if (n <= 0) throw FailureError(Failure::Transient, "RPC send failed");
        sent += n;
    }
    boost::beast::http::response_parser<boost::beast::http::string_body> parser;
    parser.body_limit(32 * 1024 * 1024);
    parser.header_limit(64 * 1024);
    parser.eager(true);
    boost::beast::flat_buffer buffer;
    char bytes[4096];
    while (!parser.is_done()) {
        wait(s, false);
        const int n = static_cast<int>(::recv(s, bytes, sizeof(bytes), 0));
        if (n < 0 && would_block()) continue;
        if (n < 0) throw FailureError(Failure::Transient, "RPC receive failed");
        boost::system::error_code error;
        if (n == 0) {
            if (!parser.got_some()) throw FailureError(Failure::Transient, "RPC connection closed without a response");
            parser.put_eof(error);
            if (error || !parser.is_done()) throw FailureError(Failure::Corrupt, "truncated RPC HTTP response");
            break;
        }
        boost::asio::buffer_copy(buffer.prepare(static_cast<size_t>(n)), boost::asio::buffer(bytes, static_cast<size_t>(n)));
        buffer.commit(static_cast<size_t>(n));
        const auto consumed = parser.put(buffer.data(), error);
        buffer.consume(consumed);
        if (error && error != boost::beast::http::error::need_more)
            throw FailureError(Failure::Corrupt, "invalid RPC HTTP framing: " + error.message());
    }
    return {static_cast<int>(parser.get().result_int()), std::move(parser.get().body())};
}

} // namespace geocoin::tools::gpu_miner
