#pragma once

#include "geocoin/types.hpp"

#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <string>

namespace geocoin::core {

inline bool NodeVerboseEnabled() {
    static const bool enabled = (std::getenv("GEOCOIN_NODE_VERBOSE") != nullptr);
    return enabled;
}

inline std::string HashToHex(const Hash256& hash) {
    std::ostringstream ss;
    for (auto byte : hash) {
        ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
    }
    return ss.str();
}

inline std::string Uint256ToHex(const uint256& value) {
    std::ostringstream ss;
    ss << std::hex << std::setfill('0');
    for (int i = 3; i >= 0; --i) {
        ss << std::setw(16) << value.data[i];
    }
    return ss.str();
}

} // namespace geocoin::core
