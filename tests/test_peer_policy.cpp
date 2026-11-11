#include "tcp_test_client.hpp"
#include "geocoin/network/peer_manager.hpp"
#include "geocoin/network/peer_manager_internal.hpp"
#include "geocoin/crypto/geo_area.hpp"
#include "geocoin/network/p2p_server.hpp"
#include "geocoin/network/secure_handshake.hpp"
#include "geocoin/network/services.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace geocoin::network {

struct PeerManagerTestAccess {
    static std::unique_ptr<SecureChannel> activate_test_secure_channel(
        PeerManager& manager,
        const std::string& address
    ) {
        std::lock_guard<std::mutex> lock(manager.mutex_);
        auto it = manager.peers_.find(address);
        if (it == manager.peers_.end() || !it->second.peer) {
            return nullptr;
        }

        const Hash256 binding = crypto::sha256(bytes(address.begin(), address.end()));
        SecureChannelKeyMaterial client_keys;
        SecureChannelKeyMaterial manager_keys;
        std::fill(client_keys.send_key.begin(), client_keys.send_key.end(), 0x41);
        std::fill(client_keys.receive_key.begin(), client_keys.receive_key.end(), 0x42);
        std::fill(manager_keys.send_key.begin(), manager_keys.send_key.end(), 0x42);
        std::fill(manager_keys.receive_key.begin(), manager_keys.receive_key.end(), 0x41);
        std::fill(
            client_keys.send_nonce_prefix.begin(), client_keys.send_nonce_prefix.end(), 0x43);
        std::fill(
            client_keys.receive_nonce_prefix.begin(), client_keys.receive_nonce_prefix.end(), 0x44);
        std::fill(
            manager_keys.send_nonce_prefix.begin(), manager_keys.send_nonce_prefix.end(), 0x44);
        std::fill(
            manager_keys.receive_nonce_prefix.begin(), manager_keys.receive_nonce_prefix.end(), 0x43);
        client_keys.channel_binding = binding;
        manager_keys.channel_binding = binding;

        auto client = std::make_unique<SecureChannel>(std::move(client_keys));
        if (!it->second.peer->activate_secure_channel(std::move(manager_keys))) {
            return nullptr;
        }
        it->second.channel_binding = binding;
        it->second.secure_established = true;
        return client;
    }

    static bool age_connected_context(PeerManager& manager,
                                      const std::string& address,
                                      std::chrono::seconds age) {
        std::lock_guard<std::mutex> lock(manager.mutex_);
        auto it = manager.peers_.find(address);
        if (it == manager.peers_.end()) {
            return false;
        }
        const auto then = std::chrono::steady_clock::now() - age;
        it->second.connected_at = then;
        it->second.handshake_started_at = then;
        it->second.last_message_at = then;
        it->second.last_useful_message_at = then;
        return true;
    }

    static bool start_test_auth_session(PeerManager& manager,
                                        const std::string& address,
                                        const bytes& private_key) {
        std::lock_guard<std::mutex> lock(manager.mutex_);
        auto it = manager.peers_.find(address);
        if (it == manager.peers_.end() || !it->second.peer) {
            return false;
        }
        const Hash256 binding = crypto::sha256(bytes(address.begin(), address.end()));
        it->second.channel_binding = binding;
        if (!manager.authenticator_.start_as_prover(
                private_key, binding, it->second.auth_session)) {
            return false;
        }
        const auto now = std::chrono::steady_clock::now();
        it->second.peer->set_state(PeerState::AUTHENTICATING);
        it->second.last_message_at = now;
        it->second.last_useful_message_at = now;
        it->second.last_score_decay_at = now;
        return true;
    }

    static bool age_auth_last_activity(PeerManager& manager,
                                       const std::string& address,
                                       std::chrono::seconds age) {
        std::lock_guard<std::mutex> lock(manager.mutex_);
        auto it = manager.peers_.find(address);
        if (it == manager.peers_.end() || !it->second.auth_session) {
            return false;
        }
        it->second.auth_session->last_activity = std::chrono::steady_clock::now() - age;
        it->second.last_useful_message_at = std::chrono::steady_clock::now();
        return true;
    }

    static bool age_auth_progress(PeerManager& manager,
                                  const std::string& address,
                                  std::chrono::seconds age) {
        std::lock_guard<std::mutex> lock(manager.mutex_);
        auto it = manager.peers_.find(address);
        if (it == manager.peers_.end() || !it->second.auth_session) {
            return false;
        }
        const auto now = std::chrono::steady_clock::now();
        it->second.auth_session->last_activity = now;
        it->second.last_useful_message_at = now - age;
        return true;
    }

    static bool force_ready_idle(PeerManager& manager,
                                 const std::string& address,
                                 std::chrono::seconds age) {
        std::lock_guard<std::mutex> lock(manager.mutex_);
        auto it = manager.peers_.find(address);
        if (it == manager.peers_.end() || !it->second.peer) {
            return false;
        }
        it->second.peer->set_state(PeerState::READY);
        auto info = it->second.peer->get_info();
        info.last_seen = std::chrono::steady_clock::now() - age;
        it->second.peer->update_info(info);
        it->second.last_ping = std::chrono::steady_clock::now();
        return true;
    }

    static bool force_ready_waiting_for_pong(PeerManager& manager,
                                             const std::string& address,
                                             std::chrono::seconds age) {
        std::lock_guard<std::mutex> lock(manager.mutex_);
        auto it = manager.peers_.find(address);
        if (it == manager.peers_.end() || !it->second.peer) {
            return false;
        }
        const auto now = std::chrono::steady_clock::now();
        const auto sent_at = now - age;
        it->second.peer->set_state(PeerState::READY);
        auto info = it->second.peer->get_info();
        info.last_seen = now;
        it->second.peer->update_info(info);
        it->second.last_ping = now;
        it->second.last_ping_sent_at = sent_at;
        it->second.last_pong_at = sent_at - std::chrono::seconds(1);
        return true;
    }

    static bool set_policy_score_for_decay(PeerManager& manager,
                                           const std::string& address,
                                           uint32_t score,
                                           std::chrono::seconds last_decay_age) {
        std::lock_guard<std::mutex> lock(manager.mutex_);
        auto it = manager.peers_.find(address);
        if (it == manager.peers_.end()) {
            return false;
        }
        it->second.policy_score = score;
        it->second.last_score_decay_at = std::chrono::steady_clock::now() - last_decay_age;
        return true;
    }

    static bool handle_reject_payload(PeerManager& manager,
                                      const std::string& address,
                                      const bytes& payload) {
        std::lock_guard<std::mutex> lock(manager.mutex_);
        auto it = manager.peers_.find(address);
        if (it == manager.peers_.end()) {
            return false;
        }
        manager.handle_reject(it->second, payload);
        return true;
    }

    static void seed_outbound_attempt(PeerManager& manager,
                                      const std::string& address,
                                      bool manual,
                                      uint32_t fail_count,
                                      std::chrono::seconds age) {
        std::lock_guard<std::mutex> lock(manager.mutex_);
        PeerManager::OutboundAttemptState state;
        const auto then = std::chrono::steady_clock::now() - age;
        state.fail_count = fail_count;
        state.last_attempt = then;
        state.next_attempt = then;
        state.manual = manual;
        state.last_error = manual ? "manual-seeded" : "seeded";
        manager.outbound_attempts_[address] = std::move(state);
    }

    static void seed_outbound_backoff(PeerManager& manager,
                                      const std::string& address,
                                      uint32_t fail_count,
                                      std::chrono::seconds remaining) {
        std::lock_guard<std::mutex> lock(manager.mutex_);
        PeerManager::OutboundAttemptState state;
        const auto now = std::chrono::steady_clock::now();
        state.fail_count = fail_count;
        state.last_attempt = now;
        state.next_attempt = now + remaining;
        state.manual = true;
        state.in_progress = false;
        state.last_error = "tcp-connect-failed";
        manager.outbound_attempts_[address] = std::move(state);
    }

    static void prune_outbound_attempts(PeerManager& manager) {
        std::lock_guard<std::mutex> lock(manager.mutex_);
        manager.prune_outbound_attempts_unlocked();
    }

    static size_t outbound_attempt_count(PeerManager& manager) {
        std::lock_guard<std::mutex> lock(manager.mutex_);
        return manager.outbound_attempts_.size();
    }

    static bool has_outbound_attempt(PeerManager& manager, const std::string& address) {
        std::lock_guard<std::mutex> lock(manager.mutex_);
        return manager.outbound_attempts_.find(address) != manager.outbound_attempts_.end();
    }

    static void begin_outbound_attempt(PeerManager& manager,
                                       const std::string& address,
                                       bool manual) {
        std::lock_guard<std::mutex> lock(manager.mutex_);
        manager.record_outbound_attempt_unlocked(address, manual);
    }

    static void finish_outbound_attempt(PeerManager& manager,
                                        const std::string& address,
                                        bool success,
                                        bool manual) {
        std::lock_guard<std::mutex> lock(manager.mutex_);
        manager.record_outbound_result_unlocked(
            address,
            success,
            manual,
            success ? "connected" : "test-connect-failed"
        );
    }

    static void add_temp_ban(PeerManager& manager, const std::string& ip) {
        std::lock_guard<std::mutex> lock(manager.mutex_);
        manager.add_temp_ban_unlocked(ip);
    }

    static size_t temp_ban_count(PeerManager& manager) {
        std::lock_guard<std::mutex> lock(manager.mutex_);
        return manager.temp_banned_ips_.size();
    }

    static bool is_banned(PeerManager& manager, const std::string& endpoint) {
        std::lock_guard<std::mutex> lock(manager.mutex_);
        return manager.is_peer_banned_unlocked(endpoint);
    }

    static bool set_peer_counters(PeerManager& manager,
                                  const std::string& address,
                                  uint64_t bytes_sent,
                                  uint64_t bytes_received) {
        std::lock_guard<std::mutex> lock(manager.mutex_);
        auto it = manager.peers_.find(address);
        if (it == manager.peers_.end() || !it->second.peer) {
            return false;
        }
        auto info = it->second.peer->get_info();
        info.bytes_sent = bytes_sent;
        info.bytes_received = bytes_received;
        it->second.peer->update_info(info);
        return true;
    }

    static uint64_t received_message_count(PeerManager& manager,
                                           const std::string& address,
                                           MessageType type) {
        std::lock_guard<std::mutex> lock(manager.mutex_);
        const auto it = manager.peers_.find(address);
        if (it == manager.peers_.end()) {
            return 0;
        }
        return it->second.recv_message_counts[static_cast<size_t>(type)];
    }

    static bool mark_ping_sent(PeerManager& manager,
                               const std::string& address,
                               std::chrono::milliseconds age) {
        std::lock_guard<std::mutex> lock(manager.mutex_);
        auto it = manager.peers_.find(address);
        if (it == manager.peers_.end() || !it->second.peer) {
            return false;
        }
        const auto sent_at = std::chrono::steady_clock::now() - age;
        it->second.peer->set_state(PeerState::READY);
        it->second.last_ping_sent_at = sent_at;
        it->second.last_pong_at = sent_at - std::chrono::milliseconds(1);
        return true;
    }

    static bool age_last_ping(PeerManager& manager,
                              const std::string& address,
                              std::chrono::seconds age) {
        std::lock_guard<std::mutex> lock(manager.mutex_);
        auto it = manager.peers_.find(address);
        if (it == manager.peers_.end() || !it->second.peer) {
            return false;
        }
        it->second.peer->set_state(PeerState::READY);
        auto info = it->second.peer->get_info();
        info.last_seen = std::chrono::steady_clock::now();
        it->second.peer->update_info(info);
        it->second.last_ping = std::chrono::steady_clock::now() - age;
        return true;
    }

    static void insert_alias_peer(PeerManager& manager,
                                  const std::string& map_key,
                                  std::shared_ptr<Peer> peer,
                                  bool inbound) {
        std::lock_guard<std::mutex> lock(manager.mutex_);
        PeerManager::PeerContext ctx;
        ctx.peer = std::move(peer);
        ctx.is_inbound = inbound;
        const auto now = std::chrono::steady_clock::now();
        ctx.connected_at = now;
        ctx.handshake_started_at = now;
        ctx.last_message_at = now;
        ctx.last_useful_message_at = now;
        ctx.last_ping = now;
        ctx.last_score_decay_at = now;
        manager.peers_[map_key] = std::move(ctx);
        manager.refresh_peer_metrics_unlocked();
    }
};

} // namespace geocoin::network

namespace {

int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

geocoin::bytes protect_test_frame(
    geocoin::network::SecureChannel& channel,
    geocoin::network::MessageType type,
    const geocoin::bytes& payload
) {
    const geocoin::bytes frame = geocoin::network::create_message(type, payload);
    geocoin::bytes record;
    check(channel.protect(frame, record), "test client encrypts P2P frame");
    return record;
}

geocoin::bytes make_key(uint8_t seed) {
    geocoin::bytes key(32);
    for (size_t i = 0; i < key.size(); ++i) {
        key[i] = static_cast<geocoin::byte>(seed + i);
    }
    return key;
}

geocoin::Address make_address(uint8_t seed) {
    geocoin::Address address;
    check(geocoin::crypto::compute_address_from_private_key(make_key(seed), address),
          "private key derives a valid GEO address");
    return address;
}

geocoin::Hash256 hash_from_counter(uint32_t value) {
    geocoin::Hash256 hash{};
    hash[0] = static_cast<geocoin::byte>((value >> 24) & 0xffU);
    hash[1] = static_cast<geocoin::byte>((value >> 16) & 0xffU);
    hash[2] = static_cast<geocoin::byte>((value >> 8) & 0xffU);
    hash[3] = static_cast<geocoin::byte>(value & 0xffU);
    for (size_t i = 4; i < hash.size(); ++i) {
        hash[i] = static_cast<geocoin::byte>((value + i * 13U) & 0xffU);
    }
    return hash;
}

struct LocalSocketPair {
    int first = -1;
    int second = -1;

    LocalSocketPair() {
        int fds[2] = {-1, -1};
        if (geocoin::test::make_test_socket_pair(fds)) {
            first = fds[0];
            second = fds[1];
        }
    }

    ~LocalSocketPair() {
        if (first >= 0) {
            geocoin::test::close_test_socket(first);
        }
        if (second >= 0) {
            geocoin::test::close_test_socket(second);
        }
    }

    int release_first() {
        const int fd = first;
        first = -1;
        return fd;
    }

    bool send_second(const geocoin::bytes& data) const {
        if (second < 0) {
            return false;
        }
        return geocoin::test::send_test_fd(second, data.data(), data.size(), 0) == static_cast<ssize_t>(data.size());
    }

    bool recv_second(geocoin::bytes& out, size_t max_size = 4096) const {
        if (second < 0 || max_size == 0) {
            out.clear();
            return false;
        }
        out.resize(max_size);
        const ssize_t received = geocoin::test::receive_test_fd(second, out.data(), out.size(), geocoin::test::kTestDontWait);
        if (received <= 0) {
            out.clear();
            return false;
        }
        out.resize(static_cast<size_t>(received));
        return true;
    }

    explicit operator bool() const {
        return first >= 0 && second >= 0;
    }
};

struct LocalTcpListener {
    int fd = -1;
    uint16_t port = 0;

    LocalTcpListener() {
        fd = geocoin::test::create_test_tcp_socket();
        if (fd < 0) {
            return;
        }

        const int one = 1;
        (void)::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof(one));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;

        if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
            ::listen(fd, 4) != 0) {
            close_fd();
            return;
        }

        sockaddr_in bound{};
        geocoin::test::TestSocketLength len = sizeof(bound);
        if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &len) != 0) {
            close_fd();
            return;
        }
        port = ntohs(bound.sin_port);
    }

    ~LocalTcpListener() {
        close_fd();
    }

    void close_fd() {
        if (fd >= 0) {
            geocoin::test::close_test_socket(fd);
            fd = -1;
        }
    }

    std::string address() const {
        return "127.0.0.1:" + std::to_string(port);
    }

    int accept_one(std::string& remote_address) const {
        if (fd < 0) {
            return -1;
        }
        sockaddr_in remote{};
        geocoin::test::TestSocketLength len = sizeof(remote);
        const int accepted = geocoin::test::test_socket_fd(
            ::accept(fd, reinterpret_cast<sockaddr*>(&remote), &len));
        if (accepted < 0) {
            return -1;
        }

        char ip[INET_ADDRSTRLEN] = {};
        const char* converted = ::inet_ntop(AF_INET, &remote.sin_addr, ip, sizeof(ip));
        remote_address = std::string(converted ? converted : "127.0.0.1") +
            ":" + std::to_string(ntohs(remote.sin_port));
        return accepted;
    }

    explicit operator bool() const {
        return fd >= 0 && port != 0;
    }
};

uint16_t reserve_free_loopback_port() {
    const int fd = geocoin::test::create_test_tcp_socket();
    if (fd < 0) {
        return 0;
    }

    const int one = 1;
    (void)::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        geocoin::test::close_test_socket(fd);
        return 0;
    }

    sockaddr_in bound{};
    geocoin::test::TestSocketLength len = sizeof(bound);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &len) != 0) {
        geocoin::test::close_test_socket(fd);
        return 0;
    }
    const uint16_t port = ntohs(bound.sin_port);
    geocoin::test::close_test_socket(fd);
    return port;
}

int connect_loopback_tcp(uint16_t port) {
    const int fd = geocoin::test::create_test_tcp_socket();
    if (fd < 0) {
        return -1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        geocoin::test::close_test_socket(fd);
        return -1;
    }
    return fd;
}

void close_fd_if_open(int& fd) {
    if (fd >= 0) {
        geocoin::test::close_test_socket(fd);
        fd = -1;
    }
}

void pump_managers(
    geocoin::network::PeerManager& first,
    geocoin::network::PeerManager& second,
    int rounds = 1
) {
    for (int i = 0; i < rounds; ++i) {
        first.update();
        second.update();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

bool pump_until_authenticated(
    geocoin::network::PeerManager& first,
    geocoin::network::PeerManager& second
) {
    for (int i = 0; i < 200; ++i) {
        pump_managers(first, second);
        if (first.get_stats().authenticated_peers == 1 &&
            second.get_stats().authenticated_peers == 1) {
            return true;
        }
    }
    return false;
}

bool connect_authenticated_pair(
    geocoin::network::PeerManager& outbound,
    geocoin::network::PeerManager& inbound,
    std::string& outbound_address,
    std::string& inbound_address
) {
    const geocoin::bytes outbound_key = make_key(0xa1);
    const geocoin::bytes inbound_key = make_key(0xb1);

    outbound.set_private_key(outbound_key);
    inbound.set_private_key(inbound_key);
    outbound.set_get_height_callback([] { return static_cast<geocoin::Height>(111); });
    inbound.set_get_height_callback([] { return static_cast<geocoin::Height>(222); });

    LocalTcpListener listener;
    if (!listener) {
        check(false, "loopback TCP listener is available for PeerManager handshake test");
        return false;
    }

    outbound_address = listener.address();
    if (!outbound.add_outbound_peer(outbound_address, outbound_key, true)) {
        check(false, "outbound PeerManager connects to local listener");
        return false;
    }

    const int accepted = listener.accept_one(inbound_address);
    if (accepted < 0 || inbound_address.empty()) {
        check(false, "local listener accepts outbound PeerManager connection");
        return false;
    }

    if (!inbound.add_inbound_peer(accepted, inbound_address, inbound_key)) {
        check(false, "inbound PeerManager accepts local connection");
        return false;
    }

    return true;
}

geocoin::network::VersionMessage valid_version_message(uint8_t key_seed) {
    geocoin::network::VersionMessage version;
    version.version = geocoin::network::kP2PProtocolVersion;
    version.services = geocoin::network::build_local_services(false);
    version.timestamp = 123456789;
    version.user_agent = "degree/test";
    version.height = 12;
    version.auth_address = make_address(key_seed);
    return version;
}

geocoin::bytes valid_plaintext_hello_frame() {
    geocoin::network::SecureHelloMessage hello;
    hello.hello.x25519_public_key.assign(32, 0x11);
    hello.hello.nonce.assign(32, 0x22);
    hello.hello.ml_kem_public_key.assign(
        geocoin::crypto::ml_kem768::kPublicKeySize, 0x33);
    return geocoin::network::create_message(
        geocoin::network::MessageType::SECURE_HELLO,
        hello.serialize());
}

// Sends a genuine SECURE_HELLO from every address and returns how many
// inbound connections received SECURE_REPLY, i.e. paid for a key exchange.
size_t count_served_handshakes(
    geocoin::network::PeerManager& peers,
    const geocoin::bytes& private_key,
    const std::vector<std::string>& addresses
) {
    auto initiator = geocoin::network::SecureHandshake::create_initiator();
    geocoin::network::SecureHelloMessage hello;
    hello.hello = initiator->hello();
    const geocoin::bytes hello_frame = geocoin::network::create_message(
        geocoin::network::MessageType::SECURE_HELLO, hello.serialize());

    std::vector<std::unique_ptr<LocalSocketPair>> sockets;
    for (const auto& address : addresses) {
        auto pair = std::make_unique<LocalSocketPair>();
        check(static_cast<bool>(*pair), "local socketpair is available for handshake throttling test");
        check(peers.add_inbound_peer(pair->release_first(), address, private_key),
              "inbound peer is admitted before its handshake");
        check(pair->send_second(hello_frame), "client sends SECURE_HELLO");
        sockets.push_back(std::move(pair));
    }
    for (int round = 0; round < 20; ++round) {
        peers.update();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    size_t served = 0;
    for (const auto& pair : sockets) {
        geocoin::bytes frame;
        geocoin::network::MessageHeader header;
        geocoin::bytes payload;
        if (pair->recv_second(frame, 65536) &&
            geocoin::network::parse_message(frame, header, payload) &&
            header.type == geocoin::network::MessageType::SECURE_REPLY) {
            ++served;
        }
    }
    return served;
}

void test_inbound_handshake_throttling() {
    const geocoin::bytes private_key = make_key(0x6a);
    {
        geocoin::network::PeerManager peers;
        peers.set_inbound_handshake_limits(0.0, 100.0, 0.0, 2.0);
        check(count_served_handshakes(peers, private_key,
                  {"127.0.0.61:4001", "127.0.0.62:4002", "127.0.0.63:4003"}) == 2,
              "one /24 group cannot run more key exchanges than its handshake budget");
        LocalSocketPair retry;
        check(static_cast<bool>(retry), "local socketpair is available for throttled retry");
        check(peers.add_inbound_peer(retry.release_first(), "127.0.0.63:4004", private_key),
              "a throttled handshake does not ban the address");
    }
    {
        geocoin::network::PeerManager peers;
        peers.set_inbound_handshake_limits(0.0, 2.0, 0.0, 100.0);
        check(count_served_handshakes(peers, private_key,
                  {"127.0.1.61:4001", "127.0.2.61:4002", "127.0.3.61:4003"}) == 2,
              "the node-wide handshake budget bounds key exchanges across groups");
    }
}

void test_peer_direct_socket_and_inventory_policy() {
    check(!geocoin::network::Peer("bad host").connect(std::chrono::milliseconds(0)),
          "Peer::connect rejects malformed address and clamps non-positive timeout");

    LocalSocketPair sockets;
    if (!sockets) {
        check(false, "local socketpair is available for direct peer test");
        return;
    }

    geocoin::network::Peer peer("127.0.0.20:3030", sockets.release_first());
    check(peer.is_connected(), "Peer constructed from socketpair is connected");
    check(peer.get_state() == geocoin::network::PeerState::CONNECTED,
          "Peer constructed from socketpair starts CONNECTED");

    geocoin::bytes zero_read_buffer{0xaa};
    check(peer.receive(zero_read_buffer, 0) == 0 && zero_read_buffer.empty(),
          "Peer::receive with zero max size is a no-op");

    const geocoin::bytes outbound = valid_plaintext_hello_frame();
    check(peer.send(outbound) == static_cast<ssize_t>(outbound.size()),
          "Peer::send writes full payload to socketpair");
    geocoin::bytes got_outbound;
    check(sockets.recv_second(got_outbound), "socketpair peer receives sent bytes");
    check(got_outbound == outbound, "Peer::send preserves payload bytes");

    const auto sent_info = peer.get_info();
    check(sent_info.bytes_sent == outbound.size(), "Peer bytes_sent updates after send");

    const geocoin::bytes inbound{0x09, 0x08, 0x07};
    check(sockets.send_second(inbound), "socketpair writes inbound bytes");
    geocoin::bytes inbound_buffer;
    check(peer.receive(inbound_buffer, 16) == static_cast<ssize_t>(inbound.size()),
          "Peer::receive reads inbound payload");
    check(inbound_buffer == inbound, "Peer::receive preserves inbound bytes");

    const auto received_info = peer.get_info();
    check(received_info.bytes_received == inbound.size(),
          "Peer bytes_received updates after receive");

    geocoin::network::PeerInfo saturated = peer.get_info();
    saturated.bytes_sent = (std::numeric_limits<uint64_t>::max)() - 1;
    peer.update_info(saturated);
    check(peer.send(outbound) == static_cast<ssize_t>(outbound.size()),
          "Peer::send succeeds after stats update");
    check(peer.get_info().bytes_sent == (std::numeric_limits<uint64_t>::max)(),
          "Peer bytes_sent saturates instead of overflowing");
    (void)sockets.recv_second(got_outbound);

    geocoin::network::PeerInfo old_seen = peer.get_info();
    old_seen.last_seen = std::chrono::steady_clock::now() - std::chrono::seconds(10);
    peer.update_info(old_seen);
    check(peer.is_timeout(1), "Peer::is_timeout detects stale last_seen");
    peer.update_last_seen();
    check(!peer.is_timeout(1), "Peer::update_last_seen clears timeout");

    const geocoin::Hash256 known{};
    const geocoin::Hash256 unknown = hash_from_counter(2);
    peer.add_known_inventory(known);
    check(peer.has_known_inventory(known), "Peer tracks known inventory hash");
    check(!peer.has_known_inventory(unknown), "Peer reports missing inventory hash");
    const auto unknown_hashes = peer.filter_unknown_inventory({known, unknown});
    check(unknown_hashes.size() == 1 && unknown_hashes[0] == unknown,
          "Peer filters known inventory in one lock");

    for (uint32_t i = 10; i <= 50010; ++i) {
        peer.add_known_inventory(hash_from_counter(i));
    }
    check(!peer.has_known_inventory(known), "Peer evicts oldest known inventory above cap");
    check(peer.has_known_inventory(hash_from_counter(50010)),
          "Peer keeps newest known inventory after cap eviction");

    peer.disconnect();
    check(!peer.is_connected(), "Peer::disconnect closes socketpair peer");
    check(peer.send({0x01}) == -1, "Peer::send rejects disconnected peer");
    geocoin::bytes disconnected_buffer;
    check(peer.receive(disconnected_buffer, 8) == -1, "Peer::receive rejects disconnected peer");
}

void test_bare_host_validation() {
    geocoin::network::PeerManager peers;
    const geocoin::bytes private_key = make_key(0x42);

    check(!peers.add_outbound_peer("bad host", private_key, true),
          "PeerManager rejects bare hostnames containing whitespace before connect");
    check(!peers.add_outbound_peer(std::string(300, 'a'), private_key, true),
          "PeerManager rejects overlong bare hostnames before connect");
    check(!peers.add_outbound_peer("", private_key, true),
          "PeerManager rejects empty bare hostname before connect");
}

void test_inbound_address_validation() {
    geocoin::network::PeerManager peers;
    const geocoin::bytes private_key = make_key(0x52);

    check(!peers.add_inbound_peer(-1, "unknown", private_key),
          "PeerManager rejects inbound peers with unknown address");
    check(!peers.add_inbound_peer(-1, "127.0.0.1:4040junk", private_key),
          "PeerManager rejects inbound peers with malformed endpoint");
}

void test_public_policy_defaults_and_limits() {
    geocoin::network::PeerManager peers;
    const geocoin::bytes private_key = make_key(0x32);

    check(peers.get_peer_count() == 0, "new PeerManager starts empty");
    check(peers.get_connected_peers().empty(), "new PeerManager has no connected peers");
    check(peers.get_authenticated_peers().empty(), "new PeerManager has no authenticated peers");
    check(peers.broadcast(geocoin::network::MessageType::PING, {}) == 0,
          "broadcast to empty peer set sends nothing");
    check(peers.broadcast(geocoin::network::MessageType::TX, {0x01, 0x02}) == 0,
          "broadcast handles malformed TX payload without peers");
    check(peers.broadcast(geocoin::network::MessageType::BLOCK, {0x03, 0x04}) == 0,
          "broadcast handles malformed BLOCK payload without peers");
    check(peers.relay_inventory(geocoin::network::InvMessage::InvType::TX, {}) == 0,
          "empty inventory relay is a no-op");
    check(!peers.send_message(nullptr, geocoin::network::MessageType::PING, {}),
          "send_message rejects null peer");

    auto stats = peers.get_stats();
    check(stats.total_peers == 0 && stats.connected_peers == 0,
          "empty PeerManager stats are zero");
    check(peers.get_peer_stats().empty(), "empty PeerManager has no peer stats");
    check(peers.get_policy_reject_counters().empty(), "empty PeerManager has no policy rejects");
    check(peers.get_connection_diagnostics().peers.empty(),
          "empty diagnostics have no peers");

    peers.set_archive_mode(true);
    check(peers.is_archive_mode(), "archive mode can be enabled");
    peers.set_archive_mode(false);
    check(!peers.is_archive_mode(), "archive mode can be disabled");

    peers.set_max_peers(0, 0);
    check(!peers.can_accept_inbound(), "zero inbound limit blocks inbound capacity");
    check(!peers.can_add_outbound(), "zero outbound limit blocks outbound capacity");
    check(!peers.add_outbound_peer("127.0.0.9:3030", private_key, false),
          "zero outbound limit rejects before TCP connect");

    geocoin::network::PeerManager send_limit_peers;
    LocalSocketPair sockets;
    if (sockets) {
        const std::string address = "127.0.0.70:3030";
        check(send_limit_peers.add_inbound_peer(sockets.release_first(), address, private_key),
              "inbound peer can be added for send limit test");
        auto peer = send_limit_peers.get_peer(address);
        check(!send_limit_peers.send_message(
                  peer,
                  geocoin::network::MessageType::PING,
                  geocoin::bytes(geocoin::network::detail::kMaxMessagePayload + 1, 0)),
              "send_message rejects payload larger than policy limit");
    } else {
        check(false, "local socketpair is available for send limit test");
    }
}

void test_inbound_lifecycle_and_disconnect_diagnostics() {
    geocoin::network::PeerManager peers;
    const geocoin::bytes private_key = make_key(0x62);
    const std::string address = "127.0.0.2:3030";
    LocalSocketPair sockets;

    if (!sockets) {
        check(false, "local socketpair is available for inbound peer lifecycle test");
        return;
    }

    peers.set_max_peers(1, 1);
    check(peers.add_inbound_peer(sockets.release_first(), address, private_key),
          "valid inbound peer can be tracked");
    check(peers.get_peer_count() == 1, "inbound peer increments peer count");
    check(!peers.can_accept_inbound(), "inbound limit is enforced after accepting peer");
    check(!peers.add_inbound_peer(-1, address, private_key),
          "duplicate inbound peer is rejected");
    check(!peers.add_outbound_peer(address, private_key, true),
          "duplicate outbound peer is rejected before TCP connect");

    auto peer = peers.get_peer(address);
    check(peer != nullptr, "get_peer returns tracked inbound peer");
    check(peers.get_connected_peers().size() == 1, "connected peer list includes inbound peer");
    check(peers.get_authenticated_peers().empty(), "unauthenticated inbound peer is not READY");

    auto stats = peers.get_stats();
    check(stats.total_peers == 1 && stats.connected_peers == 1 && stats.inbound_peers == 1,
          "stats count tracked inbound peer");

    auto peer_stats = peers.get_peer_stats();
    check(peer_stats.size() == 1 && peer_stats[0].address == address && peer_stats[0].inbound,
          "peer stats expose inbound peer details");

    check(peers.disconnect_peer(peer, "unit-test-disconnect"),
          "disconnect_peer(peer, reason) removes tracked peer");
    check(peers.get_peer_count() == 0, "peer count returns to zero after disconnect");

    auto diagnostics = peers.get_connection_diagnostics();
    check(!diagnostics.recent_disconnects.empty(), "disconnect diagnostics record recent disconnect");
    check(diagnostics.disconnect_reasons["unit-test-disconnect"] == 1,
          "disconnect reason counter is recorded");

    LocalSocketPair remove_sockets;
    if (!remove_sockets) {
        check(false, "local socketpair is available for remove_peer test");
        return;
    }
    const std::string removable = "127.0.0.3:3030";
    check(peers.add_inbound_peer(remove_sockets.release_first(), removable, private_key),
          "inbound peer can be added for remove_peer");
    peers.remove_peer(removable);
    check(peers.get_peer(removable) == nullptr, "remove_peer(address) removes tracked peer");
    diagnostics = peers.get_connection_diagnostics();
    check(diagnostics.disconnect_reasons["remove-peer"] == 1,
          "remove_peer records remove-peer disconnect reason");

    LocalSocketPair ip_disconnect_sockets;
    if (!ip_disconnect_sockets) {
        check(false, "local socketpair is available for disconnect_peer(ip) test");
        return;
    }
    const std::string ip_disconnect = "127.0.0.12:3030";
    check(peers.add_inbound_peer(ip_disconnect_sockets.release_first(), ip_disconnect, private_key),
          "inbound peer can be added for disconnect_peer(ip)");
    check(peers.disconnect_peer("127.0.0.12"),
          "disconnect_peer(ip) removes matching tracked peer");
    check(peers.get_peer(ip_disconnect) == nullptr,
          "disconnect_peer(ip) removes the host:port peer entry");
    diagnostics = peers.get_connection_diagnostics();
    check(diagnostics.disconnect_reasons["rpc-disconnect"] == 1,
          "disconnect_peer(ip) records rpc-disconnect reason");
}

void test_manual_bans_and_added_nodes() {
    geocoin::network::PeerManager peers;
    const geocoin::bytes private_key = make_key(0x72);
    const std::string banned = "192.0.2.20:3030";

    check(!peers.set_manual_ban("", 60), "manual ban rejects empty address");
    check(peers.set_manual_ban(banned, 60), "manual ban accepts host:port");
    auto bans = peers.list_bans();
    check(bans.size() == 1 && bans[0].address == "192.0.2.20" && bans[0].manual,
          "manual ban is listed by normalized IP");
    check(!peers.add_outbound_peer(banned, private_key, true),
          "outbound connection to manually banned peer is rejected before connect");
    LocalSocketPair banned_inbound;
    if (banned_inbound) {
        check(!peers.add_inbound_peer(banned_inbound.release_first(), banned, private_key),
              "inbound connection from manually banned peer is rejected");
    } else {
        check(false, "local socketpair is available for banned inbound test");
    }
    check(peers.remove_manual_ban("192.0.2.20"), "manual ban can be removed by IP");
    check(!peers.remove_manual_ban("192.0.2.20"), "removing absent manual ban returns false");
    check(peers.list_bans().empty(), "ban list is empty after removal");

    LocalSocketPair active_inbound;
    if (active_inbound) {
        const std::string active = "198.51.100.21:3030";
        check(peers.add_inbound_peer(active_inbound.release_first(), active, private_key),
              "inbound peer can be added before manual ban");
        check(peers.set_manual_ban(active, 60),
              "manual ban disconnects already connected peer");
        check(peers.get_peer(active) == nullptr,
              "manual-ban removes active peer by IP");
        auto diagnostics = peers.get_connection_diagnostics();
        check(diagnostics.disconnect_reasons["manual-ban"] == 1,
              "manual ban disconnect reason is recorded");
        check(peers.remove_manual_ban("198.51.100.21"),
              "manual ban for active peer can be removed");
    } else {
        check(false, "local socketpair is available for active manual ban test");
    }

    peers.set_manual_ban("198.51.100.7:4040", 0);
    check(!peers.list_bans().empty(), "zero ban duration uses default duration");
    peers.clear_manual_bans();
    check(peers.list_bans().empty(), "clear_manual_bans clears active bans");

    peers.add_node("127.0.0.1:3030");
    peers.add_node("127.0.0.1:3030");
    peers.add_node("127.0.0.2:3030");
    auto added = peers.get_added_nodes();
    check(added.size() == 2, "add_node ignores duplicates");
    check(std::find(added.begin(), added.end(), "127.0.0.1:3030") != added.end(),
          "added nodes include first address");
    peers.remove_node("127.0.0.1:3030");
    added = peers.get_added_nodes();
    check(added.size() == 1 && added[0] == "127.0.0.2:3030",
          "remove_node deletes persistent target");

    geocoin::network::PeerManager capped_nodes;
    for (size_t i = 0; i < 1024; ++i) {
        capped_nodes.add_node("10.10.0." + std::to_string(i) + ":3030");
    }
    capped_nodes.add_node("10.10.255.255:3030");
    check(capped_nodes.get_added_nodes().size() == 1024,
          "add_node enforces persistent addnode cap");
}

void test_outbound_attempt_pruning_preserves_manual_and_added_targets() {
    geocoin::network::PeerManager peers;
    const std::string manual_target = "203.0.113.250:3030";
    const std::string added_target = "203.0.113.251:3030";

    peers.add_node(added_target);
    geocoin::network::PeerManagerTestAccess::seed_outbound_attempt(
        peers,
        manual_target,
        true,
        7,
        std::chrono::seconds(10000)
    );
    geocoin::network::PeerManagerTestAccess::seed_outbound_attempt(
        peers,
        added_target,
        false,
        3,
        std::chrono::seconds(10001)
    );

    for (size_t i = 0; i < 8210; ++i) {
        geocoin::network::PeerManagerTestAccess::seed_outbound_attempt(
            peers,
            "198.18." + std::to_string(i / 256) + "." + std::to_string(i % 256) + ":3030",
            false,
            static_cast<uint32_t>(i + 1),
            std::chrono::seconds(9000 + static_cast<int>(i))
        );
    }

    const size_t before = geocoin::network::PeerManagerTestAccess::outbound_attempt_count(peers);
    check(before > 8192, "outbound attempt seed exceeds pruning cap");

    geocoin::network::PeerManagerTestAccess::prune_outbound_attempts(peers);
    const size_t after = geocoin::network::PeerManagerTestAccess::outbound_attempt_count(peers);
    check(after <= 8192, "outbound attempt pruning enforces cap");
    check(after < before, "outbound attempt pruning removes old candidates");
    check(geocoin::network::PeerManagerTestAccess::has_outbound_attempt(peers, manual_target),
          "outbound attempt pruning preserves manual targets");
    check(geocoin::network::PeerManagerTestAccess::has_outbound_attempt(peers, added_target),
          "outbound attempt pruning preserves persistent addnode targets");

    for (size_t i = 0; i < 32; ++i) {
        geocoin::network::PeerManagerTestAccess::seed_outbound_attempt(
            peers,
            "198.19.0." + std::to_string(i) + ":3030",
            false,
            1,
            std::chrono::seconds(8000 + static_cast<int>(i))
        );
    }
    check(geocoin::network::PeerManagerTestAccess::outbound_attempt_count(peers) > 8192,
          "successful-result pruning test exceeds outbound attempt cap");
    const std::string successful_target = "203.0.113.252:3030";
    geocoin::network::PeerManagerTestAccess::begin_outbound_attempt(
        peers,
        successful_target,
        false
    );
    geocoin::network::PeerManagerTestAccess::finish_outbound_attempt(
        peers,
        successful_target,
        true,
        false
    );
    check(geocoin::network::PeerManagerTestAccess::outbound_attempt_count(peers) <= 8192,
          "successful outbound result also enforces diagnostics cap");
}

void test_temp_ban_bounds_and_ipv6_grouping() {
    geocoin::network::PeerManager peers;

    geocoin::network::PeerManagerTestAccess::add_temp_ban(
        peers,
        "2001:db8:1:2::1"
    );
    check(geocoin::network::PeerManagerTestAccess::is_banned(
              peers,
              "[2001:db8:1:2::ffff]:3030"),
          "temporary IPv6 ban covers the complete /64 network group");
    check(!geocoin::network::PeerManagerTestAccess::is_banned(
              peers,
              "[2001:db8:1:3::1]:3030"),
          "temporary IPv6 ban does not cover a different /64 network group");

    for (size_t i = 0; i < geocoin::network::detail::kPolicyMaxTempBans + 128; ++i) {
        const std::string ip =
            "10.0." + std::to_string((i / 256) % 256) + "." + std::to_string(i % 256);
        geocoin::network::PeerManagerTestAccess::add_temp_ban(peers, ip);
    }
    check(geocoin::network::PeerManagerTestAccess::temp_ban_count(peers) <=
              geocoin::network::detail::kPolicyMaxTempBans,
          "temporary ban table remains within its hard memory bound");
}

void test_outbound_self_connect_and_backoff_diagnostics() {
    geocoin::network::PeerManager peers;
    const geocoin::bytes private_key = make_key(0x82);

    const std::string in_flight = "127.0.0.1:2";
    check(peers.should_try_outbound(in_flight, true),
          "new outbound target can be attempted");
    geocoin::network::PeerManagerTestAccess::begin_outbound_attempt(peers, in_flight, true);
    check(!peers.should_try_outbound(in_flight, true),
          "concurrent outbound attempt to the same target is suppressed");
    auto in_flight_diagnostics = peers.get_connection_diagnostics();
    const auto in_flight_it = std::find_if(
        in_flight_diagnostics.outbound_attempts.begin(),
        in_flight_diagnostics.outbound_attempts.end(),
        [&](const geocoin::network::PeerOutboundAttemptInfo& info) {
            return info.address == in_flight;
        }
    );
    check(in_flight_it != in_flight_diagnostics.outbound_attempts.end() &&
              in_flight_it->in_progress,
          "outbound diagnostics expose an in-progress attempt");
    geocoin::network::PeerManagerTestAccess::finish_outbound_attempt(
        peers,
        in_flight,
        false,
        true
    );

    peers.set_private_key(private_key);
    peers.set_local_port(3030);
    check(!peers.add_outbound_peer("127.0.0.1:3030", private_key, true),
          "self outbound connection is rejected before TCP connect");

    peers.set_local_port(0);
    peers.set_outbound_connect_timeout(std::chrono::milliseconds(1));
    const std::string unreachable = "127.0.0.1:1";
    check(!peers.connect_to_node(unreachable, {}),
          "failed outbound connect returns false and records attempt");
    check(!peers.add_outbound_peer(unreachable, private_key, false),
          "regular outbound retry is blocked by backoff after failure");

    geocoin::network::PeerManagerTestAccess::seed_outbound_backoff(
        peers,
        unreachable,
        5,
        std::chrono::minutes(5)
    );
    check(!peers.connect_to_node(unreachable, {}),
          "onetry bypasses retry deadline and reports the immediate TCP failure");

    auto diagnostics = peers.get_connection_diagnostics();
    auto it = std::find_if(
        diagnostics.outbound_attempts.begin(),
        diagnostics.outbound_attempts.end(),
        [&](const geocoin::network::PeerOutboundAttemptInfo& info) {
            return info.address == unreachable;
        }
    );
    check(it != diagnostics.outbound_attempts.end(), "outbound attempt is present in diagnostics");
    if (it != diagnostics.outbound_attempts.end()) {
        check(it->fail_count == 6,
              "onetry attempted immediately and incremented a deep backoff failure");
        check(it->manual, "connect_to_node records manual attempt");
        check(!it->in_progress, "completed outbound attempt is not reported in progress");
        check(it->last_error == "tcp-connect-failed" || it->last_error == "backoff",
              "outbound diagnostics record failure reason");
    }
}

void test_inbound_eviction_when_full() {
    geocoin::network::PeerManager peers;
    const geocoin::bytes private_key = make_key(0x8c);
    LocalSocketPair first;
    LocalSocketPair second;
    const std::string old_address = "127.0.0.10:3030";
    const std::string new_address = "127.0.0.11:3030";

    if (!first || !second) {
        check(false, "local socketpairs are available for inbound eviction test");
        return;
    }

    peers.set_max_peers(1, 0);
    check(peers.add_inbound_peer(first.release_first(), old_address, private_key),
          "first inbound peer can be accepted");
    check(peers.add_inbound_peer(second.release_first(), new_address, private_key),
          "new inbound peer can evict unauthenticated old peer when full");
    check(peers.get_peer(old_address) == nullptr,
          "old inbound peer was evicted");
    check(peers.get_peer(new_address) != nullptr,
          "new inbound peer remains connected");
    check(peers.get_peer_count() == 1,
          "inbound eviction keeps peer count at limit");

    auto diagnostics = peers.get_connection_diagnostics();
    check(diagnostics.disconnect_reasons["inbound-evicted"] == 1,
          "inbound eviction reason is recorded");
}

void test_wire_policy_disconnects() {
    const geocoin::bytes private_key = make_key(0x92);

    {
        geocoin::network::PeerManager peers;
        LocalSocketPair sockets;
        const std::string address = "127.0.0.4:3030";
        if (!sockets) {
            check(false, "local socketpair is available for bad-frame test");
            return;
        }

        check(peers.add_inbound_peer(sockets.release_first(), address, private_key),
              "inbound peer can be added for bad-frame test");
        geocoin::bytes bad_frame = geocoin::network::create_message(
            geocoin::network::MessageType::PING,
            {}
        );
        bad_frame[0] ^= 0x7f;
        check(sockets.send_second(bad_frame), "bad frame is written to peer socket");
        peers.update();
        check(peers.get_peer(address) == nullptr, "bad frame disconnects inbound peer");

        auto diagnostics = peers.get_connection_diagnostics();
        check(diagnostics.disconnect_reasons["bad-network-magic"] == 1,
              "wrong network magic is rejected from the header alone");
    }

    {
        geocoin::network::PeerManager peers;
        LocalSocketPair sockets;
        const std::string address = "127.0.0.6:3030";
        if (!sockets) {
            check(false, "local socketpair is available for unknown header test");
            return;
        }

        check(peers.add_inbound_peer(sockets.release_first(), address, private_key),
              "inbound peer can be added for unknown header test");
        geocoin::bytes bad_header = geocoin::network::create_message(
            geocoin::network::MessageType::PING,
            {}
        );
        bad_header[4] = 0xff;
        check(sockets.send_second(bad_header), "unknown message header is written to peer socket");
        peers.update();
        check(peers.get_peer(address) == nullptr, "unknown message header disconnects inbound peer");

        auto diagnostics = peers.get_connection_diagnostics();
        check(diagnostics.disconnect_reasons["bad-message-header"] == 1,
              "unknown message header disconnect reason is recorded");
    }

    {
        geocoin::network::PeerManager peers;
        LocalSocketPair sockets;
        const std::string address = "127.0.0.5:3030";
        if (!sockets) {
            check(false, "local socketpair is available for preauth INV test");
            return;
        }

        check(peers.add_inbound_peer(sockets.release_first(), address, private_key),
              "inbound peer can be added for preauth INV test");

        geocoin::network::InvMessage inv;
        geocoin::network::InvMessage::InvItem item;
        item.type = geocoin::network::InvMessage::InvType::TX;
        item.hash = {};
        item.hash[0] = 0x42;
        inv.items.push_back(item);
        const auto message = geocoin::network::create_message(
            geocoin::network::MessageType::INV,
            inv.serialize()
        );
        check(sockets.send_second(message), "preauth INV is written to peer socket");
        peers.update();
        check(peers.get_peer(address) == nullptr, "preauth INV disconnects inbound peer");

        auto diagnostics = peers.get_connection_diagnostics();
        check(diagnostics.disconnect_reasons["preauth-message"] == 1,
              "preauth message disconnect reason is recorded");
        check(!diagnostics.policy_rejects.empty(), "preauth message increments policy diagnostics");
    }

    {
        geocoin::network::PeerManager peers;
        LocalSocketPair sockets;
        const std::string address = "127.0.0.7:3030";
        if (!sockets) {
            check(false, "local socketpair is available for malformed INV test");
            return;
        }

        check(peers.add_inbound_peer(sockets.release_first(), address, private_key),
              "inbound peer can be added for malformed INV test");
        auto client_channel =
            geocoin::network::PeerManagerTestAccess::activate_test_secure_channel(peers, address);
        check(client_channel != nullptr, "malformed INV test activates encrypted transport");
        auto peer = peers.get_peer(address);
        check(peer != nullptr, "malformed INV peer exists");
        if (peer) {
            peer->set_state(geocoin::network::PeerState::READY);
        }
        const geocoin::bytes malformed_payload{0x01, 0x02, 0x03};
        const auto message = client_channel
            ? protect_test_frame(
                  *client_channel, geocoin::network::MessageType::INV, malformed_payload)
            : geocoin::bytes{};
        check(sockets.send_second(message), "malformed INV is written to peer socket");
        peers.update();
        check(peers.get_peer(address) == nullptr, "malformed INV disconnects inbound peer");

        auto diagnostics = peers.get_connection_diagnostics();
        check(diagnostics.disconnect_reasons["inv-malformed"] == 1,
              "malformed INV disconnect reason is recorded");
        check(diagnostics.policy_rejects["inv-malformed"] == 1,
              "malformed INV increments policy diagnostics");
        const auto bans = peers.list_bans();
        check(std::any_of(bans.begin(), bans.end(), [](const geocoin::network::PeerBanInfo& ban) {
                  return ban.address == "127.0.0.7" && !ban.manual;
              }),
              "malformed INV adds temporary ban to ban listing");

        LocalSocketPair banned_retry;
        if (banned_retry) {
            check(!peers.add_inbound_peer(banned_retry.release_first(), "127.0.0.7:3031", private_key),
                  "temp-banned IP cannot reconnect immediately");
        } else {
            check(false, "local socketpair is available for temp ban retry test");
        }
    }

    {
        geocoin::network::PeerManager peers;
        LocalSocketPair sockets;
        const std::string address = "127.0.0.8:3030";
        if (!sockets) {
            check(false, "local socketpair is available for oversized GETDATA test");
            return;
        }

        check(peers.add_inbound_peer(sockets.release_first(), address, private_key),
              "inbound peer can be added for oversized GETDATA test");
        auto client_channel =
            geocoin::network::PeerManagerTestAccess::activate_test_secure_channel(peers, address);
        check(client_channel != nullptr, "oversized GETDATA test activates encrypted transport");
        auto peer = peers.get_peer(address);
        check(peer != nullptr, "oversized GETDATA peer exists");
        if (peer) {
            peer->set_state(geocoin::network::PeerState::READY);
        }
        geocoin::network::InvMessage getdata;
        getdata.items.reserve(geocoin::network::detail::kPolicyMaxGetDataItems + 1);
        for (size_t i = 0; i <= geocoin::network::detail::kPolicyMaxGetDataItems; ++i) {
            geocoin::Hash256 hash{};
            hash[0] = static_cast<geocoin::byte>(i & 0xff);
            hash[1] = static_cast<geocoin::byte>((i >> 8) & 0xff);
            getdata.items.push_back({geocoin::network::InvMessage::InvType::BLOCK, hash});
        }
        const auto message = client_channel
            ? protect_test_frame(
                  *client_channel,
                  geocoin::network::MessageType::GETDATA,
                  getdata.serialize())
            : geocoin::bytes{};
        check(sockets.send_second(message), "oversized GETDATA is written to peer socket");
        peers.update();
        peers.update();
        check(peers.get_peer(address) == nullptr, "oversized GETDATA disconnects inbound peer");

        auto diagnostics = peers.get_connection_diagnostics();
        check(diagnostics.disconnect_reasons["message-payload-limit"] == 1,
              "oversized GETDATA is rejected from its header length");
        check(diagnostics.policy_rejects["message-payload-limit"] == 1,
              "oversized GETDATA increments early payload diagnostics");
    }
}

void test_reject_message_handler_records_valid_and_malformed_payloads() {
    geocoin::network::PeerManager peers;
    const geocoin::bytes private_key = make_key(0x98);
    LocalSocketPair sockets;
    const std::string address = "127.0.0.44:3030";

    if (!sockets) {
        check(false, "local socketpair is available for REJECT handler test");
        return;
    }

    check(peers.add_inbound_peer(sockets.release_first(), address, private_key),
          "inbound peer can be added for REJECT handler test");

    geocoin::network::RejectMessage reject;
    reject.rejected_type = geocoin::network::MessageType::TX;
    reject.code = 1;
    reject.reason = "unit-test";
    reject.rejected_hash = hash_from_counter(440);

    check(geocoin::network::PeerManagerTestAccess::handle_reject_payload(
              peers,
              address,
              reject.serialize()),
          "valid REJECT payload is handled");
    check(peers.get_peer(address) != nullptr,
          "valid REJECT payload does not disconnect peer");

    check(geocoin::network::PeerManagerTestAccess::handle_reject_payload(
              peers,
              address,
              geocoin::bytes{0x01, 0x02}),
          "malformed REJECT payload reaches handler");
    auto rejected_peer = peers.get_peer(address);
    check(rejected_peer != nullptr && !rejected_peer->is_connected(),
          "malformed REJECT payload closes peer transport");

    auto diagnostics = peers.get_connection_diagnostics();
    check(diagnostics.disconnect_reasons["reject-malformed"] == 1,
          "malformed REJECT records disconnect reason");
    check(diagnostics.policy_rejects["reject-malformed"] == 1,
          "malformed REJECT increments policy diagnostics");
}

void test_p2p_server_lifecycle_accept_and_whitelist() {
    const geocoin::bytes private_key = make_key(0xa0);

    {
        const uint16_t port = reserve_free_loopback_port();
        if (port == 0) {
            check(false, "free loopback port is available for P2PServer accept test");
            return;
        }

        auto peers = std::make_shared<geocoin::network::PeerManager>();
        geocoin::network::P2PServer server(peers, private_key, port);
        check(!server.is_running(), "new P2PServer starts stopped");
        check(server.get_port() == port, "P2PServer exposes configured port");
        check(server.get_accepted_connections() == 0, "P2PServer starts with zero accepted connections");
        check(server.start(), "P2PServer starts on a free loopback port");
        check(server.is_running(), "P2PServer reports running after start");
        check(!server.start(), "P2PServer rejects duplicate start while running");
        check(server.get_last_start_error() == 0, "successful P2PServer start clears last error");

        int client_fd = connect_loopback_tcp(port);
        check(client_fd >= 0, "loopback client connects to P2PServer");
        for (int i = 0; i < 200 && server.get_accepted_connections() == 0; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        check(server.get_accepted_connections() == 1, "P2PServer counts accepted inbound connection");
        check(peers->get_peer_count() == 1, "P2PServer hands accepted socket to PeerManager");

        close_fd_if_open(client_fd);
        server.stop();
        check(!server.is_running(), "P2PServer reports stopped after stop");
        server.stop();
        check(!server.is_running(), "P2PServer stop is idempotent");
    }

    {
        const uint16_t port = reserve_free_loopback_port();
        if (port == 0) {
            check(false, "free loopback port is available for P2PServer whitelist test");
            return;
        }

        auto peers = std::make_shared<geocoin::network::PeerManager>();
        geocoin::network::P2PServer server(
            peers,
            private_key,
            port,
            {"203.0.113.10"}
        );
        check(server.start(), "P2PServer starts with inbound whitelist");
        int client_fd = connect_loopback_tcp(port);
        check(client_fd >= 0, "loopback client connects to whitelisted P2PServer socket");
        for (int i = 0; i < 100; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        check(server.get_accepted_connections() == 0,
              "P2PServer does not count non-whitelisted inbound connection");
        check(peers->get_peer_count() == 0,
              "P2PServer whitelist rejection does not add peer");

        close_fd_if_open(client_fd);
        server.stop();
        check(!server.is_running(), "whitelist P2PServer stops");
    }
}

void test_handshake_version_and_auth_rejections() {
    const geocoin::bytes private_key = make_key(0xa5);

    auto send_to_inbound_and_update = [&](geocoin::network::PeerManager& peers,
                                          geocoin::network::MessageType type,
                                          const geocoin::bytes& payload,
                                          const std::string& address,
                                          const geocoin::bytes& key) {
        peers.set_private_key(key);
        LocalSocketPair sockets;
        if (!sockets) {
            check(false, "local socketpair is available for handshake rejection test");
            return;
        }
        check(peers.add_inbound_peer(sockets.release_first(), address, key),
              "inbound peer can be added for handshake rejection test");
        geocoin::bytes message = geocoin::network::create_message(type, payload);
        if (type == geocoin::network::MessageType::VERSION) {
            auto channel = geocoin::network::PeerManagerTestAccess::activate_test_secure_channel(
                peers, address);
            check(channel != nullptr, "VERSION rejection test activates encrypted transport");
            if (channel) {
                message = protect_test_frame(*channel, type, payload);
            }
        }
        check(sockets.send_second(message), "handshake rejection frame is written to peer socket");
        peers.update();
    };

    {
        auto version = valid_version_message(0xa5);
        version.version = 1;
        geocoin::network::PeerManager peers;
        send_to_inbound_and_update(
            peers,
            geocoin::network::MessageType::VERSION,
            version.serialize(),
            "127.0.0.30:3030",
            private_key
        );
        check(peers.get_peer("127.0.0.30:3030") == nullptr,
              "unsupported VERSION disconnects inbound peer");
        auto diagnostics = peers.get_connection_diagnostics();
        check(diagnostics.disconnect_reasons["version-unsupported"] == 1,
              "unsupported VERSION disconnect reason is recorded");
        check(diagnostics.policy_rejects["version-unsupported"] == 1,
              "unsupported VERSION increments policy diagnostics");
    }

    {
        auto version = valid_version_message(0xa5);
        version.services = geocoin::network::service_flag(geocoin::network::ServiceFlags::NODE_NETWORK);
        geocoin::network::PeerManager peers;
        send_to_inbound_and_update(
            peers,
            geocoin::network::MessageType::VERSION,
            version.serialize(),
            "127.0.0.31:3030",
            private_key
        );
        check(peers.get_peer("127.0.0.31:3030") == nullptr,
              "VERSION missing required service bits disconnects inbound peer");
        auto diagnostics = peers.get_connection_diagnostics();
        check(diagnostics.disconnect_reasons["version-missing-required-services"] == 1,
              "missing service VERSION disconnect reason is recorded");
    }

    {
        auto version = valid_version_message(0xa5);
        version.user_agent.push_back('\n');
        geocoin::network::PeerManager peers;
        send_to_inbound_and_update(
            peers,
            geocoin::network::MessageType::VERSION,
            version.serialize(),
            "127.0.0.32:3030",
            private_key
        );
        check(peers.get_peer("127.0.0.32:3030") == nullptr,
              "non-printable VERSION user-agent disconnects inbound peer");
        auto diagnostics = peers.get_connection_diagnostics();
        check(diagnostics.disconnect_reasons["version-invalid-user-agent"] == 1,
              "invalid user-agent VERSION disconnect reason is recorded");
    }

    {
        auto version = valid_version_message(0xa5);
        version.auth_address = geocoin::Address("not-a-geo-address");
        geocoin::network::PeerManager peers;
        send_to_inbound_and_update(
            peers,
            geocoin::network::MessageType::VERSION,
            version.serialize(),
            "127.0.0.33:3030",
            private_key
        );
        check(peers.get_peer("127.0.0.33:3030") == nullptr,
              "invalid VERSION auth address disconnects inbound peer");
        auto diagnostics = peers.get_connection_diagnostics();
        check(diagnostics.disconnect_reasons["version-invalid-auth-address"] == 1,
              "invalid auth address VERSION disconnect reason is recorded");
    }

    {
        auto version = valid_version_message(0xa5);
        geocoin::network::PeerManager peers;
        send_to_inbound_and_update(
            peers,
            geocoin::network::MessageType::VERSION,
            version.serialize(),
            "127.0.0.38:3030",
            private_key
        );
        check(peers.get_peer("127.0.0.38:3030") == nullptr,
              "self-connection through a transport alias is rejected by node identity");
        const auto diagnostics = peers.get_connection_diagnostics();
        const auto self_reason = diagnostics.disconnect_reasons.find("self-auth-identity");
        check(self_reason != diagnostics.disconnect_reasons.end() && self_reason->second == 1,
              "identity-based self-connection reason is recorded");
    }

    {
        auto version = valid_version_message(0xa5);
        geocoin::bytes payload = version.serialize();
        payload.push_back(0xff);
        geocoin::network::PeerManager peers;
        send_to_inbound_and_update(
            peers,
            geocoin::network::MessageType::VERSION,
            payload,
            "127.0.0.34:3030",
            private_key
        );
        check(peers.get_peer("127.0.0.34:3030") == nullptr,
              "malformed VERSION disconnects inbound peer");
        auto diagnostics = peers.get_connection_diagnostics();
        check(diagnostics.disconnect_reasons["version-malformed"] == 1,
              "malformed VERSION disconnect reason is recorded");
    }

    {
        geocoin::network::PeerManager peers;
        send_to_inbound_and_update(
            peers,
            geocoin::network::MessageType::CHALLENGE,
            geocoin::bytes{0x01, 0x02, 0x03},
            "127.0.0.35:3030",
            private_key
        );
        check(peers.get_peer("127.0.0.35:3030") == nullptr,
              "malformed CHALLENGE disconnects inbound peer");
        auto diagnostics = peers.get_connection_diagnostics();
        check(diagnostics.disconnect_reasons["message-payload-limit"] == 1,
              "malformed CHALLENGE length is rejected before buffering its body");
    }

    {
        geocoin::network::ChallengeMessage challenge;
        challenge.challenge = geocoin::crypto::create_challenge();
        challenge.challenge.session_id.assign(16, 0x7a);
        geocoin::network::PeerManager peers;
        send_to_inbound_and_update(
            peers,
            geocoin::network::MessageType::CHALLENGE,
            challenge.serialize(),
            "127.0.0.36:3030",
            private_key
        );
        check(peers.get_peer("127.0.0.36:3030") == nullptr,
              "CHALLENGE without auth session disconnects inbound peer");
        auto diagnostics = peers.get_connection_diagnostics();
        check(diagnostics.disconnect_reasons["preauth-message"] == 1,
              "CHALLENGE before VERSION is rejected by handshake ordering");
    }

    {
        const auto challenge = geocoin::crypto::create_challenge();
        geocoin::network::ResponseMessage response;
        response.response = geocoin::crypto::respond_to_challenge(private_key, challenge);
        geocoin::network::PeerManager peers;
        send_to_inbound_and_update(
            peers,
            geocoin::network::MessageType::RESPONSE,
            response.serialize(),
            "127.0.0.37:3030",
            private_key
        );
        check(peers.get_peer("127.0.0.37:3030") == nullptr,
              "RESPONSE without auth session disconnects inbound peer");
        auto diagnostics = peers.get_connection_diagnostics();
        check(diagnostics.disconnect_reasons["preauth-message"] == 1,
              "RESPONSE before VERSION is rejected by handshake ordering");
    }
}

void test_authenticated_peer_manager_flow_and_relay() {
    geocoin::network::PeerManager outbound;
    geocoin::network::PeerManager inbound;
    std::string outbound_address;
    std::string inbound_address;
    int outbound_callbacks = 0;
    int inbound_callbacks = 0;
    std::vector<geocoin::network::MessageType> outbound_messages;
    std::vector<geocoin::network::MessageType> inbound_messages;

    outbound.set_peer_authenticated_callback(
        [&](std::shared_ptr<geocoin::network::Peer> peer, geocoin::Height height) {
            check(peer != nullptr && peer->get_address() == outbound_address,
                  "outbound authenticated callback receives listener address");
            check(height == 222, "outbound callback receives inbound height from VERSION");
            ++outbound_callbacks;
        }
    );
    inbound.set_peer_authenticated_callback(
        [&](std::shared_ptr<geocoin::network::Peer> peer, geocoin::Height height) {
            check(peer != nullptr && peer->get_address() == inbound_address,
                  "inbound authenticated callback receives accepted remote address");
            check(height == 111, "inbound callback receives outbound height from VERSION");
            ++inbound_callbacks;
        }
    );
    outbound.set_message_callback(
        [&](std::shared_ptr<geocoin::network::Peer> peer,
            geocoin::network::MessageType type,
            const geocoin::bytes& payload) {
            check(peer != nullptr && peer->get_address() == outbound_address,
                  "outbound message callback receives listener address");
            outbound_messages.push_back(type);
            if (type == geocoin::network::MessageType::INV) {
                auto inv = geocoin::network::InvMessage::deserialize(payload);
                check(inv.items.size() == 1, "outbound INV callback receives one item");
            }
        }
    );
    inbound.set_message_callback(
        [&](std::shared_ptr<geocoin::network::Peer> peer,
            geocoin::network::MessageType type,
            const geocoin::bytes& payload) {
            (void)payload;
            check(peer != nullptr && peer->get_address() == inbound_address,
                  "inbound message callback receives accepted remote address");
            inbound_messages.push_back(type);
        }
    );

    if (!connect_authenticated_pair(outbound, inbound, outbound_address, inbound_address)) {
        return;
    }

    check(pump_until_authenticated(outbound, inbound),
          "two PeerManagers complete full challenge-response authentication");
    check(outbound_callbacks == 1, "outbound authenticated callback fires once");
    check(inbound_callbacks == 1, "inbound authenticated callback fires once");
    check(outbound.get_authenticated_peers().size() == 1,
          "outbound manager exposes authenticated peer");
    check(inbound.get_authenticated_peers().size() == 1,
          "inbound manager exposes authenticated peer");

    check(geocoin::network::PeerManagerTestAccess::received_message_count(
              outbound, outbound_address, geocoin::network::MessageType::CHALLENGE) == 3 &&
          geocoin::network::PeerManagerTestAccess::received_message_count(
              outbound, outbound_address, geocoin::network::MessageType::RESPONSE) == 3,
          "outbound authenticates the listener and also proves its own identity");
    check(geocoin::network::PeerManagerTestAccess::received_message_count(
              inbound, inbound_address, geocoin::network::MessageType::CHALLENGE) == 3 &&
          geocoin::network::PeerManagerTestAccess::received_message_count(
              inbound, inbound_address, geocoin::network::MessageType::RESPONSE) == 3,
          "inbound verifies and proves both halves of mutual authentication");

    auto outbound_stats = outbound.get_peer_stats();
    auto inbound_stats = inbound.get_peer_stats();
    check(outbound_stats.size() == 1 && outbound_stats[0].authenticated &&
              outbound_stats[0].state == "ready" &&
              outbound_stats[0].starting_height == 222,
          "outbound stats expose authenticated READY peer and remote height");
    check(inbound_stats.size() == 1 && inbound_stats[0].authenticated &&
              inbound_stats[0].state == "ready" &&
              inbound_stats[0].starting_height == 111,
          "inbound stats expose authenticated READY peer and remote height");

    geocoin::network::AddrMessage addrs;
    addrs.addresses.push_back({geocoin::network::build_local_services(false),
                               "127.0.0.50",
                               3030,
                               987654321});
    check(inbound.broadcast(geocoin::network::MessageType::ADDR, addrs.serialize()) == 1,
          "broadcast sends ADDR to authenticated peer");
    for (int i = 0; i < 200 &&
             std::find(outbound_messages.begin(),
                       outbound_messages.end(),
                       geocoin::network::MessageType::ADDR) == outbound_messages.end(); ++i) {
        pump_managers(outbound, inbound);
    }
    check(std::find(outbound_messages.begin(),
                    outbound_messages.end(),
                    geocoin::network::MessageType::ADDR) != outbound_messages.end(),
          "authenticated outbound peer receives broadcast ADDR callback");

    const auto inventory_hash = hash_from_counter(700);
    check(inbound.relay_inventory(
              geocoin::network::InvMessage::InvType::TX,
              {inventory_hash}) == 1,
          "relay_inventory sends first unknown item to authenticated peer");
    for (int i = 0; i < 200 &&
             std::find(outbound_messages.begin(),
                       outbound_messages.end(),
                       geocoin::network::MessageType::INV) == outbound_messages.end(); ++i) {
        pump_managers(outbound, inbound);
    }
    check(std::find(outbound_messages.begin(),
                    outbound_messages.end(),
                    geocoin::network::MessageType::INV) != outbound_messages.end(),
          "authenticated outbound peer receives relayed INV callback");
    check(inbound.relay_inventory(
              geocoin::network::InvMessage::InvType::TX,
              {inventory_hash}) == 0,
          "relay_inventory skips item already marked known for peer");

    check(outbound.broadcast(geocoin::network::MessageType::PING, {}) == 1,
          "broadcast sends PING to authenticated peer");
    pump_managers(outbound, inbound, 10);
    check(std::find(inbound_messages.begin(),
                    inbound_messages.end(),
                    geocoin::network::MessageType::PING) == inbound_messages.end(),
          "system PING is handled internally and not forwarded as app callback");
    check(outbound.get_stats().total_bytes_sent > 0 &&
              inbound.get_stats().total_bytes_received > 0,
          "authenticated managers account for transmitted bytes");
}

void test_duplicate_authenticated_identity_uses_one_slot() {
    const geocoin::bytes remote_key = make_key(0xc1);
    const geocoin::bytes hub_key = make_key(0xd1);
    geocoin::network::PeerManager first_remote;
    geocoin::network::PeerManager second_remote;
    geocoin::network::PeerManager hub;
    first_remote.set_private_key(remote_key);
    second_remote.set_private_key(remote_key);
    hub.set_private_key(hub_key);

    int hub_callbacks = 0;
    hub.set_peer_authenticated_callback(
        [&](std::shared_ptr<geocoin::network::Peer>, geocoin::Height) {
            ++hub_callbacks;
        }
    );

    LocalTcpListener listener;
    if (!listener) {
        check(false, "loopback listener is available for duplicate identity test");
        return;
    }

    const auto connect_remote = [&](geocoin::network::PeerManager& remote,
                                    std::string& inbound_address) {
        if (!remote.add_outbound_peer(listener.address(), remote_key, true)) {
            return false;
        }
        const int accepted = listener.accept_one(inbound_address);
        if (accepted < 0 || inbound_address.empty()) {
            return false;
        }
        if (!hub.add_inbound_peer(accepted, inbound_address, hub_key)) {
            return false;
        }
        return true;
    };

    std::string first_address;
    check(connect_remote(first_remote, first_address),
          "first transport for a node identity connects");
    check(pump_until_authenticated(first_remote, hub),
          "first transport for a node identity authenticates");
    check(hub_callbacks == 1, "first authenticated identity reaches node callback");

    std::string second_address;
    check(connect_remote(second_remote, second_address),
          "second transport claiming the same node identity connects");
    bool duplicate_rejected = false;
    for (int i = 0; i < 400; ++i) {
        pump_managers(second_remote, hub);
        const auto diagnostics = hub.get_connection_diagnostics();
        if (diagnostics.disconnect_reasons.count("duplicate-auth-identity") != 0 &&
            hub.get_stats().authenticated_peers == 1) {
            duplicate_rejected = true;
            break;
        }
    }

    check(duplicate_rejected,
          "second transport claiming an authenticated identity is disconnected");
    check(hub.get_stats().authenticated_peers == 1,
          "one cryptographic node identity consumes only one authenticated slot");
    check(hub_callbacks == 1,
          "duplicate identity never reaches the authenticated-peer callback");
}

void test_wire_ping_and_ready_callback() {
    const geocoin::bytes private_key = make_key(0x9a);

    {
        geocoin::network::PeerManager peers;
        LocalSocketPair sockets;
        const std::string address = "127.0.0.12:3030";
        if (!sockets) {
            check(false, "local socketpair is available for ping test");
            return;
        }

        check(peers.add_inbound_peer(sockets.release_first(), address, private_key),
              "inbound peer can be added for ping test");
        const auto ping = geocoin::network::create_message(geocoin::network::MessageType::PING, {});
        check(sockets.send_second(ping), "PING is written to peer socket");
        peers.update();

        geocoin::bytes reply;
        check(!sockets.recv_second(reply), "preauth PING receives no PONG");
        check(peers.get_peer(address) == nullptr, "preauth PING disconnects peer");
        const auto diagnostics = peers.get_connection_diagnostics();
        const auto preauth_reason = diagnostics.disconnect_reasons.find("preauth-message");
        check(preauth_reason != diagnostics.disconnect_reasons.end() && preauth_reason->second == 1,
              "preauth PING is rejected by handshake ordering");
    }

    {
        geocoin::network::PeerManager peers;
        LocalSocketPair sockets;
        const std::string address = "127.0.0.13:3030";
        int callbacks = 0;
        geocoin::network::MessageType callback_type = geocoin::network::MessageType::VERSION;
        size_t callback_payload_size = 0;

        if (!sockets) {
            check(false, "local socketpair is available for READY callback test");
            return;
        }

        peers.set_message_callback([&](std::shared_ptr<geocoin::network::Peer> peer,
                                       geocoin::network::MessageType type,
                                       const geocoin::bytes& payload) {
            check(peer != nullptr && peer->get_address() == address,
                  "READY callback receives expected peer");
            ++callbacks;
            callback_type = type;
            callback_payload_size = payload.size();
        });

        check(peers.add_inbound_peer(sockets.release_first(), address, private_key),
              "inbound peer can be added for READY callback test");
        auto client_channel =
            geocoin::network::PeerManagerTestAccess::activate_test_secure_channel(peers, address);
        check(client_channel != nullptr, "READY callback test activates encrypted transport");
        auto peer = peers.get_peer(address);
        check(peer != nullptr, "READY callback test peer exists");
        if (peer) {
            peer->set_state(geocoin::network::PeerState::READY);
        }

        geocoin::network::AddrMessage addr;
        const auto message = client_channel
            ? protect_test_frame(
                  *client_channel, geocoin::network::MessageType::ADDR, addr.serialize())
            : geocoin::bytes{};
        check(sockets.send_second(message), "ADDR is written to READY peer socket");
        peers.update();

        check(callbacks == 1, "READY application message reaches callback once");
        check(callback_type == geocoin::network::MessageType::ADDR,
              "READY callback receives ADDR type");
        check(callback_payload_size == addr.serialize().size(),
              "READY callback receives original payload");

        const auto stats = peers.get_peer_stats();
        check(stats.size() == 1 && stats[0].state == "ready" && stats[0].last_message_type == "addr",
              "READY callback test updates peer stats");
    }
}

void test_global_peer_update_budget_and_rotation() {
    geocoin::network::PeerManager peers;
    const geocoin::bytes private_key = make_key(0xe2);
    constexpr size_t peer_count = 10;
    constexpr size_t frames_per_peer =
        geocoin::network::detail::kMaxFramesPerPeerUpdate;

    std::vector<std::unique_ptr<LocalSocketPair>> sockets;
    std::vector<std::string> addresses;
    sockets.reserve(peer_count);
    addresses.reserve(peer_count);

    for (size_t i = 0; i < peer_count; ++i) {
        auto socket_pair = std::make_unique<LocalSocketPair>();
        if (!*socket_pair) {
            check(false, "socketpair is available for global P2P budget test");
            return;
        }
        const std::string address =
            "127.20.0." + std::to_string(i + 1) + ":3030";
        check(peers.add_inbound_peer(socket_pair->release_first(), address, private_key),
              "global P2P budget test adds an inbound peer");
        auto channel =
            geocoin::network::PeerManagerTestAccess::activate_test_secure_channel(peers, address);
        check(channel != nullptr, "global P2P budget test activates encrypted transport");
        auto peer = peers.get_peer(address);
        check(peer != nullptr, "global P2P budget test peer exists");
        if (!channel || !peer) {
            return;
        }
        peer->set_state(geocoin::network::PeerState::READY);

        geocoin::bytes records;
        for (size_t frame = 0; frame < frames_per_peer; ++frame) {
            const auto record = protect_test_frame(
                *channel, geocoin::network::MessageType::VERACK, {});
            records.insert(records.end(), record.begin(), record.end());
        }
        check(socket_pair->send_second(records),
              "global P2P budget test writes the encrypted frame batch");
        addresses.push_back(address);
        sockets.push_back(std::move(socket_pair));
    }

    auto total_processed = [&]() {
        uint64_t total = 0;
        for (const auto& address : addresses) {
            total += geocoin::network::PeerManagerTestAccess::received_message_count(
                peers, address, geocoin::network::MessageType::VERACK);
        }
        return total;
    };

    peers.update();
    check(total_processed() == geocoin::network::detail::kMaxFramesPerManagerUpdate,
          "one PeerManager update enforces the global frame budget");

    peers.update();
    check(total_processed() == peer_count * frames_per_peer,
          "round-robin update gives deferred peers service on the next cycle");
}

void test_timeout_disconnect_and_policy_decay_paths() {
    const geocoin::bytes private_key = make_key(0xac);

    {
        geocoin::network::PeerManager peers;
        LocalSocketPair sockets;
        const std::string address = "127.0.0.80:3030";
        if (!sockets) {
            check(false, "local socketpair is available for transport timeout test");
            return;
        }

        check(peers.add_inbound_peer(sockets.release_first(), address, private_key),
              "inbound peer can be added for transport-disconnected test");
        auto peer = peers.get_peer(address);
        check(peer != nullptr, "transport-disconnected peer exists");
        if (peer) {
            peer->disconnect();
        }
        peers.update();
        check(peers.get_peer(address) == nullptr,
              "update removes peer that closed transport");
        auto diagnostics = peers.get_connection_diagnostics();
        check(diagnostics.disconnect_reasons["transport-disconnected"] == 1,
              "transport-disconnected reason is recorded");
    }

    {
        geocoin::network::PeerManager peers;
        LocalSocketPair sockets;
        const std::string address = "127.0.0.81:3030";
        if (!sockets) {
            check(false, "local socketpair is available for version-stall test");
            return;
        }

        check(peers.add_inbound_peer(sockets.release_first(), address, private_key),
              "inbound peer can be added for version-stall test");
        check(geocoin::network::PeerManagerTestAccess::age_connected_context(
                  peers,
                  address,
                  std::chrono::seconds(geocoin::network::detail::kVersionStallSeconds + 1)),
              "version-stall test ages connected context");
        peers.update();
        check(peers.get_peer(address) == nullptr,
              "version-stall peer is disconnected without waiting in real time");
        auto diagnostics = peers.get_connection_diagnostics();
        check(diagnostics.disconnect_reasons["version-stall"] == 1,
              "version-stall reason is recorded");
    }

    {
        geocoin::network::PeerManager peers;
        LocalSocketPair sockets;
        const std::string address = "127.0.0.82:3030";
        if (!sockets) {
            check(false, "local socketpair is available for auth-timeout test");
            return;
        }

        check(peers.add_inbound_peer(sockets.release_first(), address, private_key),
              "inbound peer can be added for auth-timeout test");
        check(geocoin::network::PeerManagerTestAccess::start_test_auth_session(peers, address, private_key),
              "auth-timeout test starts synthetic auth session");
        check(geocoin::network::PeerManagerTestAccess::age_auth_last_activity(
                  peers,
                  address,
                  std::chrono::seconds(geocoin::network::detail::kAuthTimeoutSeconds + 1)),
              "auth-timeout test ages auth session");
        peers.update();
        check(peers.get_peer(address) == nullptr,
              "auth-timeout peer is disconnected without waiting in real time");
        auto diagnostics = peers.get_connection_diagnostics();
        check(diagnostics.disconnect_reasons["auth-timeout"] == 1,
              "auth-timeout reason is recorded");
    }

    {
        geocoin::network::PeerManager peers;
        LocalSocketPair sockets;
        const std::string address = "127.0.0.83:3030";
        if (!sockets) {
            check(false, "local socketpair is available for auth-progress-stall test");
            return;
        }

        check(peers.add_inbound_peer(sockets.release_first(), address, private_key),
              "inbound peer can be added for auth-progress-stall test");
        check(geocoin::network::PeerManagerTestAccess::start_test_auth_session(peers, address, private_key),
              "auth-progress-stall test starts synthetic auth session");
        check(geocoin::network::PeerManagerTestAccess::age_auth_progress(
                  peers,
                  address,
                  std::chrono::seconds(geocoin::network::detail::kAuthProgressStallSeconds + 1)),
              "auth-progress-stall test ages useful-message timestamp");
        peers.update();
        check(peers.get_peer(address) == nullptr,
              "auth-progress-stall peer is disconnected without waiting in real time");
        auto diagnostics = peers.get_connection_diagnostics();
        check(diagnostics.disconnect_reasons["auth-progress-stall"] == 1,
              "auth-progress-stall reason is recorded");
    }

    {
        geocoin::network::PeerManager peers;
        LocalSocketPair sockets;
        const std::string address = "127.0.0.84:3030";
        if (!sockets) {
            check(false, "local socketpair is available for idle-timeout test");
            return;
        }

        check(peers.add_inbound_peer(sockets.release_first(), address, private_key),
              "inbound peer can be added for idle-timeout test");
        check(geocoin::network::PeerManagerTestAccess::force_ready_idle(
                  peers,
                  address,
                  std::chrono::seconds(geocoin::network::detail::kPeerTimeoutSeconds + 1)),
              "idle-timeout test ages ready peer last_seen");
        peers.update();
        check(peers.get_peer(address) == nullptr,
              "idle-timeout peer is disconnected without waiting in real time");
        auto diagnostics = peers.get_connection_diagnostics();
        check(diagnostics.disconnect_reasons["idle-timeout"] == 1,
              "idle-timeout reason is recorded");
    }

    {
        geocoin::network::PeerManager peers;
        LocalSocketPair sockets;
        const std::string address = "127.0.0.85:3030";
        if (!sockets) {
            check(false, "local socketpair is available for ping-timeout test");
            return;
        }

        check(peers.add_inbound_peer(sockets.release_first(), address, private_key),
              "inbound peer can be added for ping-timeout test");
        check(geocoin::network::PeerManagerTestAccess::force_ready_waiting_for_pong(
                  peers,
                  address,
                  std::chrono::seconds(geocoin::network::detail::kPingReplyTimeoutSeconds + 1)),
              "ping-timeout test ages pending ping");
        peers.update();
        check(peers.get_peer(address) == nullptr,
              "ping-timeout peer is disconnected without waiting in real time");
        auto diagnostics = peers.get_connection_diagnostics();
        check(diagnostics.disconnect_reasons["ping-timeout"] == 1,
              "ping-timeout reason is recorded");
    }

    {
        geocoin::network::PeerManager peers;
        LocalSocketPair sockets;
        const std::string address = "127.0.0.86:3030";
        if (!sockets) {
            check(false, "local socketpair is available for policy-decay test");
            return;
        }

        check(peers.add_inbound_peer(sockets.release_first(), address, private_key),
              "inbound peer can be added for policy-decay test");
        check(geocoin::network::PeerManagerTestAccess::set_policy_score_for_decay(
                  peers,
                  address,
                  17,
                  geocoin::network::detail::kPolicyScoreDecayInterval * 3),
              "policy-decay test seeds stale policy score");
        peers.update();
        const auto stats = peers.get_peer_stats();
        check(stats.size() == 1 && stats[0].policy_score < 17,
              "policy score decays during update without disconnecting peer");
    }
}

void test_inbound_diversity_and_stats_saturation() {
    const geocoin::bytes private_key = make_key(0xb4);

    {
        geocoin::network::PeerManager peers;
        peers.set_max_peers(64, 0);
        std::vector<std::unique_ptr<LocalSocketPair>> sockets;

        for (size_t i = 0; i < geocoin::network::detail::kPolicyMaxInboundPerIp; ++i) {
            auto pair = std::make_unique<LocalSocketPair>();
            if (!*pair) {
                check(false, "socketpair is available for same-IP diversity test");
                return;
            }
            const std::string address = "127.0.10.10:" + std::to_string(3000 + i);
            check(peers.add_inbound_peer(pair->release_first(), address, private_key),
                  "same-IP inbound peer is accepted until per-IP limit");
            sockets.push_back(std::move(pair));
        }

        LocalSocketPair rejected;
        if (!rejected) {
            check(false, "socketpair is available for same-IP rejection");
            return;
        }
        check(!peers.add_inbound_peer(
                  rejected.release_first(),
                  "127.0.10.10:3999",
                  private_key),
              "same-IP inbound diversity limit rejects the next connection");
    }

    {
        geocoin::network::PeerManager peers;
        peers.set_max_peers(80, 0);
        std::vector<std::unique_ptr<LocalSocketPair>> sockets;

        for (size_t i = 0; i < geocoin::network::detail::kPolicyMaxInboundPerGroup; ++i) {
            auto pair = std::make_unique<LocalSocketPair>();
            if (!*pair) {
                check(false, "socketpair is available for group diversity test");
                return;
            }
            const std::string address = "127.0.11." + std::to_string(i + 1) + ":3030";
            check(peers.add_inbound_peer(pair->release_first(), address, private_key),
                  "same-group inbound peer is accepted until group limit");
            sockets.push_back(std::move(pair));
        }

        LocalSocketPair rejected;
        if (!rejected) {
            check(false, "socketpair is available for same-group rejection");
            return;
        }
        check(!peers.add_inbound_peer(
                  rejected.release_first(),
                  "127.0.11.250:3030",
                  private_key),
              "same /24 inbound diversity limit rejects the next connection");
    }

    {
        geocoin::network::PeerManager peers;
        LocalSocketPair first;
        LocalSocketPair second;
        if (!first || !second) {
            check(false, "socketpairs are available for stats saturation test");
            return;
        }

        const std::string first_addr = "127.0.12.1:3030";
        const std::string second_addr = "127.0.12.2:3030";
        check(peers.add_inbound_peer(first.release_first(), first_addr, private_key),
              "first peer is added for stats saturation");
        check(peers.add_inbound_peer(second.release_first(), second_addr, private_key),
              "second peer is added for stats saturation");
        check(geocoin::network::PeerManagerTestAccess::set_peer_counters(
                  peers,
                  first_addr,
                  (std::numeric_limits<uint64_t>::max)() - 1,
                  (std::numeric_limits<uint64_t>::max)() - 2),
              "first peer counters are seeded near saturation");
        check(geocoin::network::PeerManagerTestAccess::set_peer_counters(
                  peers,
                  second_addr,
                  10,
                  20),
              "second peer counters are seeded beyond remaining headroom");

        const auto stats = peers.get_stats();
        check(stats.total_bytes_sent == (std::numeric_limits<uint64_t>::max)(),
              "PeerManager sent byte stats saturate instead of overflowing");
        check(stats.total_bytes_received == (std::numeric_limits<uint64_t>::max)(),
              "PeerManager received byte stats saturate instead of overflowing");
    }
}

void test_ping_rtt_and_scheduled_ping_paths() {
    const geocoin::bytes private_key = make_key(0xb8);

    {
        geocoin::network::PeerManager peers;
        LocalSocketPair sockets;
        const std::string address = "127.0.13.1:3030";
        if (!sockets) {
            check(false, "socketpair is available for PONG RTT test");
            return;
        }

        check(peers.add_inbound_peer(sockets.release_first(), address, private_key),
              "inbound peer can be added for PONG RTT test");
        auto client_channel =
            geocoin::network::PeerManagerTestAccess::activate_test_secure_channel(peers, address);
        check(client_channel != nullptr, "PONG RTT test activates encrypted transport");
        check(geocoin::network::PeerManagerTestAccess::mark_ping_sent(
                  peers,
                  address,
                  std::chrono::milliseconds(5)),
              "PONG RTT test marks a pending ping");
        const auto pong = client_channel
            ? protect_test_frame(*client_channel, geocoin::network::MessageType::PONG, {})
            : geocoin::bytes{};
        check(sockets.send_second(pong), "PONG is written to peer socket");
        peers.update();
        const auto stats = peers.get_peer_stats();
        check(stats.size() == 1 && stats[0].ping_rtt_ms > 0,
              "PONG updates peer RTT diagnostics");
        check(peers.get_peer(address) != nullptr,
              "valid PONG keeps peer connected");
    }

    {
        geocoin::network::PeerManager peers;
        LocalSocketPair sockets;
        const std::string address = "127.0.13.2:3030";
        if (!sockets) {
            check(false, "socketpair is available for scheduled PING test");
            return;
        }

        check(peers.add_inbound_peer(sockets.release_first(), address, private_key),
              "inbound peer can be added for scheduled PING test");
        auto client_channel =
            geocoin::network::PeerManagerTestAccess::activate_test_secure_channel(peers, address);
        check(client_channel != nullptr, "scheduled PING test activates encrypted transport");
        check(geocoin::network::PeerManagerTestAccess::age_last_ping(
                  peers,
                  address,
                  std::chrono::seconds(geocoin::network::detail::kPingIntervalSeconds + 1)),
              "scheduled PING test ages last_ping");
        peers.update();

        geocoin::bytes outbound_record;
        check(sockets.recv_second(outbound_record), "scheduled PING is sent to socket peer");
        geocoin::bytes outbound;
        check(client_channel && client_channel->unprotect(outbound_record, outbound),
              "scheduled PING record decrypts");
        geocoin::network::MessageHeader header;
        geocoin::bytes payload;
        check(geocoin::network::parse_message(outbound, header, payload),
              "scheduled PING frame parses");
        check(header.type == geocoin::network::MessageType::PING && payload.empty(),
              "scheduled keepalive sends empty PING");
        check(peers.get_peer(address) != nullptr,
              "successful scheduled PING keeps peer connected");
    }
}

void test_disconnect_peer_pointer_fallback() {
    geocoin::network::PeerManager peers;
    LocalSocketPair sockets;
    if (!sockets) {
        check(false, "socketpair is available for pointer fallback disconnect test");
        return;
    }

    auto peer = std::make_shared<geocoin::network::Peer>(
        "127.0.14.1:3030",
        sockets.release_first()
    );
    geocoin::network::PeerManagerTestAccess::insert_alias_peer(
        peers,
        "127.0.14.2:3030",
        peer,
        true
    );

    check(peers.get_peer_count() == 1,
          "alias inserted peer is counted before pointer fallback disconnect");
    check(peers.disconnect_peer(peer, "pointer-fallback"),
          "disconnect_peer(peer) finds a tracked peer by pointer when address lookup misses");
    check(peers.get_peer_count() == 0,
          "pointer fallback disconnect removes alias entry");

    const auto diagnostics = peers.get_connection_diagnostics();
    const auto pointer_reason = diagnostics.disconnect_reasons.find("pointer-fallback");
    check(pointer_reason != diagnostics.disconnect_reasons.end() && pointer_reason->second == 1,
          "pointer fallback disconnect reason is recorded");
}

void test_ready_inv_rate_limit_disconnects() {
    geocoin::network::PeerManager peers;
    const geocoin::bytes private_key = make_key(0xbc);
    LocalSocketPair sockets;
    const std::string address = "127.0.15.1:3030";
    if (!sockets) {
        check(false, "socketpair is available for INV rate-limit test");
        return;
    }

    check(peers.add_inbound_peer(sockets.release_first(), address, private_key),
          "inbound peer can be added for INV rate-limit test");
    auto client_channel =
        geocoin::network::PeerManagerTestAccess::activate_test_secure_channel(peers, address);
    check(client_channel != nullptr, "INV rate-limit test activates encrypted transport");
    auto peer = peers.get_peer(address);
    check(peer != nullptr, "INV rate-limit peer exists");
    if (!peer) {
        return;
    }
    peer->set_state(geocoin::network::PeerState::READY);

    geocoin::network::InvMessage inv;
    inv.items.push_back({geocoin::network::InvMessage::InvType::TX, hash_from_counter(1501)});
    for (size_t i = 0; i <= geocoin::network::detail::kPolicyMaxInvMsgsPerWindow; ++i) {
        const auto message = client_channel
            ? protect_test_frame(
                  *client_channel, geocoin::network::MessageType::INV, inv.serialize())
            : geocoin::bytes{};
        check(sockets.send_second(message), "INV rate-limit frame is written to peer socket");
    }
    for (size_t i = 0; i < 8 && peers.get_peer(address) != nullptr; ++i) {
        peers.update();
    }

    check(peers.get_peer(address) == nullptr,
          "INV rate-limit disconnects noisy READY peer");
    const auto diagnostics = peers.get_connection_diagnostics();
    const auto disconnect_reason = diagnostics.disconnect_reasons.find("inv-rate-limit");
    check(disconnect_reason != diagnostics.disconnect_reasons.end() && disconnect_reason->second == 1,
          "INV rate-limit disconnect reason is recorded");
    const auto policy_reason = diagnostics.policy_rejects.find("inv-rate-limit");
    check(policy_reason != diagnostics.policy_rejects.end() && policy_reason->second == 1,
          "INV rate-limit policy reject is recorded");
}

void test_ready_sync_burst_does_not_trip_rate_limit() {
    geocoin::network::PeerManager peers;
    const geocoin::bytes private_key = make_key(0xc0);
    LocalSocketPair sockets;
    const std::string address = "127.0.15.5:3030";
    if (!sockets) {
        check(false, "socketpair is available for honest sync burst test");
        return;
    }

    check(peers.add_inbound_peer(sockets.release_first(), address, private_key),
          "inbound peer can be added for honest sync burst test");
    auto client_channel =
        geocoin::network::PeerManagerTestAccess::activate_test_secure_channel(peers, address);
    check(client_channel != nullptr, "honest sync burst test activates encrypted transport");
    auto peer = peers.get_peer(address);
    check(peer != nullptr, "honest sync burst peer exists");
    if (!peer || !client_channel) {
        return;
    }
    peer->set_state(geocoin::network::PeerState::READY);

    // These are the smallest bursts that exceeded the former production
    // limits during a four-node loadnet catch-up (8 GETDATA and 32 BLOCK per
    // ten seconds). They are normal scheduler traffic, not an abuse flood.
    geocoin::network::InvMessage getdata;
    getdata.items.push_back({
        geocoin::network::InvMessage::InvType::BLOCK,
        hash_from_counter(1601)
    });
    for (size_t i = 0; i < 9; ++i) {
        const auto frame = protect_test_frame(
            *client_channel,
            geocoin::network::MessageType::GETDATA,
            getdata.serialize()
        );
        check(sockets.send_second(frame), "honest GETDATA burst frame is written");
    }
    for (size_t i = 0; i < 33; ++i) {
        const auto frame = protect_test_frame(
            *client_channel,
            geocoin::network::MessageType::BLOCK,
            {}
        );
        check(sockets.send_second(frame), "honest BLOCK burst frame is written");
    }
    for (size_t i = 0; i < 8 && peers.get_peer(address) != nullptr; ++i) {
        peers.update();
    }

    check(peers.get_peer(address) != nullptr,
          "normal block-sync refill burst keeps the READY peer connected");
    const auto diagnostics = peers.get_connection_diagnostics();
    check(diagnostics.policy_rejects.count("getdata-rate-limit") == 0,
          "normal GETDATA refill burst is not classified as abuse");
    check(diagnostics.policy_rejects.count("block-rate-limit") == 0,
          "normal BLOCK response burst is not classified as abuse");
}

void test_ready_addr_rate_limit_disconnects() {
    geocoin::network::PeerManager peers;
    const geocoin::bytes private_key = make_key(0xbd);
    LocalSocketPair sockets;
    const std::string address = "127.0.15.2:3030";
    if (!sockets) {
        check(false, "socketpair is available for ADDR rate-limit test");
        return;
    }

    check(peers.add_inbound_peer(sockets.release_first(), address, private_key),
          "inbound peer can be added for ADDR rate-limit test");
    auto client_channel =
        geocoin::network::PeerManagerTestAccess::activate_test_secure_channel(peers, address);
    check(client_channel != nullptr, "ADDR rate-limit test activates encrypted transport");
    auto peer = peers.get_peer(address);
    check(peer != nullptr, "ADDR rate-limit peer exists");
    if (!peer) {
        return;
    }
    peer->set_state(geocoin::network::PeerState::READY);

    geocoin::network::AddrMessage addr;
    addr.addresses.push_back({0, "8.8.8.8", 3030, 1});
    for (size_t i = 0; i <= geocoin::network::detail::kPolicyMaxAddrMsgsPerWindow; ++i) {
        const auto message = client_channel
            ? protect_test_frame(
                  *client_channel, geocoin::network::MessageType::ADDR, addr.serialize())
            : geocoin::bytes{};
        check(sockets.send_second(message), "ADDR rate-limit frame is written to peer socket");
    }
    for (size_t i = 0; i < 8 && peers.get_peer(address) != nullptr; ++i) {
        peers.update();
    }

    check(peers.get_peer(address) == nullptr,
          "ADDR rate-limit disconnects noisy READY peer");
    const auto diagnostics = peers.get_connection_diagnostics();
    const auto disconnect_reason = diagnostics.disconnect_reasons.find("addr-rate-limit");
    check(disconnect_reason != diagnostics.disconnect_reasons.end() && disconnect_reason->second == 1,
          "ADDR rate-limit disconnect reason is recorded");
    const auto policy_reason = diagnostics.policy_rejects.find("addr-rate-limit");
    check(policy_reason != diagnostics.policy_rejects.end() && policy_reason->second == 1,
          "ADDR rate-limit policy reject is recorded");
}

void test_ready_addr_size_limits_disconnect() {
    {
        geocoin::network::PeerManager peers;
        const geocoin::bytes private_key = make_key(0xbe);
        LocalSocketPair sockets;
        const std::string address = "127.0.15.3:3030";
        if (!sockets) {
            check(false, "socketpair is available for ADDR item-limit test");
            return;
        }

        check(peers.add_inbound_peer(sockets.release_first(), address, private_key),
              "inbound peer can be added for ADDR item-limit test");
        auto client_channel =
            geocoin::network::PeerManagerTestAccess::activate_test_secure_channel(peers, address);
        check(client_channel != nullptr, "ADDR item-limit test activates encrypted transport");
        auto peer = peers.get_peer(address);
        check(peer != nullptr, "ADDR item-limit peer exists");
        if (!peer) {
            return;
        }
        peer->set_state(geocoin::network::PeerState::READY);

        geocoin::network::AddrMessage addr;
        for (size_t i = 0; i <= geocoin::network::detail::kPolicyMaxAddrItems; ++i) {
            addr.addresses.push_back({0, "8.8.8.8", 3030, 1});
        }
        const auto message = client_channel
            ? protect_test_frame(
                  *client_channel, geocoin::network::MessageType::ADDR, addr.serialize())
            : geocoin::bytes{};
        check(sockets.send_second(message), "oversized ADDR item list is written to peer socket");
        for (size_t i = 0; i < 8 && peers.get_peer(address) != nullptr; ++i) {
            peers.update();
        }

        check(peers.get_peer(address) == nullptr,
              "ADDR item limit disconnects a READY peer before the Node callback");
        const auto diagnostics = peers.get_connection_diagnostics();
        const auto policy_reason = diagnostics.policy_rejects.find("addr-too-many-items");
        check(policy_reason != diagnostics.policy_rejects.end() && policy_reason->second == 1,
              "ADDR item-limit policy reject is recorded");
    }

    {
        geocoin::network::PeerManager peers;
        const geocoin::bytes private_key = make_key(0xbf);
        LocalSocketPair sockets;
        const std::string address = "127.0.15.4:3030";
        if (!sockets) {
            check(false, "socketpair is available for ADDR payload-limit test");
            return;
        }

        check(peers.add_inbound_peer(sockets.release_first(), address, private_key),
              "inbound peer can be added for ADDR payload-limit test");
        auto client_channel =
            geocoin::network::PeerManagerTestAccess::activate_test_secure_channel(peers, address);
        check(client_channel != nullptr, "ADDR payload-limit test activates encrypted transport");
        auto peer = peers.get_peer(address);
        check(peer != nullptr, "ADDR payload-limit peer exists");
        if (!peer) {
            return;
        }
        peer->set_state(geocoin::network::PeerState::READY);

        geocoin::bytes oversized_payload(
            geocoin::network::detail::kPolicyMaxAddrPayloadBytes + 1,
            0
        );
        const auto message = client_channel
            ? protect_test_frame(
                  *client_channel, geocoin::network::MessageType::ADDR, oversized_payload)
            : geocoin::bytes{};
        check(sockets.send_second(message), "oversized ADDR payload is written to peer socket");
        for (size_t i = 0; i < 8 && peers.get_peer(address) != nullptr; ++i) {
            peers.update();
        }

        check(peers.get_peer(address) == nullptr,
              "ADDR payload byte limit disconnects a READY peer before deserialization");
        const auto diagnostics = peers.get_connection_diagnostics();
        const auto policy_reason = diagnostics.policy_rejects.find("message-payload-limit");
        check(policy_reason != diagnostics.policy_rejects.end() && policy_reason->second == 1,
              "ADDR payload-limit is rejected from its header length");
    }
}

} // namespace

int main() {
    test_peer_direct_socket_and_inventory_policy();
    test_bare_host_validation();
    test_inbound_address_validation();
    test_inbound_handshake_throttling();
    test_public_policy_defaults_and_limits();
    test_inbound_lifecycle_and_disconnect_diagnostics();
    test_manual_bans_and_added_nodes();
    test_outbound_attempt_pruning_preserves_manual_and_added_targets();
    test_temp_ban_bounds_and_ipv6_grouping();
    test_outbound_self_connect_and_backoff_diagnostics();
    test_inbound_eviction_when_full();
    test_wire_policy_disconnects();
    test_reject_message_handler_records_valid_and_malformed_payloads();
    test_p2p_server_lifecycle_accept_and_whitelist();
    test_handshake_version_and_auth_rejections();
    test_authenticated_peer_manager_flow_and_relay();
    test_duplicate_authenticated_identity_uses_one_slot();
    test_wire_ping_and_ready_callback();
    test_global_peer_update_budget_and_rotation();
    test_timeout_disconnect_and_policy_decay_paths();
    test_inbound_diversity_and_stats_saturation();
    test_ping_rtt_and_scheduled_ping_paths();
    test_disconnect_peer_pointer_fallback();
    test_ready_inv_rate_limit_disconnects();
    test_ready_sync_burst_does_not_trip_rate_limit();
    test_ready_addr_size_limits_disconnect();
    test_ready_addr_rate_limit_disconnects();

    if (failures == 0) {
        std::cout << "Peer policy tests passed.\n";
        return 0;
    }

    std::cerr << failures << " peer policy test(s) failed.\n";
    return 1;
}
