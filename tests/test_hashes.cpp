#include "geocoin/crypto/geo_area.hpp"
#include "geocoin/crypto/ioke_ripemd160.h"
#include "geocoin/crypto/ioke_sha256.h"
#include "geocoin/utils/bytes.hpp"
#include <iostream>
#include <cstring>

using namespace geocoin;

namespace {
int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

void test_sha256_vectors() {
    {
        bytes data;
        Hash256 h = crypto::sha256(data);
        const std::string hex = utils::bytes_to_hex(h.data(), h.size());
        check(hex == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
              "SHA256(\"\")");
    }
    {
        const std::string s = "abc";
        bytes data(s.begin(), s.end());
        Hash256 h = crypto::sha256(data);
        const std::string hex = utils::bytes_to_hex(h.data(), h.size());
        check(hex == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
              "SHA256(\"abc\")");
    }
}

void test_ripemd160_vectors() {
    {
        bytes data;
        Hash160 h = crypto::ripemd160(data);
        const std::string hex = utils::bytes_to_hex(h.data(), h.size());
        check(hex == "9c1185a5c5e9fc54612808977ee8f548b2258d31",
              "RIPEMD160(\"\")");
    }
    {
        const std::string s = "abc";
        bytes data(s.begin(), s.end());
        Hash160 h = crypto::ripemd160(data);
        const std::string hex = utils::bytes_to_hex(h.data(), h.size());
        check(hex == "8eb208f7e05d987a9b044a8e98c6b087f15a0bfc",
              "RIPEMD160(\"abc\")");
    }
}

void test_ioke_sha256_c_api() {
    unsigned char out[IOKE_SHA256_DIGEST_LENGTH]{};
    unsigned char out_split[IOKE_SHA256_DIGEST_LENGTH]{};
    const std::string abc = "abc";

    check(IOKE_SHA256_Init(nullptr) == 0, "SHA256 Init rejects null ctx");
    IOKE_SHA256_CTX ctx{};
    check(IOKE_SHA256_Init(&ctx) == 1, "SHA256 Init accepts ctx");
    check(IOKE_SHA256_Update(nullptr, abc.data(), abc.size()) == 0, "SHA256 Update rejects null ctx");
    check(IOKE_SHA256_Update(&ctx, nullptr, 0) == 1, "SHA256 Update accepts null data with zero length");
    check(IOKE_SHA256_Update(&ctx, nullptr, 1) == 0, "SHA256 Update rejects null data with nonzero length");
    check(IOKE_SHA256_Final(nullptr, &ctx) == 0, "SHA256 Final rejects null digest");
    check(IOKE_SHA256_Final(out, nullptr) == 0, "SHA256 Final rejects null ctx");

    check(IOKE_SHA256(reinterpret_cast<const unsigned char*>(abc.data()), abc.size(), out) == out,
          "SHA256 one-shot returns output pointer");
    check(utils::bytes_to_hex(out, sizeof(out)) ==
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "SHA256 C one-shot abc vector");
    check(IOKE_SHA256(nullptr, 1, out) == nullptr, "SHA256 one-shot rejects null input with nonzero length");
    check(IOKE_SHA256(reinterpret_cast<const unsigned char*>(abc.data()), abc.size(), nullptr) == nullptr,
          "SHA256 one-shot rejects null output");

    bytes long_data(200);
    for (size_t i = 0; i < long_data.size(); ++i) {
        long_data[i] = static_cast<byte>(i * 3 + 1);
    }
    const Hash256 expected_long = crypto::sha256(long_data);

    check(IOKE_SHA256_Init(&ctx) == 1, "SHA256 split init succeeds");
    check(IOKE_SHA256_Update(&ctx, long_data.data(), 17) == 1, "SHA256 split update buffers partial block");
    check(IOKE_SHA256_Update(&ctx, long_data.data() + 17, long_data.size() - 17) == 1,
          "SHA256 split update processes full blocks");
    check(IOKE_SHA256_Final(out_split, &ctx) == 1, "SHA256 split final succeeds");
    check(bytes(out_split, out_split + IOKE_SHA256_DIGEST_LENGTH) ==
              bytes(expected_long.begin(), expected_long.end()),
          "SHA256 split C API matches crypto wrapper");

    uint8_t legacy_out[IOKE_SHA256_DIGEST_LENGTH]{0xff};
    sha256(nullptr, 1, legacy_out);
    check(bytes(legacy_out, legacy_out + IOKE_SHA256_DIGEST_LENGTH) == bytes(IOKE_SHA256_DIGEST_LENGTH, 0),
          "legacy sha256 wrapper zeroes output on invalid input");
    sha256(nullptr, 0, nullptr);

    check(ioke_sha256_backend_available(IOKE_SHA256_BACKEND_AUTO) == 1,
          "SHA256 auto backend is always available");
    check(ioke_sha256_backend_available(IOKE_SHA256_BACKEND_LEGACY) == 1,
          "SHA256 legacy backend is always available");
    check(ioke_sha256_backend_available(99) == 0,
          "SHA256 invalid backend is unavailable");
    ioke_sha256_set_backend(IOKE_SHA256_BACKEND_LEGACY);
    check(std::strcmp(ioke_sha256_backend_name(), "legacy") == 0, "SHA256 legacy backend name");
    ioke_sha256_set_backend(IOKE_SHA256_BACKEND_NATIVE);
    check(std::string(ioke_sha256_backend_name()).find("native") != std::string::npos ||
              std::strcmp(ioke_sha256_backend_name(), "legacy") == 0,
          "SHA256 native backend name falls back when unavailable");
    ioke_sha256_set_backend(99);
    check(std::string(ioke_sha256_backend_name()).starts_with("auto-"),
          "SHA256 invalid backend request resets to auto");

    uint32_t state[8]{};
    ioke_sha256_init_state(state);
    ioke_sha256_write_hash(out, state);
    check(utils::bytes_to_hex(out, sizeof(out)) ==
              "6a09e667bb67ae853c6ef372a54ff53a510e527f9b05688c1f83d9ab5be0cd19",
          "SHA256 initial state writes IV");
    ioke_sha256_set_backend(IOKE_SHA256_BACKEND_AUTO);
}

void test_ioke_ripemd160_c_api() {
    const std::string abc = "abc";
    unsigned char out[IOKE_RIPEMD160_DIGEST_LENGTH]{};
    unsigned char split[IOKE_RIPEMD160_DIGEST_LENGTH]{};

    check(IOKE_RIPEMD160_Init(nullptr) == 0, "RIPEMD160 Init rejects null ctx");
    IOKE_RIPEMD160_CTX ctx{};
    check(IOKE_RIPEMD160_Init(&ctx) == 1, "RIPEMD160 Init accepts ctx");
    check(IOKE_RIPEMD160_Update(nullptr, abc.data(), abc.size()) == 0,
          "RIPEMD160 Update rejects null ctx");
    check(IOKE_RIPEMD160_Update(&ctx, nullptr, 0) == 1,
          "RIPEMD160 Update accepts null data with zero length");
    check(IOKE_RIPEMD160_Update(&ctx, nullptr, 1) == 0,
          "RIPEMD160 Update rejects null data with nonzero length");
    check(IOKE_RIPEMD160_Final(nullptr, &ctx) == 0, "RIPEMD160 Final rejects null digest");
    check(IOKE_RIPEMD160_Final(out, nullptr) == 0, "RIPEMD160 Final rejects null ctx");

    check(IOKE_RIPEMD160_Init(&ctx) == 1, "RIPEMD160 vector init succeeds");
    check(IOKE_RIPEMD160_Update(&ctx, abc.data(), abc.size()) == 1, "RIPEMD160 vector update succeeds");
    check(IOKE_RIPEMD160_Final(out, &ctx) == 1, "RIPEMD160 vector final succeeds");
    check(utils::bytes_to_hex(out, sizeof(out)) == "8eb208f7e05d987a9b044a8e98c6b087f15a0bfc",
          "RIPEMD160 C abc vector");

    bytes long_data(257);
    for (size_t i = 0; i < long_data.size(); ++i) {
        long_data[i] = static_cast<byte>(255 - (i % 251));
    }
    const Hash160 expected_long = crypto::ripemd160(long_data);
    check(IOKE_RIPEMD160_Init(&ctx) == 1, "RIPEMD160 long init succeeds");
    check(IOKE_RIPEMD160_Update(&ctx, long_data.data(), 3) == 1,
          "RIPEMD160 split update buffers partial data");
    check(IOKE_RIPEMD160_Update(&ctx, long_data.data() + 3, long_data.size() - 3) == 1,
          "RIPEMD160 split update processes long data");
    check(IOKE_RIPEMD160_Final(split, &ctx) == 1, "RIPEMD160 long final succeeds");
    check(bytes(split, split + IOKE_RIPEMD160_DIGEST_LENGTH) ==
              bytes(expected_long.begin(), expected_long.end()),
          "RIPEMD160 split C API matches crypto wrapper");
}

void test_byte_helpers() {
    check(utils::hex_digit_value('0') == 0, "hex digit decimal parses");
    check(utils::hex_digit_value('F') == 15, "hex digit uppercase parses");
    check(utils::hex_digit_value('z') == -1, "invalid hex digit is rejected");
    check(utils::is_hex_string("00AaFf"), "mixed-case hex string is accepted");
    check(!utils::is_hex_string("00gg"), "invalid hex string is rejected");

    bytes parsed;
    check(utils::hex_to_bytes_checked("00aBff", parsed), "valid mixed-case hex parses");
    check(parsed == bytes({0x00, 0xAB, 0xFF}), "hex bytes match expected data");
    check(utils::bytes_to_hex(parsed) == "00abff", "bytes_to_hex emits lowercase hex");
    check(utils::hex_to_bytes("00ff") == bytes({0x00, 0xFF}), "hex_to_bytes returns parsed bytes");
    check(utils::hex_to_bytes("0xz").empty(), "hex_to_bytes returns empty on invalid input");

    check(!utils::hex_to_bytes_checked("abc", parsed), "odd-length hex is rejected");
    check(!utils::hex_to_bytes_checked("00xz", parsed), "non-hex characters are rejected");

    Hash256 hash{};
    std::string hash_hex(64, 'a');
    check(utils::hex_to_hash256_checked(hash_hex, hash), "valid hash256 hex parses");
    check(utils::hash256_to_hex(hash) == hash_hex, "hash256_to_hex emits parsed hash");
    check(!utils::hex_to_hash256_checked("aa", hash), "short hash256 hex is rejected");
    check(!utils::hex_to_hash256_checked(std::string(64, 'x'), hash), "invalid hash256 hex is rejected");
    check(utils::is_null_hash256(Hash256{}), "zero hash is null");
    hash[0] = 1;
    check(!utils::is_null_hash256(hash), "non-zero hash is not null");

    const bytes le = utils::encode_u64_le(0x0102030405060708ull);
    uint64_t decoded = 0;
    check(utils::decode_u64_le_exact(le, decoded), "u64 little-endian decode succeeds");
    check(decoded == 0x0102030405060708ull, "u64 little-endian roundtrip");
    check(!utils::decode_u64_le_exact(bytes{1, 2, 3}, decoded), "short u64 little-endian rejects");
    check(utils::read_u64_le_unchecked(le, 0) == 0x0102030405060708ull,
          "unchecked u64 read returns expected value");

    bytes writable(8, 0);
    utils::write_u64_le(writable, 0, 0x8877665544332211ull);
    check(utils::read_u64_le_unchecked(writable, 0) == 0x8877665544332211ull,
          "u64 write/read little-endian roundtrip");

    const bytes height_le = utils::encode_height_le(123456);
    Height height = 0;
    check(utils::decode_height_le_exact(height_le, height) && height == 123456,
          "height little-endian roundtrip");
    check(!utils::decode_height_le_exact(bytes{1, 2}, height), "short height little-endian rejects");
}
} // namespace

int main() {
    test_sha256_vectors();
    test_ripemd160_vectors();
    test_ioke_sha256_c_api();
    test_ioke_ripemd160_c_api();
    test_byte_helpers();

    if (failures == 0) {
        std::cout << "All hash tests passed.\n";
        return 0;
    }

    std::cerr << failures << " test(s) failed.\n";
    return 1;
}
