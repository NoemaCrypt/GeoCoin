#pragma once

#include <cstdint>
#include <chrono>

namespace geocoin::network::policy {

// Peer admission and node response generation must enforce the same budgets.
inline constexpr uint32_t kPolicyMaxGetDataItems = 64;
inline constexpr uint32_t kPolicyMaxAddrItems = 64;
inline constexpr uint32_t kPolicyMaxTxMsgsPerWindow = 64;
inline constexpr auto kPolicyRateWindow = std::chrono::seconds(10);

} // namespace geocoin::network::policy
