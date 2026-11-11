#include "geocoin/utils/bytes.hpp"
#include "geocoin/utils/u64_math.hpp"
#include "geocoin/utils/varint.hpp"
#include "geocoin/consensus/consensus.hpp"
#include <cstdint>
#include <iostream>
#include <limits>
#include <random>
#include <string>

using namespace geocoin;

namespace {

int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

void test_mul_u64_wide_basic() {
    const auto r1 = utils::mul_u64_wide(1000000ULL, 1000000ULL);
    check(r1.lo == 1000000000000ULL && r1.hi == 0ULL, "mul_u64_wide simple");

    const auto r2 = utils::mul_u64_wide(
        std::numeric_limits<uint64_t>::max(),
        std::numeric_limits<uint64_t>::max()
    );
    check(r2.lo == 1ULL, "mul_u64_wide overflow lo");
    check(r2.hi == (std::numeric_limits<uint64_t>::max() - 1ULL), "mul_u64_wide overflow hi");
}

void test_div_u128_by_u64_basic() {
    uint64_t rem = 0;
    const uint64_t q1 = utils::div_u128_by_u64(0, 1000ULL, 7ULL, &rem);
    check(q1 == (1000ULL / 7ULL), "div_u128_by_u64 hi==0 quotient");
    check(rem == (1000ULL % 7ULL), "div_u128_by_u64 hi==0 remainder");

    const uint64_t q2 = utils::div_u128_by_u64(1ULL, 0ULL, 2ULL, &rem); // 2^64 / 2
    check(q2 == (1ULL << 63), "div_u128_by_u64 hi!=0 quotient");
    check(rem == 0ULL, "div_u128_by_u64 hi!=0 remainder");
}

void test_mul_div_helpers_basic() {
    check(utils::mul_div_u64_floor(100ULL, 333ULL, 100ULL) == 333ULL, "mul_div floor");
    check(utils::mul_div_u64_ceil(100ULL, 333ULL, 100ULL) == 333ULL, "mul_div ceil exact");
    check(utils::mul_div_u64_floor(10ULL, 1ULL, 3ULL) == 3ULL, "mul_div floor non-exact");
    check(utils::mul_div_u64_ceil(10ULL, 1ULL, 3ULL) == 4ULL, "mul_div ceil non-exact");
    check(utils::mul_div_u64_floor(10ULL, 1ULL, 0ULL) == 0ULL, "mul_div floor rejects zero divisor");
    check(utils::mul_div_u64_ceil(10ULL, 1ULL, 0ULL) == 0ULL, "mul_div ceil rejects zero divisor");
}

void test_foundation_cut_does_not_overflow() {
    using geocoin::consensus::calculate_foundation_reward_cut;

    check(calculate_foundation_reward_cut(0) == 0, "foundation cut handles zero");
    check(calculate_foundation_reward_cut(-1) == 0, "foundation cut rejects negative total");
    check(calculate_foundation_reward_cut(999) == 99, "foundation cut rounds down");

    const Amount maximum = (std::numeric_limits<Amount>::max)();
    check(calculate_foundation_reward_cut(maximum) == maximum / 10,
          "foundation cut handles Amount maximum without overflow");
}

void test_bytes_helpers() {
    check(utils::hex_digit_value('0') == 0, "hex digit lower numeric");
    check(utils::hex_digit_value('9') == 9, "hex digit upper numeric");
    check(utils::hex_digit_value('a') == 10, "hex digit lowercase");
    check(utils::hex_digit_value('F') == 15, "hex digit uppercase");
    check(utils::hex_digit_value('x') == -1, "hex digit rejects non-hex");
    check(utils::is_hex_string("09afAF"), "is_hex_string accepts mixed case hex");
    check(!utils::is_hex_string("09xz"), "is_hex_string rejects invalid chars");

    const bytes raw = {0x00, 0x7f, 0x80, 0xff};
    check(utils::bytes_to_hex(raw) == "007f80ff", "bytes_to_hex formats byte vector");

    bytes parsed;
    check(utils::hex_to_bytes_checked("007F80ff", parsed), "hex_to_bytes_checked accepts mixed case");
    check(parsed == raw, "hex_to_bytes_checked parses bytes");
    check(!utils::hex_to_bytes_checked("123", parsed), "hex_to_bytes_checked rejects odd length");
    check(!utils::hex_to_bytes_checked("12xz", parsed), "hex_to_bytes_checked rejects bad digit");
    check(utils::hex_to_bytes("ff00") == bytes({0xff, 0x00}), "hex_to_bytes returns parsed data");
    check(utils::hex_to_bytes("ff0").empty(), "hex_to_bytes returns empty on invalid hex");

    Hash256 hash{};
    hash[0] = 0xab;
    hash[31] = 0xcd;
    const std::string hash_hex = utils::hash256_to_hex(hash);
    check(hash_hex.size() == 64 && hash_hex.substr(0, 2) == "ab" && hash_hex.substr(62, 2) == "cd",
          "hash256_to_hex formats whole hash");
    Hash256 parsed_hash{};
    check(utils::hex_to_hash256_checked(hash_hex, parsed_hash), "hex_to_hash256_checked accepts 32 bytes");
    check(parsed_hash == hash, "hex_to_hash256_checked parses hash");
    check(!utils::hex_to_hash256_checked("00", parsed_hash), "hex_to_hash256_checked rejects short value");
    check(!utils::is_null_hash256(hash), "is_null_hash256 rejects non-zero hash");
    Hash256 null_hash{};
    check(utils::is_null_hash256(null_hash), "is_null_hash256 accepts all-zero hash");

    const uint64_t value = 0x1122334455667788ULL;
    const bytes encoded = utils::encode_u64_le(value);
    check(encoded == bytes({0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11}),
          "encode_u64_le writes little-endian value");
    uint64_t decoded = 0;
    check(utils::decode_u64_le_exact(encoded, decoded) && decoded == value,
          "decode_u64_le_exact reads eight bytes");
    check(!utils::decode_u64_le_exact(bytes{1, 2, 3}, decoded),
          "decode_u64_le_exact rejects wrong length");
    check(utils::read_u64_le_unchecked(bytes{0, 1, 2, 3, 4, 5, 6, 7, 8}, 1) == 0x0807060504030201ULL,
          "read_u64_le_unchecked reads at offset");
    bytes writable(9, 0);
    utils::write_u64_le(writable, 1, value);
    check(std::equal(encoded.begin(), encoded.end(), writable.begin() + 1),
          "write_u64_le writes at offset");

    Height height = 0;
    check(utils::decode_height_le_exact(utils::encode_height_le(12345), height) && height == 12345,
          "height little-endian helpers roundtrip");
    check(!utils::decode_height_le_exact(bytes{1, 2}, height),
          "decode_height_le_exact rejects wrong length");
}

void test_carry_borrow_and_signed_helpers() {
    uint64_t out = 0;
    uint64_t carry = 0;
    utils::add_u64_carry(std::numeric_limits<uint64_t>::max(), 1, 0, out, carry);
    check(out == 0 && carry == 1, "add_u64_carry detects first add carry");
    utils::add_u64_carry(std::numeric_limits<uint64_t>::max(), 0, 1, out, carry);
    check(out == 0 && carry == 1, "add_u64_carry detects carry-in overflow");

    uint64_t borrow = 0;
    utils::sub_u64_borrow(0, 1, 0, out, borrow);
    check(out == std::numeric_limits<uint64_t>::max() && borrow == 1,
          "sub_u64_borrow detects first subtract borrow");
    utils::sub_u64_borrow(0, 0, 1, out, borrow);
    check(out == std::numeric_limits<uint64_t>::max() && borrow == 1,
          "sub_u64_borrow detects borrow-in underflow");

    uint64_t rem = 123;
    check(utils::div_u128_by_u64(1, 2, 0, &rem) == 0 && rem == 0,
          "div_u128_by_u64 rejects zero divisor and clears remainder");
    rem = 123;
    check(utils::div_u128_by_u64_fast(1, 2, 0, &rem) == 0 && rem == 0,
          "div_u128_by_u64_fast rejects zero divisor and clears remainder");
    check(utils::div_u128_by_u64_fast(0, 99, 10, &rem) == 9 && rem == 9,
          "div_u128_by_u64_fast handles single-limb dividend");

    const auto minus_one = utils::i128_neg({1, 0});
    check(utils::i128_is_negative(minus_one), "i128_neg makes negative value");
    const auto plus_one = utils::i128_abs(minus_one);
    check(!utils::i128_is_negative(plus_one) && plus_one.lo == 1 && plus_one.hi == 0,
          "i128_abs returns positive magnitude");
    const auto sum = utils::i128_add({std::numeric_limits<uint64_t>::max(), 0}, {2, 0});
    check(sum.lo == 1 && sum.hi == 1, "i128_add propagates carry");
    const auto diff = utils::i128_sub({1, 1}, {2, 0});
    check(diff.lo == std::numeric_limits<uint64_t>::max() && diff.hi == 0,
          "i128_sub propagates borrow through signed helper");
    const auto product = utils::i64_mul_wide(-3, 7);
    check(utils::i128_is_negative(product) && product.lo == static_cast<uint64_t>(-21),
          "i64_mul_wide preserves sign");
    check(utils::i128_abs_to_long_double(product) >= 21.0L &&
          utils::i128_abs_to_long_double(product) < 22.0L,
          "i128_abs_to_long_double returns magnitude");
}

#if defined(__SIZEOF_INT128__) && !(defined(_WIN32) && defined(__clang__))
void test_against_u128_reference() {
    std::mt19937_64 rng(0xC0FFEEULL);

    for (int i = 0; i < 2000; ++i) {
        const uint64_t a = rng();
        const uint64_t b = rng();
        const uint64_t d = (rng() | 1ULL);

        const auto our_mul = utils::mul_u64_wide(a, b);
        const unsigned __int128 ref_mul =
            static_cast<unsigned __int128>(a) * static_cast<unsigned __int128>(b);
        check(our_mul.lo == static_cast<uint64_t>(ref_mul), "mul reference lo");
        check(our_mul.hi == static_cast<uint64_t>(ref_mul >> 64), "mul reference hi");

        const uint64_t hi = rng() % d; // гарантируем quotient < 2^64
        const uint64_t lo = rng();
        uint64_t our_rem = 0;
        const uint64_t our_q = utils::div_u128_by_u64(hi, lo, d, &our_rem);
        const unsigned __int128 ref_dividend =
            (static_cast<unsigned __int128>(hi) << 64) | static_cast<unsigned __int128>(lo);
        const unsigned __int128 ref_q = ref_dividend / static_cast<unsigned __int128>(d);
        const unsigned __int128 ref_r = ref_dividend % static_cast<unsigned __int128>(d);
        check(our_q == static_cast<uint64_t>(ref_q), "div reference quotient");
        check(our_rem == static_cast<uint64_t>(ref_r), "div reference remainder");

        const uint64_t our_floor = utils::mul_div_u64_floor(a, b, d);
        const uint64_t our_ceil = utils::mul_div_u64_ceil(a, b, d);
        const uint64_t ref_floor = static_cast<uint64_t>(ref_mul / static_cast<unsigned __int128>(d));
        const uint64_t ref_rem2 = static_cast<uint64_t>(ref_mul % static_cast<unsigned __int128>(d));
        uint64_t ref_ceil = ref_floor;
        if (ref_rem2 != 0 && ref_ceil != std::numeric_limits<uint64_t>::max()) {
            ++ref_ceil;
        }
        check(our_floor == ref_floor, "mul_div floor reference");
        check(our_ceil == ref_ceil, "mul_div ceil reference");
    }
}
#endif

void test_portable_reference_vectors() {
    struct MulVector {
        uint64_t a;
        uint64_t b;
        uint64_t hi;
        uint64_t lo;
    };
    const MulVector mul_vectors[] = {
        {0ULL, 0ULL, 0ULL, 0ULL},
        {1ULL, std::numeric_limits<uint64_t>::max(), 0ULL, std::numeric_limits<uint64_t>::max()},
        {0xFFFFFFFFULL, 0xFFFFFFFFULL, 0ULL, 0xFFFFFFFE00000001ULL},
        {0x100000000ULL, 0x100000000ULL, 1ULL, 0ULL},
        {std::numeric_limits<uint64_t>::max(), std::numeric_limits<uint64_t>::max(),
         std::numeric_limits<uint64_t>::max() - 1ULL, 1ULL},
    };
    for (const auto& v : mul_vectors) {
        const auto r = utils::mul_u64_wide(v.a, v.b);
        check(r.hi == v.hi, "portable mul vector hi");
        check(r.lo == v.lo, "portable mul vector lo");
    }

    struct DivVector {
        uint64_t hi;
        uint64_t lo;
        uint64_t divisor;
        uint64_t quotient;
        uint64_t remainder;
    };
    const DivVector div_vectors[] = {
        {0ULL, 1000ULL, 7ULL, 142ULL, 6ULL},
        {1ULL, 0ULL, 2ULL, 1ULL << 63, 0ULL},
        {0ULL, std::numeric_limits<uint64_t>::max(), 10ULL, 1844674407370955161ULL, 5ULL},
        {1ULL, 1ULL, 3ULL, 6148914691236517205ULL, 2ULL},
        {7ULL, 0xFFFFFFFFFFFFFFFFULL, 9ULL, 16397105843297379214ULL, 1ULL},
    };
    for (const auto& v : div_vectors) {
        uint64_t rem = 0;
        const uint64_t q = utils::div_u128_by_u64(v.hi, v.lo, v.divisor, &rem);
        check(q == v.quotient, "portable div vector quotient");
        check(rem == v.remainder, "portable div vector remainder");
    }
}

void test_varint_canonical_roundtrip() {
    struct Vector {
        uint64_t value;
        size_t encoded_size;
    };
    const Vector vectors[] = {
        {0, 1},
        {0xFC, 1},
        {0xFD, 3},
        {0xFFFF, 3},
        {0x10000, 5},
        {0xFFFFFFFFULL, 5},
        {0x100000000ULL, 9},
        {std::numeric_limits<uint64_t>::max(), 9},
    };

    for (const auto& v : vectors) {
        const bytes encoded = utils::encode_varint(v.value);
        check(encoded.size() == v.encoded_size, "varint encoded size");
        size_t offset = 0;
        check(utils::decode_varint(encoded, offset) == v.value, "varint roundtrip value");
        check(offset == encoded.size(), "varint decode consumes exact size");
    }
}

void expect_varint_throw(const bytes& encoded, const std::string& message) {
    bool threw = false;
    try {
        size_t offset = 0;
        (void)utils::decode_varint(encoded, offset);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    check(threw, message);
}

void test_varint_rejects_truncated_and_noncanonical() {
    expect_varint_throw({}, "varint rejects empty buffer");
    expect_varint_throw({0xFD, 0x01}, "varint rejects truncated 0xFD payload");
    expect_varint_throw({0xFE, 0x01, 0x02, 0x03}, "varint rejects truncated 0xFE payload");
    expect_varint_throw({0xFF, 1, 2, 3, 4, 5, 6, 7}, "varint rejects truncated 0xFF payload");
    expect_varint_throw({0xFD, 0xFC, 0x00}, "varint rejects noncanonical 0xFD encoding");
    expect_varint_throw({0xFE, 0xFF, 0xFF, 0x00, 0x00}, "varint rejects noncanonical 0xFE encoding");
    expect_varint_throw({0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0},
                        "varint rejects noncanonical 0xFF encoding");
}

void test_uint256_comparison_and_arithmetic() {
    uint256 zero;
    check(zero.is_zero(), "uint256 default is zero");
    check(uint256(0).is_zero(), "uint256 zero constructor is zero");
    check(uint256(1) != uint256(2), "uint256 inequality");
    check(uint256(1) < uint256(2), "uint256 less-than");
    check(uint256(2) > uint256(1), "uint256 greater-than");
    check(uint256(2) >= uint256(2), "uint256 greater-equal");
    check(uint256(2) <= uint256(2), "uint256 less-equal");

    uint256 high;
    high.data[3] = 1;
    check(uint256(std::numeric_limits<uint64_t>::max()) < high,
          "uint256 comparison checks high limbs first");

    uint256 carry_a(std::numeric_limits<uint64_t>::max());
    uint256 carry_b(1);
    uint256 sum = carry_a + carry_b;
    check(sum.data[0] == 0 && sum.data[1] == 1, "uint256 addition propagates carry");

    uint256 borrow_a;
    borrow_a.data[1] = 1;
    uint256 diff = borrow_a - uint256(1);
    check(diff.data[0] == std::numeric_limits<uint64_t>::max() && diff.data[1] == 0,
          "uint256 subtraction propagates borrow");

    check((high * 0).is_zero(), "uint256 multiply by zero");
    check((high * 1) == high, "uint256 multiply by one");
    uint256 small_product = uint256(0x100000000ULL) * 0x100000000ULL;
    check(small_product.data[0] == 0 && small_product.data[1] == 1,
          "uint256 single-limb multiply widens to next limb");
    uint256 multi;
    multi.data[0] = std::numeric_limits<uint64_t>::max();
    multi.data[1] = 1;
    uint256 multi_product = multi * 2;
    check(multi_product.data[0] == std::numeric_limits<uint64_t>::max() - 1 &&
          multi_product.data[1] == 3,
          "uint256 multi-limb multiply propagates carry");

    check((high / 0).is_zero(), "uint256 division by zero returns zero");
    uint256 dividend;
    dividend.data[1] = 1; // 2^64
    uint256 quotient = dividend / 2;
    check(quotient.data[0] == (1ULL << 63) && quotient.data[1] == 0,
          "uint256 division propagates remainder across limbs");
    check(uint256(42).get_double() >= 42.0 && uint256(42).get_double() < 43.0,
          "uint256 get_double returns approximate value");
}

} // namespace

int main() {
    test_bytes_helpers();
    test_mul_u64_wide_basic();
    test_div_u128_by_u64_basic();
    test_mul_div_helpers_basic();
    test_foundation_cut_does_not_overflow();
    test_carry_borrow_and_signed_helpers();
#if defined(__SIZEOF_INT128__) && !(defined(_WIN32) && defined(__clang__))
    test_against_u128_reference();
#endif
    test_portable_reference_vectors();
    test_varint_canonical_roundtrip();
    test_varint_rejects_truncated_and_noncanonical();
    test_uint256_comparison_and_arithmetic();

    if (failures == 0) {
        std::cout << "All u64_math tests passed.\n";
        return 0;
    }

    std::cerr << failures << " u64_math test(s) failed.\n";
    return 1;
}
