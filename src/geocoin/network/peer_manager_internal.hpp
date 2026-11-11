#pragma once

#include "geocoin/network/network_utils.hpp"
#include "geocoin/network/message_limits.hpp"
#include "geocoin/network/peer.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>

namespace geocoin::network::detail {

inline bool sync_debug_enabled() {
    static const bool enabled = (std::getenv("GEOCOIN_SYNC_DEBUG") != nullptr);
    return enabled;
}

inline constexpr size_t kMaxInboundPeers = 256;
inline constexpr size_t kMaxOutboundPeers = 256;
inline constexpr size_t kMaxMessagePayload = 4 * 1024 * 1024;
inline constexpr size_t kMaxReceiveBuffer = 8 * 1024 * 1024;
inline constexpr size_t kMaxPreAuthReceiveBuffer = 8 * 1024;
inline constexpr size_t kMaxFramesPerPeerUpdate = 64;
inline constexpr size_t kMaxFramesPerManagerUpdate = 512;
inline constexpr size_t kMaxReceiveBytesPerManagerUpdate = 2 * 1024 * 1024;
inline constexpr size_t kAuthRounds = 3;
inline constexpr int kPeerTimeoutSeconds = 60;
inline constexpr int kAuthTimeoutSeconds = 120;
inline constexpr int kVersionStallSeconds = 30;
inline constexpr int kAuthProgressStallSeconds = 60;
inline constexpr int kPingIntervalSeconds = 30;
inline constexpr int kPingReplyTimeoutSeconds = 45;
inline constexpr uint32_t kPolicyBanScoreThreshold = 100;
inline constexpr uint32_t kPolicyScoreDecayPoints = 5;
inline constexpr auto kPolicyScoreDecayInterval = std::chrono::seconds(60);
inline constexpr uint32_t kPolicyMaxInvItems = 2048;
using policy::kPolicyMaxGetDataItems;
using policy::kPolicyMaxAddrItems;
inline constexpr size_t kPolicyMaxAddrPayloadBytes = 8 * 1024;
inline constexpr size_t kPolicyMaxInvMsgsPerWindow = 32;
// The block scheduler keeps up to 16 blocks in flight, requests four blocks
// per GETDATA, and refills the window as responses arrive.  A fast honest peer
// can therefore emit well above eight request messages in ten seconds.  Keep
// the request/response ceilings aligned with that normal refill rate; the
// per-message item, payload, global-frame and receive-byte budgets remain the
// primary amplification limits.
inline constexpr size_t kPolicyMaxGetDataMsgsPerWindow = 32;
inline constexpr size_t kPolicyMaxAddrMsgsPerWindow = 4;
inline constexpr size_t kPolicyMaxAllMsgsPerWindow = 256;
using policy::kPolicyMaxTxMsgsPerWindow;
inline constexpr size_t kPolicyMaxBlockMsgsPerWindow = 128;
inline constexpr size_t kPolicyMaxHeadersMsgsPerWindow = 16;
inline constexpr size_t kPolicyMaxPingMsgsPerWindow = 8;
inline constexpr size_t kPolicyRecentDisconnectLimit = 128;
inline constexpr size_t kPolicyMaxTempBans = 4096;
inline constexpr size_t kPolicyMaxInboundPerIp = 8;
inline constexpr size_t kPolicyMaxInboundPerGroup = 32;
using policy::kPolicyRateWindow;
inline constexpr auto kPolicyTempBanDuration = std::chrono::minutes(30);

inline std::string extract_ip(const std::string& address) {
    std::string host;
    uint16_t port = 0;
    if (NetworkUtils::split_host_port(address, host, port)) {
        return host;
    }
    return NetworkUtils::normalize_ip_address(address);
}

inline const char* peer_state_name(PeerState state) {
    switch (state) {
        case PeerState::DISCONNECTED:
            return "disconnected";
        case PeerState::CONNECTING:
            return "connecting";
        case PeerState::CONNECTED:
            return "connected";
        case PeerState::AUTHENTICATING:
            return "authenticating";
        case PeerState::AUTHENTICATED:
            return "authenticated";
        case PeerState::SYNCING:
            return "syncing";
        case PeerState::READY:
            return "ready";
    }
    return "unknown";
}

} // namespace geocoin::network::detail
