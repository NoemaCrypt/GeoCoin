#include "geocoin/core/blockchain.hpp"
#include "geocoin/network/addrman.hpp"
#include "geocoin/network/messages.hpp"
#include "geocoin/network/mempool.hpp"
#include "geocoin/network/network_utils.hpp"
#include "geocoin/network/peer_manager.hpp"
#include "geocoin/network/services.hpp"
#include "geocoin/node/node.hpp"
#include "geocoin/utils/bytes.hpp"
#include "secure_channel_test_fixture.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace geocoin::network {

struct PeerManagerTestAccess {
    static std::unique_ptr<SecureChannel> force_authenticated_ready(
        PeerManager& manager,
        const std::string& address,
        Height height
    ) {
        std::lock_guard<std::mutex> lock(manager.mutex_);
        auto it = manager.peers_.find(address);
        if (it == manager.peers_.end() || !it->second.peer) {
            return nullptr;
        }

        auto secure_fixture = geocoin::test::make_secure_channel_fixture();
        if (!it->second.peer->activate_secure_channel(std::move(secure_fixture.peer_keys))) {
            return nullptr;
        }
        it->second.channel_binding = secure_fixture.channel_binding;
        it->second.secure_established = true;

        auto session = std::make_shared<AuthSession>();
        session->session_id = {};
        session->role = AuthRole::PROVER;
        session->status = AuthStatus::COMPLETED;
        session->current_round = 3;
        session->started_at = std::chrono::steady_clock::now();
        session->last_activity = session->started_at;
        session->channel_binding = secure_fixture.channel_binding;
        it->second.auth_session = std::move(session);

        auto info = it->second.peer->get_info();
        info.height = height;
        it->second.peer->update_info(info);
        it->second.peer->set_state(PeerState::READY);
        it->second.last_message_at = std::chrono::steady_clock::now();
        it->second.last_useful_message_at = it->second.last_message_at;
        return std::move(secure_fixture.remote_channel);
    }
};

struct MempoolTestAccess {
    static void seed_relay_transaction(Mempool& mempool, const core::Transaction& tx)
    {
        const Hash256 tx_hash = tx.get_hash();
        const size_t tx_size = tx.serialize().size();
        {
            std::lock_guard<std::mutex> lock(mempool.mutex_);
            MempoolEntry entry;
            entry.tx = tx;
            entry.fee = 0;
            entry.fee_per_byte = 0;
            entry.added_at = std::chrono::steady_clock::now();
            entry.size = tx_size;
            mempool.transactions_[tx_hash] = entry;
            mempool.current_size_ += tx_size;
            ++mempool.transactions_updated_;
        }
        {
            std::lock_guard<std::mutex> lock(mempool.relay_mutex_);
            mempool.new_transactions_.insert(tx_hash);
        }
    }
};

} // namespace geocoin::network

namespace geocoin::node {

struct NodeTestAccess {
    static void pause_background_updates(Node& node) {
        node.running_.store(false, std::memory_order_release);
        node.stop_background_threads();
        node.running_.store(true, std::memory_order_release);
    }

    static void active_transition(Node& node, const services::ChainTransition& transition) {
        node.handle_active_chain_transition(transition);
    }

    static void receive(Node& node, std::shared_ptr<network::Peer> peer,
                        network::MessageType type, const bytes& payload) {
        node.handle_peer_message(std::move(peer), type, payload);
    }

    static void add_address(Node& node, const std::string& address) {
        initialize_addrman(node);
        node.addrman_->add(address);
    }

    static size_t queued_transactions(Node& node, const std::string& address) {
        std::lock_guard<std::mutex> lock(node.transaction_request_mutex_);
        const auto it = node.pending_transaction_inventory_.find(address);
        return it == node.pending_transaction_inventory_.end() ? 0 : it->second.hashes.size();
    }

    static void next_request_window(Node& node) {
        {
            std::lock_guard<std::mutex> lock(node.transaction_request_mutex_);
            for (auto& [address, pending] : node.pending_transaction_inventory_) pending.next_request = {};
        }
        node.request_pending_transactions();
    }

    static void queue_only(Node& node, std::shared_ptr<network::Peer> peer, const std::vector<Hash256>& hashes) {
        node.queue_transaction_requests(std::move(peer), hashes);
    }

    static void expire_requests(Node& node) {
        std::lock_guard<std::mutex> lock(node.transaction_request_mutex_);
        for (auto& [address, pending] : node.pending_transaction_inventory_) {
            for (auto& [hash, deadline] : pending.requested) deadline = {};
        }
    }

    static Height active_height(const Node& node)
    {
        return node.get_active_height();
    }

    static void handle_peer_authenticated(Node& node, std::shared_ptr<network::Peer> peer, Height peer_height)
    {
        node.handle_peer_authenticated(std::move(peer), peer_height);
    }

    static void relay_transactions(Node& node, std::shared_ptr<network::Peer> exclude_peer = nullptr)
    {
        node.relay_inventory(std::move(exclude_peer));
    }

    static uint16_t p2p_port(const Node& node)
    {
        return node.config_.p2p_port;
    }

    static void initialize_addrman(Node& node)
    {
        std::lock_guard<std::mutex> lock(node.addrman_mutex_);
        if (!node.addrman_) {
            node.addrman_ = std::make_shared<network::AddrMan>();
        }
    }

    static std::vector<std::string> known_addresses(const Node& node)
    {
        std::shared_ptr<network::AddrMan> addrman;
        {
            std::lock_guard<std::mutex> lock(node.addrman_mutex_);
            addrman = node.addrman_;
        }
        return addrman ? addrman->get_all() : std::vector<std::string>{};
    }

    static bool has_pending_witness(Node& node, const Hash256& hash)
    {
        std::lock_guard<std::mutex> lock(node.witness_request_mutex_);
        return node.pending_witness_requests_.find(utils::hash256_to_hex(hash)) !=
            node.pending_witness_requests_.end();
    }

    static bool age_pending_witness(
        Node& node,
        const Hash256& hash,
        std::chrono::minutes age
    ) {
        std::lock_guard<std::mutex> lock(node.witness_request_mutex_);
        const auto it = node.pending_witness_requests_.find(utils::hash256_to_hex(hash));
        if (it == node.pending_witness_requests_.end()) {
            return false;
        }
        it->second = std::chrono::steady_clock::now() - age;
        return true;
    }
};

} // namespace geocoin::node

namespace {

void expect(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::filesystem::path make_temp_dir(const std::string& name)
{
    const auto path = std::filesystem::temp_directory_path()
        / (name + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
    return path;
}

geocoin::bytes make_key(uint8_t seed)
{
    geocoin::bytes key(32);
    for (size_t i = 0; i < key.size(); ++i) {
        key[i] = static_cast<geocoin::byte>(seed + i);
    }
    return key;
}

geocoin::Hash256 hash_from_seed(uint8_t seed)
{
    geocoin::Hash256 hash{};
    for (size_t i = 0; i < hash.size(); ++i) {
        hash[i] = static_cast<geocoin::byte>(seed + i * 13);
    }
    return hash;
}

class SocketPair {
public:
    SocketPair()
    {
#ifndef _WIN32
        int fds[2] = {-1, -1};
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
            throw std::runtime_error("socketpair failed");
        }
        node_fd_ = fds[0];
        remote_fd_ = fds[1];
#else
        throw std::runtime_error("socketpair test is not supported on Windows");
#endif
    }

    ~SocketPair()
    {
#ifndef _WIN32
        if (node_fd_ >= 0) {
            ::close(node_fd_);
        }
        if (remote_fd_ >= 0) {
            ::close(remote_fd_);
        }
#endif
    }

    int take_node_fd()
    {
        const int fd = node_fd_;
        node_fd_ = -1;
        return fd;
    }

    void set_remote_channel(std::unique_ptr<geocoin::network::SecureChannel> channel)
    {
        remote_channel_ = std::move(channel);
        wire_buffer_.clear();
    }

    void send_message(geocoin::network::MessageType type, const geocoin::bytes& payload)
    {
        const geocoin::bytes framed = geocoin::network::create_message(type, payload);
        if (!remote_channel_) {
            throw std::runtime_error("secure socketpair channel is not active");
        }
        geocoin::bytes record;
        if (!remote_channel_->protect(framed, record)) {
            throw std::runtime_error("socketpair message encryption failed");
        }
#ifndef _WIN32
        size_t sent_total = 0;
        while (sent_total < record.size()) {
            const ssize_t sent = ::send(
                remote_fd_,
                record.data() + sent_total,
                record.size() - sent_total,
                0
            );
            if (sent <= 0) {
                throw std::runtime_error("socket send failed");
            }
            sent_total += static_cast<size_t>(sent);
        }
#endif
    }

    std::vector<geocoin::network::MessageType> read_message_types(
        size_t expected = 1, std::vector<geocoin::bytes>* payloads = nullptr)
    {
        std::vector<geocoin::network::MessageType> types;
        if (!remote_channel_) {
            return types;
        }

#ifndef _WIN32
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        while (std::chrono::steady_clock::now() < deadline && types.size() < expected) {
            while (types.size() < expected) {
                size_t record_size = 0;
                const auto size_status = geocoin::network::SecureChannel::inspect_record_size(
                    wire_buffer_.data(), wire_buffer_.size(), record_size);
                if (size_status == geocoin::network::SecureRecordSizeStatus::INCOMPLETE) {
                    break;
                }
                if (size_status == geocoin::network::SecureRecordSizeStatus::INVALID) {
                    wire_buffer_.clear();
                    throw std::runtime_error("invalid secure record in socketpair test");
                }

                geocoin::bytes record(
                    wire_buffer_.begin(), wire_buffer_.begin() + record_size);
                wire_buffer_.erase(
                    wire_buffer_.begin(), wire_buffer_.begin() + record_size);
                geocoin::bytes framed;
                if (!remote_channel_->unprotect(record, framed)) {
                    throw std::runtime_error(
                        "secure record authentication failed in socketpair test");
                }

                geocoin::network::MessageHeader header;
                geocoin::bytes payload;
                if (!geocoin::network::parse_message(framed, header, payload)) {
                    throw std::runtime_error("decrypted socketpair frame is malformed");
                }
                types.push_back(header.type);
                if (payloads) payloads->push_back(std::move(payload));
            }

            if (types.size() >= expected) {
                break;
            }

            geocoin::bytes chunk(8192);
            const ssize_t received = ::recv(remote_fd_, chunk.data(), chunk.size(), MSG_DONTWAIT);
            if (received > 0) {
                chunk.resize(static_cast<size_t>(received));
                wire_buffer_.insert(wire_buffer_.end(), chunk.begin(), chunk.end());
            } else if (received == 0) {
                break;
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
#endif

        return types;
    }

    bool is_remote_open() const
    {
#ifndef _WIN32
        char byte = 0;
        const ssize_t received = ::recv(remote_fd_, &byte, 1, MSG_DONTWAIT | MSG_PEEK);
        return received > 0 || (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
#else
        return false;
#endif
    }

    void drain()
    {
        (void)read_message_types(100);
    }

private:
    int node_fd_ = -1;
    int remote_fd_ = -1;
    std::unique_ptr<geocoin::network::SecureChannel> remote_channel_;
    geocoin::bytes wire_buffer_;
};

geocoin::node::NodeConfig base_config(const std::filesystem::path& dir)
{
    geocoin::node::NodeConfig config;
    config.data_dir = dir.string();
    config.enable_auto_connect = false;
    config.enable_p2p_listen = false;
    config.enable_mining = false;
    return config;
}

std::shared_ptr<geocoin::network::Peer> attach_ready_peer(
    geocoin::node::Node& node,
    SocketPair& sockets,
    geocoin::Height peer_height = 0,
    bool archive = false,
    const std::string& address = "127.0.0.77:3030"
)
{
    auto peer_manager = node.get_peer_manager();
    expect(peer_manager != nullptr, "node exposes peer manager after start");
    expect(peer_manager->add_inbound_peer(sockets.take_node_fd(), address, make_key(0x91)),
           "ready peer is added through inbound socketpair");
    auto peer = peer_manager->get_peer(address);
    expect(peer != nullptr, "ready peer can be looked up");

    auto remote_channel =
        geocoin::network::PeerManagerTestAccess::force_authenticated_ready(
            *peer_manager,
            address,
            peer_height);
    expect(remote_channel != nullptr, "test peer is force-authenticated over a secure channel");
    sockets.set_remote_channel(std::move(remote_channel));

    auto info = peer->get_info();
    info.height = peer_height;
    info.services = geocoin::network::build_local_services(archive);
    peer->update_info(info);
    return peer;
}

void expect_response_type(
    SocketPair& sockets,
    geocoin::node::Node& node,
    geocoin::network::MessageType request_type,
    const geocoin::bytes& payload,
    geocoin::network::MessageType expected_type,
    const char* message
)
{
    sockets.drain();
    sockets.send_message(request_type, payload);
    node.get_peer_manager()->update();
    const auto types = sockets.read_message_types();
    expect(!types.empty(), message);
    expect(types.front() == expected_type, message);
}

void test_ready_peer_message_routing()
{
    const auto dir = make_temp_dir("geocoin-node-peer-messages");
    std::error_code ec;
    try {
        geocoin::node::Node node(base_config(dir));
        expect(node.start(make_key(0x21)), "isolated node starts for peer message tests");

        SocketPair sockets;
        auto peer = attach_ready_peer(node, sockets);
        expect(peer->get_state() == geocoin::network::PeerState::READY, "test peer is READY");

        geocoin::core::Block known_genesis;
        expect(node.get_block_by_height(0, known_genesis),
               "local genesis is available for witness request routing");

        expect(!node.request_witness(known_genesis.get_hash()),
               "request_witness reports no archive peer before one is advertised");

        auto archive_info = peer->get_info();
        archive_info.services = geocoin::network::build_local_services(true);
        peer->update_info(archive_info);
        sockets.drain();
        expect(!node.request_witness(hash_from_seed(0x11)),
               "request_witness rejects hashes absent from the local block index");
        expect(node.request_witness(known_genesis.get_hash()),
               "request_witness sends GETDATA to authenticated archive peer");
        auto witness_types = sockets.read_message_types();
        expect(!witness_types.empty() && witness_types.front() == geocoin::network::MessageType::GETDATA,
               "request_witness emits GETDATA");
        expect(geocoin::node::NodeTestAccess::age_pending_witness(
                   node, known_genesis.get_hash(), std::chrono::minutes(3)),
               "witness request fixture can age the pending reservation");
        expect(node.request_witness(known_genesis.get_hash()),
               "expired witness reservation can be retried");
        witness_types = sockets.read_message_types();
        expect(!witness_types.empty() && witness_types.front() == geocoin::network::MessageType::GETDATA,
               "retrying an expired witness request emits a fresh GETDATA");

        geocoin::network::InvMessage inv_tx;
        inv_tx.items.push_back({geocoin::network::InvMessage::InvType::TX, hash_from_seed(0x31)});
        expect_response_type(
            sockets,
            node,
            geocoin::network::MessageType::INV,
            inv_tx.serialize(),
            geocoin::network::MessageType::GETDATA,
            "INV with unknown TX triggers GETDATA"
        );

        geocoin::network::InvMessage inv_block;
        inv_block.items.push_back({geocoin::network::InvMessage::InvType::BLOCK, hash_from_seed(0x32)});
        const auto peer_height_before_inv = peer->get_info().height;
        sockets.drain();
        sockets.send_message(geocoin::network::MessageType::INV, inv_block.serialize());
        node.get_peer_manager()->update();
        expect(peer->get_info().height == peer_height_before_inv,
               "unvalidated block INV cannot advance peer height");

        geocoin::network::GetBlocksMessage get_headers;
        get_headers.version = 1;
        get_headers.stop_hash = {};
        expect_response_type(
            sockets,
            node,
            geocoin::network::MessageType::GETHEADERS,
            get_headers.serialize(),
            geocoin::network::MessageType::HEADERS,
            "GETHEADERS receives HEADERS response"
        );

        geocoin::network::HeadersMessage empty_headers;
        sockets.drain();
        sockets.send_message(geocoin::network::MessageType::HEADERS, empty_headers.serialize());
        node.get_peer_manager()->update();
        expect(node.get_sync_manager()->get_stats().active_peers >= 0,
               "empty HEADERS message is routed to sync manager");

        expect_response_type(
            sockets,
            node,
            geocoin::network::MessageType::GETBLOCKS,
            get_headers.serialize(),
            geocoin::network::MessageType::INV,
            "GETBLOCKS receives INV response"
        );

        geocoin::core::Block genesis;
        expect(node.get_block_by_height(0, genesis), "genesis block is available");
        geocoin::network::InvMessage get_data;
        get_data.items.push_back({geocoin::network::InvMessage::InvType::BLOCK, genesis.get_hash()});
        expect_response_type(
            sockets,
            node,
            geocoin::network::MessageType::GETDATA,
            get_data.serialize(),
            geocoin::network::MessageType::BLOCK,
            "GETDATA for genesis receives BLOCK response"
        );

        geocoin::core::Transaction relay_tx;
        relay_tx.version = 2;
        relay_tx.type = geocoin::core::TransactionType::STANDARD;
        relay_tx.timestamp = 123;
        relay_tx.outputs.push_back({1, geocoin::Address("GEOrelay-output"), {}});
        geocoin::network::MempoolTestAccess::seed_relay_transaction(*node.get_mempool(), relay_tx);

        sockets.drain();
        geocoin::node::NodeTestAccess::relay_transactions(node);
        // The 500 ms background coalescer may win the race with the explicit
        // drain above. Read a small asynchronous batch and require that it
        // contains the inventory announcement, regardless of unrelated
        // scheduler traffic ordering.
        auto relay_types = sockets.read_message_types(8);
        const bool relay_observed = std::find(
            relay_types.begin(),
            relay_types.end(),
            geocoin::network::MessageType::INV) != relay_types.end();
        expect(relay_observed || peer->has_known_inventory(relay_tx.get_hash()),
               "relay_transactions sends INV for queued mempool tx");

        geocoin::network::InvMessage get_relay_tx;
        get_relay_tx.items.push_back({geocoin::network::InvMessage::InvType::TX, relay_tx.get_hash()});
        expect_response_type(
            sockets,
            node,
            geocoin::network::MessageType::GETDATA,
            get_relay_tx.serialize(),
            geocoin::network::MessageType::TX,
            "GETDATA for mempool transaction receives TX response"
        );

        // The immediate relay queue was drained above.  A peer that connects
        // afterwards must still receive the complete mempool inventory, or a
        // transaction can remain permanently isolated after a reconnect.
        SocketPair reconnect_sockets;
        auto reconnect_peer = attach_ready_peer(
            node,
            reconnect_sockets,
            0,
            false,
            "127.0.0.78:3030"
        );
        reconnect_sockets.drain();
        geocoin::node::NodeTestAccess::handle_peer_authenticated(node, reconnect_peer, 0);
        geocoin::node::NodeTestAccess::relay_transactions(node);
        const auto reconnect_types = reconnect_sockets.read_message_types();
        expect(
            std::find(
                reconnect_types.begin(),
                reconnect_types.end(),
                geocoin::network::MessageType::INV
            ) != reconnect_types.end(),
            "newly authenticated peer receives mempool inventory backfill"
        );
        expect(
            reconnect_peer->has_known_inventory(relay_tx.get_hash()),
            "mempool backfill marks the transaction known for the new session"
        );

        geocoin::network::TxMessage tx_msg;
        tx_msg.tx = geocoin::core::Transaction::create_coinbase(
            9,
            geocoin::Address("GEOpeer-tx-miner"),
            50
        );
        sockets.drain();
        sockets.send_message(geocoin::network::MessageType::TX, tx_msg.serialize());
        node.get_peer_manager()->update();
        expect(peer->has_known_inventory(tx_msg.tx.get_hash()),
               "TX message marks peer inventory as known");

        const uint16_t gossip_port = geocoin::node::NodeTestAccess::p2p_port(node);
        const uint16_t wrong_gossip_port = gossip_port == 65535
            ? static_cast<uint16_t>(gossip_port - 1)
            : static_cast<uint16_t>(gossip_port + 1);
        const uint64_t now_seconds = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
        geocoin::node::NodeTestAccess::initialize_addrman(node);

        geocoin::network::AddrMessage addr;
        const uint64_t services = geocoin::network::build_local_services(false);
        addr.addresses.push_back({services, "8.8.8.8", gossip_port, now_seconds});
        addr.addresses.push_back({services, "2606:4700:4700::1111", gossip_port, now_seconds});
        addr.addresses.push_back({services, "10.23.45.67", gossip_port, now_seconds});
        addr.addresses.push_back({services, "127.99.88.77", gossip_port, now_seconds});
        addr.addresses.push_back({services, "169.254.23.45", gossip_port, now_seconds});
        addr.addresses.push_back({services, "224.1.2.3", gossip_port, now_seconds});
        addr.addresses.push_back({services, "internal.gossip.invalid", gossip_port, now_seconds});
        addr.addresses.push_back({services, "8.8.4.4", wrong_gossip_port, now_seconds});
        addr.addresses.push_back({services, "", gossip_port, now_seconds});
        addr.addresses.push_back({
            services,
            "1.1.1.1",
            gossip_port,
            now_seconds + 3 * 60 * 60
        });
        sockets.drain();
        expect(peer->get_state() == geocoin::network::PeerState::READY,
               "peer remains READY before ADDR gossip");
        sockets.send_message(geocoin::network::MessageType::ADDR, addr.serialize());
        const std::string expected_public_ipv4 =
            geocoin::network::NetworkUtils::make_endpoint("8.8.8.8", gossip_port);
        for (size_t i = 0; i < 8; ++i) {
            node.get_peer_manager()->update();
            const auto current_addresses = geocoin::node::NodeTestAccess::known_addresses(node);
            if (std::find(current_addresses.begin(), current_addresses.end(), expected_public_ipv4) !=
                current_addresses.end()) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        expect(node.get_peer_manager()->get_peer("127.0.0.77:3030") != nullptr,
               "ADDR handling keeps valid ready peer connected");

        const auto known_addresses = geocoin::node::NodeTestAccess::known_addresses(node);
        const auto contains_known = [&](const std::string& endpoint) {
            return std::find(known_addresses.begin(), known_addresses.end(), endpoint) != known_addresses.end();
        };
        expect(contains_known(expected_public_ipv4),
               "ADDR gossip accepts numeric global-unicast IPv4 on the configured P2P port");
        expect(contains_known(geocoin::network::NetworkUtils::make_endpoint(
                   "2606:4700:4700::1111", gossip_port)),
               "ADDR gossip accepts numeric global-unicast IPv6 on the configured P2P port");
        expect(!contains_known(geocoin::network::NetworkUtils::make_endpoint("10.23.45.67", gossip_port)),
               "ADDR gossip rejects RFC1918 targets");
        expect(!contains_known(geocoin::network::NetworkUtils::make_endpoint("127.99.88.77", gossip_port)),
               "ADDR gossip rejects loopback targets");
        expect(!contains_known(geocoin::network::NetworkUtils::make_endpoint("169.254.23.45", gossip_port)),
               "ADDR gossip rejects link-local targets");
        expect(!contains_known(geocoin::network::NetworkUtils::make_endpoint("224.1.2.3", gossip_port)),
               "ADDR gossip rejects multicast targets");
        expect(!contains_known(geocoin::network::NetworkUtils::make_endpoint(
                   "internal.gossip.invalid", gossip_port)),
               "ADDR gossip rejects hostnames");
        expect(!contains_known(geocoin::network::NetworkUtils::make_endpoint("8.8.4.4", wrong_gossip_port)),
               "ADDR gossip rejects targets on an unexpected port");
        expect(!contains_known(geocoin::network::NetworkUtils::make_endpoint("1.1.1.1", gossip_port)),
               "ADDR gossip rejects implausible future timestamps");

        geocoin::network::RejectMessage reject;
        reject.rejected_type = geocoin::network::MessageType::TX;
        reject.code = 1;
        reject.reason = "unit-test-reject";
        sockets.drain();
        sockets.send_message(geocoin::network::MessageType::REJECT, reject.serialize());
        node.get_peer_manager()->update();
        expect(peer->is_connected(), "REJECT message is logged without disconnecting peer");

        node.get_sync_manager()->stop_sync();
        const geocoin::Height equal_height =
            geocoin::node::NodeTestAccess::active_height(node);
        SocketPair equal_peer_sockets;
        auto equal_peer = attach_ready_peer(
            node,
            equal_peer_sockets,
            equal_height,
            false,
            "127.0.0.79:3031"
        );
        geocoin::node::NodeTestAccess::handle_peer_authenticated(
            node,
            equal_peer,
            equal_height
        );
        expect(!node.get_sync_manager()->is_syncing(),
               "authenticated equal-height peer does not start active sync");
        expect(node.get_sync_manager()->get_stats().status == geocoin::network::SyncStatus::COMPLETED,
               "authenticated equal-height peer initializes completed sync state");
        expect(node.get_sync_manager()->get_stats().target_height == equal_height,
               "authenticated equal-height peer initializes persisted sync height");

        SocketPair peer_for_auth_sockets;
        auto peer_for_auth = attach_ready_peer(
            node,
            peer_for_auth_sockets,
            7,
            false,
            "127.0.0.78:3031"
        );
        sockets.drain();
        geocoin::node::NodeTestAccess::handle_peer_authenticated(node, peer_for_auth, 7);
        expect(!node.get_sync_manager()->is_syncing(),
               "authenticated ahead peer starts discovery without unvalidated sync work");
        expect(node.get_sync_manager()->get_stats().target_height == equal_height,
               "authenticated peer height remains a discovery hint");
        auto auth_types = peer_for_auth_sockets.read_message_types(2);
        expect(std::find(auth_types.begin(), auth_types.end(),
                         geocoin::network::MessageType::GETHEADERS) != auth_types.end(),
               "authenticated ahead peer receives a real header discovery request");

        geocoin::network::BlockMessage block_msg;
        block_msg.block = genesis;
        sockets.drain();
        expect(node.request_witness(genesis.get_hash()),
               "request_witness can mark known genesis block for witness restore");
        sockets.drain();
        sockets.send_message(geocoin::network::MessageType::BLOCK, block_msg.serialize());
        node.get_peer_manager()->update();
        expect(node.get_peer_manager()->get_peer("127.0.0.77:3030") != nullptr,
               "duplicate genesis block response keeps peer connected");
        expect(!geocoin::node::NodeTestAccess::has_pending_witness(node, genesis.get_hash()),
               "validated witness restore consumes the pending request");

        SocketPair invalid_restore_sockets;
        auto invalid_restore_peer = attach_ready_peer(
            node,
            invalid_restore_sockets,
            0,
            true,
            "127.0.0.76:3030"
        );
        sockets.drain();
        invalid_restore_sockets.drain();
        expect(node.request_witness(genesis.get_hash()),
               "known block can be requested again for invalid-restore regression test");
        sockets.drain();
        invalid_restore_sockets.drain();

        geocoin::network::BlockMessage forged_restore;
        forged_restore.block = genesis;
        expect(!forged_restore.block.transactions.empty() &&
                   !forged_restore.block.transactions.front().outputs.empty(),
               "genesis fixture has a mutable committed output");
        forged_restore.block.transactions.front().outputs.front().value += 1;
        invalid_restore_sockets.send_message(
            geocoin::network::MessageType::BLOCK,
            forged_restore.serialize()
        );
        node.get_peer_manager()->update();
        node.get_peer_manager()->update();
        expect(!invalid_restore_peer->is_connected(),
               "block body that does not match the known header disconnects restore peer");
        expect(geocoin::node::NodeTestAccess::has_pending_witness(node, genesis.get_hash()),
               "invalid witness restore does not consume the pending request");
        geocoin::core::Block stored_genesis;
        expect(node.get_block_by_height(0, stored_genesis),
               "genesis remains readable after forged restore");
        expect(stored_genesis.serialize() == genesis.serialize(),
               "forged witness restore cannot overwrite the stored block body");

        expect_response_type(
            sockets,
            node,
            geocoin::network::MessageType::HEADERS,
            geocoin::bytes{0xff, 0x00},
            geocoin::network::MessageType::REJECT,
            "malformed callback message receives REJECT"
        );
        expect(!peer->is_connected(),
               "deeply malformed application payload disconnects the offending peer");

        SocketPair too_many_sockets;
        auto too_many_peer = attach_ready_peer(
            node,
            too_many_sockets,
            0,
            false,
            "127.0.0.79:3032"
        );
        geocoin::network::InvMessage too_many_getdata;
        for (size_t i = 0; i < 2049; ++i) {
            geocoin::Hash256 unique_hash{};
            uint64_t value = static_cast<uint64_t>(i) + 1;
            for (size_t byte_index = 0; byte_index < sizeof(value); ++byte_index) {
                unique_hash[byte_index] = static_cast<geocoin::byte>(value >> (byte_index * 8));
            }
            too_many_getdata.items.push_back({
                geocoin::network::InvMessage::InvType::BLOCK,
                unique_hash
            });
        }
        too_many_sockets.drain();
        too_many_sockets.send_message(geocoin::network::MessageType::GETDATA, too_many_getdata.serialize());
        node.get_peer_manager()->update();
        (void)too_many_sockets.read_message_types();
        node.get_peer_manager()->update();
        expect(!too_many_peer->is_connected() || !too_many_sockets.is_remote_open(),
               "oversized GETDATA disconnects offending peer");

        node.stop();
        std::filesystem::remove_all(dir, ec);
    } catch (...) {
        std::filesystem::remove_all(dir, ec);
        throw;
    }
}

void test_committed_tip_and_transaction_relay() {
    using namespace geocoin;
    using network::InvMessage;
    using network::MessageType;
    const auto dir = make_temp_dir("geocoin-node-block-relay");
    try {
        {
            node::Node node(base_config(dir));
            expect(node.start(make_key(0x37)), "start node for committed tip relay");
            node::NodeTestAccess::pause_background_updates(node);
            SocketPair sockets, known_sockets;
            auto peer = attach_ready_peer(node, sockets);
            auto known_peer = attach_ready_peer(node, known_sockets, 0, false, "127.0.0.78:3030");
            sockets.drain();
            known_sockets.drain();

            core::Block block;
            expect(node.get_block_by_height(0, block), "relay fixture has a block body");
            // Exercise the committed-event observer while controlling when
            // its periodic relay runs; consensus itself is covered elsewhere.
            for (size_t i = 1; i <= 20; ++i) {
                block.header.nonce = i;
                services::ChainTransition transition;
                transition.connected.push_back({i, block.get_hash(), block, true});
                node::NodeTestAccess::active_transition(node, transition);
            }
            expect(sockets.read_message_types(1).empty(),
                   "rapid committed events do not push unsolicited block bodies");
            known_peer->add_known_inventory(block.get_hash());

            std::set<Hash256> remaining;
            for (size_t i = 0; i < network::Mempool::kRelayInventoryBatch; ++i) {
                core::Transaction tx;
                tx.version = 1;
                tx.timestamp = 1000 + i;
                tx.outputs.push_back({1, Address("GEOrelay-output"), {}});
                network::MempoolTestAccess::seed_relay_transaction(*node.get_mempool(), tx);
                remaining.insert(tx.get_hash());
            }
            node::NodeTestAccess::relay_transactions(node);
            std::vector<bytes> payloads;
            expect(sockets.read_message_types(1, &payloads) == std::vector<MessageType>{MessageType::INV},
                   "block and transaction announcements share one INV frame");
            const auto first = InvMessage::deserialize(payloads.at(0));
            expect(first.items.size() == network::Mempool::kRelayInventoryBatch,
                   "mixed inventory stays within the full INV item budget");
            expect(first.items.front().type == InvMessage::InvType::BLOCK &&
                       first.items.front().hash == block.get_hash(),
                   "rapid commits coalesce to the latest tip hash");
            for (size_t i = 1; i < first.items.size(); ++i) {
                expect(first.items[i].type == InvMessage::InvType::TX &&
                           remaining.erase(first.items[i].hash) == 1,
                       "mixed inventory preserves every transaction item type");
            }
            payloads.clear();
            expect(known_sockets.read_message_types(1, &payloads) == std::vector<MessageType>{MessageType::INV},
                   "peer which knows the tip still receives transaction inventory");
            const auto known = InvMessage::deserialize(payloads.at(0));
            expect(known.items.size() + 1 == first.items.size() &&
                       std::all_of(known.items.begin(), known.items.end(), [](const auto& item) {
                           return item.type == InvMessage::InvType::TX;
                       }), "known block filtering preserves mixed batch item types");

            node::NodeTestAccess::relay_transactions(node);
            payloads.clear();
            expect(sockets.read_message_types(1, &payloads) == std::vector<MessageType>{MessageType::INV},
                   "transaction displaced by the block is retained for the next batch");
            const auto last = InvMessage::deserialize(payloads.at(0));
            expect(last.items.size() == 1 && last.items[0].type == InvMessage::InvType::TX &&
                       remaining.erase(last.items[0].hash) == 1 && remaining.empty(),
                   "relay drains all transactions exactly once across mixed batch boundary");
            node::NodeTestAccess::relay_transactions(node);
            expect(sockets.read_message_types(1).empty(), "drained inventory is not announced again");
            expect(peer->has_known_inventory(block.get_hash()), "announced tip is marked known for its peer");
            node.stop();
        }
        std::filesystem::remove_all(dir);
    } catch (...) {
        std::filesystem::remove_all(dir);
        throw;
    }
}

void test_inventory_and_address_batches() {
    using namespace geocoin;
    using namespace geocoin::network;
    const auto dir = make_temp_dir("geocoin-node-peer-batches");
    try {
      {
        // The constructor opens real storage; no background threads run here.
        node::Node node(base_config(dir));
        SocketPair sockets;
        auto peer = attach_ready_peer(node, sockets);
        for (size_t i = 1; i <= 128; ++i) {
            node::NodeTestAccess::add_address(node,
                "8.23.1." + std::to_string(i) + ":" +
                std::to_string(node::NodeTestAccess::p2p_port(node)));
        }
        node::NodeTestAccess::handle_peer_authenticated(node, peer, 0);
        std::vector<bytes> payloads;
        auto types = sockets.read_message_types(1, &payloads);
        expect(types == std::vector<MessageType>{MessageType::ADDR},
               "populated address book produces an ADDR message");
        expect(AddrMessage::deserialize(payloads.at(0)).addresses.size() == 64,
               "outgoing address batch obeys peer admission limit");

        InvMessage inventory;
        std::set<Hash256> expected;
        for (size_t i = 0; i < 2048; ++i) {
            Hash256 hash{};
            hash[0] = static_cast<byte>(i);
            hash[1] = static_cast<byte>(i >> 8);
            inventory.items.push_back({InvMessage::InvType::TX, hash});
            expected.insert(hash);
        }
        node::NodeTestAccess::receive(node, peer, MessageType::INV, inventory.serialize());
        expect(node::NodeTestAccess::queued_transactions(node, peer->get_address()) == 1984,
               "large INV retains every hash beyond the first request");
        std::set<Hash256> requested;
        for (size_t window = 0; window < 32; ++window) {
            payloads.clear();
            types = sockets.read_message_types(1, &payloads);
            expect(types == std::vector<MessageType>{MessageType::GETDATA}, "one GETDATA per window");
            const auto request = InvMessage::deserialize(payloads.at(0));
            expect(request.items.size() == 64, "request fits TX receive window");
            for (const auto& item : request.items) {
                expect(requested.insert(item.hash).second, "inventory hash requested exactly once");
            }
            // Advance only the scheduler clock; no wall-clock sleep is needed.
            node::NodeTestAccess::next_request_window(node);
        }
        expect(requested == expected, "all 2048 announced transactions are requested");
        expect(node::NodeTestAccess::queued_transactions(node, peer->get_address()) == 0,
               "fully requested inventory leaves an empty queue");
        node::NodeTestAccess::receive(node, peer, MessageType::INV, inventory.serialize());
        const auto retained = node::NodeTestAccess::queued_transactions(node, peer->get_address());
        node::NodeTestAccess::receive(node, peer, MessageType::INV, inventory.serialize());
        expect(node::NodeTestAccess::queued_transactions(node, peer->get_address()) == retained,
               "repeated inventory does not duplicate queued hashes");

        std::vector<Hash256> fresh{hash_from_seed(0x41), hash_from_seed(0x42), hash_from_seed(0x43)};
        node::NodeTestAccess::queue_only(node, peer, fresh);
        node::NodeTestAccess::expire_requests(node);
        node::NodeTestAccess::next_request_window(node);
        payloads.clear();
        types = sockets.read_message_types(1, &payloads);
        expect(types == std::vector<MessageType>{MessageType::GETDATA}, "timed out requests are retried");
        const auto retry = InvMessage::deserialize(payloads.at(0));
        expect(retry.items.size() == 64, "retry shares the normal request budget");
        for (size_t i = 0; i < fresh.size(); ++i) {
            expect(retry.items[i].hash == fresh[i], "fresh inventory precedes timed out requests");
        }
        peer->disconnect();
        node::NodeTestAccess::next_request_window(node);
        expect(node::NodeTestAccess::queued_transactions(node, peer->get_address()) == 0,
               "disconnected session releases queued inventory");

        SocketPair first_queue_sockets, second_queue_sockets;
        auto first_queue = attach_ready_peer(node, first_queue_sockets, 0, false, "127.0.0.81:3030");
        auto second_queue = attach_ready_peer(node, second_queue_sockets, 0, false, "127.0.0.82:3030");
        std::vector<Hash256> large_inventory;
        for (uint32_t i = 0; i < 65537; ++i) {
            Hash256 hash{};
            for (size_t j = 0; j < sizeof(i); ++j) hash[j] = static_cast<byte>(i >> (8 * j));
            large_inventory.push_back(hash);
        }
        node::NodeTestAccess::queue_only(node, first_queue, large_inventory);
        node::NodeTestAccess::queue_only(node, second_queue, large_inventory);
        const auto first_count = node::NodeTestAccess::queued_transactions(node, first_queue->get_address());
        const auto second_count = node::NodeTestAccess::queued_transactions(node, second_queue->get_address());
        expect(first_count == 50000, "per-peer inventory memory is bounded");
        expect(first_count + second_count == 65536, "global inventory memory is bounded");
      }
      std::filesystem::remove_all(dir);
    } catch (...) {
        std::filesystem::remove_all(dir);
        throw;
    }
}

} // namespace

int main()
{
    try {
        test_committed_tip_and_transaction_relay();
        test_inventory_and_address_batches();
        test_ready_peer_message_routing();
        std::cout << "test_node_peer_messages_units: ok\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "test_node_peer_messages_units failed: " << e.what() << "\n";
        return 1;
    }
}
