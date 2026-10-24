#pragma once

#include "geocoin/types.hpp"

#include <string>

namespace geocoin::node::detail {

inline constexpr Height kInitialBlockDownloadLagThreshold = 144;

inline bool is_height_behind(Height local_height, Height best_peer_height) {
    return best_peer_height > local_height;
}

inline bool is_height_behind_by_more_than(
    Height local_height,
    Height best_peer_height,
    Height threshold
) {
    return best_peer_height > local_height && (best_peer_height - local_height) > threshold;
}

inline std::string make_sync_lag_reason(Height local_height, Height best_peer_height) {
    return "local height " + std::to_string(local_height) +
           " is behind best peer height " + std::to_string(best_peer_height);
}

} // namespace geocoin::node::detail
