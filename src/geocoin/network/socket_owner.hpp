#pragma once

#include "geocoin/network/peer_transport.hpp"
#include <utility>
#ifdef _WIN32
#include <winsock2.h>
#else
#include <unistd.h>
#endif

namespace geocoin::network::detail {

class SocketOwner {
public:
    explicit SocketOwner(SocketHandle socket = -1) noexcept : socket_(socket) {}
    ~SocketOwner() { reset(); }
    SocketOwner(const SocketOwner&) = delete;
    SocketOwner& operator=(const SocketOwner&) = delete;
    SocketOwner(SocketOwner&& other) noexcept : socket_(other.release()) {}
    SocketOwner& operator=(SocketOwner&& other) noexcept {
        if (this != &other) reset(other.release());
        return *this;
    }
    SocketHandle get() const noexcept { return socket_; }
    SocketHandle release() noexcept { return std::exchange(socket_, -1); }
    void reset(SocketHandle replacement = -1) noexcept {
        if (socket_ != -1) {
#ifdef _WIN32
            closesocket(static_cast<SOCKET>(socket_));
#else
            ::close(socket_);
#endif
        }
        socket_ = replacement;
    }
private:
    SocketHandle socket_;
};

} // namespace geocoin::network::detail
