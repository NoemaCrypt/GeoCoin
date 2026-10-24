#include "geocoin/network/peer_transport.hpp"
#include "geocoin/network/socket_owner.hpp"

#include "geocoin/network/network_utils.hpp"
#include "geocoin/utils/logger.hpp"

#include <algorithm>
#include <cerrno>
#include <limits>
#include <memory>
#include <utility>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace geocoin::network {

namespace {

#ifdef _WIN32
void ensure_socket_runtime_initialized() {
    static const bool initialized = []() {
        WSADATA data{};
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    (void)initialized;
}

void close_socket_portable(SocketHandle socket_fd) noexcept {
    if (socket_fd >= 0) {
        closesocket(static_cast<SOCKET>(socket_fd));
    }
}

int last_socket_error() noexcept {
    return WSAGetLastError();
}

bool is_connect_in_progress(int error) noexcept {
    return error == WSAEINPROGRESS || error == WSAEWOULDBLOCK;
}

bool is_would_block(int error) noexcept {
    return error == WSAEWOULDBLOCK;
}

bool is_interrupted(int error) noexcept {
    return error == WSAEINTR;
}

bool is_expected_connect_failure(int error) noexcept {
    return error == WSAETIMEDOUT || error == WSAECONNREFUSED || error == WSAEHOSTUNREACH;
}

bool set_non_blocking(SocketHandle socket_fd) noexcept {
    u_long mode = 1;
    return ioctlsocket(static_cast<SOCKET>(socket_fd), FIONBIO, &mode) == 0;
}

using socket_length_t = int;
#else
void ensure_socket_runtime_initialized() {}

void close_socket_portable(SocketHandle socket_fd) noexcept {
    if (socket_fd >= 0) {
        (void)::close(socket_fd);
    }
}

int last_socket_error() noexcept {
    return errno;
}

bool is_connect_in_progress(int error) noexcept {
    return error == EINPROGRESS;
}

bool is_would_block(int error) noexcept {
    return error == EAGAIN || error == EWOULDBLOCK;
}

bool is_interrupted(int error) noexcept {
    return error == EINTR;
}

bool is_expected_connect_failure(int error) noexcept {
    return error == ETIMEDOUT || error == ECONNREFUSED || error == EHOSTUNREACH;
}

bool set_non_blocking(SocketHandle socket_fd) noexcept {
    const int flags = fcntl(socket_fd, F_GETFL, 0);
    return flags >= 0 && fcntl(socket_fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

using socket_length_t = socklen_t;
#endif

size_t max_socket_io_size() noexcept {
#ifdef _WIN32
    return static_cast<size_t>((std::numeric_limits<int>::max)());
#else
    return static_cast<size_t>((std::numeric_limits<ssize_t>::max)());
#endif
}

int wait_writable(SocketHandle fd, std::chrono::milliseconds timeout) noexcept {
    if (timeout.count() <= 0) {
        timeout = std::chrono::milliseconds(1);
    }
    timeout = std::min(timeout, std::chrono::milliseconds((std::numeric_limits<int>::max)()));
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0) return 0;
#ifdef _WIN32
        fd_set write_fds, error_fds;
        FD_ZERO(&write_fds);
        FD_ZERO(&error_fds);
        FD_SET(static_cast<SOCKET>(fd), &write_fds);
        FD_SET(static_cast<SOCKET>(fd), &error_fds);
        timeval wait_time{};
        wait_time.tv_sec = static_cast<long>(remaining.count() / 1000);
        wait_time.tv_usec = static_cast<long>((remaining.count() % 1000) * 1000);
        const int result = ::select(0, nullptr, &write_fds, &error_fds, &wait_time);
#else
        // poll has no FD_SETSIZE restriction. Database files and other peers
        // can legitimately put a socket descriptor above select's bitset.
        pollfd descriptor{fd, POLLOUT, 0};
        const int result = ::poll(&descriptor, 1, static_cast<int>(remaining.count()));
#endif
        if (result >= 0 || !is_interrupted(last_socket_error())) return result;
    }
}

class TcpPeerTransport final : public PeerTransport {
public:
    TcpPeerTransport(std::string address, detail::SocketOwner connected_socket)
        : address_(std::move(address)), socket_(std::move(connected_socket)) {
        ensure_socket_runtime_initialized();
        if (socket_.get() >= 0 && !set_non_blocking(socket_.get())) {
            GEO_LOG_WARN("network", "Failed to set accepted peer socket non-blocking");
            close();
        }
    }

    ~TcpPeerTransport() override {
        close();
    }

    bool connect(std::chrono::milliseconds timeout) override {
        if (is_open()) {
            return true;
        }
        if (timeout.count() <= 0) {
            timeout = std::chrono::milliseconds(1);
        }

        std::string host;
        uint16_t port = 0;
        if (!NetworkUtils::parse_peer_address(address_, host, port, 3030)) {
            return false;
        }

        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;

        addrinfo* resolved_raw = nullptr;
        const std::string port_text = std::to_string(port);
        const int resolve_result = getaddrinfo(
            host.c_str(),
            port_text.c_str(),
            &hints,
            &resolved_raw
        );
        if (resolve_result != 0 || resolved_raw == nullptr) {
            GEO_LOG_WARN("network", "Address resolution failed for " << address_
                << " (error=" << resolve_result << ")");
            return false;
        }
        const std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> resolved(
            resolved_raw,
            &freeaddrinfo
        );

        int last_error = 0;
        for (const addrinfo* candidate = resolved.get(); candidate != nullptr;
             candidate = candidate->ai_next) {
            const SocketHandle fd = static_cast<SocketHandle>(::socket(
                candidate->ai_family,
                candidate->ai_socktype,
                candidate->ai_protocol
            ));
#ifdef _WIN32
            if (fd == static_cast<SocketHandle>(INVALID_SOCKET)) {
#else
            if (fd < 0) {
#endif
                last_error = last_socket_error();
                continue;
            }

            if (!set_non_blocking(fd)) {
                last_error = last_socket_error();
                close_socket_portable(fd);
                continue;
            }

            int result = ::connect(fd, candidate->ai_addr, candidate->ai_addrlen);
            if (result < 0) {
                last_error = last_socket_error();
                if (!is_connect_in_progress(last_error)) {
                    close_socket_portable(fd);
                    continue;
                }

                result = wait_writable(fd, timeout);
                if (result <= 0) {
#ifdef _WIN32
                    last_error = result == 0 ? WSAETIMEDOUT : last_socket_error();
#else
                    last_error = result == 0 ? ETIMEDOUT : last_socket_error();
#endif
                    close_socket_portable(fd);
                    continue;
                }

                int connect_error = 0;
                socket_length_t error_size = sizeof(connect_error);
#ifdef _WIN32
                const int option_result = getsockopt(
                    fd,
                    SOL_SOCKET,
                    SO_ERROR,
                    reinterpret_cast<char*>(&connect_error),
                    &error_size
                );
#else
                const int option_result = getsockopt(
                    fd,
                    SOL_SOCKET,
                    SO_ERROR,
                    &connect_error,
                    &error_size
                );
#endif
                if (option_result < 0 || connect_error != 0) {
                    last_error = connect_error != 0 ? connect_error : last_socket_error();
                    close_socket_portable(fd);
                    continue;
                }
            }

            socket_.reset(fd);
            return true;
        }

        if (is_expected_connect_failure(last_error)) {
            GEO_LOG_DEBUG("network", "Connect failed for " << address_
                << " (error=" << last_error << ")");
        } else {
            GEO_LOG_WARN("network", "Connect failed for " << address_
                << " (error=" << last_error << ")");
        }
        return false;
    }

    void close() noexcept override {
        socket_.reset();
    }

    bool is_open() const noexcept override {
        return socket_.get() >= 0;
    }

    std::ptrdiff_t send(const bytes& data) override {
        if (!is_open()) {
            return -1;
        }

        size_t total_sent = 0;
        int retries = 0;
        constexpr int kMaxRetries = 4;
        while (total_sent < data.size()) {
            const size_t chunk_size = std::min(data.size() - total_sent, max_socket_io_size());
#ifdef _WIN32
            const int sent = ::send(
                socket_.get(),
                reinterpret_cast<const char*>(data.data() + total_sent),
                static_cast<int>(chunk_size),
                0
            );
#else
            int flags = 0;
#ifdef MSG_NOSIGNAL
            flags |= MSG_NOSIGNAL;
#endif
            const ssize_t sent = ::send(
                socket_.get(),
                data.data() + total_sent,
                chunk_size,
                flags
            );
#endif
            if (sent > 0) {
                total_sent += static_cast<size_t>(sent);
                continue;
            }
            if (sent == 0) {
                break;
            }

            const int error = last_socket_error();
            if (is_interrupted(error)) {
                continue;
            }
            if (is_would_block(error)) {
                const int ready = wait_writable(socket_.get(), std::chrono::milliseconds(250));
                if (ready > 0 && ++retries <= kMaxRetries) {
                    continue;
                }
            }

            GEO_LOG_WARN("network", "Peer transport send failed (error=" << error << ")");
            break;
        }

        if (total_sent != data.size()) {
            GEO_LOG_WARN("network", "Peer transport send incomplete: " << total_sent
                << " of " << data.size() << " bytes");
            close();
        }
        const auto max_result = static_cast<size_t>(
            (std::numeric_limits<std::ptrdiff_t>::max)()
        );
        return static_cast<std::ptrdiff_t>(std::min(total_sent, max_result));
    }

    std::ptrdiff_t receive(bytes& buffer, size_t max_size) override {
        if (!is_open()) {
            return -1;
        }

        const size_t read_size = std::min(max_size, max_socket_io_size());
        if (read_size == 0) {
            buffer.clear();
            return 0;
        }

        buffer.resize(read_size);
#ifdef _WIN32
        const int received = ::recv(
            socket_.get(),
            reinterpret_cast<char*>(buffer.data()),
            static_cast<int>(read_size),
            0
        );
#else
        const ssize_t received = ::recv(
            socket_.get(),
            buffer.data(),
            read_size,
            MSG_DONTWAIT
        );
#endif
        if (received > 0) {
            buffer.resize(static_cast<size_t>(received));
            return static_cast<std::ptrdiff_t>(received);
        }

        buffer.clear();
        if (received == 0) {
            close();
            return 0;
        }

        const int error = last_socket_error();
        if (is_would_block(error) || is_interrupted(error)) {
            return 0;
        }

        GEO_LOG_WARN("network", "Peer transport receive failed (error=" << error << ")");
        close();
        return -1;
    }

private:
    std::string address_;
    detail::SocketOwner socket_;
};

} // namespace

std::unique_ptr<PeerTransport> make_tcp_peer_transport(
    const std::string& address,
    SocketHandle connected_socket
) {
    detail::SocketOwner socket(connected_socket);
    return std::make_unique<TcpPeerTransport>(address, std::move(socket));
}

} // namespace geocoin::network
