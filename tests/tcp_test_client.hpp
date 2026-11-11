#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

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
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace geocoin::test {

#ifdef _WIN32
using TestSocket = SOCKET;
using TestSocketLength = int;
inline constexpr TestSocket kInvalidTestSocket = INVALID_SOCKET;
#else
using TestSocket = int;
using TestSocketLength = socklen_t;
inline constexpr TestSocket kInvalidTestSocket = -1;
#endif

inline bool ensure_test_sockets() {
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

inline void close_test_socket(TestSocket socket) noexcept {
    if (socket == kInvalidTestSocket) {
        return;
    }
#ifdef _WIN32
    ::closesocket(socket);
#else
    ::close(socket);
#endif
}

// The node's direct-peer test API takes int descriptors. On Windows use a
// connected loopback TCP pair instead of the POSIX-only socketpair call.
inline int test_socket_fd(TestSocket socket) {
    if (socket == kInvalidTestSocket) return -1;
#ifdef _WIN32
    if (socket > static_cast<TestSocket>(std::numeric_limits<int>::max())) {
        close_test_socket(socket);
        return -1;
    }
#endif
    return static_cast<int>(socket);
}

inline int create_test_tcp_socket() {
    if (!ensure_test_sockets()) return -1;
    return test_socket_fd(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
}

inline bool make_test_socket_pair(int (&fds)[2]) {
    fds[0] = fds[1] = -1;
#ifdef _WIN32
    if (!ensure_test_sockets()) return false;
    const TestSocket listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == kInvalidTestSocket) return false;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    TestSocketLength length = sizeof(address);
    if (::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        ::listen(listener, 1) != 0 ||
        ::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
        close_test_socket(listener);
        return false;
    }
    const TestSocket client = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (client == kInvalidTestSocket ||
        ::connect(client, reinterpret_cast<sockaddr*>(&address), length) != 0) {
        close_test_socket(client);
        close_test_socket(listener);
        return false;
    }
    const TestSocket accepted = ::accept(listener, nullptr, nullptr);
    close_test_socket(listener);
    fds[0] = test_socket_fd(accepted);
    fds[1] = test_socket_fd(client);
    if (fds[0] < 0 || fds[1] < 0) {
        close_test_socket(static_cast<TestSocket>(fds[0]));
        close_test_socket(static_cast<TestSocket>(fds[1]));
        fds[0] = fds[1] = -1;
        return false;
    }
    return true;
#else
    return ::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0;
#endif
}

inline constexpr int kTestDontWait = 0x10000000;

inline int send_test_fd(int fd, const void* data, size_t size, int flags = 0) {
    const int length = static_cast<int>(std::min(size, static_cast<size_t>(std::numeric_limits<int>::max())));
    return static_cast<int>(::send(fd, static_cast<const char*>(data), length, flags));
}

inline int receive_test_fd(int fd, void* data, size_t size, int flags = 0) {
    if (flags & kTestDontWait) {
        flags &= ~kTestDontWait;
#ifdef _WIN32
        WSAPOLLFD pending{};
        pending.fd = static_cast<SOCKET>(fd);
        pending.events = POLLIN;
        const int ready = ::WSAPoll(&pending, 1, 0);
        if (ready <= 0) {
            if (ready == 0) ::WSASetLastError(WSAEWOULDBLOCK);
            return -1;
        }
#else
        flags |= MSG_DONTWAIT;
#endif
    }
    const int length = static_cast<int>(std::min(size, static_cast<size_t>(std::numeric_limits<int>::max())));
    return static_cast<int>(::recv(fd, static_cast<char*>(data), length, flags));
}

inline int test_socket_error() {
#ifdef _WIN32
    return ::WSAGetLastError();
#else
    return errno;
#endif
}

inline bool test_socket_interrupted(int error) {
#ifdef _WIN32
    return error == WSAEINTR;
#else
    return error == EINTR;
#endif
}

inline bool test_socket_would_block(int error) {
#ifdef _WIN32
    return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS;
#else
    return error == EAGAIN || error == EWOULDBLOCK || error == EINPROGRESS;
#endif
}

inline bool set_test_nonblocking(TestSocket socket) {
#ifdef _WIN32
    u_long mode = 1;
    return ::ioctlsocket(socket, FIONBIO, &mode) == 0;
#else
    const int flags = ::fcntl(socket, F_GETFL, 0);
    return flags >= 0 && ::fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

inline int wait_test_socket(TestSocket socket, short events, int timeout_ms) {
    if (timeout_ms < 0) {
        timeout_ms = 0;
    }
#ifdef _WIN32
    WSAPOLLFD poll_fd{};
    poll_fd.fd = socket;
    poll_fd.events = events;
    return ::WSAPoll(&poll_fd, 1, timeout_ms);
#else
    pollfd poll_fd{};
    poll_fd.fd = socket;
    poll_fd.events = events;
    return ::poll(&poll_fd, 1, timeout_ms);
#endif
}

class TcpStream {
public:
    TcpStream() = default;
    ~TcpStream() { close(); }

    TcpStream(const TcpStream&) = delete;
    TcpStream& operator=(const TcpStream&) = delete;

    TcpStream(TcpStream&& other) noexcept : fd_(other.fd_) { other.fd_ = kInvalidTestSocket; }
    TcpStream& operator=(TcpStream&& other) noexcept {
        if (this != &other) {
            close();
            fd_ = other.fd_;
            other.fd_ = kInvalidTestSocket;
        }
        return *this;
    }

    bool valid() const noexcept { return fd_ != kInvalidTestSocket; }

    void close() noexcept {
        close_test_socket(fd_);
        fd_ = kInvalidTestSocket;
    }

    void adopt(TestSocket fd) noexcept {
        close();
        fd_ = fd;
    }

    bool connect_within(const char* host, uint16_t port, int timeout_ms, bool& timed_out) {
        timed_out = false;
        close();
        if (!ensure_test_sockets()) {
            return false;
        }
        const TestSocket fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd == kInvalidTestSocket || !set_test_nonblocking(fd)) {
            close_test_socket(fd);
            return false;
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        if (::inet_pton(AF_INET, host, &address.sin_addr) != 1) {
            close_test_socket(fd);
            return false;
        }
        const int connected = ::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address));
        if (connected != 0 && !test_socket_would_block(test_socket_error())) {
            close_test_socket(fd);
            return false;
        }
        if (connected != 0) {
            const int ready = wait_test_socket(fd, POLLOUT, timeout_ms);
            if (ready == 0) {
                timed_out = true;
                close_test_socket(fd);
                return false;
            }
            if (ready < 0) {
                close_test_socket(fd);
                return false;
            }
            int so_error = 0;
            TestSocketLength length = sizeof(so_error);
            if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so_error), &length) != 0 ||
                so_error != 0) {
                close_test_socket(fd);
                return false;
            }
        }
        fd_ = fd;
        return true;
    }

    void connect(const char* host, uint16_t port, int timeout_ms = 2000) {
        bool timed_out = false;
        if (!connect_within(host, port, timeout_ms, timed_out)) {
            throw std::runtime_error(timed_out ? "timed out connecting to RPC server"
                                               : "failed to connect to RPC server");
        }
    }

    void write_all(const std::string& data, int timeout_ms = 2000) {
        if (!valid()) {
            throw std::runtime_error("failed to write HTTP request");
        }
        size_t sent_total = 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (sent_total < data.size()) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                throw std::runtime_error("timed out writing HTTP request");
            }
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
            const int ready = wait_test_socket(fd_, POLLOUT, static_cast<int>(remaining.count()));
            if (ready == 0) {
                throw std::runtime_error("timed out writing HTTP request");
            }
            if (ready < 0) {
                if (test_socket_interrupted(test_socket_error())) {
                    continue;
                }
                throw std::runtime_error("failed to write HTTP request");
            }
#ifdef _WIN32
            const int sent = ::send(fd_, data.data() + sent_total, static_cast<int>(data.size() - sent_total), 0);
            if (sent == SOCKET_ERROR) {
#else
            const ssize_t sent = ::send(fd_, data.data() + sent_total, data.size() - sent_total, MSG_NOSIGNAL);
            if (sent < 0) {
#endif
                if (test_socket_would_block(test_socket_error()) || test_socket_interrupted(test_socket_error())) {
                    continue;
                }
                throw std::runtime_error("failed to write HTTP request");
            }
            sent_total += static_cast<size_t>(sent);
        }
    }

    std::string read_until_close(int timeout_ms = 5000) {
        if (!valid()) {
            throw std::runtime_error("failed to read HTTP response");
        }
        std::string raw;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (true) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                throw std::runtime_error("timed out reading HTTP response");
            }
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
            const int ready = wait_test_socket(fd_, POLLIN, static_cast<int>(remaining.count()));
            if (ready == 0) {
                throw std::runtime_error("timed out reading HTTP response");
            }
            if (ready < 0) {
                if (test_socket_interrupted(test_socket_error())) {
                    continue;
                }
                throw std::runtime_error("failed to read HTTP response");
            }
            char buffer[4096];
#ifdef _WIN32
            const int received = ::recv(fd_, buffer, sizeof(buffer), 0);
            if (received == SOCKET_ERROR) {
#else
            const ssize_t received = ::recv(fd_, buffer, sizeof(buffer), 0);
            if (received < 0) {
#endif
                if (test_socket_would_block(test_socket_error()) || test_socket_interrupted(test_socket_error())) {
                    continue;
                }
                if (raw.empty()) {
                    throw std::runtime_error("failed to read HTTP response");
                }
                break;
            }
            if (received == 0) {
                break;
            }
            raw.append(buffer, static_cast<size_t>(received));
        }
        return raw;
    }

private:
    TestSocket fd_ = kInvalidTestSocket;
};

class BoundListener {
public:
    explicit BoundListener(int backlog = SOMAXCONN) {
        if (!ensure_test_sockets()) {
            throw std::runtime_error("socket subsystem is unavailable");
        }
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ == kInvalidTestSocket) {
            throw std::runtime_error("failed to open listen socket");
        }
        const int enabled = 1;
        ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&enabled), sizeof(enabled));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(0);
        if (::bind(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
            ::listen(fd_, backlog) != 0) {
            close();
            throw std::runtime_error("failed to bind listen socket");
        }
        sockaddr_in bound{};
        TestSocketLength length = sizeof(bound);
        if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&bound), &length) != 0) {
            close();
            throw std::runtime_error("failed to read listen port");
        }
        port_ = ntohs(bound.sin_port);
    }

    ~BoundListener() { close(); }

    BoundListener(const BoundListener&) = delete;
    BoundListener& operator=(const BoundListener&) = delete;

    uint16_t port() const noexcept { return port_; }

    bool accept_into(TcpStream& stream) {
        if (fd_ == kInvalidTestSocket) {
            return false;
        }
        const TestSocket client = ::accept(fd_, nullptr, nullptr);
        if (client == kInvalidTestSocket) {
            return false;
        }
        stream.adopt(client);
        return true;
    }

    void close() noexcept {
        close_test_socket(fd_);
        fd_ = kInvalidTestSocket;
    }

private:
    TestSocket fd_ = kInvalidTestSocket;
    uint16_t port_ = 0;
};

inline uint16_t bind_free_loopback_port() {
    BoundListener listener(1);
    return listener.port();
}

} // namespace geocoin::test
