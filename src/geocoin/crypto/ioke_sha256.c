#include "geocoin/crypto/ioke_sha256.h"

#include <limits.h>
#include <stdint.h>
#include <stdatomic.h>
#include <string.h>

#if defined(_MSC_VER) && \
    (defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86))
#include <intrin.h>
#elif (defined(__x86_64__) || defined(__i386__)) && \
      (defined(__GNUC__) || defined(__clang__))
#include <cpuid.h>
#endif


#if defined(__clang__) || defined(__GNUC__)
    #define IOKE_SHA256_MAYBE_UNUSED __attribute__((unused))
#else
    #define IOKE_SHA256_MAYBE_UNUSED
#endif

static const uint32_t SHA256_K[64] IOKE_SHA256_MAYBE_UNUSED = {
    0x428A2F98U, 0x71374491U, 0xB5C0FBCFU, 0xE9B5DBA5U, 0x3956C25BU, 0x59F111F1U, 0x923F82A4U, 0xAB1C5ED5U,
    0xD807AA98U, 0x12835B01U, 0x243185BEU, 0x550C7DC3U, 0x72BE5D74U, 0x80DEB1FEU, 0x9BDC06A7U, 0xC19BF174U,
    0xE49B69C1U, 0xEFBE4786U, 0x0FC19DC6U, 0x240CA1CCU, 0x2DE92C6FU, 0x4A7484AAU, 0x5CB0A9DCU, 0x76F988DAU,
    0x983E5152U, 0xA831C66DU, 0xB00327C8U, 0xBF597FC7U, 0xC6E00BF3U, 0xD5A79147U, 0x06CA6351U, 0x14292967U,
    0x27B70A85U, 0x2E1B2138U, 0x4D2C6DFCU, 0x53380D13U, 0x650A7354U, 0x766A0ABBU, 0x81C2C92EU, 0x92722C85U,
    0xA2BFE8A1U, 0xA81A664BU, 0xC24B8B70U, 0xC76C51A3U, 0xD192E819U, 0xD6990624U, 0xF40E3585U, 0x106AA070U,
    0x19A4C116U, 0x1E376C08U, 0x2748774CU, 0x34B0BCB5U, 0x391C0CB3U, 0x4ED8AA4AU, 0x5B9CCA4FU, 0x682E6FF3U,
    0x748F82EEU, 0x78A5636FU, 0x84C87814U, 0x8CC70208U, 0x90BEFFFAU, 0xA4506CEBU, 0xBEF9A3F7U, 0xC67178F2U
};

static const uint32_t SHA256_IV[8] = {
    0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
    0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U
};

static int ioke_sha256_backend_override = IOKE_SHA256_BACKEND_AUTO;
static int ioke_sha256_native_available_cached = -1;

/* =============================================================================
 * Architecture availability
 * ============================================================================= */

#if (defined(__aarch64__) || defined(__arm__) || defined(_M_ARM64) || defined(_M_ARM)) && \
    defined(__ARM_NEON) && defined(__ARM_FEATURE_CRYPTO)
    #define IOKE_SHA256_USE_ARM_CRYPTO 1
#endif

#if defined(IOKE_SHA256_HAVE_X86_SHA_NI)
void ioke_sha256_compress_x86_sha_ni(uint32_t state[8], const uint8_t* data, size_t blocks);

static int ioke_sha256_can_use_x86_sha_ni(void) {
#if defined(_MSC_VER) && \
    (defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86))
    int regs[4] = {0, 0, 0, 0};
    __cpuidex(regs, 0, 0);
    const int max_leaf = regs[0];
    if (max_leaf < 7) {
        return 0;
    }

    __cpuidex(regs, 1, 0);
    const int has_ssse3 = (regs[2] & (1 << 9)) != 0;
    const int has_sse41 = (regs[2] & (1 << 19)) != 0;

    __cpuidex(regs, 7, 0);
    const int has_sha = (regs[1] & (1 << 29)) != 0;
    return has_sha && has_ssse3 && has_sse41;
#elif (defined(__x86_64__) || defined(__i386__)) && \
      (defined(__GNUC__) || defined(__clang__))
    // Clang versions shipped with supported Linux distributions do not all
    // accept "sha" in __builtin_cpu_supports. Query the same CPUID bits used
    // by the Windows path without requiring a newer compiler.
    unsigned int eax, ebx, ecx, edx;
    if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx)) {
        return 0;
    }
    const int has_ssse3 = (ecx & (1u << 9)) != 0;
    const int has_sse41 = (ecx & (1u << 19)) != 0;
    if (!__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx)) {
        return 0;
    }
    return (ebx & (1u << 29)) != 0 && has_ssse3 && has_sse41;
#else
    return 0;
#endif
}
#else
static int ioke_sha256_can_use_x86_sha_ni(void) {
    return 0;
}
#endif

static int ioke_sha256_can_use_native(void) {
    if (ioke_sha256_native_available_cached >= 0) {
        return ioke_sha256_native_available_cached;
    }

    int available = 0;
#if defined(IOKE_SHA256_HAVE_X86_SHA_NI)
    if (ioke_sha256_can_use_x86_sha_ni()) {
        available = 1;
    }
#endif
#if defined(IOKE_SHA256_USE_ARM_CRYPTO)
    available = 1;
#endif
    ioke_sha256_native_available_cached = available;
    return available;
}

static uint32_t IOKE_SHA256_MAYBE_UNUSED ioke_load_be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) |
           ((uint32_t)p[3]);
}

static void ioke_store_be32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)(v);
}

static void ioke_store_be64(uint8_t* p, uint64_t v) {
    p[0] = (uint8_t)(v >> 56);
    p[1] = (uint8_t)(v >> 48);
    p[2] = (uint8_t)(v >> 40);
    p[3] = (uint8_t)(v >> 32);
    p[4] = (uint8_t)(v >> 24);
    p[5] = (uint8_t)(v >> 16);
    p[6] = (uint8_t)(v >> 8);
    p[7] = (uint8_t)(v);
}

/* =============================================================================
 * Implementation selection
 * ============================================================================= */

#if defined(IOKE_SHA256_FORCE_LEGACY)
    #define IOKE_SHA256_FORCE_LEGACY_IMPL 1
#elif defined(IOKE_SHA256_FORCE_X86_SHA_NI)
    #define IOKE_SHA256_FORCE_X86_IMPL 1
#elif defined(IOKE_SHA256_FORCE_ARM_CRYPTO)
    #define IOKE_SHA256_FORCE_ARM_IMPL 1
#endif

/* =============================================================================
 * ARMv8 SHA2 intrinsics implementation
 * ============================================================================= */

#if defined(IOKE_SHA256_USE_ARM_CRYPTO)
#include <arm_neon.h>

static void ioke_sha256_compress_arm(uint32_t state[8], const uint8_t* data, size_t blocks) {
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

        TMP0 = vaddq_u32(MSG0, vld1q_u32(&SHA256_K[0x00]));

        MSG0 = vsha256su0q_u32(MSG0, MSG1);
        TMP2 = STATE0;
        TMP1 = vaddq_u32(MSG1, vld1q_u32(&SHA256_K[0x04]));
        STATE0 = vsha256hq_u32(STATE0, STATE1, TMP0);
        STATE1 = vsha256h2q_u32(STATE1, TMP2, TMP0);
        MSG0 = vsha256su1q_u32(MSG0, MSG2, MSG3);

        MSG1 = vsha256su0q_u32(MSG1, MSG2);
        TMP2 = STATE0;
        TMP0 = vaddq_u32(MSG2, vld1q_u32(&SHA256_K[0x08]));
        STATE0 = vsha256hq_u32(STATE0, STATE1, TMP1);
        STATE1 = vsha256h2q_u32(STATE1, TMP2, TMP1);
        MSG1 = vsha256su1q_u32(MSG1, MSG3, MSG0);

        MSG2 = vsha256su0q_u32(MSG2, MSG3);
        TMP2 = STATE0;
        TMP1 = vaddq_u32(MSG3, vld1q_u32(&SHA256_K[0x0c]));
        STATE0 = vsha256hq_u32(STATE0, STATE1, TMP0);
        STATE1 = vsha256h2q_u32(STATE1, TMP2, TMP0);
        MSG2 = vsha256su1q_u32(MSG2, MSG0, MSG1);

        MSG3 = vsha256su0q_u32(MSG3, MSG0);
        TMP2 = STATE0;
        TMP0 = vaddq_u32(MSG0, vld1q_u32(&SHA256_K[0x10]));
        STATE0 = vsha256hq_u32(STATE0, STATE1, TMP1);
        STATE1 = vsha256h2q_u32(STATE1, TMP2, TMP1);
        MSG3 = vsha256su1q_u32(MSG3, MSG1, MSG2);

        MSG0 = vsha256su0q_u32(MSG0, MSG1);
        TMP2 = STATE0;
        TMP1 = vaddq_u32(MSG1, vld1q_u32(&SHA256_K[0x14]));
        STATE0 = vsha256hq_u32(STATE0, STATE1, TMP0);
        STATE1 = vsha256h2q_u32(STATE1, TMP2, TMP0);
        MSG0 = vsha256su1q_u32(MSG0, MSG2, MSG3);

        MSG1 = vsha256su0q_u32(MSG1, MSG2);
        TMP2 = STATE0;
        TMP0 = vaddq_u32(MSG2, vld1q_u32(&SHA256_K[0x18]));
        STATE0 = vsha256hq_u32(STATE0, STATE1, TMP1);
        STATE1 = vsha256h2q_u32(STATE1, TMP2, TMP1);
        MSG1 = vsha256su1q_u32(MSG1, MSG3, MSG0);

        MSG2 = vsha256su0q_u32(MSG2, MSG3);
        TMP2 = STATE0;
        TMP1 = vaddq_u32(MSG3, vld1q_u32(&SHA256_K[0x1c]));
        STATE0 = vsha256hq_u32(STATE0, STATE1, TMP0);
        STATE1 = vsha256h2q_u32(STATE1, TMP2, TMP0);
        MSG2 = vsha256su1q_u32(MSG2, MSG0, MSG1);

        MSG3 = vsha256su0q_u32(MSG3, MSG0);
        TMP2 = STATE0;
        TMP0 = vaddq_u32(MSG0, vld1q_u32(&SHA256_K[0x20]));
        STATE0 = vsha256hq_u32(STATE0, STATE1, TMP1);
        STATE1 = vsha256h2q_u32(STATE1, TMP2, TMP1);
        MSG3 = vsha256su1q_u32(MSG3, MSG1, MSG2);

        MSG0 = vsha256su0q_u32(MSG0, MSG1);
        TMP2 = STATE0;
        TMP1 = vaddq_u32(MSG1, vld1q_u32(&SHA256_K[0x24]));
        STATE0 = vsha256hq_u32(STATE0, STATE1, TMP0);
        STATE1 = vsha256h2q_u32(STATE1, TMP2, TMP0);
        MSG0 = vsha256su1q_u32(MSG0, MSG2, MSG3);

        MSG1 = vsha256su0q_u32(MSG1, MSG2);
        TMP2 = STATE0;
        TMP0 = vaddq_u32(MSG2, vld1q_u32(&SHA256_K[0x28]));
        STATE0 = vsha256hq_u32(STATE0, STATE1, TMP1);
        STATE1 = vsha256h2q_u32(STATE1, TMP2, TMP1);
        MSG1 = vsha256su1q_u32(MSG1, MSG3, MSG0);

        MSG2 = vsha256su0q_u32(MSG2, MSG3);
        TMP2 = STATE0;
        TMP1 = vaddq_u32(MSG3, vld1q_u32(&SHA256_K[0x2c]));
        STATE0 = vsha256hq_u32(STATE0, STATE1, TMP0);
        STATE1 = vsha256h2q_u32(STATE1, TMP2, TMP0);
        MSG2 = vsha256su1q_u32(MSG2, MSG0, MSG1);

        MSG3 = vsha256su0q_u32(MSG3, MSG0);
        TMP2 = STATE0;
        TMP0 = vaddq_u32(MSG0, vld1q_u32(&SHA256_K[0x30]));
        STATE0 = vsha256hq_u32(STATE0, STATE1, TMP1);
        STATE1 = vsha256h2q_u32(STATE1, TMP2, TMP1);
        MSG3 = vsha256su1q_u32(MSG3, MSG1, MSG2);

        TMP2 = STATE0;
        TMP1 = vaddq_u32(MSG1, vld1q_u32(&SHA256_K[0x34]));
        STATE0 = vsha256hq_u32(STATE0, STATE1, TMP0);
        STATE1 = vsha256h2q_u32(STATE1, TMP2, TMP0);

        TMP2 = STATE0;
        TMP0 = vaddq_u32(MSG2, vld1q_u32(&SHA256_K[0x38]));
        STATE0 = vsha256hq_u32(STATE0, STATE1, TMP1);
        STATE1 = vsha256h2q_u32(STATE1, TMP2, TMP1);

        TMP2 = STATE0;
        TMP1 = vaddq_u32(MSG3, vld1q_u32(&SHA256_K[0x3c]));
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
#endif

/* =============================================================================
 * Legacy portable implementation
 * ============================================================================= */

static uint32_t ioke_rotr32(uint32_t x, uint32_t n) {
    return (x >> n) | (x << (32U - n));
}

static void ioke_sha256_compress_legacy(uint32_t state[8], const uint8_t* data, size_t blocks) {
    while (blocks--) {
        uint32_t w[64];
        uint32_t a = state[0];
        uint32_t b = state[1];
        uint32_t c = state[2];
        uint32_t d = state[3];
        uint32_t e = state[4];
        uint32_t f = state[5];
        uint32_t g = state[6];
        uint32_t h = state[7];

        for (uint32_t i = 0; i < 16; ++i) {
            w[i] = ioke_load_be32(data + (i * 4U));
        }

        for (uint32_t i = 16; i < 64; ++i) {
            const uint32_t s0 = ioke_rotr32(w[i - 15], 7) ^ ioke_rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = ioke_rotr32(w[i - 2], 17) ^ ioke_rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        for (uint32_t i = 0; i < 64; ++i) {
            const uint32_t S1 = ioke_rotr32(e, 6) ^ ioke_rotr32(e, 11) ^ ioke_rotr32(e, 25);
            const uint32_t ch = (e & f) ^ ((~e) & g);
            const uint32_t temp1 = h + S1 + ch + SHA256_K[i] + w[i];
            const uint32_t S0 = ioke_rotr32(a, 2) ^ ioke_rotr32(a, 13) ^ ioke_rotr32(a, 22);
            const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t temp2 = S0 + maj;

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

        data += 64;
    }
}

static void ioke_sha256_compress_blocks(uint32_t state[8], const uint8_t* data, size_t blocks) {
#if defined(IOKE_SHA256_FORCE_X86_IMPL)
    ioke_sha256_compress_x86_sha_ni(state, data, blocks);
#elif defined(IOKE_SHA256_FORCE_ARM_IMPL)
    ioke_sha256_compress_arm(state, data, blocks);
#elif defined(IOKE_SHA256_FORCE_LEGACY_IMPL)
    ioke_sha256_compress_legacy(state, data, blocks);
#else
    if (ioke_sha256_backend_override == IOKE_SHA256_BACKEND_LEGACY) {
        ioke_sha256_compress_legacy(state, data, blocks);
        return;
    }
    if (ioke_sha256_backend_override == IOKE_SHA256_BACKEND_NATIVE) {
#if defined(IOKE_SHA256_HAVE_X86_SHA_NI)
        if (ioke_sha256_can_use_native()) {
            ioke_sha256_compress_x86_sha_ni(state, data, blocks);
            return;
        }
#endif
#if defined(IOKE_SHA256_USE_ARM_CRYPTO)
        ioke_sha256_compress_arm(state, data, blocks);
        return;
#endif
        ioke_sha256_compress_legacy(state, data, blocks);
        return;
    }

    static atomic_int impl_selected = -1;
    int selected = atomic_load_explicit(&impl_selected, memory_order_acquire);
    if (selected < 0) {
        int choice = 0;
#if defined(IOKE_SHA256_HAVE_X86_SHA_NI)
        if (ioke_sha256_can_use_native()) {
            choice = 1;
        }
#endif
#if defined(IOKE_SHA256_USE_ARM_CRYPTO)
        if (choice == 0) {
            choice = 2;
        }
#endif
        int expected = -1;
        atomic_compare_exchange_strong_explicit(
            &impl_selected,
            &expected,
            choice,
            memory_order_release,
            memory_order_acquire
        );
        selected = atomic_load_explicit(&impl_selected, memory_order_acquire);
    }

    if (selected == 1) {
#if defined(IOKE_SHA256_HAVE_X86_SHA_NI)
        ioke_sha256_compress_x86_sha_ni(state, data, blocks);
#else
        ioke_sha256_compress_legacy(state, data, blocks);
#endif
    } else if (selected == 2) {
#if defined(IOKE_SHA256_USE_ARM_CRYPTO)
        ioke_sha256_compress_arm(state, data, blocks);
#else
        ioke_sha256_compress_legacy(state, data, blocks);
#endif
    } else {
        ioke_sha256_compress_legacy(state, data, blocks);
    }
#endif
}

void ioke_sha256_set_backend(int backend) {
    if (backend == IOKE_SHA256_BACKEND_LEGACY ||
        backend == IOKE_SHA256_BACKEND_NATIVE ||
        backend == IOKE_SHA256_BACKEND_AUTO) {
        ioke_sha256_backend_override = backend;
    } else {
        ioke_sha256_backend_override = IOKE_SHA256_BACKEND_AUTO;
    }
}

int ioke_sha256_backend_available(int backend) {
    if (backend == IOKE_SHA256_BACKEND_AUTO || backend == IOKE_SHA256_BACKEND_LEGACY) {
        return 1;
    }
    if (backend == IOKE_SHA256_BACKEND_NATIVE) {
        return ioke_sha256_can_use_native();
    }
    return 0;
}

const char* ioke_sha256_backend_name(void) {
    if (ioke_sha256_backend_override == IOKE_SHA256_BACKEND_LEGACY) {
        return "legacy";
    }
    if (ioke_sha256_backend_override == IOKE_SHA256_BACKEND_NATIVE) {
        return ioke_sha256_backend_available(IOKE_SHA256_BACKEND_NATIVE) ? "native" : "legacy";
    }
    if (ioke_sha256_backend_available(IOKE_SHA256_BACKEND_NATIVE)) {
        return "auto-native";
    }
    return "auto-legacy";
}

int IOKE_SHA256_Init(IOKE_SHA256_CTX* ctx) {
    if (!ctx) {
        return 0;
    }

    for (int i = 0; i < 8; ++i) {
        ctx->state[i] = SHA256_IV[i];
    }
    ctx->total_len = 0;
    ctx->buffer_len = 0;
    memset(ctx->buffer, 0, sizeof(ctx->buffer));
    return 1;
}

int IOKE_SHA256_Update(IOKE_SHA256_CTX* ctx, const void* data, size_t len) {
    const uint8_t* p;

    if (!ctx) {
        return 0;
    }
    if (len == 0) {
        return 1;
    }
    if (!data) {
        return 0;
    }

    /* SHA-256 length field is 64-bit bit-count; reject inputs that would wrap it. */
    if ((uint64_t)len > (UINT64_MAX >> 3) - ctx->total_len) {
        return 0;
    }
    ctx->total_len += (uint64_t)len;

    p = (const uint8_t*)data;

    if (ctx->buffer_len > 0) {
        const size_t need = IOKE_SHA256_BLOCK_LENGTH - (size_t)ctx->buffer_len;
        if (len < need) {
            memcpy(ctx->buffer + ctx->buffer_len, p, len);
            ctx->buffer_len += (uint32_t)len;
            return 1;
        }

        memcpy(ctx->buffer + ctx->buffer_len, p, need);
        ioke_sha256_compress_blocks(ctx->state, ctx->buffer, 1);
        ctx->buffer_len = 0;
        p += need;
        len -= need;
    }

    if (len >= IOKE_SHA256_BLOCK_LENGTH) {
        const size_t blocks = len / IOKE_SHA256_BLOCK_LENGTH;
        ioke_sha256_compress_blocks(ctx->state, p, blocks);
        p += (blocks * IOKE_SHA256_BLOCK_LENGTH);
        len -= (blocks * IOKE_SHA256_BLOCK_LENGTH);
    }

    if (len > 0) {
        memcpy(ctx->buffer, p, len);
        ctx->buffer_len = (uint32_t)len;
    }

    return 1;
}

int IOKE_SHA256_Final(unsigned char md[IOKE_SHA256_DIGEST_LENGTH], IOKE_SHA256_CTX* ctx) {
    uint64_t bit_len;
    size_t i;

    if (!ctx || !md) {
        return 0;
    }
    if (ctx->total_len > (UINT64_MAX >> 3)) {
        return 0;
    }

    bit_len = ctx->total_len << 3;

    i = (size_t)ctx->buffer_len;
    ctx->buffer[i++] = 0x80U;

    if (i > 56U) {
        memset(ctx->buffer + i, 0, IOKE_SHA256_BLOCK_LENGTH - i);
        ioke_sha256_compress_blocks(ctx->state, ctx->buffer, 1);
        i = 0;
    }

    memset(ctx->buffer + i, 0, 56U - i);
    ioke_store_be64(ctx->buffer + 56, bit_len);
    ioke_sha256_compress_blocks(ctx->state, ctx->buffer, 1);

    for (i = 0; i < 8; ++i) {
        ioke_store_be32(md + (i * 4U), ctx->state[i]);
    }

    memset(ctx, 0, sizeof(*ctx));
    return 1;
}

unsigned char* IOKE_SHA256(const unsigned char* data, size_t len, unsigned char md[IOKE_SHA256_DIGEST_LENGTH]) {
    IOKE_SHA256_CTX ctx;

    if (!md) {
        return (unsigned char*)0;
    }
    if (len > 0 && !data) {
        return (unsigned char*)0;
    }
    if (!IOKE_SHA256_Init(&ctx)) {
        return (unsigned char*)0;
    }
    if (!IOKE_SHA256_Update(&ctx, data, len)) {
        return (unsigned char*)0;
    }
    if (!IOKE_SHA256_Final(md, &ctx)) {
        return (unsigned char*)0;
    }

    return md;
}

void sha256(uint8_t in[], uint32_t size, uint8_t out[]) {
    if (!out) {
        return;
    }

    if (!IOKE_SHA256((const unsigned char*)in, (size_t)size, (unsigned char*)out)) {
        memset(out, 0, IOKE_SHA256_DIGEST_LENGTH);
    }
}

void ioke_sha256_init_state(uint32_t state[8]) {
    for (int i = 0; i < 8; ++i) {
        state[i] = SHA256_IV[i];
    }
}

void ioke_sha256_compress(uint32_t state[8], const uint8_t* data, size_t blocks) {
    ioke_sha256_compress_blocks(state, data, blocks);
}

#if !defined(IOKE_SHA256_HAVE_X86_SHA_NI)
void ioke_sha256_compress2_x86_sha_ni(
    uint32_t state_a[8], const uint8_t* data_a,
    uint32_t state_b[8], const uint8_t* data_b,
    size_t blocks
) {
    ioke_sha256_compress(state_a, data_a, blocks);
    ioke_sha256_compress(state_b, data_b, blocks);
}

void ioke_sha256_compress4_x86_sha_ni(
    uint32_t state_a[8], const uint8_t* data_a,
    uint32_t state_b[8], const uint8_t* data_b,
    uint32_t state_c[8], const uint8_t* data_c,
    uint32_t state_d[8], const uint8_t* data_d,
    size_t blocks
) {
    ioke_sha256_compress(state_a, data_a, blocks);
    ioke_sha256_compress(state_b, data_b, blocks);
    ioke_sha256_compress(state_c, data_c, blocks);
    ioke_sha256_compress(state_d, data_d, blocks);
}

void ioke_sha256_expand_rounds_x86_sha_ni(const uint8_t block[64], void* rounds_out) {
    (void)block;
    (void)rounds_out;
}

void ioke_sha256_compress2_scheduled_x86_sha_ni(
    uint32_t state_a[8], uint32_t state_b[8], const void* rounds
) {
    (void)rounds;
    (void)state_a;
    (void)state_b;
}
#endif

void ioke_sha256_write_hash(uint8_t out[IOKE_SHA256_DIGEST_LENGTH], const uint32_t state[8]) {
    for (int i = 0; i < 8; ++i) {
        ioke_store_be32(out + (i * 4), state[i]);
    }
}
