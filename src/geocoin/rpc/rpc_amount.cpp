#include "geocoin/rpc/rpc_amount.hpp"
#include "geocoin/rpc/rpc_params.hpp"
#include "geocoin/consensus/consensus.hpp"
#include "geocoin/utils/u64_math.hpp"

#include <cctype>
#include <limits>
#include <string_view>

namespace geocoin::rpc {
namespace {

bool u128_greater(const utils::U128Parts& left, const utils::U128Parts& right) {
    return left.hi > right.hi || (left.hi == right.hi && left.lo > right.lo);
}

uint64_t amount_max_u64() {
    return static_cast<uint64_t>((std::numeric_limits<Amount>::max)());
}

bool parse_decimal_coins(std::string_view token, Amount& out)
{
    if (token.empty() || token.front() == '-') {
        return false;
    }

    size_t pos = 0;
    std::string digits;
    digits.reserve(token.size());
    size_t integer_digits = 0;
    while (pos < token.size() && std::isdigit(static_cast<unsigned char>(token[pos])) != 0) {
        digits.push_back(token[pos++]);
        ++integer_digits;
    }
    if (integer_digits == 0) {
        return false;
    }

    int64_t fractional_digits = 0;
    if (pos < token.size() && token[pos] == '.') {
        ++pos;
        const size_t fraction_begin = pos;
        while (pos < token.size() && std::isdigit(static_cast<unsigned char>(token[pos])) != 0) {
            digits.push_back(token[pos++]);
        }
        if (pos == fraction_begin) {
            return false;
        }
        fractional_digits = static_cast<int64_t>(pos - fraction_begin);
    }

    int64_t exponent = 0;
    if (pos < token.size() && (token[pos] == 'e' || token[pos] == 'E')) {
        ++pos;
        bool negative_exponent = false;
        if (pos < token.size() && (token[pos] == '+' || token[pos] == '-')) {
            negative_exponent = token[pos] == '-';
            ++pos;
        }
        if (pos == token.size()) {
            return false;
        }
        int64_t magnitude = 0;
        for (; pos < token.size(); ++pos) {
            const unsigned char ch = static_cast<unsigned char>(token[pos]);
            if (std::isdigit(ch) == 0 || magnitude > 1000000) {
                return false;
            }
            magnitude = magnitude * 10 + static_cast<int64_t>(ch - '0');
        }
        exponent = negative_exponent ? -magnitude : magnitude;
    }
    if (pos != token.size()) {
        return false;
    }

    int64_t scale = fractional_digits - exponent;
    while (scale > 8 && !digits.empty() && digits.back() == '0') {
        digits.pop_back();
        --scale;
    }
    if (scale > 8) {
        return false; // Fractions smaller than one angle are never rounded.
    }

    const auto first_non_zero = digits.find_first_not_of('0');
    if (first_non_zero == std::string::npos) {
        return false;
    }
    digits.erase(0, first_non_zero);

    const int64_t zeros_to_append = 8 - scale;
    if (zeros_to_append < 0 || zeros_to_append > 19 ||
        digits.size() > 19 - static_cast<size_t>(zeros_to_append)) {
        return false;
    }
    digits.append(static_cast<size_t>(zeros_to_append), '0');

    uint64_t amount = 0;
    const uint64_t max_amount = amount_max_u64();
    for (const char ch : digits) {
        const uint64_t digit = static_cast<uint64_t>(ch - '0');
        if (amount > (max_amount - digit) / 10) {
            return false;
        }
        amount = amount * 10 + digit;
    }
    if (amount == 0) {
        return false;
    }
    out = static_cast<Amount>(amount);
    return true;
}

} // namespace

Amount rpc_coins_to_positive_amount(const json& value, const std::string& error_message) {
    std::string token;
    if (value.is_string()) {
        token = value.get<std::string>();
    } else if (value.is_number()) {
        token = value.dump();
    } else {
        throw RPCException(RPC_INVALID_PARAMETER, error_message);
    }

    Amount amount = 0;
    if (!parse_decimal_coins(token, amount)) {
        throw RPCException(RPC_INVALID_PARAMETER, error_message);
    }
    return amount;
}

Amount rpc_angles_to_positive_amount(const json& value, const std::string& error_message) {
    if (value.is_number_unsigned()) {
        const uint64_t angles = value.get<uint64_t>();
        if (angles == 0 || angles > amount_max_u64()) {
            throw RPCException(RPC_INVALID_PARAMETER, error_message);
        }
        return static_cast<Amount>(angles);
    }

    if (value.is_number_integer()) {
        const int64_t angles = value.get<int64_t>();
        if (angles <= 0) {
            throw RPCException(RPC_INVALID_PARAMETER, error_message);
        }
        return static_cast<Amount>(angles);
    }

    throw RPCException(RPC_INVALID_PARAMETER, error_message);
}

Amount rpc_required_amount_field(
    const json& object,
    const std::string& field,
    const std::string& error_message
) {
    const uint64_t value = required_u64_field(object, field, error_message);
    if (value > amount_max_u64()) {
        throw RPCException(RPC_INVALID_PARAMETER, error_message);
    }
    return static_cast<Amount>(value);
}

Amount rpc_optional_amount_field(
    const json& object,
    const std::string& field,
    Amount default_value,
    const std::string& error_message
) {
    if (!object.is_object() || !object.contains(field) || object[field].is_null()) {
        return default_value;
    }
    return rpc_required_amount_field(object, field, error_message);
}

Amount checked_add_amount_or_throw(
    Amount left,
    Amount right,
    const std::string& error_message,
    RPCErrorCode code
) {
    if ((right > 0 && left > (std::numeric_limits<Amount>::max)() - right) ||
        (right < 0 && left < (std::numeric_limits<Amount>::min)() - right)) {
        throw RPCException(code, error_message);
    }
    return left + right;
}

uint64_t saturating_mul_u64(uint64_t left, uint64_t right) {
    const uint64_t max_value = (std::numeric_limits<uint64_t>::max)();
    if (left != 0 && right > max_value / left) {
        return max_value;
    }
    return left * right;
}

Amount mul_div_amount_ceil_clamped(Amount amount, uint64_t multiplier, uint64_t divisor) {
    if (amount <= 0) {
        return 0;
    }
    if (divisor == 0) {
        return (std::numeric_limits<Amount>::max)();
    }

    const uint64_t max_amount = amount_max_u64();
    const utils::U128Parts product = utils::mul_u64_wide(static_cast<uint64_t>(amount), multiplier);
    const utils::U128Parts max_product = utils::mul_u64_wide(max_amount, divisor);
    if (u128_greater(product, max_product)) {
        return (std::numeric_limits<Amount>::max)();
    }

    uint64_t remainder = 0;
    const uint64_t quotient = utils::div_u128_by_u64_fast(
        product.hi,
        product.lo,
        divisor,
        &remainder
    );
    if (remainder == 0) {
        return static_cast<Amount>(quotient);
    }
    if (quotient >= max_amount) {
        return (std::numeric_limits<Amount>::max)();
    }
    return static_cast<Amount>(quotient + 1);
}

Amount calc_sidechain_fee_checked(Amount amount, uint32_t fee_bps, const std::string& error_message) {
    if (amount <= 0) {
        return consensus::params::MIN_TX_FEE;
    }

    const uint64_t max_amount = amount_max_u64();
    const utils::U128Parts product = utils::mul_u64_wide(static_cast<uint64_t>(amount), fee_bps);
    const utils::U128Parts max_product = utils::mul_u64_wide(max_amount, 10000u);
    if (u128_greater(product, max_product)) {
        throw RPCException(RPC_INVALID_PARAMETER, error_message);
    }

    uint64_t fee = utils::div_u128_by_u64_fast(product.hi, product.lo, 10000u, nullptr);
    const uint64_t min_fee = static_cast<uint64_t>(consensus::params::MIN_TX_FEE);
    if (fee < min_fee) {
        fee = min_fee;
    }
    if (fee > max_amount) {
        throw RPCException(RPC_INVALID_PARAMETER, error_message);
    }
    return static_cast<Amount>(fee);
}

} // namespace geocoin::rpc
