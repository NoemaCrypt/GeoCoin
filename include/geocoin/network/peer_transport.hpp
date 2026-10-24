#pragma once

#include "geocoin/types.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace geocoin::network {

#ifdef _WIN32
using SocketHandle = std::intptr_t;
#else
using SocketHandle = int;
#endif

/**
 * Byte-stream transport owned by a Peer session.
 *
 * Message parsing, authentication and peer policy intentionally live above
 * this interface. A future authenticated-encryption transport can wrap this
 * stream without giving socket ownership to PeerManager.
 */
class PeerTransport {
public:
    virtual ~PeerTransport() = default;

    virtual bool connect(std::chrono::milliseconds timeout) = 0;
    virtual void close() noexcept = 0;
    virtual bool is_open() const noexcept = 0;

    // send() returns bytes written, or -1 when no stream is available. A
    // partial write is terminal: the transport must close the stream because
    // retrying a framed P2P message above this boundary would duplicate data.
    virtual std::ptrdiff_t send(const bytes& data) = 0;

    // receive() returns 0 both for "no data yet" and orderly EOF. is_open()
    // distinguishes those cases. A negative result is a terminal I/O error.
    virtual std::ptrdiff_t receive(bytes& buffer, size_t max_size) = 0;
};

std::unique_ptr<PeerTransport> make_tcp_peer_transport(
    const std::string& address,
    SocketHandle connected_socket = -1
);

} // namespace geocoin::network
