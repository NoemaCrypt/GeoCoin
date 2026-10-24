#include "geocoin/network/addrman.hpp"
#include "geocoin/network/dns_resolver.hpp"
#include "geocoin/network/dns_seeds.h"
#include "geocoin/network/p2p_server.hpp"
#include "geocoin/network/peer_discovery.hpp"
#include "geocoin/network/peer_manager.hpp"
#include "geocoin/network/peer_manager_internal.hpp"
#include "secure_channel_test_fixture.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>
#include <utility>

#ifndef _WIN32
#include <arpa/inet.h>
#include <csignal>
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace geocoin::network {
struct PeerDiscoveryTestAccess {
    static void use_loopback_seed(PeerDiscovery& discovery) {
        std::lock_guard<std::mutex> lock(discovery.mutex_);
        discovery.dns_seeds_ = {"127.0.0.1"};
    }
};
struct PeerManagerTestAccess {
    static std::shared_ptr<Peer> insert(PeerManager& manager, const std::string& address,
                                       std::unique_ptr<PeerTransport> transport, bool ping_due = false) {
        std::lock_guard<std::mutex> lock(manager.mutex_);
        PeerManager::PeerContext ctx;
        ctx.peer = std::make_shared<Peer>(address, std::move(transport));
        const auto now = std::chrono::steady_clock::now();
        ctx.connected_at = ctx.last_message_at = ctx.last_useful_message_at = now;
        ctx.last_ping = ping_due ? now - std::chrono::seconds(31) : now;
        auto result = ctx.peer;
        manager.peers_[address] = std::move(ctx);
        return result;
    }
    static std::unique_ptr<SecureChannel> ready(PeerManager& manager, const std::string& address) {
        std::lock_guard<std::mutex> lock(manager.mutex_);
        auto& ctx = manager.peers_.at(address);
        auto fixture = test::make_secure_channel_fixture();
        if (!ctx.peer->activate_secure_channel(std::move(fixture.peer_keys))) {
            throw std::runtime_error("secure fixture failed");
        }
        ctx.secure_established = true;
        ctx.channel_binding = fixture.channel_binding;
        ctx.peer->set_state(PeerState::READY);
        return std::move(fixture.remote_channel);
    }
};
} // namespace geocoin::network

namespace {
using namespace geocoin;
using namespace geocoin::network;
using namespace std::chrono_literals;
int checks = 0;

void check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
    ++checks;
}

struct TemporaryDirectory {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("geocoin-p2p-review-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    TemporaryDirectory() { std::filesystem::create_directories(path); }
    ~TemporaryDirectory() { std::error_code ec; std::filesystem::remove_all(path, ec); }
};

class CallbackTransport : public PeerTransport {
public:
    bool open = true;
    std::function<std::ptrdiff_t()> on_send;
    bool connect(std::chrono::milliseconds) override { return open; }
    void close() noexcept override { open = false; }
    bool is_open() const noexcept override { return open; }
    std::ptrdiff_t send(const bytes& data) override {
        return on_send ? on_send() : static_cast<std::ptrdiff_t>(data.size());
    }
    std::ptrdiff_t receive(bytes& data, size_t) override { data.clear(); return 0; }
};

void test_session_replacement() {
    PeerManager manager;
    const std::string address = "127.30.0.3:3030";
    auto transport = std::make_unique<CallbackTransport>();
    auto* observer = transport.get();
    auto old_peer = PeerManagerTestAccess::insert(manager, address, std::move(transport), true);
    auto channel = PeerManagerTestAccess::ready(manager, address);
    std::shared_ptr<Peer> replacement;
    observer->on_send = [&] {
        // The manager releases its mutex for PING. Model a reconnect that
        // replaces the map entry during that unlocked interval.
        replacement = PeerManagerTestAccess::insert(manager, address, std::make_unique<CallbackTransport>());
        return -1;
    };
    manager.update();
    check(replacement && replacement->is_connected(), "old PING failure must not disconnect replacement session");
    check(manager.get_peer(address) == replacement, "replacement remains registered");
    check(!old_peer->is_connected(), "failed old session is closed");
}

#ifndef _WIN32
struct Fd {
    int value;
    explicit Fd(int fd = -1) : value(fd) {}
    ~Fd() { if (value >= 0) ::close(value); }
    int release() { return std::exchange(value, -1); }
};

void test_buffered_eof() {
    PeerManager manager;
    int pair[2];
    check(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0, "socketpair");
    Fd remote(pair[1]);
    const std::string address = "127.30.0.1:3030";
    check(manager.add_inbound_peer(pair[0], address, bytes(32, 7)), "inbound admission");
    auto channel = PeerManagerTestAccess::ready(manager, address);
    bytes batch;
    for (size_t i = 0; i < detail::kMaxFramesPerPeerUpdate; ++i) {
        bytes record;
        check(channel->protect(create_message(MessageType::VERACK, {}), record), "encrypt prefix");
        batch.insert(batch.end(), record.begin(), record.end());
    }
    bytes last;
    check(channel->protect(create_message(MessageType::ADDR, AddrMessage{}.serialize()), last), "encrypt final message");
    batch.insert(batch.end(), last.begin(), last.end());
    size_t delivered = 0;
    manager.set_message_callback([&](auto, auto type, const auto&) {
        if (type == MessageType::ADDR) ++delivered;
    });
    check(::send(remote.value, batch.data(), batch.size(), 0) == static_cast<ssize_t>(batch.size()), "write batch");
    check(::shutdown(remote.value, SHUT_WR) == 0, "orderly remote EOF");
    manager.update();
    check(delivered == 0, "first update honors frame budget");
    manager.update();
    check(delivered == 1, "buffered application message is delivered before EOF");
    manager.update();
    check(!manager.get_peer(address), "EOF removes the drained peer");
    check(manager.get_connection_diagnostics().policy_rejects.empty(), "orderly EOF never penalizes peer");
}

void test_invalid_inbound() {
    PeerManager manager;
    check(!manager.add_inbound_peer(-1, "127.30.0.2:3030", bytes(32, 7)), "invalid socket cannot be admitted");
    check(manager.get_stats().total_peers == 0, "invalid socket leaves no peer entry");
    int pair[2];
    check(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0, "socketpair");
    Fd remote(pair[1]);
    check(!manager.add_inbound_peer(pair[0], "invalid", bytes(32, 7)), "reject malformed endpoint");
    char byte;
    check(::recv(remote.value, &byte, 1, MSG_DONTWAIT) == 0, "rejected inbound socket is released");
}

void test_callback_failure() {
    PeerManager manager;
    int pair[2];
    check(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0, "socketpair");
    Fd remote(pair[1]);
    const std::string address = "127.30.0.4:3030";
    check(manager.add_inbound_peer(pair[0], address, bytes(32, 7)), "inbound admission");
    auto channel = PeerManagerTestAccess::ready(manager, address);
    size_t called = 0;
    manager.set_message_callback([&](auto, auto, const auto&) {
        if (++called == 1) throw std::runtime_error("local application failure");
    });
    for (int i = 0; i < 2; ++i) {
        bytes record;
        check(channel->protect(create_message(MessageType::ADDR, AddrMessage{}.serialize()), record), "encrypt application message");
        check(::send(remote.value, record.data(), record.size(), 0) == static_cast<ssize_t>(record.size()), "send application message");
    }
    manager.update();
    check(called == 2, "local callback failure does not discard other callbacks");
    check(manager.get_peer(address)->is_connected(), "local callback failure preserves peer");
    check(manager.get_connection_diagnostics().policy_rejects.empty(), "local callback failure does not penalize peer");
}

void test_high_fd(bool send_path) {
    rlimit original_limit{};
    check(::getrlimit(RLIMIT_NOFILE, &original_limit) == 0, "read descriptor limit");
    const rlim_t required_limit = FD_SETSIZE + 64;
    if (original_limit.rlim_max < required_limit) {
        std::cout << "SKIP: process hard limit cannot accommodate high descriptor test\n";
        std::exit(77);
    }
    struct RestoreLimit {
        rlimit value;
        ~RestoreLimit() { ::setrlimit(RLIMIT_NOFILE, &value); }
    } restore_limit{original_limit};
    if (original_limit.rlim_cur < required_limit) {
        rlimit raised = original_limit;
        raised.rlim_cur = required_limit;
        check(::setrlimit(RLIMIT_NOFILE, &raised) == 0, "raise test process soft descriptor limit");
    }
    Fd listener(::socket(AF_INET, SOCK_STREAM, 0));
    check(listener.value >= 0, "listener socket");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    check(::bind(listener.value, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "bind loopback");
    check(::listen(listener.value, 1) == 0, "listen");
    socklen_t length = sizeof(address);
    check(::getsockname(listener.value, reinterpret_cast<sockaddr*>(&address), &length) == 0, "bound port");
    std::vector<std::unique_ptr<Fd>> reserved;
    while (reserved.empty() || reserved.back()->value < FD_SETSIZE + 16) {
        auto fd = std::make_unique<Fd>(::open("/dev/null", O_RDONLY));
        check(fd->value >= 0, "reserve descriptors beyond fd_set capacity");
        reserved.push_back(std::move(fd));
    }
    auto transport = make_tcp_peer_transport("127.0.0.1:" + std::to_string(ntohs(address.sin_port)));
    check(transport->connect(1s), "connect works with descriptor above FD_SETSIZE");
    Fd accepted(::accept(listener.value, nullptr, nullptr));
    check(accepted.value >= 0, "accept high descriptor connection");
    if (!send_path) return;

    // A socketpair and small send buffer force the writable-wait path.
    int pair[2];
    check(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0, "send socketpair");
    Fd reader(pair[1]);
    int capacity = 4096;
    check(::setsockopt(pair[0], SOL_SOCKET, SO_SNDBUF, &capacity, sizeof(capacity)) == 0, "small send buffer");
    auto sending = make_tcp_peer_transport("local:3030", pair[0]);
    const bytes payload(24000, 0x5a);
    bytes received;
    std::thread drain([&] {
        std::this_thread::sleep_for(20ms);
        bytes chunk(32768);
        while (received.size() < payload.size()) {
            auto n = ::recv(reader.value, chunk.data(), chunk.size(), 0);
            if (n <= 0) break;
            received.insert(received.end(), chunk.begin(), chunk.begin() + n);
        }
    });
    const auto sent = sending->send(payload);
    sending->close();
    drain.join();
    check(sent == static_cast<std::ptrdiff_t>(payload.size()), "high descriptor writable wait completes send");
    check(received == payload, "high descriptor send preserves every byte");
}

void test_address_store() {
    TemporaryDirectory dir;
    const auto path = dir.path / "peers.dat";
    const std::string original = "127.0.0.1:3030 0 0 0\n";
    { std::ofstream file(path); file << original; }
    AddrMan manager;
    check(manager.add("127.0.0.2:3030"), "address to save");
    rlimit previous{};
    check(::getrlimit(RLIMIT_FSIZE, &previous) == 0, "read file limit");
    const auto old_handler = std::signal(SIGXFSZ, SIG_IGN);
    rlimit limited = previous;
    limited.rlim_cur = 0;
    check(::setrlimit(RLIMIT_FSIZE, &limited) == 0, "inject write failure");
    const bool saved = manager.save(path.string());
    const int restored = ::setrlimit(RLIMIT_FSIZE, &previous);
    std::signal(SIGXFSZ, old_handler);
    check(restored == 0, "restore file limit");
    check(!saved, "save reports the write failure");
    std::ifstream file(path);
    check(std::string(std::istreambuf_iterator<char>(file), {}) == original, "failed save preserves previous address book");
    check(manager.save(path.string()), "save retries successfully");
    AddrMan restored_manager;
    check(restored_manager.load(path.string()), "reload saved address book");
    check(restored_manager.get_all() == manager.get_all(), "successful save round trip");
}
#endif

void test_discovery_thread() {
    TemporaryDirectory dir;
    PeerDiscovery discovery((dir.path / "peers.dat").string());
    // No seeds: this thread finishes without performing DNS or network I/O.
    discovery.start_dns_thread(0, true);
    std::this_thread::sleep_for(100ms);
    discovery.stop_dns_thread();
    discovery.start_dns_thread(0, true);
    std::this_thread::sleep_for(100ms);
    discovery.stop_dns_thread();
    check(true, "completed DNS threads can be joined and restarted");
}

void test_listener() {
    auto manager = std::make_shared<PeerManager>();
    P2PServer server(manager, bytes(32, 7), 0, {"127.0.0.1", "::1"});
    check(server.start(), "ephemeral listener starts");
    check(server.get_port() != 0, "listener reports actual assigned port");
    auto transport = make_tcp_peer_transport("127.0.0.1:" + std::to_string(server.get_port()));
    check(transport->connect(1s), "connect to reported listener port");
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (server.get_accepted_connections() == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    check(server.get_accepted_connections() == 1, "listener accepts the connection");
    std::thread first([&] { server.stop(); });
    std::thread second([&] { server.stop(); });
    first.join();
    second.join();
    check(!server.is_running(), "concurrent stops finish");
    check(server.start(), "listener can restart after stop");
    server.stop();
}

void test_dns_addresses() {
    const auto result = DNSResolver::resolve_seeds_with_port(
        std::vector<std::string>{"127.0.0.1", "::1", "127.0.0.1"}, 4040);
    check(result == std::vector<std::string>{"127.0.0.1:4040", "[::1]:4040"},
          "DNS resolver keeps IPv4 and IPv6 and removes duplicate endpoints");
    check(DNSResolver::resolve_seeds_with_port(std::vector<std::string>{"127.0.0.1"}, 65536).empty(),
          "DNS resolver rejects an out-of-range port");
}

void test_testnet_discovery() {
    TemporaryDirectory dir;
    PeerDiscovery discovery((dir.path / "peers.dat").string());
    discovery.initialize(true);
    PeerDiscoveryTestAccess::use_loopback_seed(discovery);
    check(discovery.load_from_dns() == 1, "testnet resolves its local test seed");
    const auto peers = discovery.get_all_peers();
    check(peers.size() == 1 && peers.front().port == DEFAULT_TESTNET_P2P_PORT,
          "testnet discovery uses the testnet P2P port");
}
} // namespace

int main(int argc, char** argv) {
    try {
        check(argc == 2, "scenario argument required");
        const std::string scenario = argv[1];
        if (scenario == "discovery_thread") test_discovery_thread();
        else if (scenario == "listener") test_listener();
        else if (scenario == "session_replacement") test_session_replacement();
        else if (scenario == "dns_addresses") test_dns_addresses();
        else if (scenario == "testnet_discovery") test_testnet_discovery();
#ifndef _WIN32
        else if (scenario == "buffered_eof") test_buffered_eof();
        else if (scenario == "invalid_inbound") test_invalid_inbound();
        else if (scenario == "high_fd_connect") test_high_fd(false);
        else if (scenario == "high_fd_send") test_high_fd(true);
        else if (scenario == "address_store") test_address_store();
        else if (scenario == "callback_failure") test_callback_failure();
#endif
        else throw std::runtime_error("unknown scenario");
        std::cout << scenario << ": " << checks << " checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
