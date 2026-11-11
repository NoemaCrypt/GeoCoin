#include "geocoin/consensus/consensus.hpp"

#include "geocoin/consensus/chainparams.hpp"
#include "geocoin/consensus/proof_of_work.hpp"

#include <algorithm>

namespace geocoin::consensus {
namespace {

uint256 max_uint256() {
    uint256 value;
    value.data[0] = 0xFFFFFFFFFFFFFFFFULL;
    value.data[1] = 0xFFFFFFFFFFFFFFFFULL;
    value.data[2] = 0xFFFFFFFFFFFFFFFFULL;
    value.data[3] = 0xFFFFFFFFFFFFFFFFULL;
    return value;
}

uint256 add_one(const uint256& value) {
    uint256 result = value;
    for (int i = 0; i < 4; ++i) {
        const uint64_t before = result.data[i];
        result.data[i] += 1;
        if (result.data[i] > before) {
            break;
        }
    }
    return result;
}

void shift_left_one(uint256& value) {
    uint64_t carry = 0;
    for (int i = 0; i < 4; ++i) {
        const uint64_t new_carry = value.data[i] >> 63;
        value.data[i] = (value.data[i] << 1) | carry;
        carry = new_carry;
    }
}

bool get_bit(const uint256& value, int bit_index) {
    const int word_index = bit_index / 64;
    const int offset = bit_index % 64;
    return ((value.data[word_index] >> offset) & 1ULL) != 0;
}

uint256 divide_uint256(const uint256& numerator, const uint256& denominator) {
    uint256 quotient;
    uint256 remainder;

    if (denominator.is_zero()) {
        return quotient;
    }

    for (int bit = 255; bit >= 0; --bit) {
        shift_left_one(remainder);
        if (get_bit(numerator, bit)) {
            remainder.data[0] |= 1ULL;
        }

        if (remainder >= denominator) {
            remainder = remainder - denominator;
            quotient.data[bit / 64] |= (1ULL << (bit % 64));
        }
    }

    return quotient;
}

uint256 divide_u320_by_u64_saturating(const uint64_t value[5], uint64_t divisor) {
    if (divisor == 0) {
        return max_uint256();
    }

    uint64_t quotient[5] = {0, 0, 0, 0, 0};
    uint64_t remainder = 0;
    for (int i = 4; i >= 0; --i) {
        quotient[i] = utils::div_u128_by_u64(
            remainder,
            value[i],
            divisor,
            &remainder
        );
    }

    if (quotient[4] != 0) {
        return max_uint256();
    }

    uint256 result;
    for (int i = 0; i < 4; ++i) {
        result.data[i] = quotient[i];
    }
    return result;
}

void add_uint256_to_u320(uint64_t sum[5], const uint256& value) {
    uint64_t carry = 0;
    for (int i = 0; i < 4; ++i) {
        utils::add_u64_carry(sum[i], value.data[i], carry, sum[i], carry);
    }
    uint64_t ignored = 0;
    utils::add_u64_carry(sum[4], 0, carry, sum[4], ignored);
}

uint256 mul_div_uint256_saturating(
    const uint256& value,
    uint64_t multiplier,
    uint64_t divisor
) {
    if (divisor == 0) {
        return max_uint256();
    }

    // Preserve the fifth limb of the 256x64 product. Truncating it before the
    // division can wrap an easy target into a much harder one (notably near
    // the testnet pow limit) and changes consensus difficulty unexpectedly.
    uint64_t product[5] = {0, 0, 0, 0, 0};
    uint64_t carry = 0;
    for (int i = 0; i < 4; ++i) {
        const utils::U128Parts wide = utils::mul_u64_wide(value.data[i], multiplier);
        uint64_t carry_bit = 0;
        utils::add_u64_carry(wide.lo, carry, 0, product[i], carry_bit);
        uint64_t ignored = 0;
        utils::add_u64_carry(wide.hi, carry_bit, 0, carry, ignored);
    }
    product[4] = carry;

    return divide_u320_by_u64_saturating(product, divisor);
}

} // namespace

bool check_difficulty(
    uint32_t bits,
    const chain::BlockIndex* prev_index,
    Timestamp block_timestamp,
    ValidationState& state
) {
    if (!prev_index) {
        // Genesis блок - проверяем начальную сложность
        if (bits != Params().initial_bits()) {
            return state.invalid(RejectCode::INVALID, "bad-diffbits", "incorrect difficulty for genesis");
        }
        return true;
    }

    // Per-block DAA: пересчитываем ожидаемую сложность для каждого блока
    uint32_t expected_bits = calculate_next_difficulty(prev_index, block_timestamp);
    if (bits != expected_bits) {
        return state.invalid(RejectCode::INVALID, "bad-diffbits", "incorrect difficulty adjustment");
    }

    return true;
}

uint32_t calculate_next_difficulty(
    const chain::BlockIndex* prev_index,
    Timestamp block_timestamp
) {
    if (!prev_index) {
        return Params().initial_bits();
    }

    // The isolated load network deliberately keeps a trivial target so stress
    // tests exercise full PoW validation without depending on host hash rate.
    if (Params().fixed_difficulty()) {
        return Params().initial_bits();
    }

    // Минимальная сложность после долгой паузы допустима только в testnet.
    // Mainnet не использует timestamp-triggered emergency drop: при разрешенном
    // future drift такой механизм можно активировать раньше реального порога.
    if (Params().allow_min_difficulty_blocks()) {
        const uint64_t min_gap = static_cast<uint64_t>(params::TARGET_BLOCK_TIME) * 2ULL;
        const uint64_t tip_time = prev_index->get_block_time();
        if (static_cast<uint64_t>(block_timestamp) > tip_time + min_gap) {
            return Params().pow_limit_bits();
        }
    }

    // Для самого первого блока solve-time еще нет. Начиная со второго блока
    // используем нарастающее окно, чтобы сеть не зависела от точно угаданного
    // стартового hashrate в течение первых 60 блоков.
    const Height window = std::min(prev_index->height, params::DIFFICULTY_DAA_WINDOW);
    if (window == 0) {
        return prev_index->get_bits();
    }

    // LWMA: усредняем targets последних N блоков и взвешиваем более свежие
    // solve-times сильнее. В отличие от прежней схемы ошибка одного и того же
    // 60-блочного окна не умножается повторно на previous target каждый блок.
    const Height start_height = prev_index->height - window;
    const chain::BlockIndex* previous = prev_index->get_ancestor(start_height);
    if (!previous) {
        return prev_index->get_bits();
    }

    uint64_t weighted_solvetime = 0;
    uint64_t target_sum[5] = {0, 0, 0, 0, 0};
    int64_t previous_mtp = static_cast<int64_t>(previous->get_median_time_past());
    const int64_t max_solvetime = static_cast<int64_t>(params::TARGET_BLOCK_TIME) * 6;

    for (Height i = 1; i <= window; ++i) {
        const chain::BlockIndex* current = prev_index->get_ancestor(start_height + i);
        if (!current) {
            return prev_index->get_bits();
        }

        const int64_t current_mtp = static_cast<int64_t>(current->get_median_time_past());
        int64_t solvetime = current_mtp - previous_mtp;
        solvetime = std::clamp<int64_t>(solvetime, 1, max_solvetime);
        weighted_solvetime += static_cast<uint64_t>(i) * static_cast<uint64_t>(solvetime);
        add_uint256_to_u320(target_sum, ProofOfWork::bits_to_target(current->get_bits()));
        previous_mtp = current_mtp;
    }

    const uint64_t weight_denominator =
        static_cast<uint64_t>(window) * static_cast<uint64_t>(window + 1) *
        static_cast<uint64_t>(params::TARGET_BLOCK_TIME) / 2ULL;
    uint256 average_target = divide_u320_by_u64_saturating(
        target_sum,
        static_cast<uint64_t>(window)
    );
    uint256 new_target = mul_div_uint256_saturating(
        average_target,
        weighted_solvetime,
        weight_denominator
    );

    // Дополнительный consensus-bound: не более x2/÷2 относительно прошлого
    // блока, независимо от timestamp-профиля окна.
    const uint256 previous_target = ProofOfWork::bits_to_target(prev_index->get_bits());
    const uint256 hardest_allowed = previous_target / params::MAX_DIFFICULTY_ADJUSTMENT;
    const uint256 easiest_allowed = mul_div_uint256_saturating(
        previous_target,
        params::MAX_DIFFICULTY_ADJUSTMENT,
        1
    );
    if (new_target < hardest_allowed) {
        new_target = hardest_allowed;
    }
    if (new_target > easiest_allowed) {
        new_target = easiest_allowed;
    }

    const uint256 max_target = ProofOfWork::bits_to_target(Params().pow_limit_bits());
    if (new_target > max_target) {
        new_target = max_target;
    }

    return ProofOfWork::target_to_bits(new_target);
}

uint256 calculate_chainwork(
    const chain::BlockIndex* prev_index,
    uint32_t bits
) {
    if (!ProofOfWork::is_valid_target_bits(bits)) {
        return prev_index ? prev_index->chain_work : uint256{};
    }

    const uint256 target = ProofOfWork::bits_to_target(bits);

    uint256 work;
    const uint256 max_target = max_uint256();
    if (target == max_target) {
        work = uint256(1);
    } else {
        const uint256 denominator = add_one(target);
        // floor(2^256 / (target + 1)) without representing 2^256:
        // floor((2^256 - 1 - target) / (target + 1)) + 1.
        work = add_one(divide_uint256(max_target - target, denominator));
    }

    if (prev_index) {
        work = work + prev_index->chain_work;
    }

    return work;
}

} // namespace geocoin::consensus
