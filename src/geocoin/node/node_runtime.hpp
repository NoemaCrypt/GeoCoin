#pragma once

#include <cstdlib>

namespace geocoin::node::detail {

inline bool node_verbose_enabled() {
    static const bool enabled = (std::getenv("GEOCOIN_NODE_VERBOSE") != nullptr);
    return enabled;
}

inline bool sync_debug_enabled() {
    static const bool enabled = (std::getenv("GEOCOIN_SYNC_DEBUG") != nullptr);
    return enabled;
}

} // namespace geocoin::node::detail
