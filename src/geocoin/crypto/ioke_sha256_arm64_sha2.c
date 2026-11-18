/* ARMv8 Crypto Extensions SHA-256 compression (Apple silicon baseline;
 * runtime gate: sysctlbyname("hw.optional.arm.FEAT_SHA256")). Extracted
 * from the ioke_sha256.c inline static (plan B1); the compat TU selects it
 * at runtime exactly like the x86 SHA-NI translation unit. */
#include "geocoin/crypto/ioke_sha256.h"

#include <stdint.h>
#include <string.h>

#if defined(GEOCOIN_MACOS_ARM64)
#include <arm_neon.h>

static const uint32_t SHA256_K_ARM64[64] = {
    0x428A2F98U, 0x71374491U, 0xB5C0FBCFU, 0xE9B5DBA5U, 0x3956C25BU, 0x59F111F1U, 0x923F82A4U, 0xAB1C5ED5U,
    0xD807AA98U, 0x12835B01U, 0x243185BEU, 0x550C7DC3U, 0x72BE5D74U, 0x80DEB1FEU, 0x9BDC06A7U, 0xC19BF174U,
    0xE49B69C1U, 0xEFBE4786U, 0x0FC19DC6U, 0x240CA1CCU, 0x2DE92C6FU, 0x4A7484AAU, 0x5CB0A9DCU, 0x76F988DAU,
    0x983E5152U, 0xA831C66DU, 0xB00327C8U, 0xBF597FC7U, 0xC6E00BF3U, 0xD5A79147U, 0x06CA6351U, 0x14292967U,
    0x27B70A85U, 0x2E1B2138U, 0x4D2C6DFCU, 0x53380D13U, 0x650A7354U, 0x766A0ABBU, 0x81C2C92EU, 0x92722C85U,
    0xA2BFE8A1U, 0xA81A664BU, 0xC24B8B70U, 0xC76C51A3U, 0xD192E819U, 0xD6990624U, 0xF40E3585U, 0x106AA070U,
    0x19A4C116U, 0x1E376C08U, 0x2748774CU, 0x34B0BCB5U, 0x391C0CB3U, 0x4ED8AA4AU, 0x5B9CCA4FU, 0x682E6FF3U,
    0x748F82EEU, 0x78A5636FU, 0x84C87814U, 0x8CC70208U, 0x90BEFFFAU, 0xA4506CEBU, 0xBEF9A3F7U, 0xC67178F2U
};
void ioke_sha256_compress_arm64_sha2(uint32_t state[8], const uint8_t* data, size_t blocks) {
    uint32x4_t STATE0, STATE1, ABEF_SAVE, CDGH_SAVE;
    uint32x4_t MSG0, MSG1, MSG2, MSG3;
    uint32x4_t TMP0, TMP1, TMP2;

    STATE0 = vld1q_u32(&state[0]);
    STATE1 = vld1q_u32(&state[4]);

    while (blocks--) {
        ABEF_SAVE = STATE0;
        CDGH_SAVE = STATE1;

        MSG0 = vld1q_u32((const uint32_t*)(data + 0));
        MSG1 = vld1q_u32((const uint32_t*)(data + 16));
        MSG2 = vld1q_u32((const uint32_t*)(data + 32));
        MSG3 = vld1q_u32((const uint32_t*)(data + 48));

        MSG0 = vreinterpretq_u32_u8(vrev32q_u8(vreinterpretq_u8_u32(MSG0)));
        MSG1 = vreinterpretq_u32_u8(vrev32q_u8(vreinterpretq_u8_u32(MSG1)));
        MSG2 = vreinterpretq_u32_u8(vrev32q_u8(vreinterpretq_u8_u32(MSG2)));
        MSG3 = vreinterpretq_u32_u8(vrev32q_u8(vreinterpretq_u8_u32(MSG3)));

        TMP0 = vaddq_u32(MSG0, vld1q_u32(&SHA256_K_ARM64[0x00]));

        MSG0 = vsha256su0q_u32(MSG0, MSG1);
        TMP2 = STATE0;
        TMP1 = vaddq_u32(MSG1, vld1q_u32(&SHA256_K_ARM64[0x04]));
        STATE0 = vsha256hq_u32(STATE0, STATE1, TMP0);
        STATE1 = vsha256h2q_u32(STATE1, TMP2, TMP0);
        MSG0 = vsha256su1q_u32(MSG0, MSG2, MSG3);

        MSG1 = vsha256su0q_u32(MSG1, MSG2);
        TMP2 = STATE0;
        TMP0 = vaddq_u32(MSG2, vld1q_u32(&SHA256_K_ARM64[0x08]));
        STATE0 = vsha256hq_u32(STATE0, STATE1, TMP1);
        STATE1 = vsha256h2q_u32(STATE1, TMP2, TMP1);
        MSG1 = vsha256su1q_u32(MSG1, MSG3, MSG0);

        MSG2 = vsha256su0q_u32(MSG2, MSG3);
        TMP2 = STATE0;
        TMP1 = vaddq_u32(MSG3, vld1q_u32(&SHA256_K_ARM64[0x0c]));
        STATE0 = vsha256hq_u32(STATE0, STATE1, TMP0);
        STATE1 = vsha256h2q_u32(STATE1, TMP2, TMP0);
        MSG2 = vsha256su1q_u32(MSG2, MSG0, MSG1);

        MSG3 = vsha256su0q_u32(MSG3, MSG0);
        TMP2 = STATE0;
        TMP0 = vaddq_u32(MSG0, vld1q_u32(&SHA256_K_ARM64[0x10]));
        STATE0 = vsha256hq_u32(STATE0, STATE1, TMP1);
        STATE1 = vsha256h2q_u32(STATE1, TMP2, TMP1);
        MSG3 = vsha256su1q_u32(MSG3, MSG1, MSG2);

        MSG0 = vsha256su0q_u32(MSG0, MSG1);
        TMP2 = STATE0;
        TMP1 = vaddq_u32(MSG1, vld1q_u32(&SHA256_K_ARM64[0x14]));
        STATE0 = vsha256hq_u32(STATE0, STATE1, TMP0);
        STATE1 = vsha256h2q_u32(STATE1, TMP2, TMP0);
        MSG0 = vsha256su1q_u32(MSG0, MSG2, MSG3);

        MSG1 = vsha256su0q_u32(MSG1, MSG2);
        TMP2 = STATE0;
        TMP0 = vaddq_u32(MSG2, vld1q_u32(&SHA256_K_ARM64[0x18]));
        STATE0 = vsha256hq_u32(STATE0, STATE1, TMP1);
        STATE1 = vsha256h2q_u32(STATE1, TMP2, TMP1);
        MSG1 = vsha256su1q_u32(MSG1, MSG3, MSG0);

        MSG2 = vsha256su0q_u32(MSG2, MSG3);
        TMP2 = STATE0;
        TMP1 = vaddq_u32(MSG3, vld1q_u32(&SHA256_K_ARM64[0x1c]));
        STATE0 = vsha256hq_u32(STATE0, STATE1, TMP0);
        STATE1 = vsha256h2q_u32(STATE1, TMP2, TMP0);
        MSG2 = vsha256su1q_u32(MSG2, MSG0, MSG1);

        MSG3 = vsha256su0q_u32(MSG3, MSG0);
        TMP2 = STATE0;
        TMP0 = vaddq_u32(MSG0, vld1q_u32(&SHA256_K_ARM64[0x20]));
        STATE0 = vsha256hq_u32(STATE0, STATE1, TMP1);
        STATE1 = vsha256h2q_u32(STATE1, TMP2, TMP1);
        MSG3 = vsha256su1q_u32(MSG3, MSG1, MSG2);

        MSG0 = vsha256su0q_u32(MSG0, MSG1);
        TMP2 = STATE0;
        TMP1 = vaddq_u32(MSG1, vld1q_u32(&SHA256_K_ARM64[0x24]));
        STATE0 = vsha256hq_u32(STATE0, STATE1, TMP0);
        STATE1 = vsha256h2q_u32(STATE1, TMP2, TMP0);
        MSG0 = vsha256su1q_u32(MSG0, MSG2, MSG3);

        MSG1 = vsha256su0q_u32(MSG1, MSG2);
        TMP2 = STATE0;
        TMP0 = vaddq_u32(MSG2, vld1q_u32(&SHA256_K_ARM64[0x28]));
        STATE0 = vsha256hq_u32(STATE0, STATE1, TMP1);
        STATE1 = vsha256h2q_u32(STATE1, TMP2, TMP1);
        MSG1 = vsha256su1q_u32(MSG1, MSG3, MSG0);

        MSG2 = vsha256su0q_u32(MSG2, MSG3);
        TMP2 = STATE0;
        TMP1 = vaddq_u32(MSG3, vld1q_u32(&SHA256_K_ARM64[0x2c]));
        STATE0 = vsha256hq_u32(STATE0, STATE1, TMP0);
        STATE1 = vsha256h2q_u32(STATE1, TMP2, TMP0);
        MSG2 = vsha256su1q_u32(MSG2, MSG0, MSG1);

        MSG3 = vsha256su0q_u32(MSG3, MSG0);
        TMP2 = STATE0;
        TMP0 = vaddq_u32(MSG0, vld1q_u32(&SHA256_K_ARM64[0x30]));
        STATE0 = vsha256hq_u32(STATE0, STATE1, TMP1);
        STATE1 = vsha256h2q_u32(STATE1, TMP2, TMP1);
        MSG3 = vsha256su1q_u32(MSG3, MSG1, MSG2);

        TMP2 = STATE0;
        TMP1 = vaddq_u32(MSG1, vld1q_u32(&SHA256_K_ARM64[0x34]));
        STATE0 = vsha256hq_u32(STATE0, STATE1, TMP0);
        STATE1 = vsha256h2q_u32(STATE1, TMP2, TMP0);

        TMP2 = STATE0;
        TMP0 = vaddq_u32(MSG2, vld1q_u32(&SHA256_K_ARM64[0x38]));
        STATE0 = vsha256hq_u32(STATE0, STATE1, TMP1);
        STATE1 = vsha256h2q_u32(STATE1, TMP2, TMP1);

        TMP2 = STATE0;
        TMP1 = vaddq_u32(MSG3, vld1q_u32(&SHA256_K_ARM64[0x3c]));
        STATE0 = vsha256hq_u32(STATE0, STATE1, TMP0);
        STATE1 = vsha256h2q_u32(STATE1, TMP2, TMP0);

        TMP2 = STATE0;
        STATE0 = vsha256hq_u32(STATE0, STATE1, TMP1);
        STATE1 = vsha256h2q_u32(STATE1, TMP2, TMP1);

        STATE0 = vaddq_u32(STATE0, ABEF_SAVE);
        STATE1 = vaddq_u32(STATE1, CDGH_SAVE);

        data += 64;
    }

    vst1q_u32(&state[0], STATE0);
    vst1q_u32(&state[4], STATE1);
}

typedef struct {
    uint32x4_t first;
    uint32x4_t second;
} Sha256State;

static inline void sha256_round4(Sha256State* state, uint32x4_t message) {
    const uint32x4_t first = state->first;
    state->first = vsha256hq_u32(first, state->second, message);
    state->second = vsha256h2q_u32(state->second, first, message);
}

static inline void sha256_add_state(Sha256State* state, Sha256State saved) {
    state->first = vaddq_u32(state->first, saved.first);
    state->second = vaddq_u32(state->second, saved.second);
}

static inline uint32x4_t sha256_schedule4(const uint32x4_t words[4], unsigned round) {
    return vsha256su1q_u32(vsha256su0q_u32(words[round & 3], words[(round + 1) & 3]),
                          words[(round + 2) & 3], words[(round + 3) & 3]);
}

void ioke_sha256d124_arm64_prepare(IokeSha256d124Arm64* prepared, const uint8_t header[124]) {
    ioke_sha256_init_state(prepared->midstate);
    ioke_sha256_compress_arm64_sha2(prepared->midstate, header, 1);

    /* Bytes 64–123 of the header and the first padding bit. The 64-bit nonce
     * starts at byte 116 (the second and third words of the last vector). */
    uint8_t tail[64] = {0};
    memcpy(tail, header + 64, 60);
    tail[60] = 0x80;
    for (unsigned word = 0; word < 4; ++word) {
        vst1q_u32(prepared->tail + word * 4,
                  vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(tail + word * 16))));
    }

    /* The first twelve rounds cannot depend on the nonce at word 13. */
    Sha256State prefix = {vld1q_u32(prepared->midstate), vld1q_u32(prepared->midstate + 4)};
    for (unsigned round = 0; round < 3; ++round) {
        sha256_round4(&prefix, vaddq_u32(vld1q_u32(prepared->tail + round * 4),
                                       vld1q_u32(SHA256_K_ARM64 + round * 4)));
    }
    vst1q_u32(prepared->prefix_state, prefix.first);
    vst1q_u32(prepared->prefix_state + 4, prefix.second);

    /* The third compression block contains only zeros and 124 * 8 bits. */
    const uint32x4_t zero = vdupq_n_u32(0);
    uint32x4_t words[4] = {zero, zero, zero, vsetq_lane_u32(124 * 8, zero, 3)};
    for (unsigned round = 0; round < 16; ++round) {
        vst1q_u32(prepared->length_rounds + round * 4,
                  vaddq_u32(words[round & 3], vld1q_u32(SHA256_K_ARM64 + round * 4)));
        if (round < 12) {
            words[round & 3] = sha256_schedule4(words, round);
        }
    }
}

static inline __attribute__((always_inline)) void sha256d124_lanes(
    const IokeSha256d124Arm64* prepared, const uint64_t* nonces, uint8_t* const* output, unsigned lanes) {
    const Sha256State midstate = {vld1q_u32(prepared->midstate), vld1q_u32(prepared->midstate + 4)};
    const Sha256State prefix = {vld1q_u32(prepared->prefix_state), vld1q_u32(prepared->prefix_state + 4)};
    Sha256State state[4];
    uint32x4_t words[4][4];
    const uint32x4_t tail = vld1q_u32(prepared->tail + 12);

#pragma clang loop unroll(full)
    for (unsigned lane = 0; lane < lanes; ++lane) {
        state[lane] = prefix;
        words[lane][0] = vld1q_u32(prepared->tail);
        words[lane][1] = vld1q_u32(prepared->tail + 4);
        words[lane][2] = vld1q_u32(prepared->tail + 8);
        words[lane][3] = vsetq_lane_u32(__builtin_bswap32((uint32_t)nonces[lane]), tail, 1);
        words[lane][3] = vsetq_lane_u32(__builtin_bswap32((uint32_t)(nonces[lane] >> 32)), words[lane][3], 2);
    }

    /* Keep independent SHA256H/SHA256H2 chains in flight together. */
#pragma clang loop unroll(full)
    for (unsigned round = 0; round < 16; ++round) {
        const uint32x4_t constant = vld1q_u32(SHA256_K_ARM64 + round * 4);
#pragma clang loop unroll(full)
        for (unsigned lane = 0; lane < lanes; ++lane) {
            if (round >= 3) {
                sha256_round4(&state[lane], vaddq_u32(words[lane][round & 3], constant));
            }
            if (round < 12) {
                words[lane][round & 3] = sha256_schedule4(words[lane], round);
            }
        }
    }

#pragma clang loop unroll(full)
    for (unsigned lane = 0; lane < lanes; ++lane) {
        sha256_add_state(&state[lane], midstate);
    }
    Sha256State saved[4];
    memcpy(saved, state, lanes * sizeof(*state));
#pragma clang loop unroll(full)
    for (unsigned round = 0; round < 16; ++round) {
        const uint32x4_t next = vld1q_u32(prepared->length_rounds + round * 4);
#pragma clang loop unroll(full)
        for (unsigned lane = 0; lane < lanes; ++lane) {
            sha256_round4(&state[lane], next);
        }
    }

    uint32_t initial_words[8];
    ioke_sha256_init_state(initial_words);
    const Sha256State initial = {vld1q_u32(initial_words), vld1q_u32(initial_words + 4)};
    const uint32x4_t zero = vdupq_n_u32(0);
#pragma clang loop unroll(full)
    for (unsigned lane = 0; lane < lanes; ++lane) {
        sha256_add_state(&state[lane], saved[lane]);
        /* The first digest is already in the SHA-256 message's word order. */
        words[lane][0] = state[lane].first;
        words[lane][1] = state[lane].second;
        words[lane][2] = vsetq_lane_u32(0x80000000, zero, 0);
        words[lane][3] = vsetq_lane_u32(32 * 8, zero, 3);
        state[lane] = initial;
    }
#pragma clang loop unroll(full)
    for (unsigned round = 0; round < 16; ++round) {
        const uint32x4_t constant = vld1q_u32(SHA256_K_ARM64 + round * 4);
#pragma clang loop unroll(full)
        for (unsigned lane = 0; lane < lanes; ++lane) {
            sha256_round4(&state[lane], vaddq_u32(words[lane][round & 3], constant));
            if (round < 12) {
                words[lane][round & 3] = sha256_schedule4(words[lane], round);
            }
        }
    }
#pragma clang loop unroll(full)
    for (unsigned lane = 0; lane < lanes; ++lane) {
        sha256_add_state(&state[lane], initial);
        vst1q_u8(output[lane], vrev32q_u8(vreinterpretq_u8_u32(state[lane].first)));
        vst1q_u8(output[lane] + 16, vrev32q_u8(vreinterpretq_u8_u32(state[lane].second)));
    }
}

void ioke_sha256d124_arm64_hash2(const IokeSha256d124Arm64* prepared, uint64_t nonce_a, uint64_t nonce_b,
                                uint8_t out_a[32], uint8_t out_b[32]) {
    const uint64_t nonces[2] = {nonce_a, nonce_b};
    uint8_t* const output[2] = {out_a, out_b};
    sha256d124_lanes(prepared, nonces, output, 2);
}

void ioke_sha256d124_arm64_hash4(const IokeSha256d124Arm64* prepared, const uint64_t nonces[4],
                                uint8_t out[4][32]) {
    uint8_t* const output[4] = {out[0], out[1], out[2], out[3]};
    sha256d124_lanes(prepared, nonces, output, 4);
}
#endif
