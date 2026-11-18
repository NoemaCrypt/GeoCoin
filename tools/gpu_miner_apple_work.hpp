#pragma once

#include "gpu_miner_common.hpp"
#include <array>

namespace geocoin::tools::gpu_miner::apple {

// Prepare the same SHA-256d midstate and compact target for Metal and OpenCL.
inline geocoin::uint256 bits_to_target(uint32_t bits)
{
    const uint8_t exponent = static_cast<uint8_t>(bits >> 24);
    uint32_t mantissa = bits & 0x007fffff;
    const bool negative = (bits & 0x00800000) != 0;
    geocoin::uint256 target;
    target.data[0] = target.data[1] = target.data[2] = target.data[3] = 0;
    if (exponent <= 3) {
        mantissa >>= 8 * (3 - exponent);
        target.data[0] = mantissa;
    } else {
        const int shift = 8 * (exponent - 3);
        const int word_index = shift / 64;
        const int bit_shift = shift % 64;
        if (word_index < 4) target.data[word_index] |= static_cast<uint64_t>(mantissa) << bit_shift;
        if (bit_shift > 64 - 24 && word_index + 1 < 4) {
            target.data[word_index + 1] |= static_cast<uint64_t>(mantissa) >> (64 - bit_shift);
        }
    }
    if (negative) target.data[0] = target.data[1] = target.data[2] = target.data[3] = 0;
    return target;
}

inline void target_to_be32(const geocoin::uint256& target, unsigned char out[32])
{
    for (int word = 3; word >= 0; --word) {
        const uint64_t value = target.data[word];
        const int offset = (3 - word) * 8;
        for (int i = 0; i < 8; ++i) {
            out[offset + i] = static_cast<unsigned char>((value >> ((7 - i) * 8)) & 0xffu);
        }
    }
}

inline uint32_t host_rotr32(uint32_t x, uint32_t n)
{
    return (x >> n) | (x << (32 - n));
}

inline uint32_t host_load_be32(const unsigned char* p)
{
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

inline constexpr uint32_t kHostSha256[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

inline void host_sha256_transform(uint32_t state[8], const unsigned char block[64])
{
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = host_load_be32(block + i * 4);
    }
    for (int i = 16; i < 64; ++i) {
        const uint32_t s0 = host_rotr32(w[i - 15], 7) ^ host_rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = host_rotr32(w[i - 2], 17) ^ host_rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = state[0];
    uint32_t b = state[1];
    uint32_t c = state[2];
    uint32_t d = state[3];
    uint32_t e = state[4];
    uint32_t f = state[5];
    uint32_t g = state[6];
    uint32_t h = state[7];

    for (int i = 0; i < 64; ++i) {
        const uint32_t s1 = host_rotr32(e, 6) ^ host_rotr32(e, 11) ^ host_rotr32(e, 25);
        const uint32_t ch = (e & f) ^ ((~e) & g);
        const uint32_t temp1 = h + s1 + ch + kHostSha256[i] + w[i];
        const uint32_t s0 = host_rotr32(a, 2) ^ host_rotr32(a, 13) ^ host_rotr32(a, 22);
        const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t temp2 = s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

inline uint32_t host_sha_ch(uint32_t x, uint32_t y, uint32_t z)
{
    return z ^ (x & (y ^ z));
}

inline uint32_t host_sha_maj(uint32_t x, uint32_t y, uint32_t z)
{
    return (x & y) | (z & (x ^ y));
}

inline uint32_t host_sha_big0(uint32_t x)
{
    return host_rotr32(x, 2) ^ host_rotr32(x, 13) ^ host_rotr32(x, 22);
}

inline uint32_t host_sha_big1(uint32_t x)
{
    return host_rotr32(x, 6) ^ host_rotr32(x, 11) ^ host_rotr32(x, 25);
}

inline void host_sha256_step(uint32_t& a, uint32_t& b, uint32_t& c, uint32_t& d,
                             uint32_t& e, uint32_t& f, uint32_t& g, uint32_t& h,
                             uint32_t x, uint32_t k)
{
    h += k + x + host_sha_big1(e) + host_sha_ch(e, f, g);
    d += h;
    h += host_sha_big0(a) + host_sha_maj(a, b, c);
}

inline void host_precompute_tail_round13(const uint32_t midstate[8], const uint32_t tail_words[13], uint32_t out[8])
{
    uint32_t a = midstate[0];
    uint32_t b = midstate[1];
    uint32_t c = midstate[2];
    uint32_t d = midstate[3];
    uint32_t e = midstate[4];
    uint32_t f = midstate[5];
    uint32_t g = midstate[6];
    uint32_t h = midstate[7];

    host_sha256_step(a, b, c, d, e, f, g, h, tail_words[0], kHostSha256[0]);
    host_sha256_step(h, a, b, c, d, e, f, g, tail_words[1], kHostSha256[1]);
    host_sha256_step(g, h, a, b, c, d, e, f, tail_words[2], kHostSha256[2]);
    host_sha256_step(f, g, h, a, b, c, d, e, tail_words[3], kHostSha256[3]);
    host_sha256_step(e, f, g, h, a, b, c, d, tail_words[4], kHostSha256[4]);
    host_sha256_step(d, e, f, g, h, a, b, c, tail_words[5], kHostSha256[5]);
    host_sha256_step(c, d, e, f, g, h, a, b, tail_words[6], kHostSha256[6]);
    host_sha256_step(b, c, d, e, f, g, h, a, tail_words[7], kHostSha256[7]);
    host_sha256_step(a, b, c, d, e, f, g, h, tail_words[8], kHostSha256[8]);
    host_sha256_step(h, a, b, c, d, e, f, g, tail_words[9], kHostSha256[9]);
    host_sha256_step(g, h, a, b, c, d, e, f, tail_words[10], kHostSha256[10]);
    host_sha256_step(f, g, h, a, b, c, d, e, tail_words[11], kHostSha256[11]);
    host_sha256_step(e, f, g, h, a, b, c, d, tail_words[12], kHostSha256[12]);

    out[0] = a;
    out[1] = b;
    out[2] = c;
    out[3] = d;
    out[4] = e;
    out[5] = f;
    out[6] = g;
    out[7] = h;
}

// Builds the 37-word work array: [0..7] header midstate, [8..20] tail words,
// [21..28] midstate after round 13 of the tail block, [29..36] big-endian target.
inline std::array<uint32_t, 37> make_work(const std::vector<byte>& block_bytes, uint32_t bits)
{
    std::array<uint32_t, 37> work{};
    uint32_t state[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
    };
    host_sha256_transform(state, block_bytes.data());
    for (int i = 0; i < 8; ++i) {
        work[static_cast<size_t>(i)] = state[i];
    }
    for (int i = 0; i < 13; ++i) {
        work[8u + static_cast<size_t>(i)] = host_load_be32(block_bytes.data() + 64 + i * 4);
    }
    host_precompute_tail_round13(work.data(), work.data() + 8u, work.data() + 21u);

    unsigned char target_be[32]{};
    target_to_be32(bits_to_target(bits), target_be);
    for (int i = 0; i < 8; ++i) {
        work[29u + static_cast<size_t>(i)] = host_load_be32(target_be + i * 4);
    }
    return work;
}

} // namespace geocoin::tools::gpu_miner::apple
