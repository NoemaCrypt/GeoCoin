#pragma once

#include "geocoin/core/transaction.hpp"
#include "geocoin/state/state.hpp"
#include "geocoin/consensus/consensus.hpp"
#include <string>
#include <optional>

namespace geocoin::network {

struct MempoolPolicyConfig {
    uint64_t min_relay_fee = static_cast<uint64_t>(consensus::params::MIN_TX_FEE);
    uint64_t min_relay_fee_rate_per_kb =
        static_cast<uint64_t>(consensus::params::FEE_RATE_PER_KB);
    bool enforce_coinbase_maturity = true;
    uint32_t max_new_witness_pubkeys_per_tx = 128;
};

class MempoolPolicy {
public:
    static uint64_t required_fee_for_size(size_t serialized_size,
                                          const MempoolPolicyConfig& config);

    static bool check(const core::Transaction& tx,
                      const state::ChainState& chain_state,
                      uint64_t fee,
                      const MempoolPolicyConfig& config,
                      std::string* reason,
                      std::optional<Timestamp> block_time = std::nullopt);
};

} // namespace geocoin::network
