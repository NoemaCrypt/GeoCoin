#pragma once

#include "geocoin/network/sync_manager.hpp"
#include "geocoin/utils/metrics.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <string>

namespace geocoin::network::detail {

inline bool sync_debug_enabled() {
    static const bool enabled = (std::getenv("GEOCOIN_SYNC_DEBUG") != nullptr);
    return enabled;
}

inline uint64_t steady_now_ms() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count());
}

inline void set_sync_status_metric(SyncStatus status) {
    GEO_METRIC_SET("sync.status", static_cast<int64_t>(status));
}

inline std::string hash_to_hex(const Hash256& hash) {
    std::ostringstream ss;
    for (auto byte : hash) {
        ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
    }
    return ss.str();
}

} // namespace geocoin::network::detail
