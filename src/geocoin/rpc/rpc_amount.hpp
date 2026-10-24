#pragma once

#include "geocoin/rpc/rpc_types.hpp"
#include "geocoin/types.hpp"

#include <cstdint>
#include <string>

namespace geocoin::rpc {

Amount rpc_coins_to_positive_amount(const json& value, const std::string& error_message);
Amount rpc_angles_to_positive_amount(const json& value, const std::string& error_message);

Amount rpc_required_amount_field(
    const json& object,
    const std::string& field,
    const std::string& error_message
);
Amount rpc_optional_amount_field(
    const json& object,
    const std::string& field,
    Amount default_value,
    const std::string& error_message
);

Amount checked_add_amount_or_throw(
    Amount left,
    Amount right,
    const std::string& error_message,
    RPCErrorCode code = RPC_INVALID_PARAMETER
);

uint64_t saturating_mul_u64(uint64_t left, uint64_t right);

Amount mul_div_amount_ceil_clamped(Amount amount, uint64_t multiplier, uint64_t divisor);

Amount calc_sidechain_fee_checked(
    Amount amount,
    uint32_t fee_bps,
    const std::string& error_message = "sidechain fee превышает допустимый диапазон"
);

} // namespace geocoin::rpc
