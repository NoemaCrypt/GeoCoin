#pragma once

#include "geocoin/types.hpp"
#include "geocoin/consensus/consensus.hpp"
#include "geocoin/utils/u64_math.hpp"

namespace geocoin::consensus {

inline Amount calc_sidechain_fee(Amount amount, uint32_t fee_bps) {
    if (amount <= 0) {
        return params::MIN_TX_FEE;
    }
    const uint64_t bps = fee_bps;
    const uint64_t min_fee = static_cast<uint64_t>(params::MIN_TX_FEE);
    uint64_t fee = utils::mul_div_u64_floor(static_cast<uint64_t>(amount), bps, 10000u);
    if (fee < min_fee) {
        fee = min_fee;
    }
    return static_cast<Amount>(fee);
}

} // namespace geocoin::consensus
