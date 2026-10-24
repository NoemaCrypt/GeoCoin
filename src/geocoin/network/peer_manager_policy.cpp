#include "geocoin/network/peer_manager.hpp"
#include "geocoin/network/peer_manager_internal.hpp"

#include "geocoin/utils/logger.hpp"
#include "geocoin/utils/metrics.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <limits>

namespace geocoin::network {
namespace {

std::string peer_group_key_from_ip(const std::string& ip) {
    return NetworkUtils::network_group(ip);
}

std::chrono::seconds outbound_backoff_delay(uint32_t fail_count, bool manual_target, const std::string& address) {
    static constexpr std::array<int, 5> kRegularBackoff{{5, 30, 120, 600, 3600}};
    static constexpr std::array<int, 5> kManualBackoff{{2, 5, 15, 60, 120}};
    const auto& schedule = manual_target ? kManualBackoff : kRegularBackoff;
    const size_t idx = std::min<size_t>(fail_count == 0 ? 0 : fail_count - 1, schedule.size() - 1);
    const int base = schedule[idx];
    const size_t jitter_seed = std::hash<std::string>{}(address);
    const int jitter = static_cast<int>(jitter_seed % static_cast<size_t>(std::max(1, base / 4 + 1)));
    return std::chrono::seconds(base + jitter);
}

uint64_t steady_age_ms(
    std::chrono::steady_clock::time_point now,
    std::chrono::steady_clock::time_point then
) {
    if (then == std::chrono::steady_clock::time_point{}) {
        return 0;
    }
    if (now <= then) {
        return 0;
    }
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now - then).count());
}

} // namespace

bool PeerManager::admit_inbound_handshake_unlocked(const std::string& address) {
    constexpr size_t kMaxTrackedHandshakeGroups = 4096;
    const auto now = std::chrono::steady_clock::now();
    const auto refill = [now](HandshakeBucket& bucket, double rate, double burst) {
        const double elapsed = std::chrono::duration<double>(now - bucket.updated).count();
        bucket.tokens = std::min(burst, bucket.tokens + std::max(0.0, elapsed) * rate);
        bucket.updated = now;
    };

    if (!inbound_handshake_bucket_started_) {
        inbound_handshake_bucket_ = HandshakeBucket{inbound_handshake_burst_, now};
        inbound_handshake_bucket_started_ = true;
    } else {
        refill(inbound_handshake_bucket_, inbound_handshake_rate_, inbound_handshake_burst_);
    }

    std::string group = peer_group_key_from_ip(detail::extract_ip(address));
    if (group.empty()) {
        group = address;
    }
    auto it = inbound_handshake_group_buckets_.find(group);
    if (it == inbound_handshake_group_buckets_.end()) {
        if (inbound_handshake_group_buckets_.size() >= kMaxTrackedHandshakeGroups) {
            // A refilled bucket carries no history, so forgetting it is free.
            // libstdc++ 12 gives std::erase_if a const element (PR 107850).
            for (auto bucket_it = inbound_handshake_group_buckets_.begin();
                 bucket_it != inbound_handshake_group_buckets_.end();) {
                refill(bucket_it->second, inbound_group_handshake_rate_, inbound_group_handshake_burst_);
                if (bucket_it->second.tokens >= inbound_group_handshake_burst_) {
                    bucket_it = inbound_handshake_group_buckets_.erase(bucket_it);
                } else {
                    ++bucket_it;
                }
            }
            if (inbound_handshake_group_buckets_.size() >= kMaxTrackedHandshakeGroups) {
                inbound_handshake_group_buckets_.erase(inbound_handshake_group_buckets_.begin());
            }
        }
        it = inbound_handshake_group_buckets_.emplace(
            group, HandshakeBucket{inbound_group_handshake_burst_, now}).first;
    } else {
        refill(it->second, inbound_group_handshake_rate_, inbound_group_handshake_burst_);
    }

    if (inbound_handshake_bucket_.tokens < 1.0 || it->second.tokens < 1.0) {
        return false;
    }
    inbound_handshake_bucket_.tokens -= 1.0;
    it->second.tokens -= 1.0;
    return true;
}

bool PeerManager::is_peer_banned_unlocked(const std::string& address) {
    const std::string ip = detail::extract_ip(address);
    if (ip.empty()) {
        return false;
    }
    const auto now_steady = std::chrono::steady_clock::now();
    const std::string temp_key = NetworkUtils::abuse_key(ip);
    auto temp_it = temp_banned_ips_.find(temp_key);
    if (temp_it != temp_banned_ips_.end()) {
        if (now_steady < temp_it->second) {
            return true;
        }
        temp_banned_ips_.erase(temp_it);
    }

    const auto now_sys = std::chrono::system_clock::now();
    auto manual_it = manual_banned_ips_.find(ip);
    if (manual_it != manual_banned_ips_.end()) {
        if (now_sys < manual_it->second) {
            return true;
        }
        manual_banned_ips_.erase(manual_it);
    }
    return false;
}

void PeerManager::add_temp_ban_unlocked(const std::string& ip) {
    const std::string key = NetworkUtils::abuse_key(ip);
    if (key.empty()) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    for (auto it = temp_banned_ips_.begin(); it != temp_banned_ips_.end();) {
        if (it->second <= now) {
            it = temp_banned_ips_.erase(it);
        } else {
            ++it;
        }
    }

    if (temp_banned_ips_.find(key) == temp_banned_ips_.end() &&
        temp_banned_ips_.size() >= detail::kPolicyMaxTempBans) {
        const auto earliest = std::min_element(
            temp_banned_ips_.begin(),
            temp_banned_ips_.end(),
            [](const auto& lhs, const auto& rhs) { return lhs.second < rhs.second; }
        );
        if (earliest != temp_banned_ips_.end()) {
            temp_banned_ips_.erase(earliest);
        }
    }

    temp_banned_ips_[key] = now + detail::kPolicyTempBanDuration;
}

bool PeerManager::should_try_outbound_unlocked(const std::string& address, bool manual_target) const {
    const auto it = outbound_attempts_.find(address);
    if (it == outbound_attempts_.end()) {
        return true;
    }
    if (it->second.in_progress) {
        return false;
    }
    if (manual_target && it->second.fail_count <= 1) {
        return true;
    }
    return std::chrono::steady_clock::now() >= it->second.next_attempt;
}

bool PeerManager::should_try_outbound(const std::string& address, bool manual_target) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return should_try_outbound_unlocked(address, manual_target);
}

void PeerManager::record_outbound_attempt_unlocked(const std::string& address, bool manual_target) {
    auto& state = outbound_attempts_[address];
    state.last_attempt = std::chrono::steady_clock::now();
    state.manual = state.manual || manual_target;
    state.in_progress = true;
}

void PeerManager::record_outbound_result_unlocked(
    const std::string& address,
    bool success,
    bool manual_target,
    const std::string& reason
) {
    auto& state = outbound_attempts_[address];
    const auto now = std::chrono::steady_clock::now();
    state.manual = state.manual || manual_target;
    state.in_progress = false;
    if (state.last_attempt == std::chrono::steady_clock::time_point{}) {
        state.last_attempt = now;
    }

    if (success) {
        state.fail_count = 0;
        state.last_success = now;
        state.next_attempt = now;
        state.last_error.clear();
        prune_outbound_attempts_unlocked();
        GEO_METRIC_INC("peers.outbound_backoff_success_total", 1);
        return;
    }

    if (state.fail_count < std::numeric_limits<uint32_t>::max()) {
        ++state.fail_count;
    }
    state.last_error = reason.empty() ? "connect-failed" : reason;
    state.next_attempt = now + outbound_backoff_delay(state.fail_count, state.manual, address);
    prune_outbound_attempts_unlocked();
    GEO_METRIC_INC("peers.outbound_backoff_fail_total", 1);
}

void PeerManager::record_sent_message_best_effort(std::shared_ptr<Peer> peer, MessageType type) {
    if (!peer) {
        return;
    }

    std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
        return;
    }

    const auto it = peers_.find(peer->get_address());
    if (it == peers_.end() || it->second.peer != peer) {
        return;
    }
    it->second.sent_message_counts[static_cast<size_t>(type)]++;
}

void PeerManager::record_disconnect_unlocked(PeerContext& ctx, const std::string& reason) {
    if (ctx.disconnect_recorded) {
        return;
    }

    ctx.disconnect_recorded = true;
    ctx.last_disconnect_reason = reason.empty() ? "unknown" : reason;
    ++disconnect_reason_counters_[ctx.last_disconnect_reason];

    PeerDisconnectInfo info;
    info.address = ctx.peer ? ctx.peer->get_address() : "";
    info.reason = ctx.last_disconnect_reason;
    info.policy_score = ctx.policy_score;
    info.inbound = ctx.is_inbound;
    info.disconnected_at = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count());
    recent_disconnects_.push_back(std::move(info));
    while (recent_disconnects_.size() > detail::kPolicyRecentDisconnectLimit) {
        recent_disconnects_.pop_front();
    }
    if (!ctx.is_inbound && ctx.peer) {
        const bool explicit_local_disconnect =
            ctx.last_disconnect_reason == "rpc-disconnect" ||
            ctx.last_disconnect_reason == "remove-node" ||
            ctx.last_disconnect_reason == "remove-peer" ||
            ctx.last_disconnect_reason == "manual-ban" ||
            ctx.last_disconnect_reason == "shutdown";
        if (!explicit_local_disconnect) {
            record_outbound_result_unlocked(
                ctx.peer->get_address(),
                false,
                false,
                ctx.last_disconnect_reason
            );
        }
    }
    GEO_METRIC_INC("peers.disconnect_reason_total", 1);
}

void PeerManager::disconnect_peer_unlocked(PeerContext& ctx, const std::string& reason) {
    record_disconnect_unlocked(ctx, reason);
    if (ctx.peer) {
        ctx.peer->disconnect();
    }
}

void PeerManager::decay_policy_score_unlocked(PeerContext& ctx, std::chrono::steady_clock::time_point now) {
    if (ctx.policy_score == 0) {
        ctx.last_score_decay_at = now;
        return;
    }
    if (ctx.last_score_decay_at == std::chrono::steady_clock::time_point{}) {
        ctx.last_score_decay_at = now;
        return;
    }
    const auto elapsed = now - ctx.last_score_decay_at;
    if (elapsed < detail::kPolicyScoreDecayInterval) {
        return;
    }

    const auto steps = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() /
                       detail::kPolicyScoreDecayInterval.count();
    const uint64_t decay = static_cast<uint64_t>(steps) * detail::kPolicyScoreDecayPoints;
    ctx.policy_score = static_cast<uint32_t>(decay >= ctx.policy_score ? 0 : ctx.policy_score - decay);
    ctx.last_score_decay_at += detail::kPolicyScoreDecayInterval * steps;
}

bool PeerManager::inbound_diversity_limit_reached_unlocked(const std::string& incoming_address) const {
    const std::string incoming_ip = detail::extract_ip(incoming_address);
    if (incoming_ip.empty()) {
        return false;
    }
    const std::string incoming_group = peer_group_key_from_ip(incoming_ip);

    size_t same_ip = 0;
    size_t same_group = 0;
    for (const auto& [addr, ctx] : peers_) {
        if (!ctx.is_inbound || !ctx.peer || !ctx.peer->is_connected()) {
            continue;
        }
        const std::string ip = detail::extract_ip(addr);
        if (ip == incoming_ip) {
            ++same_ip;
        }
        if (!incoming_group.empty() && peer_group_key_from_ip(ip) == incoming_group) {
            ++same_group;
        }
    }

    return same_ip >= detail::kPolicyMaxInboundPerIp ||
           same_group >= detail::kPolicyMaxInboundPerGroup;
}

bool PeerManager::try_evict_inbound_peer_unlocked(
    const std::string& incoming_address,
    std::string& evicted_address
) {
    evicted_address.clear();
    const std::string incoming_ip = detail::extract_ip(incoming_address);
    const std::string incoming_group = peer_group_key_from_ip(incoming_ip);

    auto worst = peers_.end();
    int worst_score = std::numeric_limits<int>::min();
    const auto now = std::chrono::steady_clock::now();

    for (auto it = peers_.begin(); it != peers_.end(); ++it) {
        PeerContext& ctx = it->second;
        if (!ctx.is_inbound || !ctx.peer || !ctx.peer->is_connected()) {
            continue;
        }

        const std::string ip = detail::extract_ip(it->first);
        const bool same_group = !incoming_group.empty() && peer_group_key_from_ip(ip) == incoming_group;
        const bool authenticated = ctx.peer->get_state() == PeerState::READY;
        const uint64_t idle_ms = steady_age_ms(now, ctx.last_useful_message_at);

        int score = static_cast<int>(ctx.policy_score);
        if (!authenticated) {
            score += 100;
        }
        if (same_group) {
            score += 20;
        }
        if (idle_ms > 5 * 60 * 1000ULL) {
            score += 10;
        }

        if (score > worst_score) {
            worst_score = score;
            worst = it;
        }
    }

    if (worst == peers_.end() || worst_score < 100) {
        return false;
    }

    evicted_address = worst->first;
    disconnect_peer_unlocked(worst->second, "inbound-evicted");
    return true;
}

void PeerManager::record_policy_reject_unlocked(const std::string& reason) {
    ++total_policy_rejects_;
    ++policy_reject_counters_[reason];
    GEO_METRIC_INC("peers.policy_rejects_total", 1);
}

void PeerManager::penalize_peer_unlocked(PeerContext& ctx, uint32_t points, const char* reason) {
    if (!reason) {
        reason = "unknown";
    }
    record_policy_reject_unlocked(reason);

    const uint32_t old_score = ctx.policy_score;
    const uint64_t sum = static_cast<uint64_t>(ctx.policy_score) + points;
    ctx.policy_score = static_cast<uint32_t>(std::min<uint64_t>(sum, std::numeric_limits<uint32_t>::max()));

    if (ctx.peer) {
        GEO_LOG_WARN(
            "peers",
            "Policy reject from " << ctx.peer->get_address()
                << ": reason=" << reason
                << ", score=" << old_score << "->" << ctx.policy_score
        );
    }

    if (ctx.policy_score >= detail::kPolicyBanScoreThreshold && ctx.peer) {
        const std::string ip = detail::extract_ip(ctx.peer->get_address());
        if (!ip.empty()) {
            add_temp_ban_unlocked(ip);
            GEO_LOG_WARN(
                "peers",
                "Peer temporarily banned: " << ip
                    << " for " << std::chrono::duration_cast<std::chrono::seconds>(
                        detail::kPolicyTempBanDuration
                    ).count()
                    << "s"
            );
        }
        disconnect_peer_unlocked(ctx, reason);
    }
}

bool PeerManager::validate_inv_like_payload_unlocked(
    const bytes& payload,
    uint32_t max_items,
    const char* malformed_reason,
    const char* too_many_reason,
    PeerContext& ctx
) {
    if (payload.size() < 4) {
        penalize_peer_unlocked(ctx, detail::kPolicyBanScoreThreshold, malformed_reason);
        return false;
    }

    const uint32_t count = static_cast<uint32_t>(payload[0]) |
                           (static_cast<uint32_t>(payload[1]) << 8) |
                           (static_cast<uint32_t>(payload[2]) << 16) |
                           (static_cast<uint32_t>(payload[3]) << 24);

    constexpr size_t kInvItemSerializedSize = 36; // type(4) + hash(32)
    if (count > (std::numeric_limits<size_t>::max() - 4) / kInvItemSerializedSize) {
        penalize_peer_unlocked(ctx, detail::kPolicyBanScoreThreshold, malformed_reason);
        return false;
    }
    const size_t expected = 4 + static_cast<size_t>(count) * kInvItemSerializedSize;
    if (expected != payload.size()) {
        penalize_peer_unlocked(ctx, detail::kPolicyBanScoreThreshold, malformed_reason);
        return false;
    }

    if (count > max_items) {
        penalize_peer_unlocked(ctx, 20, too_many_reason);
        return false;
    }
    return true;
}

bool PeerManager::validate_addr_payload_unlocked(const bytes& payload, PeerContext& ctx) {
    if (payload.size() < 4) {
        penalize_peer_unlocked(ctx, detail::kPolicyBanScoreThreshold, "addr-malformed");
        return false;
    }
    if (payload.size() > detail::kPolicyMaxAddrPayloadBytes) {
        penalize_peer_unlocked(ctx, 20, "addr-payload-too-large");
        return false;
    }

    const uint32_t count = static_cast<uint32_t>(payload[0]) |
                           (static_cast<uint32_t>(payload[1]) << 8) |
                           (static_cast<uint32_t>(payload[2]) << 16) |
                           (static_cast<uint32_t>(payload[3]) << 24);
    if (count > detail::kPolicyMaxAddrItems) {
        penalize_peer_unlocked(ctx, 20, "addr-too-many-items");
        return false;
    }
    return true;
}

bool PeerManager::consume_rate_token_unlocked(
    PeerContext& ctx,
    MessageType type,
    std::chrono::steady_clock::time_point now,
    const char*& reject_reason
) {
    reject_reason = nullptr;
    auto prune_window = [&](std::deque<std::chrono::steady_clock::time_point>& q) {
        while (!q.empty() && (now - q.front()) > detail::kPolicyRateWindow) {
            q.pop_front();
        }
    };

    prune_window(ctx.recent_all_msgs);
    if (ctx.recent_all_msgs.size() >= detail::kPolicyMaxAllMsgsPerWindow) {
        reject_reason = "message-rate-limit";
        return false;
    }
    ctx.recent_all_msgs.push_back(now);

    if (type == MessageType::INV) {
        prune_window(ctx.recent_inv_msgs);
        if (ctx.recent_inv_msgs.size() >= detail::kPolicyMaxInvMsgsPerWindow) {
            reject_reason = "inv-rate-limit";
            return false;
        }
        ctx.recent_inv_msgs.push_back(now);
    } else if (type == MessageType::GETDATA) {
        prune_window(ctx.recent_getdata_msgs);
        if (ctx.recent_getdata_msgs.size() >= detail::kPolicyMaxGetDataMsgsPerWindow) {
            reject_reason = "getdata-rate-limit";
            return false;
        }
        ctx.recent_getdata_msgs.push_back(now);
    } else if (type == MessageType::ADDR) {
        prune_window(ctx.recent_addr_msgs);
        if (ctx.recent_addr_msgs.size() >= detail::kPolicyMaxAddrMsgsPerWindow) {
            reject_reason = "addr-rate-limit";
            return false;
        }
        ctx.recent_addr_msgs.push_back(now);
    } else if (type == MessageType::TX) {
        prune_window(ctx.recent_tx_msgs);
        if (ctx.recent_tx_msgs.size() >= detail::kPolicyMaxTxMsgsPerWindow) {
            reject_reason = "tx-rate-limit";
            return false;
        }
        ctx.recent_tx_msgs.push_back(now);
    } else if (type == MessageType::BLOCK) {
        prune_window(ctx.recent_block_msgs);
        if (ctx.recent_block_msgs.size() >= detail::kPolicyMaxBlockMsgsPerWindow) {
            reject_reason = "block-rate-limit";
            return false;
        }
        ctx.recent_block_msgs.push_back(now);
    } else if (type == MessageType::HEADERS) {
        prune_window(ctx.recent_headers_msgs);
        if (ctx.recent_headers_msgs.size() >= detail::kPolicyMaxHeadersMsgsPerWindow) {
            reject_reason = "headers-rate-limit";
            return false;
        }
        ctx.recent_headers_msgs.push_back(now);
    } else if (type == MessageType::PING || type == MessageType::PONG) {
        prune_window(ctx.recent_ping_msgs);
        if (ctx.recent_ping_msgs.size() >= detail::kPolicyMaxPingMsgsPerWindow) {
            reject_reason = "ping-rate-limit";
            return false;
        }
        ctx.recent_ping_msgs.push_back(now);
    }
    return true;
}

} // namespace geocoin::network
