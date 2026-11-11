#pragma once

#include <cstdint>
#if defined(_MSC_VER) && defined(_M_X64) && !defined(__clang__)
#include <intrin.h>
#pragma intrinsic(_umul128)
#pragma intrinsic(_udiv128)
#endif

namespace geocoin::utils {

struct U128Parts {
    uint64_t lo = 0;
    uint64_t hi = 0;
};

struct I128Parts {
    uint64_t lo = 0;
    uint64_t hi = 0; // Two's-complement high 64 bits
};

constexpr inline void add_u64_carry(
    uint64_t a,
    uint64_t b,
    uint64_t carry_in,
    uint64_t& out,
    uint64_t& carry_out
) {
    const uint64_t s1 = a + b;
    const uint64_t c1 = (s1 < a) ? 1 : 0;
    const uint64_t s2 = s1 + carry_in;
    const uint64_t c2 = (s2 < s1) ? 1 : 0;
    out = s2;
    carry_out = (c1 | c2);
}

constexpr inline void sub_u64_borrow(
    uint64_t a,
    uint64_t b,
    uint64_t borrow_in,
    uint64_t& out,
    uint64_t& borrow_out
) {
    const uint64_t d1 = a - b;
    const uint64_t b1 = (a < b) ? 1 : 0;
    const uint64_t d2 = d1 - borrow_in;
    const uint64_t b2 = (d1 < borrow_in) ? 1 : 0;
    out = d2;
    borrow_out = (b1 | b2);
}

inline U128Parts mul_u64_wide(uint64_t a, uint64_t b) {
#if defined(_MSC_VER) && defined(_M_X64) && !defined(__clang__)
    U128Parts out{};
    out.lo = _umul128(a, b, &out.hi);
    return out;
#elif defined(__x86_64__) && defined(__SIZEOF_INT128__) && !(defined(_WIN32) && defined(__clang__))
    const unsigned __int128 product = static_cast<unsigned __int128>(a) * static_cast<unsigned __int128>(b);
    U128Parts out{};
    out.lo = static_cast<uint64_t>(product);
    out.hi = static_cast<uint64_t>(product >> 64);
    return out;
#else
    const uint64_t a0 = static_cast<uint32_t>(a);
    const uint64_t a1 = a >> 32;
    const uint64_t b0 = static_cast<uint32_t>(b);
    const uint64_t b1 = b >> 32;

    const uint64_t p00 = a0 * b0;
    const uint64_t p01 = a0 * b1;
    const uint64_t p10 = a1 * b0;
    const uint64_t p11 = a1 * b1;

    U128Parts out{};
    out.lo = p00;

    uint64_t carry = 0;
    uint64_t t = out.lo + (p01 << 32);
    if (t < out.lo) {
        ++carry;
    }
    out.lo = t;

    t = out.lo + (p10 << 32);
    if (t < out.lo) {
        ++carry;
    }
    out.lo = t;

    out.hi = p11 + (p01 >> 32) + (p10 >> 32) + carry;
    return out;
#endif
}

inline uint64_t div_u128_by_u64(
    uint64_t hi,
    uint64_t lo,
    uint64_t divisor,
    uint64_t* remainder_out = nullptr
) {
    if (divisor == 0) {
        if (remainder_out) {
            *remainder_out = 0;
        }
        return 0;
    }

    if (hi == 0) {
        if (remainder_out) {
            *remainder_out = lo % divisor;
        }
        return lo / divisor;
    }

#if defined(_MSC_VER) && defined(_M_X64) && !defined(__clang__)
    // _udiv128 требует hi < divisor (иначе возможна ошибка деления).
    if (hi < divisor) {
        uint64_t rem = 0;
        const uint64_t quot = _udiv128(hi, lo, divisor, &rem);
        if (remainder_out) {
            *remainder_out = rem;
        }
        return quot;
    }
#elif defined(__x86_64__) && defined(__SIZEOF_INT128__) && !(defined(_WIN32) && defined(__clang__))
    const unsigned __int128 dividend =
        (static_cast<unsigned __int128>(hi) << 64) | static_cast<unsigned __int128>(lo);
    const unsigned __int128 q128 = dividend / static_cast<unsigned __int128>(divisor);
    const uint64_t rem = static_cast<uint64_t>(dividend % static_cast<unsigned __int128>(divisor));
    if (remainder_out) {
        *remainder_out = rem;
    }
    return static_cast<uint64_t>(q128);
#endif

    uint64_t q_hi = 0;
    uint64_t q_lo = 0;
    uint64_t r_hi = 0;
    uint64_t r_lo = 0;

    for (int bit_idx = 127; bit_idx >= 0; --bit_idx) {
        const uint64_t bit = (bit_idx >= 64)
            ? ((hi >> (bit_idx - 64)) & 1ULL)
            : ((lo >> bit_idx) & 1ULL);

        r_hi = (r_hi << 1) | (r_lo >> 63);
        r_lo = (r_lo << 1) | bit;

        bool ge = (r_hi > 0) || (r_lo >= divisor);
        if (ge) {
            if (r_lo < divisor) {
                r_lo = r_lo - divisor;
                r_hi -= 1;
            } else {
                r_lo -= divisor;
            }
        }

        q_hi = (q_hi << 1) | (q_lo >> 63);
        q_lo = (q_lo << 1) | static_cast<uint64_t>(ge ? 1 : 0);
    }

    if (remainder_out) {
        *remainder_out = r_lo;
    }
    return q_lo;
}

inline uint64_t div_u128_by_u64_fast(
    uint64_t hi,
    uint64_t lo,
    uint64_t divisor,
    uint64_t* remainder_out = nullptr
) {
    if (divisor == 0) {
        if (remainder_out) {
            *remainder_out = 0;
        }
        return 0;
    }
    if (hi == 0) {
        if (remainder_out) {
            *remainder_out = lo % divisor;
        }
        return lo / divisor;
    }
    return div_u128_by_u64(hi, lo, divisor, remainder_out);
}

inline uint64_t mul_div_u64_floor(uint64_t a, uint64_t b, uint64_t divisor) {
    if (divisor == 0) {
        return 0;
    }
    const U128Parts prod = mul_u64_wide(a, b);
    return div_u128_by_u64_fast(prod.hi, prod.lo, divisor, nullptr);
}

inline uint64_t mul_div_u64_ceil(uint64_t a, uint64_t b, uint64_t divisor) {
    if (divisor == 0) {
        return 0;
    }
    const U128Parts prod = mul_u64_wide(a, b);
    uint64_t rem = 0;
    uint64_t q = div_u128_by_u64_fast(prod.hi, prod.lo, divisor, &rem);
    if (rem != 0 && q != UINT64_MAX) {
        ++q;
    }
    return q;
}

inline I128Parts i128_neg(I128Parts v) {
    v.lo = ~v.lo;
    v.hi = ~v.hi;
    uint64_t carry = 0;
    add_u64_carry(v.lo, 1, 0, v.lo, carry);
    v.hi += carry;
    return v;
}

inline bool i128_is_negative(const I128Parts& v) {
    return (v.hi & (1ULL << 63)) != 0;
}

inline I128Parts i128_add(const I128Parts& a, const I128Parts& b) {
    I128Parts out{};
    uint64_t carry = 0;
    add_u64_carry(a.lo, b.lo, 0, out.lo, carry);
    out.hi = a.hi + b.hi + carry;
    return out;
}

inline I128Parts i128_sub(const I128Parts& a, const I128Parts& b) {
    return i128_add(a, i128_neg(b));
}

inline I128Parts i128_abs(const I128Parts& v) {
    return i128_is_negative(v) ? i128_neg(v) : v;
}

inline I128Parts i64_mul_wide(int64_t a, int64_t b) {
    const bool negative = ((a < 0) != (b < 0));

    const uint64_t ua = (a >= 0)
        ? static_cast<uint64_t>(a)
        : (static_cast<uint64_t>(~static_cast<uint64_t>(a)) + 1ULL);
    const uint64_t ub = (b >= 0)
        ? static_cast<uint64_t>(b)
        : (static_cast<uint64_t>(~static_cast<uint64_t>(b)) + 1ULL);

    const U128Parts p = mul_u64_wide(ua, ub);
    I128Parts out{p.lo, p.hi};
    if (negative) {
        out = i128_neg(out);
    }
    return out;
}

inline long double i128_abs_to_long_double(const I128Parts& v) {
    static constexpr long double kTwoPow64 = 18446744073709551616.0L;
    const I128Parts mag = i128_abs(v);
    return static_cast<long double>(mag.hi) * kTwoPow64 + static_cast<long double>(mag.lo);
}

} // namespace geocoin::utils
