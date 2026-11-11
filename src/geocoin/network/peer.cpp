#include "geocoin/network/peer.hpp"
#include "geocoin/network/messages.hpp"

#include <limits>
#include <stdexcept>
#include <utility>

namespace geocoin::network {

namespace {

uint64_t saturating_add_u64(uint64_t current, uint64_t delta) {
    const uint64_t max_value = (std::numeric_limits<uint64_t>::max)();
    return current > max_value - delta ? max_value : current + delta;
}

uint64_t size_to_u64_saturated(size_t value) {
    const uint64_t max_value = (std::numeric_limits<uint64_t>::max)();
    return value > static_cast<size_t>(max_value) ? max_value : static_cast<uint64_t>(value);
}

ssize_t size_to_ssize_saturated(size_t value) {
    const ssize_t max_value = (std::numeric_limits<ssize_t>::max)();
    return value > static_cast<size_t>(max_value) ? max_value : static_cast<ssize_t>(value);
}

} // namespace

Peer::Peer(const std::string& address, SocketHandle socket_fd)
    : Peer(address, make_tcp_peer_transport(address, socket_fd)) {}

Peer::Peer(const std::string& address, std::unique_ptr<PeerTransport> transport)
    : transport_(std::move(transport)) {
    if (!transport_) {
        throw std::invalid_argument("Peer transport must not be null");
    }

    info_.address = address;
    info_.version = kSecureWireVersion;
    info_.user_agent = "degree/0.1.0";

    if (transport_->is_open()) {
        state_ = PeerState::CONNECTED;
        info_.connected_since = std::chrono::steady_clock::now();
        info_.last_seen = info_.connected_since;
    }
}

Peer::~Peer() {
    disconnect();
}

bool Peer::connect() {
    return connect(std::chrono::milliseconds(5000));
}

bool Peer::connect(std::chrono::milliseconds timeout) {
    if (timeout.count() <= 0) {
        timeout = std::chrono::milliseconds(1);
    }

    std::lock_guard<std::mutex> transport_lock(transport_mutex_);
    if (transport_->is_open() && state_ != PeerState::DISCONNECTED) {
        return true;
    }
    secure_channel_.reset();
    {
        std::lock_guard<std::mutex> info_lock(info_mutex_);
        info_.transport_encrypted = false;
        info_.secure_wire_version = 0;
        info_.secure_suite = 0;
    }

    state_ = PeerState::CONNECTING;
    if (!transport_->connect(timeout) || !transport_->is_open()) {
        transport_->close();
        state_ = PeerState::DISCONNECTED;
        return false;
    }

    state_ = PeerState::CONNECTED;
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> info_lock(info_mutex_);
        info_.connected_since = now;
        info_.last_seen = now;
    }
    return true;
}

void Peer::disconnect() {
    std::lock_guard<std::mutex> transport_lock(transport_mutex_);
    transport_->close();
    secure_channel_.reset();
    state_ = PeerState::DISCONNECTED;
    {
        std::lock_guard<std::mutex> info_lock(info_mutex_);
        info_.transport_encrypted = false;
        info_.secure_wire_version = 0;
        info_.secure_suite = 0;
    }
}

bool Peer::is_connected() const {
    std::lock_guard<std::mutex> transport_lock(transport_mutex_);
    return transport_->is_open() && state_ != PeerState::DISCONNECTED;
}

ssize_t Peer::send(const bytes& data) {
    std::lock_guard<std::mutex> transport_lock(transport_mutex_);
    if (!transport_->is_open() || state_ == PeerState::DISCONNECTED) {
        return -1;
    }

    MessageHeader header;
    bytes payload;
    if (!parse_message(data, header, payload)) {
        transport_->close();
        state_ = PeerState::DISCONNECTED;
        return -1;
    }
    const bool open_handshake =
        header.type == MessageType::SECURE_HELLO ||
        header.type == MessageType::SECURE_REPLY;
    if (open_handshake == (secure_channel_ != nullptr)) {
        // Exactly HELLO/REPLY may cross the transport before activation; they
        // can never appear inside an established encrypted channel.
        transport_->close();
        state_ = PeerState::DISCONNECTED;
        return -1;
    }
    try {
        if (header.type == MessageType::SECURE_HELLO) {
            (void)SecureHelloMessage::deserialize(payload);
        } else if (header.type == MessageType::SECURE_REPLY) {
            (void)SecureReplyMessage::deserialize(payload);
        }
    } catch (...) {
        transport_->close();
        state_ = PeerState::DISCONNECTED;
        return -1;
    }

    bytes protected_record;
    const bytes* wire_data = &data;
    if (secure_channel_) {
        if (!secure_channel_->protect(data, protected_record)) {
            transport_->close();
            state_ = PeerState::DISCONNECTED;
            return -1;
        }
        wire_data = &protected_record;
    }

    const std::ptrdiff_t sent = transport_->send(*wire_data);
    const size_t sent_size = sent > 0 ? static_cast<size_t>(sent) : 0;

    if (sent_size > 0) {
        std::lock_guard<std::mutex> info_lock(info_mutex_);
        info_.bytes_sent = saturating_add_u64(info_.bytes_sent, size_to_u64_saturated(sent_size));
        info_.last_seen = std::chrono::steady_clock::now();
    }

    if (sent < 0 || sent_size != wire_data->size()) {
        transport_->close();
        state_ = PeerState::DISCONNECTED;
    }

    if (sent < 0 || sent_size != wire_data->size()) {
        return -1;
    }
    return size_to_ssize_saturated(data.size());
}

ssize_t Peer::receive(bytes& buffer, size_t max_size) {
    std::lock_guard<std::mutex> transport_lock(transport_mutex_);
    if (!transport_->is_open() || state_ == PeerState::DISCONNECTED) {
        return -1;
    }

    const std::ptrdiff_t received = transport_->receive(buffer, max_size);
    if (received > 0) {
        const size_t received_size = static_cast<size_t>(received);
        if (received_size > max_size || received_size > buffer.size()) {
            buffer.clear();
            transport_->close();
            state_ = PeerState::DISCONNECTED;
            return -1;
        }

        std::lock_guard<std::mutex> info_lock(info_mutex_);
        info_.bytes_received = saturating_add_u64(
            info_.bytes_received,
            size_to_u64_saturated(received_size)
        );
        info_.last_seen = std::chrono::steady_clock::now();
        return size_to_ssize_saturated(received_size);
    }

    if (received < 0 || !transport_->is_open()) {
        buffer.clear();
        transport_->close();
        state_ = PeerState::DISCONNECTED;
    }
    return received < 0 ? -1 : 0;
}

bool Peer::activate_secure_channel(SecureChannelKeyMaterial key_material) {
    std::lock_guard<std::mutex> transport_lock(transport_mutex_);
    if (secure_channel_ || !transport_->is_open() || state_ == PeerState::DISCONNECTED) {
        return false;
    }
    secure_channel_ = std::make_unique<SecureChannel>(std::move(key_material));
    {
        std::lock_guard<std::mutex> info_lock(info_mutex_);
        info_.transport_encrypted = true;
        info_.secure_wire_version = kSecureWireVersion;
        info_.secure_suite = kSecureSuiteHybridMlKem768X25519ChaCha20Poly1305;
    }
    return true;
}

bool Peer::is_secure() const {
    std::lock_guard<std::mutex> transport_lock(transport_mutex_);
    return secure_channel_ != nullptr;
}

bool Peer::decrypt_secure_record(const bytes& record, bytes& plaintext_frame) {
    std::lock_guard<std::mutex> transport_lock(transport_mutex_);
    if (!secure_channel_ || !transport_->is_open() || state_ == PeerState::DISCONNECTED) {
        plaintext_frame.clear();
        return false;
    }
    return secure_channel_->unprotect(record, plaintext_frame);
}

Hash256 Peer::secure_channel_binding() const {
    std::lock_guard<std::mutex> transport_lock(transport_mutex_);
    return secure_channel_ ? secure_channel_->channel_binding() : Hash256{};
}

PeerState Peer::get_state() const {
    std::lock_guard<std::mutex> transport_lock(transport_mutex_);
    return state_;
}

void Peer::set_state(PeerState state) {
    std::lock_guard<std::mutex> transport_lock(transport_mutex_);
    state_ = state;
}

PeerInfo Peer::get_info() const {
    std::lock_guard<std::mutex> info_lock(info_mutex_);
    return info_;
}

void Peer::update_info(const PeerInfo& info) {
    std::lock_guard<std::mutex> transport_lock(transport_mutex_);
    std::lock_guard<std::mutex> info_lock(info_mutex_);
    info_ = info;
    info_.transport_encrypted = secure_channel_ != nullptr;
    info_.secure_wire_version = secure_channel_ ? kSecureWireVersion : 0;
    info_.secure_suite = secure_channel_
        ? kSecureSuiteHybridMlKem768X25519ChaCha20Poly1305
        : 0;
}

std::string Peer::get_address() const {
    std::lock_guard<std::mutex> info_lock(info_mutex_);
    return info_.address;
}

void Peer::update_last_seen() {
    std::lock_guard<std::mutex> info_lock(info_mutex_);
    info_.last_seen = std::chrono::steady_clock::now();
}

bool Peer::is_timeout(int timeout_seconds) const {
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> info_lock(info_mutex_);
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        now - info_.last_seen
    ).count();
    return elapsed > timeout_seconds;
}

void Peer::add_known_inventory(const Hash256& hash) {
    std::lock_guard<std::mutex> lock(known_inventory_mutex_);
    known_inventory_.insert(hash);

    constexpr size_t kMaxKnownInventory = 50000;
    if (known_inventory_.size() > kMaxKnownInventory) {
        known_inventory_.erase(known_inventory_.begin());
    }
}

bool Peer::has_known_inventory(const Hash256& hash) const {
    std::lock_guard<std::mutex> lock(known_inventory_mutex_);
    return known_inventory_.find(hash) != known_inventory_.end();
}

std::vector<Hash256> Peer::filter_unknown_inventory(const std::vector<Hash256>& hashes) const {
    std::vector<Hash256> result;
    result.reserve(hashes.size());
    std::lock_guard<std::mutex> lock(known_inventory_mutex_);
    for (const auto& hash : hashes) {
        if (known_inventory_.find(hash) == known_inventory_.end()) {
            result.push_back(hash);
        }
    }
    return result;
}

} // namespace geocoin::network
