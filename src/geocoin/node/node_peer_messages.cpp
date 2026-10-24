#include "geocoin/node/node.hpp"

#include "geocoin/consensus/consensus.hpp"
#include "geocoin/network/addrman.hpp"
#include "geocoin/network/messages.hpp"
#include "geocoin/network/network_utils.hpp"
#include "geocoin/network/message_limits.hpp"
#include "geocoin/network/services.hpp"
#include "geocoin/node/node_runtime.hpp"
#include "geocoin/utils/bytes.hpp"
#include "geocoin/utils/logger.hpp"
#include "geocoin/utils/logging.hpp"
#include "geocoin/utils/metrics.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using geocoin::utils::LogPrintf;

namespace geocoin::node {
namespace {

bool endpoint_to_host_port(const std::string& endpoint, std::string& host, uint16_t& port) {
    return geocoin::network::NetworkUtils::split_host_port(endpoint, host, port);
}

bool block_has_material_witness_local(const core::Block& block) {
    for (const auto& tx : block.transactions) {
        for (const auto& w : tx.witnesses) {
            if (!w.public_key.empty() || !w.signature.empty()) {
                return true;
            }
        }
    }
    return false;
}

constexpr size_t kMaxPendingWitnessRequests = 1024;
constexpr auto kWitnessRequestTimeout = std::chrono::minutes(2);
constexpr size_t kMaxPendingTransactionHashes = 65536;
constexpr size_t kMaxPendingTransactionHashesPerPeer = 50000;
// Leave headroom beyond the receiver's ten-second rolling TX window.
constexpr auto kTransactionRequestInterval = network::policy::kPolicyRateWindow + std::chrono::seconds(1);
constexpr auto kTransactionRequestTimeout = std::chrono::minutes(1);
constexpr size_t kTransactionRequestBatch = std::min(
    network::policy::kPolicyMaxGetDataItems, network::policy::kPolicyMaxTxMsgsPerWindow);

}  // namespace

void Node::queue_transaction_requests(std::shared_ptr<network::Peer> peer,
                                      const std::vector<Hash256>& hashes) {
    if (!peer || !peer->is_connected() || hashes.empty()) return;
    std::lock_guard<std::mutex> lock(transaction_request_mutex_);
    size_t total = 0;
    for (auto it = pending_transaction_inventory_.begin(); it != pending_transaction_inventory_.end();) {
        const auto owner = it->second.peer.lock();
        if (!owner || !owner->is_connected()) {
            it = pending_transaction_inventory_.erase(it);
        } else {
            total += it->second.hashes.size() + it->second.requested.size();
            ++it;
        }
    }
    auto& pending = pending_transaction_inventory_[peer->get_address()];
    if (pending.peer.lock() != peer) {
        total -= pending.hashes.size() + pending.requested.size();
        pending = {};
        pending.peer = peer;
    }
    for (const auto& hash : hashes) {
        if (mempool_->has_transaction(hash) || pending.hashes.count(hash) || pending.requested.count(hash)) continue;
        if (total >= kMaxPendingTransactionHashes ||
            pending.hashes.size() + pending.requested.size() >= kMaxPendingTransactionHashesPerPeer) {
            GEO_METRIC_INC("node.tx_inventory_queue_full_total", 1);
            break;
        }
        pending.hashes.insert(hash);
        pending.order.push_back(hash);
        ++total;
    }
}

void Node::request_pending_transactions() {
    std::lock_guard<std::mutex> lock(transaction_request_mutex_);
    const auto now = std::chrono::steady_clock::now();
    size_t remaining_batches = 8;
    for (auto it = pending_transaction_inventory_.begin(); it != pending_transaction_inventory_.end();) {
        auto& pending = it->second;
        const auto peer = pending.peer.lock();
        if (!peer || !peer->is_connected()) {
            it = pending_transaction_inventory_.erase(it);
            continue;
        }
        ++it;
        for (auto request = pending.requested.begin(); request != pending.requested.end();) {
            if (now >= request->second) {
                if (!mempool_->has_transaction(request->first) && pending.hashes.insert(request->first).second) {
                    // Retry behind fresh inventory so missing early objects
                    // cannot indefinitely hold up the rest of an announcement.
                    pending.order.push_back(request->first);
                }
                request = pending.requested.erase(request);
            } else {
                ++request;
            }
        }
        if (now < pending.next_request || remaining_batches == 0) continue;
        network::InvMessage request;
        for (auto hash = pending.order.begin(); hash != pending.order.end() &&
             request.items.size() < kTransactionRequestBatch;) {
            if (mempool_->has_transaction(*hash)) {
                pending.hashes.erase(*hash);
                hash = pending.order.erase(hash);
            } else {
                request.items.push_back({network::InvMessage::InvType::TX, *hash});
                ++hash;
            }
        }
        if (request.items.empty()) continue;
        --remaining_batches;
        if (peer_manager_->send_message(peer, network::MessageType::GETDATA, request.serialize())) {
            for (const auto& item : request.items) {
                pending.requested[item.hash] = now + kTransactionRequestTimeout;
                pending.hashes.erase(item.hash);
                pending.order.pop_front();
            }
            pending.next_request = now + kTransactionRequestInterval;
        }
    }
}

bool Node::request_witness(const Hash256& block_hash) {
    if (!running_ || !peer_manager_ || !blockchain_ ||
        !blockchain_->has_block_index(block_hash)) {
        return false;
    }

    const std::string request_key = utils::hash256_to_hex(block_hash);
    {
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(witness_request_mutex_);
        for (auto it = pending_witness_requests_.begin();
             it != pending_witness_requests_.end();) {
            if (now - it->second >= kWitnessRequestTimeout) {
                it = pending_witness_requests_.erase(it);
                GEO_METRIC_INC("node.witness_requests_expired_total", 1);
            } else {
                ++it;
            }
        }
        if (pending_witness_requests_.find(request_key) != pending_witness_requests_.end()) {
            return true;
        }
        if (pending_witness_requests_.size() >= kMaxPendingWitnessRequests) {
            GEO_LOG_WARN("node", "Too many pending witness requests");
            return false;
        }
        // Reserve before sending so a fast response cannot race ahead of the
        // pending marker. The reservation is rolled back when no peer accepts
        // the request.
        pending_witness_requests_.emplace(request_key, now);
    }

    // Запрашиваем через стандартный GETDATA(BLOCK), но только у архивных пиров.
    network::InvMessage req;
    req.items.push_back({network::InvMessage::InvType::BLOCK, block_hash});

    const bytes payload = req.serialize();
    size_t sent = 0;
    for (const auto& peer : peer_manager_->get_authenticated_peers()) {
        if (!peer) {
            continue;
        }
        const auto& info = peer->get_info();
        if (!network::has_service(info.services, network::ServiceFlags::NODE_ARCHIVE)) {
            continue;
        }
        if (peer_manager_->send_message(peer, network::MessageType::GETDATA, payload)) {
            ++sent;
        }
    }

    if (sent > 0) {
        GEO_METRIC_INC("node.witness_requests_total", 1);
        return true;
    }

    {
        std::lock_guard<std::mutex> lock(witness_request_mutex_);
        pending_witness_requests_.erase(request_key);
    }

    GEO_LOG_WARN("node", "No archive peers available for witness request");
    GEO_METRIC_INC("node.witness_requests_no_archive_peers_total", 1);
    return false;
}


void Node::handle_peer_message(
    std::shared_ptr<network::Peer> peer,
    network::MessageType type,
    const bytes& payload
) {
    auto send_reject = [&](network::MessageType rejected_type, uint8_t code, const std::string& reason, const Hash256& rejected_hash = Hash256{}) {
        network::RejectMessage reject;
        reject.rejected_type = rejected_type;
        reject.code = code;
        reject.reason = reason;
        reject.rejected_hash = rejected_hash;
        peer_manager_->send_message(peer, network::MessageType::REJECT, reject.serialize());
    };

    try {
        switch (type) {
            case network::MessageType::INV: {
                if (detail::sync_debug_enabled()) {
                    GEO_LOG_DEBUG("node", "Received INV from " << peer->get_address());
                }
                auto msg = network::InvMessage::deserialize(payload);
                if (detail::sync_debug_enabled()) {
                    GEO_LOG_DEBUG("node", "INV has " << msg.items.size() << " items");
                }

                std::vector<Hash256> block_hashes;
                std::vector<Hash256> tx_hashes;

                for (const auto& item : msg.items) {
                    if (item.type == network::InvMessage::InvType::BLOCK) {
                        block_hashes.push_back(item.hash);
                    } else if (item.type == network::InvMessage::InvType::TX) {
                        tx_hashes.push_back(item.hash);

                    }
                }

                if (!block_hashes.empty()) {
                    // INV contains hashes, not a trustworthy height. The sync
                    // manager will ask for and validate the corresponding
                    // header chain before advancing any target.
                    sync_manager_->handle_inv(peer, block_hashes);
                }

                queue_transaction_requests(peer, tx_hashes);
                request_pending_transactions();
                break;
            }

            case network::MessageType::HEADERS: {
                auto msg = network::HeadersMessage::deserialize(payload);
                sync_manager_->handle_headers(peer, msg.headers);
                break;
            }

            case network::MessageType::BLOCK: {
                auto msg = network::BlockMessage::deserialize(payload);
                Hash256 block_hash = msg.block.get_hash();

                bool witness_requested = false;
                {
                    std::lock_guard<std::mutex> lock(witness_request_mutex_);
                    const std::string key = utils::hash256_to_hex(block_hash);
                    witness_requested = pending_witness_requests_.find(key) !=
                        pending_witness_requests_.end();
                }

                // Если этот блок пришёл как ответ на unprune/witness-запрос и уже есть в индексе,
                // обновляем дисковую копию блока (вместе с witness), не трогая active-chain.
                // Header hash alone is not enough here: an attacker can reuse a known
                // header with an arbitrary body. Validate both commitments before the
                // existing on-disk copy is replaced, and consume the request only after
                // the durable write succeeds.
                if (witness_requested && blockchain_ && block_storage_) {
                    if (const auto* idx = blockchain_->find_block_index(block_hash)) {
                        consensus::ValidationState restore_state;
                        if (!consensus::check_block(
                                msg.block,
                                restore_state,
                                consensus::ValidationMode::FULL)) {
                            GEO_METRIC_INC("node.witness_restores_failed_total", 1);
                            send_reject(
                                network::MessageType::BLOCK,
                                1,
                                "invalid-witness-restore",
                                block_hash
                            );
                            peer_manager_->disconnect_peer(peer, "invalid-witness-restore");
                            break;
                        }

                        int32_t file = 0;
                        uint32_t pos = 0;
                        if (!block_storage_->save_block_with_pos(
                                msg.block,
                                idx->height,
                                file,
                                pos,
                                true)) {
                            GEO_METRIC_INC("node.witness_restores_failed_total", 1);
                            break;
                        }

                        {
                            std::lock_guard<std::mutex> lock(witness_request_mutex_);
                            pending_witness_requests_.erase(utils::hash256_to_hex(block_hash));
                        }
                        if (!block_has_material_witness_local(msg.block)) {
                            GEO_METRIC_INC("node.witness_restores_empty_total", 1);
                        }
                        GEO_METRIC_INC("node.witness_restores_total", 1);
                    }
                }

                // Помечаем что пир знает об этом блоке (как AddInventoryKnown в Bitcoin)
                peer->add_known_inventory(block_hash);

                sync_manager_->handle_block(peer, msg.block);
                break;
            }

            case network::MessageType::TX: {
                auto msg = network::TxMessage::deserialize(payload);
                Hash256 tx_hash = msg.tx.get_hash();
                {
                    std::lock_guard<std::mutex> lock(transaction_request_mutex_);
                    const auto it = pending_transaction_inventory_.find(peer->get_address());
                    if (it != pending_transaction_inventory_.end() && it->second.peer.lock() == peer) {
                        if (it->second.hashes.erase(tx_hash)) std::erase(it->second.order, tx_hash);
                        it->second.requested.erase(tx_hash);
                        // Delayed responses also count toward the receive window.
                        it->second.next_request = std::chrono::steady_clock::now() + kTransactionRequestInterval;
                    }
                }

                // Помечаем что пир знает об этой транзакции (как AddInventoryKnown в Bitcoin)
                peer->add_known_inventory(tx_hash);

                // Обрабатываем транзакцию (добавляем в mempool и relay другим)
                handle_new_transaction(msg.tx, peer);  // Передаем источник
                break;
            }

            case network::MessageType::GETBLOCKS: {
                auto msg = network::GetBlocksMessage::deserialize(payload);

                auto headers = sync_manager_->handle_getheaders(
                    peer,
                    msg.locator_hashes,
                    msg.stop_hash
                );

                // Отправляем INV с блоками
                network::InvMessage inv;
                for (const auto& header : headers) {
                    network::InvMessage::InvItem item;
                    item.type = network::InvMessage::InvType::BLOCK;
                    // Вычисляем хеш из header
                    item.hash = header.get_hash();
                    inv.items.push_back(item);
                }

                if (!inv.items.empty()) {
                    peer_manager_->send_message(peer, network::MessageType::INV, inv.serialize());
                }
                break;
            }

            case network::MessageType::GETHEADERS: {
                auto msg = network::GetBlocksMessage::deserialize(payload);
                auto headers = sync_manager_->handle_getheaders(peer, msg.locator_hashes, msg.stop_hash);

                network::HeadersMessage response;
                response.headers = std::move(headers);
                peer_manager_->send_message(peer, network::MessageType::HEADERS, response.serialize());
                break;
            }

            case network::MessageType::GETDATA: {
                auto msg = network::InvMessage::deserialize(payload);
                static constexpr size_t kMaxServedGetDataItems =
                    network::policy::kPolicyMaxGetDataItems;
                static constexpr size_t kMaxBlockRequestsPerMessage = 4;
                static constexpr size_t kMaxResponsePayloadBytes =
                    kMaxBlockRequestsPerMessage * consensus::params::MAX_BLOCK_SIZE;

                if (msg.items.size() > kMaxServedGetDataItems) {
                    GEO_LOG_WARN(
                        "node",
                        "GETDATA too large from " << peer->get_address()
                            << ": " << msg.items.size() << " items"
                    );
                    send_reject(network::MessageType::GETDATA, 2, "too-many-items");
                    peer_manager_->disconnect_peer(peer, "getdata-too-many-items");
                    break;
                }

                const size_t requested_blocks = static_cast<size_t>(std::count_if(
                    msg.items.begin(),
                    msg.items.end(),
                    [](const network::InvMessage::InvItem& item) {
                        return item.type == network::InvMessage::InvType::BLOCK;
                    }
                ));
                if (requested_blocks > kMaxBlockRequestsPerMessage) {
                    send_reject(network::MessageType::GETDATA, 2, "too-many-block-requests");
                    peer_manager_->disconnect_peer(peer, "getdata-block-amplification");
                    break;
                }

                // Отправляем запрошенные объекты (блоки и транзакции)
                size_t response_payload_bytes = 0;
                for (const auto& item : msg.items) {
                    if (item.type == network::InvMessage::InvType::BLOCK) {
                        core::Block block;
                        if (blockchain_->get_block(item.hash, block)) {
                            network::BlockMessage block_msg;
                            block_msg.block = block;
                            const bytes response = block_msg.serialize();
                            if (response.size() > kMaxResponsePayloadBytes - response_payload_bytes) {
                                send_reject(network::MessageType::GETDATA, 2, "response-budget-exceeded");
                                break;
                            }
                            if (!peer_manager_->send_message(peer, network::MessageType::BLOCK, response)) {
                                break;
                            }
                            response_payload_bytes += response.size();
                        }
                    } else if (item.type == network::InvMessage::InvType::TX) {
                        // Ищем транзакцию в mempool
                        core::Transaction tx;
                        if (mempool_->get_transaction(item.hash, tx)) {
                            network::TxMessage tx_msg;
                            tx_msg.tx = tx;
                            const bytes response = tx_msg.serialize();
                            if (response.size() > kMaxResponsePayloadBytes - response_payload_bytes) {
                                send_reject(network::MessageType::GETDATA, 2, "response-budget-exceeded");
                                break;
                            }
                            if (!peer_manager_->send_message(peer, network::MessageType::TX, response)) {
                                break;
                            }
                            response_payload_bytes += response.size();

                            // Помечаем что пир знает об этой транзакции
                            peer->add_known_inventory(item.hash);
                        }
                    }
                }
                break;
            }

            case network::MessageType::ADDR: {
                auto msg = network::AddrMessage::deserialize(payload);
                size_t accepted = 0;

                std::shared_ptr<network::AddrMan> addrman_snapshot;
                {
                    std::lock_guard<std::mutex> lock(addrman_mutex_);
                    addrman_snapshot = addrman_;
                }
                if (!addrman_snapshot) {
                    break;
                }

                const int self_check_port = p2p_server_ ? static_cast<int>(config_.p2p_port) : 0;
                const uint64_t now_ts = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()
                ).count();

                for (const auto& a : msg.addresses) {
                    if (accepted >= network::policy::kPolicyMaxAddrItems) {
                        break;
                    }
                    if (a.ip.empty() || a.port != config_.p2p_port) {
                        continue;
                    }
                    // Отсекаем очевидно "битые" записи по timestamp.
                    if (a.timestamp > now_ts + 2 * 60 * 60) {
                        continue;
                    }

                    std::string canonical_ip;
                    if (!network::NetworkUtils::canonicalize_numeric_ip(a.ip, canonical_ip) ||
                        !network::NetworkUtils::is_global_unicast_ip(canonical_ip)) {
                        continue;
                    }

                    const std::string endpoint = network::NetworkUtils::make_endpoint(canonical_ip, a.port);
                    if (self_check_port > 0 && network::NetworkUtils::is_self_address(endpoint, self_check_port)) {
                        continue;
                    }
                    if (addrman_snapshot->add_from_gossip(
                            endpoint,
                            peer->get_address(),
                            config_.p2p_port)) {
                        ++accepted;
                    }
                }
                break;
            }

            case network::MessageType::REJECT: {
                auto msg = network::RejectMessage::deserialize(payload);
                GEO_LOG_WARN(
                    "node",
                    "Peer " << peer->get_address()
                        << " rejected message type=" << static_cast<int>(msg.rejected_type)
                        << ", code=" << static_cast<int>(msg.code)
                        << ", reason=" << msg.reason
                );
                break;
            }

            default:
                break;
        }
    } catch (const std::exception& e) {
        GEO_LOG_WARN("node", "Message handling error from " << peer->get_address() << ": " << e.what());
        send_reject(type, 1, "malformed-message");
        // The frame-level parser already accepted this message. A deeper
        // failure is a protocol violation; keeping the peer alive would let it
        // repeat parser work until a coarse rate limit eventually triggers.
        peer_manager_->disconnect_peer(peer, "malformed-node-message");
    } catch (...) {
        GEO_LOG_WARN("node", "Unknown message handling error from " << peer->get_address());
        send_reject(type, 1, "malformed-message");
        peer_manager_->disconnect_peer(peer, "malformed-node-message");
    }
}

void Node::handle_peer_authenticated(
    std::shared_ptr<network::Peer> peer,
    Height peer_height
) {
    // start_sync also records the persisted local height when the peer is not
    // ahead. In that case it returns false without sending GETHEADERS or
    // entering IBD; a later ahead peer can transition COMPLETED -> SYNCING.
    sync_manager_->start_sync(peer, peer_height);

    // A transaction may have entered the mempool while this peer was
    // disconnected or still authenticating.  The one-shot relay queue is
    // intentionally drained after an announcement, so a reconnecting peer
    // otherwise never learns about that transaction.  Re-queue the current
    // inventory; the periodic drain batches INV messages under the peer
    // rate limit instead of blasting the whole mempool in one handshake.
    if (mempool_) {
        mempool_->queue_inventory_for_relay(mempool_->get_all_transaction_hashes());
    }

    // После handshake отправляем часть известных адресов для peer discovery.
    network::AddrMessage addr_msg;
    static constexpr size_t kAddrRelayLimit = network::policy::kPolicyMaxAddrItems;
    const uint64_t now_ts = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();

    std::shared_ptr<network::AddrMan> addrman_snapshot;
    {
        std::lock_guard<std::mutex> lock(addrman_mutex_);
        addrman_snapshot = addrman_;
    }

    const uint64_t local_services = network::build_local_services(config_.archive);

    if (addrman_snapshot) {
        const auto all = addrman_snapshot->get_all();
        for (const auto& endpoint : all) {
            if (addr_msg.addresses.size() >= kAddrRelayLimit) {
                break;
            }
            std::string host;
            uint16_t port = 0;
            if (!endpoint_to_host_port(endpoint, host, port)) {
                continue;
            }
            if (port != config_.p2p_port || !network::NetworkUtils::is_global_unicast_ip(host)) {
                continue;
            }
            network::AddrMessage::NetAddr na;
            na.services = local_services;
            na.ip = host;
            na.port = port;
            na.timestamp = now_ts;
            addr_msg.addresses.push_back(std::move(na));
        }
    }

    // Если AddrMan пуст, отправим хотя бы текущих подключенных peers (кроме получателя).
    if (addr_msg.addresses.empty()) {
        const auto peers = peer_manager_->get_connected_peers();
        for (const auto& p : peers) {
            if (!p || p == peer) {
                continue;
            }
            std::string host;
            uint16_t port = 0;
            if (!endpoint_to_host_port(p->get_address(), host, port)) {
                continue;
            }
            if (port != config_.p2p_port || !network::NetworkUtils::is_global_unicast_ip(host)) {
                continue;
            }
            network::AddrMessage::NetAddr na;
            na.services = local_services;
            na.ip = host;
            na.port = port;
            na.timestamp = now_ts;
            addr_msg.addresses.push_back(std::move(na));
            if (addr_msg.addresses.size() >= kAddrRelayLimit) {
                break;
            }
        }
    }

    if (!addr_msg.addresses.empty()) {
        peer_manager_->send_message(peer, network::MessageType::ADDR, addr_msg.serialize());
    }
}

void Node::relay_inventory(std::shared_ptr<network::Peer> exclude_peer) {
    Hash256 block_hash{};
    std::vector<Hash256> tx_hashes;
    {
        std::lock_guard<std::mutex> lock(relay_mutex_);
        block_hash = pending_block_inventory_;
        pending_block_inventory_ = {};
        // A block and a full transaction batch must still fit one INV, so
        // their combined cadence stays within the per-peer message budget.
        const size_t block_items = block_hash == Hash256{} ? 0 : 1;
        tx_hashes = mempool_->get_transactions_for_relay(
            network::Mempool::kRelayInventoryBatch - block_items);
    }

    if (tx_hashes.empty() && block_hash == Hash256{}) {
        return;
    }

    if (detail::sync_debug_enabled()) {
        GEO_LOG_DEBUG("node", "Relaying inventory: " << tx_hashes.size() << " transactions, block="
                     << (block_hash != Hash256{}));
    }

    std::vector<network::InvMessage::InvItem> items;
    items.reserve(tx_hashes.size() + 1);
    if (block_hash != Hash256{}) items.push_back({network::InvMessage::InvType::BLOCK, block_hash});
    for (const auto& hash : tx_hashes) items.push_back({network::InvMessage::InvType::TX, hash});
    const size_t sent = peer_manager_->relay_inventory(items, exclude_peer);
    if (sent == 0) {
        if (detail::sync_debug_enabled()) {
            GEO_LOG_DEBUG("node", "No peers needed inventory relay");
        }
        return;
    }

    if (detail::sync_debug_enabled()) {
        GEO_LOG_DEBUG("node", "Relayed inventory to " << sent << " peers");
    }
}

}  // namespace geocoin::node
