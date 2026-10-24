#include "geocoin/network/peer_manager.hpp"
#include "geocoin/network/peer_manager_internal.hpp"
#include "geocoin/network/network_utils.hpp"
#include "geocoin/crypto/addresses.hpp"
#include "geocoin/consensus/chainparams.hpp"
#include "geocoin/consensus/consensus.hpp"
#include "geocoin/state/state.hpp"
#include "geocoin/utils/logger.hpp"
#include "geocoin/utils/metrics.hpp"
#include "geocoin/utils/secure_memory.hpp"
#include <algorithm>
#include <limits>
#include <utility>

namespace geocoin::network {

namespace {
inline bool SyncDebugEnabled() {
    return detail::sync_debug_enabled();
}

constexpr size_t kMaxInboundPeers = detail::kMaxInboundPeers;
constexpr size_t kMaxOutboundPeers = detail::kMaxOutboundPeers;
constexpr size_t kMaxMessagePayload = detail::kMaxMessagePayload;
constexpr size_t kMaxReceiveBuffer = detail::kMaxReceiveBuffer;
constexpr int kPeerTimeoutSeconds = detail::kPeerTimeoutSeconds;
constexpr int kAuthTimeoutSeconds = detail::kAuthTimeoutSeconds;
constexpr int kVersionStallSeconds = detail::kVersionStallSeconds;
constexpr int kAuthProgressStallSeconds = detail::kAuthProgressStallSeconds;
constexpr int kPingIntervalSeconds = detail::kPingIntervalSeconds;
constexpr int kPingReplyTimeoutSeconds = detail::kPingReplyTimeoutSeconds;
constexpr uint32_t kPolicyMaxInvItems = detail::kPolicyMaxInvItems;
constexpr uint32_t kPolicyMaxGetDataItems = detail::kPolicyMaxGetDataItems;
constexpr size_t kMaxAddedNodes = 1024;
constexpr size_t kMaxOutboundAttemptState = 8192;

using detail::extract_ip;
using detail::peer_state_name;

uint64_t saturating_add_u64(uint64_t current, uint64_t delta) {
    const uint64_t max_value = (std::numeric_limits<uint64_t>::max)();
    if (current > max_value - delta) {
        return max_value;
    }
    return current + delta;
}

bool receive_buffer_would_exceed_limit(
    size_t current_size,
    size_t incoming_size,
    size_t limit
) {
    return current_size > limit || incoming_size > limit - current_size;
}

constexpr size_t max_payload_for_message(MessageType type) {
    switch (type) {
        case MessageType::VERSION:
            return 512;
        case MessageType::VERACK:
        case MessageType::PING:
        case MessageType::PONG:
            return 0;
        case MessageType::ADDR:
            return detail::kPolicyMaxAddrPayloadBytes;
        case MessageType::INV:
            return 4 + static_cast<size_t>(detail::kPolicyMaxInvItems) * 36;
        case MessageType::GETDATA:
            return 4 + static_cast<size_t>(detail::kPolicyMaxGetDataItems) * 36;
        case MessageType::BLOCK:
            return consensus::params::MAX_BLOCK_SIZE;
        case MessageType::TX:
            return consensus::params::MAX_TX_SIZE;
        case MessageType::GETBLOCKS:
        case MessageType::GETHEADERS:
            return 4 + 4 + static_cast<size_t>(kMaxHeadersPerMessage) * 32 + 32;
        case MessageType::HEADERS:
            return 4 + static_cast<size_t>(kMaxHeadersPerMessage) * 124;
        case MessageType::CHALLENGE:
            return 92;
        case MessageType::RESPONSE:
            return 4 + 4096;
        case MessageType::REJECT:
            return 1 + 1 + 4 + 256 + 32;
        case MessageType::SECURE_HELLO:
            return kSecureHelloPayloadSize;
        case MessageType::SECURE_REPLY:
            return kSecureReplyPayloadSize;
    }
    return 0;
}

constexpr bool payload_size_allowed(MessageType type, size_t size) {
    if (size > max_payload_for_message(type)) {
        return false;
    }
    if (type == MessageType::CHALLENGE) {
        return size == 92;
    }
    if (type == MessageType::SECURE_HELLO) {
        return size == kSecureHelloPayloadSize;
    }
    if (type == MessageType::SECURE_REPLY) {
        return size == kSecureReplyPayloadSize;
    }
    return true;
}

uint32_t expected_network_magic() {
    return current_network_magic();
}

bool has_steady_time(std::chrono::steady_clock::time_point tp) {
    return tp != std::chrono::steady_clock::time_point{};
}

uint64_t steady_age_ms(
    std::chrono::steady_clock::time_point now,
    std::chrono::steady_clock::time_point then
) {
    if (!has_steady_time(then) || now <= then) {
        return 0;
    }
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now - then).count());
}

uint64_t steady_until_ms(
    std::chrono::steady_clock::time_point now,
    std::chrono::steady_clock::time_point then
) {
    if (!has_steady_time(then) || now >= then) {
        return 0;
    }
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(then - now).count());
}

const char* message_type_name(MessageType type) {
    switch (type) {
        case MessageType::VERSION: return "version";
        case MessageType::VERACK: return "verack";
        case MessageType::ADDR: return "addr";
        case MessageType::INV: return "inv";
        case MessageType::GETDATA: return "getdata";
        case MessageType::BLOCK: return "block";
        case MessageType::TX: return "tx";
        case MessageType::GETBLOCKS: return "getblocks";
        case MessageType::GETHEADERS: return "getheaders";
        case MessageType::HEADERS: return "headers";
        case MessageType::CHALLENGE: return "challenge";
        case MessageType::RESPONSE: return "response";
        case MessageType::PING: return "ping";
        case MessageType::PONG: return "pong";
        case MessageType::REJECT: return "reject";
        case MessageType::SECURE_HELLO: return "secure_hello";
        case MessageType::SECURE_REPLY: return "secure_reply";
    }
    return "unknown";
}

}

PeerManager::PeerManager()
    : max_inbound_(125)
    , max_outbound_(125)
{
    GEO_METRIC_SET("peers.connected", 0);
    GEO_METRIC_SET("peers.authenticated", 0);
    GEO_METRIC_SET("peers.inbound", 0);
    GEO_METRIC_SET("peers.outbound", 0);
    GEO_METRIC_SET("peers.temp_bans", 0);
    GEO_METRIC_SET("peers.manual_bans", 0);
}

PeerManager::~PeerManager() {
    std::lock_guard<std::mutex> lock(mutex_);

    // Отключаем всех пиров
    for (auto& [addr, ctx] : peers_) {
        if (ctx.peer) {
            ctx.peer->disconnect();
        }
    }
    peers_.clear();
    utils::cleanse_container(private_key_);
}

void PeerManager::set_message_callback(MessageCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    message_callback_ = callback;
}

void PeerManager::set_peer_authenticated_callback(PeerAuthenticatedCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    peer_authenticated_callback_ = callback;
}

void PeerManager::set_get_height_callback(GetHeightCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    get_height_callback_ = std::move(callback);
}

void PeerManager::clear_callbacks() {
    std::lock_guard<std::mutex> lock(mutex_);
    message_callback_ = {};
    peer_authenticated_callback_ = {};
    get_height_callback_ = {};
}

void PeerManager::set_chain_state(std::shared_ptr<state::ChainState> chain_state) {
    std::lock_guard<std::mutex> lock(mutex_);
    chain_state_ = chain_state;
}

void PeerManager::set_archive_mode(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    is_archive_mode_ = enabled;
}

bool PeerManager::is_archive_mode() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return is_archive_mode_;
}

bool PeerManager::add_outbound_peer(
    const std::string& address,
    const bytes& our_private_key,
    bool manual_target,
    bool bypass_backoff
) {
    std::chrono::milliseconds connect_timeout{5000};

    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (is_peer_banned_unlocked(address)) {
            GEO_LOG_WARN("peers", "Outbound peer is temporarily banned: " << address);
            GEO_METRIC_INC("peers.connect_outbound_fail_total", 1);
            return false;
        }

        size_t inbound = 0;
        size_t outbound = 0;
        count_peers(inbound, outbound);
        if (outbound >= max_outbound_) {
            GEO_LOG_WARN("peers", "Outbound limit reached, skip " << address);
            GEO_METRIC_INC("peers.connect_outbound_fail_total", 1);
            return false;
        }

        // Проверяем, не подключен ли уже (точное совпадение адреса)
        if (peers_.find(address) != peers_.end()) {
            GEO_LOG_DEBUG("peers", "Peer already exists (exact): " << address);
            GEO_METRIC_INC("peers.connect_outbound_fail_total", 1);
            return false;
        }

        // Проверяем, не пытаемся ли подключиться к самому себе
        if (local_port_ > 0 && NetworkUtils::is_self_address(address, local_port_)) {
            GEO_LOG_WARN("peers", "Skipping self-connection: " << address
                << " (local port: " << local_port_ << ")");
            GEO_METRIC_INC("peers.connect_outbound_fail_total", 1);
            return false;
        }

        const auto attempt_it = outbound_attempts_.find(address);
        const bool attempt_in_progress =
            attempt_it != outbound_attempts_.end() && attempt_it->second.in_progress;
        if (attempt_in_progress ||
            (!bypass_backoff && !should_try_outbound_unlocked(address, manual_target))) {
            GEO_LOG_DEBUG("peers", "Outbound peer is in backoff: " << address);
            GEO_METRIC_INC("peers.connect_outbound_backoff_total", 1);
            return false;
        }

        record_outbound_attempt_unlocked(address, manual_target);
        connect_timeout = outbound_connect_timeout_;
    }

    // Создаем пира
    auto peer = std::make_shared<Peer>(address);

    // TCP connect может ждать сетевой таймаут, поэтому выполняем его вне
    // общего mutex_: GUI/status polling не должен подвисать из-за dead peer.
    const auto connect_started = std::chrono::steady_clock::now();
    if (!peer->connect(connect_timeout)) {
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - connect_started
        ).count();
        GEO_LOG_DEBUG("peers", "TCP connect failed: " << address
            << " after " << elapsed_ms << " ms");
        {
            std::lock_guard<std::mutex> lock(mutex_);
            record_outbound_result_unlocked(address, false, manual_target, "tcp-connect-failed");
        }
        GEO_METRIC_INC("peers.connect_outbound_fail_total", 1);
        return false;
    }

    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - connect_started
    ).count();

    std::lock_guard<std::mutex> lock(mutex_);

    if (is_peer_banned_unlocked(address)) {
        GEO_LOG_WARN("peers", "Outbound peer became banned during connect: " << address);
        peer->disconnect();
        record_outbound_result_unlocked(address, false, manual_target, "banned-after-connect");
        GEO_METRIC_INC("peers.connect_outbound_fail_total", 1);
        return false;
    }

    size_t inbound = 0;
    size_t outbound = 0;
    count_peers(inbound, outbound);
    if (outbound >= max_outbound_) {
        GEO_LOG_WARN("peers", "Outbound limit reached after connect, closing " << address);
        peer->disconnect();
        record_outbound_result_unlocked(address, false, manual_target, "outbound-limit-after-connect");
        GEO_METRIC_INC("peers.connect_outbound_fail_total", 1);
        return false;
    }

    if (peers_.find(address) != peers_.end()) {
        GEO_LOG_DEBUG("peers", "Peer already exists after connect: " << address);
        peer->disconnect();
        record_outbound_result_unlocked(address, false, manual_target, "duplicate-after-connect");
        GEO_METRIC_INC("peers.connect_outbound_fail_total", 1);
        return false;
    }

    if (local_port_ > 0 && NetworkUtils::is_self_address(address, local_port_)) {
        GEO_LOG_WARN("peers", "Skipping self-connection after connect: " << address
            << " (local port: " << local_port_ << ")");
        peer->disconnect();
        record_outbound_result_unlocked(address, false, manual_target, "self-after-connect");
        GEO_METRIC_INC("peers.connect_outbound_fail_total", 1);
        return false;
    }

    // Создаем контекст
    PeerContext ctx;
    ctx.peer = peer;
    ctx.is_inbound = false;
    const auto now = std::chrono::steady_clock::now();
    ctx.connected_at = now;
    ctx.handshake_started_at = now;
    ctx.last_message_at = now;
    ctx.last_useful_message_at = now;
    ctx.last_ping = now;
    ctx.last_score_decay_at = now;

    peers_[address] = std::move(ctx);
    refresh_peer_metrics_unlocked();

    // Начинаем аутентификацию
    if (!start_authentication(peers_[address], our_private_key)) {
        GEO_LOG_ERROR("peers", "Failed to start authentication for " << address);
        disconnect_peer_unlocked(peers_[address], "auth-start-failed");
        peers_.erase(address);
        refresh_peer_metrics_unlocked();
        GEO_METRIC_INC("peers.connect_outbound_fail_total", 1);
        return false;
    }
    record_outbound_result_unlocked(address, true, manual_target, "connected");
    GEO_METRIC_INC("peers.connect_outbound_ok_total", 1);
    GEO_LOG_DEBUG("peers", "Outbound TCP connected: " << address
        << " in " << elapsed_ms << " ms");

    return true;
}


bool PeerManager::add_inbound_peer(
    SocketHandle socket_fd,
    const std::string& address,
    const bytes& our_private_key
) {
    // Ownership transfers even if validation or admission fails.
    auto transport = make_tcp_peer_transport(address, socket_fd);
    auto peer = std::make_shared<Peer>(address, std::move(transport));
    if (!peer->is_connected()) {
        GEO_METRIC_INC("peers.connect_inbound_fail_total", 1);
        return false;
    }
    if (SyncDebugEnabled()) {
        GEO_LOG_DEBUG("peers", "PeerManager::add_inbound_peer(" << address << ")");
    }
    std::string inbound_host;
    uint16_t inbound_port = 0;
    if (!NetworkUtils::split_host_port(address, inbound_host, inbound_port)) {
        GEO_LOG_WARN("peers", "Rejecting inbound with invalid address: " << address);
        GEO_METRIC_INC("peers.connect_inbound_fail_total", 1);
        return false;
    }
    Address local_auth_address;
    if (our_private_key.size() != 32 ||
        !crypto::compute_address_from_private_key(our_private_key, local_auth_address)) {
        GEO_LOG_ERROR("peers", "Rejecting inbound because local authentication key is invalid");
        GEO_METRIC_INC("peers.connect_inbound_fail_total", 1);
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    if (is_peer_banned_unlocked(address)) {
        GEO_LOG_WARN("peers", "Rejecting inbound from temporarily banned peer: " << address);
        GEO_METRIC_INC("peers.connect_inbound_fail_total", 1);
        return false;
    }

    // Проверяем, не подключен ли уже
    if (peers_.find(address) != peers_.end()) {
        if (SyncDebugEnabled()) {
            GEO_LOG_DEBUG("peers", "  Peer already connected");
        }
        GEO_METRIC_INC("peers.connect_inbound_fail_total", 1);
        return false;
    }

    if (inbound_diversity_limit_reached_unlocked(address)) {
        GEO_LOG_WARN("peers", "Rejecting inbound peer by diversity limit: " << address);
        GEO_METRIC_INC("peers.connect_inbound_fail_total", 1);
        return false;
    }

    size_t inbound = 0;
    size_t outbound = 0;
    count_peers(inbound, outbound);
    if (inbound >= max_inbound_) {
        std::string evicted_address;
        if (!try_evict_inbound_peer_unlocked(address, evicted_address)) {
            if (SyncDebugEnabled()) {
                GEO_LOG_DEBUG("peers", "  Cannot accept inbound (max reached)");
            }
            GEO_METRIC_INC("peers.connect_inbound_fail_total", 1);
            return false;
        }
        if (!evicted_address.empty()) {
            GEO_LOG_INFO("peers", "Evicted inbound peer " << evicted_address
                << " to accept " << address);
            peers_.erase(evicted_address);
            GEO_METRIC_INC("peers.disconnect_total", 1);
        }
    }

    // Создаем пира с существующим сокетом
    peer->set_state(PeerState::CONNECTED);
    peer->update_last_seen();

    // Создаем контекст
    PeerContext ctx;
    ctx.peer = peer;
    ctx.is_inbound = true;
    ctx.local_auth_address = std::move(local_auth_address);
    ctx.pending_private_key.assign(our_private_key.begin(), our_private_key.end());
    const auto now = std::chrono::steady_clock::now();
    ctx.connected_at = now;
    ctx.handshake_started_at = now;
    ctx.last_message_at = now;
    ctx.last_useful_message_at = now;
    ctx.last_ping = now;
    ctx.last_score_decay_at = now;

    peers_[address] = std::move(ctx);
    refresh_peer_metrics_unlocked();
    GEO_METRIC_INC("peers.connect_inbound_ok_total", 1);

    if (SyncDebugEnabled()) {
        GEO_LOG_DEBUG("peers", "  Inbound peer added, waiting for SECURE_HELLO");
    }
    // Ждем обязательный гибридный handshake от клиента.

    return true;
}

void PeerManager::remove_peer(const std::string& address) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = peers_.find(address);
    if (it != peers_.end()) {
        disconnect_peer_unlocked(it->second, "remove-peer");
        peers_.erase(it);
        GEO_METRIC_INC("peers.disconnect_total", 1);
        refresh_peer_metrics_unlocked();
    }
}

std::shared_ptr<Peer> PeerManager::get_peer(const std::string& address) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = peers_.find(address);
    if (it != peers_.end()) {
        return it->second.peer;
    }

    return nullptr;
}

std::vector<std::shared_ptr<Peer>> PeerManager::get_connected_peers() {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<std::shared_ptr<Peer>> result;

    for (const auto& [addr, ctx] : peers_) {
        if (ctx.peer && ctx.peer->is_connected()) {
            result.push_back(ctx.peer);
        }
    }

    return result;
}

std::vector<std::shared_ptr<Peer>> PeerManager::get_authenticated_peers() {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<std::shared_ptr<Peer>> result;

    for (const auto& [addr, ctx] : peers_) {
        if (ctx.peer &&
            ctx.peer->get_state() == PeerState::READY &&
            ctx.secure_established &&
            ctx.peer->is_secure() &&
            ctx.auth_session &&
            authenticator_.is_authenticated(ctx.auth_session)) {
            result.push_back(ctx.peer);
        }
    }

    return result;
}

bool PeerManager::send_message(
    std::shared_ptr<Peer> peer,
    MessageType type,
    const bytes& payload
) {
    if (!peer || !peer->is_connected()) {
        GEO_METRIC_INC("peers.send_fail_total", 1);
        return false;
    }

    if (payload.size() > kMaxMessagePayload || !payload_size_allowed(type, payload.size())) {
        GEO_METRIC_INC("peers.send_fail_total", 1);
        return false;
    }

    const bool is_open_handshake =
        type == MessageType::SECURE_HELLO || type == MessageType::SECURE_REPLY;
    if (is_open_handshake == peer->is_secure()) {
        // HELLO/REPLY are the only plaintext frames. Every other frame must be
        // protected, and plaintext handshake frames are forbidden afterwards.
        GEO_METRIC_INC("peers.send_fail_total", 1);
        return false;
    }

    bytes message = create_message(type, payload);
    ssize_t sent = peer->send(message);
    const bool ok = (sent == static_cast<ssize_t>(message.size()));
    if (ok) {
        GEO_METRIC_INC("peers.send_ok_total", 1);
        GEO_METRIC_INC("peers.sent_bytes_total", static_cast<int64_t>(message.size()));
        record_sent_message_best_effort(peer, type);
    } else {
        GEO_METRIC_INC("peers.send_fail_total", 1);
    }

    return ok;
}

size_t PeerManager::broadcast(
    MessageType type,
    const bytes& payload,
    std::shared_ptr<Peer> exclude
) {
    if (SyncDebugEnabled()) {
        GEO_LOG_DEBUG("peers", "[PeerManager::broadcast] START: type=" << static_cast<int>(type));
    }

    if (!payload_size_allowed(type, payload.size())) {
        GEO_METRIC_INC("peers.send_fail_total", 1);
        return 0;
    }

    auto peers = get_authenticated_peers();
    size_t count = 0;

    if (SyncDebugEnabled()) {
        GEO_LOG_DEBUG("peers", "[PeerManager::broadcast] Authenticated peers: " << peers.size());
    }

    bytes message = create_message(type, payload);

    // Вычисляем хеш для проверки known_inventory (только для TX и BLOCK)
    Hash256 inv_hash;
    bool check_known = false;

    if (type == MessageType::TX) {
        // Десериализуем TX чтобы получить хеш
        try {
            network::TxMessage msg = network::TxMessage::deserialize(payload);
            inv_hash = msg.tx.get_hash();
            check_known = true;
            if (SyncDebugEnabled()) {
                GEO_LOG_DEBUG("peers", "[PeerManager::broadcast] TX hash: " << std::hex << inv_hash[0] << "..." << std::dec);
            }
        } catch (...) {
            GEO_LOG_WARN("peers", "[PeerManager::broadcast] Failed to deserialize TX");
            // Игнорируем ошибки десериализации
        }
    } else if (type == MessageType::BLOCK) {
        // Десериализуем BLOCK чтобы получить хеш
        try {
            network::BlockMessage msg = network::BlockMessage::deserialize(payload);
            inv_hash = msg.block.get_hash();
            check_known = true;
            if (SyncDebugEnabled()) {
                GEO_LOG_DEBUG("peers", "[PeerManager::broadcast] BLOCK hash: " << std::hex << inv_hash[0] << "..." << std::dec);
            }
        } catch (...) {
            GEO_LOG_WARN("peers", "[PeerManager::broadcast] Failed to deserialize BLOCK");
            // Игнорируем ошибки десериализации
        }
    }

    for (auto& peer : peers) {
        if (SyncDebugEnabled()) {
            GEO_LOG_DEBUG("peers", "[PeerManager::broadcast] Checking peer: " << peer->get_address());
        }

        if (peer == exclude) {
            if (SyncDebugEnabled()) {
                GEO_LOG_DEBUG("peers", "[PeerManager::broadcast]   -> Excluded (source peer)");
            }
            continue;
        }

        // Проверяем known_inventory (как в Bitcoin)
        if (check_known && peer->has_known_inventory(inv_hash)) {
            if (SyncDebugEnabled()) {
                GEO_LOG_DEBUG("peers", "[PeerManager::broadcast]   -> Already knows (skipping)");
            }
            // Пир уже знает об этом объекте, не отправляем
            GEO_METRIC_INC("peers.broadcast_skip_known_total", 1);
            continue;
        }

        if (SyncDebugEnabled()) {
            GEO_LOG_DEBUG("peers", "[PeerManager::broadcast]   -> Sending...");
        }
        if (peer->send(message) == static_cast<ssize_t>(message.size())) {
            count++;
            GEO_METRIC_INC("peers.broadcast_sent_total", 1);
            GEO_METRIC_INC("peers.sent_bytes_total", static_cast<int64_t>(message.size()));
            record_sent_message_best_effort(peer, type);
            if (SyncDebugEnabled()) {
                GEO_LOG_DEBUG("peers", "[PeerManager::broadcast]   -> Sent successfully");
            }

            // Помечаем что пир теперь знает об этом объекте
            if (check_known) {
                peer->add_known_inventory(inv_hash);
                if (SyncDebugEnabled()) {
                    GEO_LOG_DEBUG("peers", "[PeerManager::broadcast]   -> Marked as known");
                }
            }
        } else {
            GEO_LOG_WARN("peers", "[PeerManager::broadcast] Send FAILED");
            GEO_METRIC_INC("peers.send_fail_total", 1);
        }
    }

    if (SyncDebugEnabled()) {
        GEO_LOG_DEBUG("peers", "[PeerManager::broadcast] DONE: sent to " << count << " peers");
    }

    return count;
}

size_t PeerManager::relay_inventory(
    InvMessage::InvType type,
    const std::vector<Hash256>& hashes,
    std::shared_ptr<Peer> exclude
) {
    std::vector<InvMessage::InvItem> items;
    items.reserve(hashes.size());
    for (const auto& hash : hashes) items.push_back({type, hash});
    return relay_inventory(items, std::move(exclude));
}

size_t PeerManager::relay_inventory(
    const std::vector<InvMessage::InvItem>& items,
    std::shared_ptr<Peer> exclude
) {
    std::vector<Hash256> hashes;
    hashes.reserve(items.size());
    for (const auto& item : items) hashes.push_back(item.hash);
    if (hashes.empty()) {
        return 0;
    }

    const auto peers = get_authenticated_peers();
    size_t sent_count = 0;

    for (const auto& peer : peers) {
        if (!peer || (exclude && peer == exclude)) {
            continue;
        }

        const auto unknown_hashes = peer->filter_unknown_inventory(hashes);
        if (unknown_hashes.empty()) {
            GEO_METRIC_INC("peers.relay_inv_skip_known_total", 1);
            continue;
        }
        // Filtering retains order, so preserve each unknown item's type
        // without taking the peer's inventory mutex once per item.
        std::vector<InvMessage::InvItem> unknown_items;
        unknown_items.reserve(unknown_hashes.size());
        for (const auto& item : items) {
            if (unknown_items.size() < unknown_hashes.size() &&
                item.hash == unknown_hashes[unknown_items.size()]) {
                unknown_items.push_back(item);
            }
        }

        bool sent_to_peer = false;
        for (size_t offset = 0; offset < unknown_hashes.size();) {
            const size_t chunk_size = std::min<size_t>(
                kPolicyMaxInvItems,
                unknown_hashes.size() - offset
            );

            InvMessage inv;
            inv.items.reserve(chunk_size);
            for (size_t i = 0; i < chunk_size; ++i) {
                inv.items.push_back(unknown_items[offset + i]);
            }

            if (!send_message(peer, MessageType::INV, inv.serialize())) {
                GEO_METRIC_INC("peers.relay_inv_send_fail_total", 1);
                break;
            }

            sent_to_peer = true;
            GEO_METRIC_INC("peers.relay_inv_sent_total", 1);
            for (size_t i = 0; i < chunk_size; ++i) {
                peer->add_known_inventory(unknown_hashes[offset + i]);
            }
            offset += chunk_size;
        }

        if (sent_to_peer) {
            ++sent_count;
        }
    }

    return sent_count;
}

void PeerManager::update() {
    std::vector<std::function<void()>> deferred_tasks;

    // Снапшот адресов, чтобы не держать mutex_ на весь цикл.
    std::vector<std::string> peer_addrs;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto now = std::chrono::steady_clock::now();
        for (auto it = temp_banned_ips_.begin(); it != temp_banned_ips_.end();) {
            if (now >= it->second) {
                it = temp_banned_ips_.erase(it);
            } else {
                ++it;
            }
        }
        refresh_peer_metrics_unlocked();
        peer_addrs.reserve(peers_.size());
        for (const auto& [addr, _] : peers_) {
            peer_addrs.push_back(addr);
        }
        if (peer_addrs.empty()) {
            next_peer_update_index_ = 0;
        } else {
            const size_t start = next_peer_update_index_ % peer_addrs.size();
            std::rotate(
                peer_addrs.begin(),
                peer_addrs.begin() + static_cast<std::ptrdiff_t>(start),
                peer_addrs.end()
            );
            next_peer_update_index_ = (start + 1) % peer_addrs.size();
        }
    }

    std::vector<std::pair<std::string, std::shared_ptr<Peer>>> to_remove;
    size_t remaining_frame_budget = detail::kMaxFramesPerManagerUpdate;
    size_t remaining_read_budget = detail::kMaxReceiveBytesPerManagerUpdate;
    for (const auto& addr : peer_addrs) {
        std::unique_lock<std::mutex> lock(mutex_);
        auto it = peers_.find(addr);
        if (it == peers_.end()) {
            continue;
        }

        auto& ctx = it->second;
        const auto now = std::chrono::steady_clock::now();
        decay_policy_score_unlocked(ctx, now);

        if (!ctx.peer || !ctx.peer->is_connected()) {
            record_disconnect_unlocked(ctx, "transport-disconnected");
            GEO_LOG_INFO("peers", "Peer " << addr << " disconnected");
            to_remove.emplace_back(addr, ctx.peer);
            continue;
        }

        const bool is_ready = (ctx.peer->get_state() == PeerState::READY);

        // Для fully-ready peers используем обычный network timeout.
        if (is_ready && ctx.peer->is_timeout(kPeerTimeoutSeconds)) {
            GEO_LOG_WARN("peers", "Peer " << addr << " timed out");
            disconnect_peer_unlocked(ctx, "idle-timeout");
            to_remove.emplace_back(addr, ctx.peer);
            continue;
        }

        if (is_ready &&
            has_steady_time(ctx.last_ping_sent_at) &&
            ctx.last_pong_at < ctx.last_ping_sent_at &&
            (now - ctx.last_ping_sent_at) > std::chrono::seconds(kPingReplyTimeoutSeconds)) {
            GEO_LOG_WARN("peers", "Peer " << addr << " ping timed out");
            disconnect_peer_unlocked(ctx, "ping-timeout");
            to_remove.emplace_back(addr, ctx.peer);
            continue;
        }

        // Для handshake/auth стадии используем более мягкий auth timeout.
        if (!is_ready) {
            if (ctx.auth_session) {
                if (authenticator_.is_timeout(ctx.auth_session, kAuthTimeoutSeconds)) {
                    GEO_LOG_WARN("peers", "Peer " << addr << " authentication timed out");
                    disconnect_peer_unlocked(ctx, "auth-timeout");
                    to_remove.emplace_back(addr, ctx.peer);
                    continue;
                }
                if (steady_age_ms(now, ctx.last_useful_message_at) >
                    static_cast<uint64_t>(kAuthProgressStallSeconds) * 1000ULL) {
                    GEO_LOG_WARN("peers", "Peer " << addr << " authentication progress stalled");
                    disconnect_peer_unlocked(ctx, "auth-progress-stall");
                    to_remove.emplace_back(addr, ctx.peer);
                    continue;
                }
            } else {
                const auto connected_for = std::chrono::duration_cast<std::chrono::seconds>(
                    now - ctx.connected_at
                ).count();
                if (connected_for > kVersionStallSeconds) {
                    GEO_LOG_WARN("peers", "Peer " << addr << " handshake timed out");
                    disconnect_peer_unlocked(ctx, "version-stall");
                    to_remove.emplace_back(addr, ctx.peer);
                    continue;
                }
            }
        }

        // Обрабатываем одного пира за одну lock-сессию.
        process_peer(
            ctx,
            lock,
            deferred_tasks,
            remaining_frame_budget,
            remaining_read_budget
        );

        if (!ctx.peer || !ctx.peer->is_connected()) {
            record_disconnect_unlocked(ctx, "transport-disconnected");
            to_remove.emplace_back(addr, ctx.peer);
            continue;
        }

        // Отправляем ping раз в 30 секунд
        const auto ping_now = std::chrono::steady_clock::now();
        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            ping_now - ctx.last_ping
        ).count();

        if (elapsed > kPingIntervalSeconds && ctx.peer->get_state() == PeerState::READY) {
            auto ping_peer = ctx.peer;
            ctx.last_ping = ping_now;
            ctx.last_ping_sent_at = ping_now;
            lock.unlock();
            bool sent = false;
            if (ping_peer) {
                sent = send_message(ping_peer, MessageType::PING, bytes());
            }
            lock.lock();
            auto it2 = peers_.find(addr);
            if (it2 != peers_.end() && it2->second.peer == ping_peer) {
                if (!sent) {
                    disconnect_peer_unlocked(it2->second, "ping-send-failed");
                    to_remove.emplace_back(addr, ping_peer);
                }
            }
        }
    }

    if (remaining_frame_budget == 0 || remaining_read_budget == 0) {
        GEO_METRIC_INC("peers.update_budget_exhausted_total", 1);
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        // Удаляем отключенных пиров
        for (const auto& [addr, peer] : to_remove) {
            auto it = peers_.find(addr);
            if (it != peers_.end() && it->second.peer == peer) {
                GEO_LOG_INFO("peers", "Removing peer " << addr);
                disconnect_peer_unlocked(it->second, "removed");
                peers_.erase(it);
                GEO_METRIC_INC("peers.disconnect_total", 1);
            }
        }
        refresh_peer_metrics_unlocked();
    }

    // Выполняем отложенные задачи (уже без блокировки)
    for (auto& task : deferred_tasks) {
        try {
            task();
        } catch (const std::exception& error) {
            GEO_LOG_ERROR("peers", "Application callback failed: " << error.what());
            GEO_METRIC_INC("peers.callback_fail_total", 1);
        } catch (...) {
            GEO_LOG_ERROR("peers", "Application callback failed");
            GEO_METRIC_INC("peers.callback_fail_total", 1);
        }
    }
}

PeerManagerStats PeerManager::get_stats() const {
    std::lock_guard<std::mutex> lock(mutex_);

    PeerManagerStats stats = {};
    stats.total_peers = peers_.size();

    size_t inbound = 0, outbound = 0;

    for (const auto& [addr, ctx] : peers_) {
        if (ctx.peer && ctx.peer->is_connected()) {
            stats.connected_peers++;

            if (ctx.peer->get_state() == PeerState::READY) {
                stats.authenticated_peers++;
            }
            if (ctx.secure_established && ctx.peer->is_secure()) {
                stats.encrypted_peers++;
            }

            const auto& info = ctx.peer->get_info();
            stats.total_bytes_sent = saturating_add_u64(stats.total_bytes_sent, info.bytes_sent);
            stats.total_bytes_received = saturating_add_u64(stats.total_bytes_received, info.bytes_received);

            if (ctx.is_inbound) {
                inbound++;
            } else {
                outbound++;
            }
        }
    }

    stats.inbound_peers = inbound;
    stats.outbound_peers = outbound;
    stats.policy_rejects = total_policy_rejects_;
    const auto now = std::chrono::steady_clock::now();
    for (const auto& [ip, until] : temp_banned_ips_) {
        if (now < until) {
            stats.active_temp_bans++;
        }
    }

    return stats;
}

void PeerManager::set_max_peers(size_t max_inbound, size_t max_outbound) {
    std::lock_guard<std::mutex> lock(mutex_);
    max_inbound_ = std::min(max_inbound, kMaxInboundPeers);
    max_outbound_ = std::min(max_outbound, kMaxOutboundPeers);
}

void PeerManager::set_inbound_handshake_limits(
    double rate, double burst, double group_rate, double group_burst
) {
    std::lock_guard<std::mutex> lock(mutex_);
    inbound_handshake_rate_ = std::max(0.0, rate);
    inbound_handshake_burst_ = std::max(0.0, burst);
    inbound_group_handshake_rate_ = std::max(0.0, group_rate);
    inbound_group_handshake_burst_ = std::max(0.0, group_burst);
    inbound_handshake_bucket_started_ = false;
    inbound_handshake_group_buckets_.clear();
}

bool PeerManager::can_accept_inbound() const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t inbound, outbound;
    count_peers(inbound, outbound);
    return inbound < max_inbound_;
}

bool PeerManager::can_add_outbound() const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t inbound, outbound;
    count_peers(inbound, outbound);
    return outbound < max_outbound_;
}

void PeerManager::process_peer(
    PeerContext& ctx,
    std::unique_lock<std::mutex>& lock,
    std::vector<std::function<void()>>& deferred_tasks,
    size_t& remaining_frame_budget,
    size_t& remaining_read_budget
) {
    if (!ctx.peer || !ctx.peer->is_connected() || remaining_frame_budget == 0) {
        return;
    }

    // Read at most the remaining state-specific buffer budget. Handshake
    // messages are tiny, so an unauthenticated socket never gets to reserve an
    // 8 MiB per-peer buffer.
    bytes buffer;
    const bool ready_before_read = ctx.peer->get_state() == PeerState::READY;
    const size_t receive_limit = ready_before_read
        ? kMaxReceiveBuffer
        : detail::kMaxPreAuthReceiveBuffer;
    if (ctx.receive_buffer.size() >= receive_limit) {
        penalize_peer_unlocked(ctx, detail::kPolicyBanScoreThreshold, "receive-buffer-limit");
        disconnect_peer_unlocked(ctx, "receive-buffer-limit");
        return;
    }
    // Drain complete buffered frames before asking the transport for more.
    // An orderly EOF closes Peer, so reading it first would discard records
    // left by the previous frame budget and misreport an authentication error.
    bool needs_read = ctx.receive_buffer.empty();
    if (!needs_read && ctx.peer->is_secure()) {
        size_t record_size = 0;
        needs_read = SecureChannel::inspect_record_size(
            ctx.receive_buffer.data(), ctx.receive_buffer.size(), record_size) ==
            SecureRecordSizeStatus::INCOMPLETE;
    } else if (!needs_read) {
        needs_read = ctx.receive_buffer.size() < 13;
        if (!needs_read) {
            try {
                const bytes header_bytes(ctx.receive_buffer.begin(), ctx.receive_buffer.begin() + 13);
                const auto header = MessageHeader::deserialize(header_bytes);
                needs_read = header.length <= kMaxMessagePayload &&
                    ctx.receive_buffer.size() < 13 + static_cast<size_t>(header.length);
            } catch (...) {
                // Let the parser below diagnose an invalid buffered header.
            }
        }
    }
    ssize_t received = 0;
    if (needs_read && remaining_read_budget != 0) {
        const size_t max_read = std::min({
            static_cast<size_t>(65536),
            receive_limit - ctx.receive_buffer.size(),
            remaining_read_budget
        });
        received = ctx.peer->receive(buffer, max_read);
    }

    if (!ctx.peer->is_connected()) {
        // An incomplete final frame is a truncated stream, not failed AEAD.
        return;
    }

    if (received <= 0 && ctx.receive_buffer.empty()) {
        return;
    }
    if (received > 0) {
        const size_t received_size = static_cast<size_t>(received);
        remaining_read_budget = received_size >= remaining_read_budget
            ? 0
            : remaining_read_budget - received_size;
        GEO_METRIC_INC("peers.recv_bytes_total", received);

        if (receive_buffer_would_exceed_limit(
                ctx.receive_buffer.size(),
                buffer.size(),
                receive_limit)) {
            penalize_peer_unlocked(ctx, detail::kPolicyBanScoreThreshold, "receive-buffer-limit");
            disconnect_peer_unlocked(ctx, "receive-buffer-limit");
            return;
        }
        ctx.receive_buffer.insert(ctx.receive_buffer.end(), buffer.begin(), buffer.end());
    }

    // Parse with an offset and compact once. Erasing the vector for every tiny
    // frame made a packet containing many PINGs quadratic in CPU time.
    size_t consumed = 0;
    size_t frames_processed = 0;
    while (frames_processed < detail::kMaxFramesPerPeerUpdate &&
           remaining_frame_budget != 0) {
        const size_t remaining = ctx.receive_buffer.size() - consumed;
        if (remaining == 0) {
            break;
        }

        MessageHeader header;
        bytes payload;
        size_t wire_size = 0;

        if (ctx.peer->is_secure()) {
            if (remaining < kSecureRecordPrefixSize) {
                break;
            }

            size_t record_size = 0;
            const auto record_status = SecureChannel::inspect_record_size(
                ctx.receive_buffer.data() + consumed,
                remaining,
                record_size
            );
            if (record_status == SecureRecordSizeStatus::INVALID) {
                penalize_peer_unlocked(
                    ctx, detail::kPolicyBanScoreThreshold, "secure-record-length");
                disconnect_peer_unlocked(ctx, "secure-record-length");
                return;
            }
            if (record_status == SecureRecordSizeStatus::INCOMPLETE) {
                break;
            }

            bytes record(
                ctx.receive_buffer.begin() + consumed,
                ctx.receive_buffer.begin() + consumed + record_size
            );
            bytes plaintext_frame;
            if (!ctx.peer->decrypt_secure_record(record, plaintext_frame)) {
                penalize_peer_unlocked(
                    ctx, detail::kPolicyBanScoreThreshold, "secure-record-auth-failed");
                disconnect_peer_unlocked(ctx, "secure-record-auth-failed");
                return;
            }
            if (!parse_message(plaintext_frame, header, payload)) {
                penalize_peer_unlocked(
                    ctx, detail::kPolicyBanScoreThreshold, "bad-encrypted-message-frame");
                disconnect_peer_unlocked(ctx, "bad-encrypted-message-frame");
                return;
            }
            wire_size = record_size;
        } else {
            if (remaining < 13) {
                break;
            }
            try {
                bytes header_bytes(
                    ctx.receive_buffer.begin() + consumed,
                    ctx.receive_buffer.begin() + consumed + 13
                );
                header = MessageHeader::deserialize(header_bytes);
            } catch (...) {
                penalize_peer_unlocked(
                    ctx, detail::kPolicyBanScoreThreshold, "bad-message-header");
                disconnect_peer_unlocked(ctx, "bad-message-header");
                return;
            }
            if (header.magic != expected_network_magic()) {
                penalize_peer_unlocked(
                    ctx, detail::kPolicyBanScoreThreshold, "bad-network-magic");
                disconnect_peer_unlocked(ctx, "bad-network-magic");
                return;
            }
            if (!payload_size_allowed(header.type, header.length)) {
                penalize_peer_unlocked(
                    ctx, detail::kPolicyBanScoreThreshold, "message-payload-limit");
                disconnect_peer_unlocked(ctx, "message-payload-limit");
                return;
            }

            const size_t message_size = 13 + static_cast<size_t>(header.length);
            if (remaining < message_size) {
                break;
            }
            bytes one_message(
                ctx.receive_buffer.begin() + consumed,
                ctx.receive_buffer.begin() + consumed + message_size
            );
            if (!parse_message(one_message, header, payload)) {
                penalize_peer_unlocked(
                    ctx, detail::kPolicyBanScoreThreshold, "bad-message-frame");
                disconnect_peer_unlocked(ctx, "bad-message-frame");
                return;
            }
            wire_size = message_size;
        }

        if (!payload_size_allowed(header.type, payload.size())) {
            penalize_peer_unlocked(
                ctx, detail::kPolicyBanScoreThreshold, "message-payload-limit");
            disconnect_peer_unlocked(ctx, "message-payload-limit");
            return;
        }

        consumed += wire_size;
        ++frames_processed;
        --remaining_frame_budget;

        // Обрабатываем сообщение.
        handle_message(ctx, header, payload, lock, deferred_tasks);

        if (!ctx.peer || !ctx.peer->is_connected()) {
            return;
        }
    }

    if (consumed != 0) {
        ctx.receive_buffer.erase(
            ctx.receive_buffer.begin(),
            ctx.receive_buffer.begin() + consumed
        );
    }
}

void PeerManager::handle_message(
    PeerContext& ctx,
    const MessageHeader& header,
    const bytes& payload,
    std::unique_lock<std::mutex>& lock,
    std::vector<std::function<void()>>& deferred_tasks
) {
    GEO_METRIC_INC("peers.recv_msgs_total", 1);
    ctx.last_message_at = std::chrono::steady_clock::now();
    ctx.last_message_type = message_type_name(header.type);
    ctx.recv_message_counts[static_cast<size_t>(header.type)]++;

    if (SyncDebugEnabled()) {
        GEO_LOG_DEBUG(
            "peers",
            "PeerManager::handle_message() type=" << static_cast<int>(header.type)
                << " from " << ctx.peer->get_address()
        );
    }

    if (payload.size() > kMaxMessagePayload) {
        GEO_LOG_ERROR("peers", "payload too large: " << payload.size());
        penalize_peer_unlocked(ctx, detail::kPolicyBanScoreThreshold, "payload-too-large");
        disconnect_peer_unlocked(ctx, "payload-too-large");
        return;
    }

    const PeerState peer_state = ctx.peer ? ctx.peer->get_state() : PeerState::DISCONNECTED;
    if (peer_state == PeerState::READY &&
        (!ctx.secure_established || !ctx.peer->is_secure())) {
        penalize_peer_unlocked(
            ctx, detail::kPolicyBanScoreThreshold, "plaintext-ready-session");
        disconnect_peer_unlocked(ctx, "plaintext-ready-session");
        return;
    }
    if (peer_state != PeerState::READY) {
        bool expected_handshake_message = false;
        if (!ctx.secure_established || !ctx.peer->is_secure()) {
            expected_handshake_message = ctx.is_inbound
                ? (header.type == MessageType::SECURE_HELLO)
                : (header.type == MessageType::SECURE_REPLY);
        } else if (!ctx.version_received) {
            expected_handshake_message = (header.type == MessageType::VERSION);
        } else if (ctx.auth_session) {
            if (ctx.auth_session->role == AuthRole::PROVER) {
                expected_handshake_message = (header.type == MessageType::CHALLENGE);
            } else if (ctx.auth_session->role == AuthRole::VERIFIER) {
                expected_handshake_message = (header.type == MessageType::RESPONSE);
            }
        }

        if (!expected_handshake_message) {
            penalize_peer_unlocked(ctx, 50, "preauth-message");
            disconnect_peer_unlocked(ctx, "preauth-message");
            return;
        }
    } else if (header.type == MessageType::VERSION ||
               header.type == MessageType::CHALLENGE ||
               header.type == MessageType::RESPONSE ||
               header.type == MessageType::SECURE_HELLO ||
               header.type == MessageType::SECURE_REPLY) {
        penalize_peer_unlocked(ctx, 50, "handshake-after-ready");
        disconnect_peer_unlocked(ctx, "handshake-after-ready");
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    ctx.last_useful_message_at = now;
    const char* rate_reject_reason = nullptr;
    if (!consume_rate_token_unlocked(ctx, header.type, now, rate_reject_reason)) {
        penalize_peer_unlocked(ctx, 20, rate_reject_reason);
        if (ctx.peer && ctx.peer->is_connected()) {
            disconnect_peer_unlocked(ctx, rate_reject_reason);
        }
        return;
    }

    // Policy-проверки для самых дешевых DoS-векторов.
    if (header.type == MessageType::INV) {
        if (!validate_inv_like_payload_unlocked(
                payload, kPolicyMaxInvItems, "inv-malformed", "inv-too-many-items", ctx)) {
            if (ctx.peer && ctx.peer->is_connected()) {
                disconnect_peer_unlocked(ctx, "inv-policy-reject");
            }
            return;
        }
    } else if (header.type == MessageType::GETDATA) {
        if (!validate_inv_like_payload_unlocked(
                payload, kPolicyMaxGetDataItems, "getdata-malformed", "getdata-too-many-items", ctx)) {
            if (ctx.peer && ctx.peer->is_connected()) {
                disconnect_peer_unlocked(ctx, "getdata-policy-reject");
            }
            return;
        }
    } else if (header.type == MessageType::ADDR) {
        if (!validate_addr_payload_unlocked(payload, ctx)) {
            if (ctx.peer && ctx.peer->is_connected()) {
                disconnect_peer_unlocked(ctx, "addr-policy-reject");
            }
            return;
        }
    }

    // Обрабатываем системные сообщения
    switch (header.type) {
        case MessageType::SECURE_HELLO:
            handle_secure_hello(ctx, payload);
            return;

        case MessageType::SECURE_REPLY:
            handle_secure_reply(ctx, payload);
            return;

        case MessageType::VERSION:
            if (SyncDebugEnabled()) {
                GEO_LOG_DEBUG("peers", "  -> handle_version");
            }
            handle_version(ctx, payload);
            return;

        case MessageType::CHALLENGE:
            if (SyncDebugEnabled()) {
                GEO_LOG_DEBUG("peers", "  -> handle_challenge");
            }
            handle_challenge(ctx, payload, lock, deferred_tasks);
            return;

        case MessageType::RESPONSE:
            if (SyncDebugEnabled()) {
                GEO_LOG_DEBUG("peers", "  -> handle_response");
            }
            handle_response(ctx, payload, lock, deferred_tasks);
            return;

        case MessageType::PING:
            if (SyncDebugEnabled()) {
                GEO_LOG_DEBUG("peers", "  -> handle_ping");
            }
            handle_ping(ctx);
            return;

        case MessageType::PONG:
            if (SyncDebugEnabled()) {
                GEO_LOG_DEBUG("peers", "  -> PONG received");
            }
            // Просто обновляем last_seen
            ctx.peer->update_last_seen();
            ctx.last_pong_at = now;
            if (has_steady_time(ctx.last_ping_sent_at) && now >= ctx.last_ping_sent_at) {
                ctx.ping_rtt_ms = steady_age_ms(now, ctx.last_ping_sent_at);
            }
            return;

        case MessageType::REJECT:
            handle_reject(ctx, payload);
            return;

        case MessageType::ADDR:
            // ADDR обрабатывается в node callback после проверки READY.
            break;

        case MessageType::VERACK:
            // Дополнительное подтверждение версии. Безопасно игнорируем.
            return;

        default:
            if (SyncDebugEnabled()) {
                GEO_LOG_DEBUG("peers", "  -> callback (type=" << static_cast<int>(header.type) << ")");
            }
            break;
    }

    // Любые "прикладные" сообщения принимаем только от полностью
    // аутентифицированного пира.
    if (!ctx.peer || ctx.peer->get_state() != PeerState::READY) {
        GEO_LOG_WARN(
            "peers",
            "Disconnecting peer " << (ctx.peer ? ctx.peer->get_address() : "unknown")
                << ": non-handshake message before READY state"
        );
        if (ctx.peer) {
            penalize_peer_unlocked(ctx, 50, "preauth-message");
            disconnect_peer_unlocked(ctx, "preauth-message");
        }
        return;
    }

    // Остальные сообщения передаем в callback
    auto callback = message_callback_;
    if (callback) {
        auto peer = ctx.peer;
        auto type = header.type;
        auto data = payload;
        deferred_tasks.emplace_back([callback = std::move(callback), peer, type, data]() {
            callback(peer, type, data);
        });
    }
}

void PeerManager::count_peers(size_t& inbound, size_t& outbound) const {
    inbound = 0;
    outbound = 0;

    for (const auto& [addr, ctx] : peers_) {
        if (ctx.peer && ctx.peer->is_connected()) {
            if (ctx.is_inbound) {
                inbound++;
            } else {
                outbound++;
            }
        }
    }
}

void PeerManager::refresh_peer_metrics_unlocked() const {
    size_t connected = 0;
    size_t authenticated = 0;
    size_t inbound = 0;
    size_t outbound = 0;
    for (const auto& [addr, ctx] : peers_) {
        (void)addr;
        if (!ctx.peer || !ctx.peer->is_connected()) {
            continue;
        }
        ++connected;
        if (ctx.peer->get_state() == PeerState::READY &&
            ctx.secure_established &&
            ctx.peer->is_secure() &&
            ctx.auth_session &&
            authenticator_.is_authenticated(ctx.auth_session)) {
            ++authenticated;
        }
        if (ctx.is_inbound) {
            ++inbound;
        } else {
            ++outbound;
        }
    }

    const auto now_steady = std::chrono::steady_clock::now();
    size_t active_temp_bans = 0;
    for (const auto& [ip, until] : temp_banned_ips_) {
        (void)ip;
        if (now_steady < until) {
            ++active_temp_bans;
        }
    }

    const auto now_sys = std::chrono::system_clock::now();
    size_t active_manual_bans = 0;
    for (const auto& [ip, until] : manual_banned_ips_) {
        (void)ip;
        if (now_sys < until) {
            ++active_manual_bans;
        }
    }

    GEO_METRIC_SET("peers.connected", static_cast<int64_t>(connected));
    GEO_METRIC_SET("peers.authenticated", static_cast<int64_t>(authenticated));
    GEO_METRIC_SET("peers.inbound", static_cast<int64_t>(inbound));
    GEO_METRIC_SET("peers.outbound", static_cast<int64_t>(outbound));
    GEO_METRIC_SET("peers.temp_bans", static_cast<int64_t>(active_temp_bans));
    GEO_METRIC_SET("peers.manual_bans", static_cast<int64_t>(active_manual_bans));
}

bool PeerManager::disconnect_peer(const std::string& address_or_ip) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = peers_.find(address_or_ip);
    if (it != peers_.end()) {
        disconnect_peer_unlocked(it->second, "rpc-disconnect");
        peers_.erase(it);
        GEO_METRIC_INC("peers.disconnect_total", 1);
        refresh_peer_metrics_unlocked();
        return true;
    }

    const std::string key_ip = extract_ip(address_or_ip);
    if (key_ip.empty()) {
        return false;
    }

    for (auto jt = peers_.begin(); jt != peers_.end(); ++jt) {
        if (extract_ip(jt->first) == key_ip) {
            disconnect_peer_unlocked(jt->second, "rpc-disconnect");
            peers_.erase(jt);
            GEO_METRIC_INC("peers.disconnect_total", 1);
            refresh_peer_metrics_unlocked();
            return true;
        }
    }
    return false;
}

bool PeerManager::disconnect_peer(std::shared_ptr<Peer> peer, const std::string& reason) {
    if (!peer) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    const std::string address = peer->get_address();

    auto it = peers_.find(address);
    if (it == peers_.end()) {
        for (auto jt = peers_.begin(); jt != peers_.end(); ++jt) {
            if (jt->second.peer == peer) {
                it = jt;
                break;
            }
        }
    }
    if (it == peers_.end()) {
        return false;
    }

    disconnect_peer_unlocked(it->second, reason.empty() ? "peer-disconnect" : reason);
    peers_.erase(it);
    GEO_METRIC_INC("peers.disconnect_total", 1);
    refresh_peer_metrics_unlocked();
    return true;
}

bool PeerManager::set_manual_ban(const std::string& address_or_ip, uint64_t ban_seconds) {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string ip = extract_ip(address_or_ip);
    if (ip.empty()) {
        return false;
    }
    if (ban_seconds == 0) {
        ban_seconds = 24 * 60 * 60;
    }
    const auto until = std::chrono::system_clock::now() + std::chrono::seconds(ban_seconds);
    manual_banned_ips_[ip] = until;

    // Если пир уже подключен — сразу отключаем.
    for (auto it = peers_.begin(); it != peers_.end();) {
        if (extract_ip(it->first) == ip) {
            disconnect_peer_unlocked(it->second, "manual-ban");
            it = peers_.erase(it);
            GEO_METRIC_INC("peers.disconnect_total", 1);
        } else {
            ++it;
        }
    }
    refresh_peer_metrics_unlocked();
    return true;
}

bool PeerManager::remove_manual_ban(const std::string& address_or_ip) {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string ip = extract_ip(address_or_ip);
    if (ip.empty()) {
        return false;
    }
    const bool removed = manual_banned_ips_.erase(ip) > 0;
    refresh_peer_metrics_unlocked();
    return removed;
}

void PeerManager::clear_manual_bans() {
    std::lock_guard<std::mutex> lock(mutex_);
    manual_banned_ips_.clear();
    temp_banned_ips_.clear();
    refresh_peer_metrics_unlocked();
}

std::vector<PeerBanInfo> PeerManager::list_bans() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<PeerBanInfo> out;

    const auto now_sys = std::chrono::system_clock::now();
    const auto now_steady = std::chrono::steady_clock::now();

    out.reserve(temp_banned_ips_.size() + manual_banned_ips_.size());

    for (const auto& [ip, until] : manual_banned_ips_) {
        if (until <= now_sys) {
            continue;
        }
        PeerBanInfo e;
        e.address = ip;
        e.banned_until = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
            until.time_since_epoch()).count());
        e.manual = true;
        out.push_back(std::move(e));
    }

    for (const auto& [ip, until_steady] : temp_banned_ips_) {
        if (until_steady <= now_steady) {
            continue;
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::seconds>(until_steady - now_steady);
        PeerBanInfo e;
        e.address = ip;
        e.banned_until = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
            (now_sys + remaining).time_since_epoch()).count());
        e.manual = false;
        out.push_back(std::move(e));
    }

    std::sort(out.begin(), out.end(), [](const PeerBanInfo& a, const PeerBanInfo& b) {
        if (a.address != b.address) return a.address < b.address;
        if (a.banned_until != b.banned_until) return a.banned_until < b.banned_until;
        return static_cast<int>(a.manual) < static_cast<int>(b.manual);
    });

    return out;
}

std::vector<PeerStats> PeerManager::get_peer_stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<PeerStats> stats;
    const auto now = std::chrono::steady_clock::now();

    int peer_id = 0;
    for (const auto& [addr, ctx] : peers_) {
        if (!ctx.peer || !ctx.peer->is_connected()) {
            continue;
        }

        PeerStats peer_stat{};
        peer_stat.id = peer_id++;
        peer_stat.address = addr;
        peer_stat.state = peer_state_name(ctx.peer->get_state());

        // Проверяем аутентификацию через состояние пира и сессию
        peer_stat.authenticated = (ctx.peer->get_state() == PeerState::READY &&
                                   ctx.secure_established &&
                                   ctx.peer->is_secure() &&
                                   ctx.auth_session &&
                                   authenticator_.is_authenticated(ctx.auth_session));
        peer_stat.inbound = ctx.is_inbound;

        // Получаем информацию о пире
        auto info = ctx.peer->get_info();
        peer_stat.encrypted = info.transport_encrypted && ctx.secure_established;
        peer_stat.secure_wire_version = info.secure_wire_version;
        peer_stat.secure_suite = info.secure_suite;

        // Конвертируем connected_since в unix timestamp
        auto connected_time = std::chrono::duration_cast<std::chrono::seconds>(
            info.connected_since.time_since_epoch()
        ).count();
        peer_stat.conntime = connected_time;

        peer_stat.bytes_sent = info.bytes_sent;
        peer_stat.bytes_recv = info.bytes_received;
        peer_stat.version = info.version;
        peer_stat.subver = info.user_agent;
        peer_stat.starting_height = info.height;
        peer_stat.auth_address = info.auth_address;

        // Конвертируем last_seen в unix timestamp для обоих полей
        auto last_seen_time = std::chrono::duration_cast<std::chrono::seconds>(
            info.last_seen.time_since_epoch()
        ).count();
        peer_stat.last_send = last_seen_time;
        peer_stat.last_recv = last_seen_time;
        peer_stat.policy_score = ctx.policy_score;
        peer_stat.last_message_type = ctx.last_message_type;
        peer_stat.last_message_age_ms = steady_age_ms(now, ctx.last_message_at);
        peer_stat.last_useful_age_ms = steady_age_ms(now, ctx.last_useful_message_at);
        peer_stat.ping_rtt_ms = ctx.ping_rtt_ms;
        peer_stat.last_disconnect_reason = ctx.last_disconnect_reason;

        stats.push_back(peer_stat);
    }

    return stats;
}

size_t PeerManager::get_peer_count() const {
    if (SyncDebugEnabled()) {
        GEO_LOG_DEBUG("peers", "PeerManager::get_peer_count() - acquiring lock...");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (SyncDebugEnabled()) {
        GEO_LOG_DEBUG("peers", "PeerManager::get_peer_count() - lock acquired");
    }

    size_t count = 0;

    for (const auto& [addr, ctx] : peers_) {
        if (ctx.peer && ctx.peer->is_connected()) {
            count++;
        }
    }

    if (SyncDebugEnabled()) {
        GEO_LOG_DEBUG("peers", "PeerManager::get_peer_count() - returning " << count);
    }
    return count;
}

std::map<std::string, uint64_t> PeerManager::get_policy_reject_counters() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::map<std::string, uint64_t> out;
    for (const auto& [reason, cnt] : policy_reject_counters_) {
        out.emplace(reason, cnt);
    }
    return out;
}

PeerConnectionDiagnostics PeerManager::get_connection_diagnostics() const {
    PeerConnectionDiagnostics diagnostics;
    diagnostics.stats = get_stats();
    diagnostics.peers = get_peer_stats();

    std::lock_guard<std::mutex> lock(mutex_);
    diagnostics.recent_disconnects.assign(recent_disconnects_.begin(), recent_disconnects_.end());

    for (const auto& [reason, cnt] : disconnect_reason_counters_) {
        diagnostics.disconnect_reasons.emplace(reason, cnt);
    }
    for (const auto& [reason, cnt] : policy_reject_counters_) {
        diagnostics.policy_rejects.emplace(reason, cnt);
    }

    const auto now = std::chrono::steady_clock::now();
    diagnostics.outbound_attempts.reserve(outbound_attempts_.size());
    for (const auto& [address, state] : outbound_attempts_) {
        PeerOutboundAttemptInfo info;
        info.address = address;
        info.fail_count = state.fail_count;
        info.last_attempt_age_ms = steady_age_ms(now, state.last_attempt);
        info.last_success_age_ms = steady_age_ms(now, state.last_success);
        info.next_attempt_in_ms = steady_until_ms(now, state.next_attempt);
        info.last_error = state.last_error;
        info.manual = state.manual;
        info.in_progress = state.in_progress;
        info.in_backoff = !info.in_progress && info.next_attempt_in_ms > 0;
        diagnostics.outbound_attempts.push_back(std::move(info));
    }
    std::sort(
        diagnostics.outbound_attempts.begin(),
        diagnostics.outbound_attempts.end(),
        [](const PeerOutboundAttemptInfo& a, const PeerOutboundAttemptInfo& b) {
            return a.address < b.address;
        }
    );

    return diagnostics;
}

bool PeerManager::connect_to_node(const std::string& address, const bytes& our_private_key) {
    bytes key_to_use;
    utils::ScopedContainerCleanser<bytes> keyCleanser(key_to_use);
    if (!our_private_key.empty()) {
        key_to_use = our_private_key;
    } else {
        std::lock_guard<std::mutex> lock(mutex_);
        key_to_use = private_key_;
    }

    // RPC `onetry` is an operator-requested immediate attempt. It bypasses a
    // previous retry deadline, but add_outbound_peer still rejects an active
    // attempt, duplicate connection, ban, self-connect or capacity overflow.
    return add_outbound_peer(address, key_to_use, true, true);
}

void PeerManager::add_node(const std::string& address) {
    bytes key_copy;
    utils::ScopedContainerCleanser<bytes> keyCleanser(key_copy);
    bool should_connect = false;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        // Проверяем, что узел ещё не добавлен
        auto it = std::find(added_nodes_.begin(), added_nodes_.end(), address);
        if (it != added_nodes_.end()) {
            return;
        }
        if (added_nodes_.size() >= kMaxAddedNodes) {
            GEO_LOG_WARN("peers", "Persistent addnode limit reached, ignoring " << address);
            return;
        }

        added_nodes_.push_back(address);

        // Пытаемся подключиться сразу, если есть приватный ключ
        if (!private_key_.empty()) {
            key_copy = private_key_;
            should_connect = true;
        }
    }

    if (should_connect) {
        add_outbound_peer(address, key_copy, true);
    }
}

void PeerManager::prune_outbound_attempts_unlocked() {
    if (outbound_attempts_.size() <= kMaxOutboundAttemptState) {
        return;
    }

    std::vector<std::string> candidates;
    candidates.reserve(outbound_attempts_.size());
    for (const auto& [address, state] : outbound_attempts_) {
        if (state.in_progress) {
            continue;
        }
        if (peers_.find(address) != peers_.end()) {
            continue;
        }
        if (std::find(added_nodes_.begin(), added_nodes_.end(), address) != added_nodes_.end()) {
            continue;
        }
        candidates.push_back(address);
    }

    std::sort(candidates.begin(), candidates.end(), [this](const std::string& a, const std::string& b) {
        const auto& left = outbound_attempts_.at(a);
        const auto& right = outbound_attempts_.at(b);
        if (left.manual != right.manual) {
            return !left.manual && right.manual;
        }
        if (left.last_success != right.last_success) {
            return left.last_success < right.last_success;
        }
        if (left.last_attempt != right.last_attempt) {
            return left.last_attempt < right.last_attempt;
        }
        return a < b;
    });

    for (const auto& address : candidates) {
        if (outbound_attempts_.size() <= kMaxOutboundAttemptState) {
            break;
        }
        outbound_attempts_.erase(address);
    }
}

void PeerManager::remove_node(const std::string& address) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Удаляем из списка добавленных узлов
    auto it = std::find(added_nodes_.begin(), added_nodes_.end(), address);
    if (it != added_nodes_.end()) {
        added_nodes_.erase(it);
    }

    // Отключаем пира, если он подключен
    auto peer_it = peers_.find(address);
    if (peer_it != peers_.end() && peer_it->second.peer) {
        disconnect_peer_unlocked(peer_it->second, "remove-node");
    }
}

std::vector<std::string> PeerManager::get_added_nodes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return added_nodes_;
}

void PeerManager::set_private_key(const bytes& private_key) {
    std::lock_guard<std::mutex> lock(mutex_);
    utils::cleanse_container(private_key_);
    private_key_ = private_key;
}

void PeerManager::clear_private_key() {
    std::lock_guard<std::mutex> lock(mutex_);
    utils::cleanse_container(private_key_);
}

void PeerManager::set_local_port(int port) {
    std::lock_guard<std::mutex> lock(mutex_);
    local_port_ = port;
}

void PeerManager::set_outbound_connect_timeout(std::chrono::milliseconds timeout) {
    if (timeout.count() <= 0) {
        timeout = std::chrono::milliseconds(1);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    outbound_connect_timeout_ = timeout;
}

} // namespace geocoin::network
