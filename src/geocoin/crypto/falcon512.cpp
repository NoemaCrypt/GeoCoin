#include "geocoin/crypto/falcon512.hpp"
#include "geocoin/utils/secure_memory.hpp"

#include <cstdint>
#include <cstring>
#include <openssl/crypto.h>

extern "C" {
#include "sig/falcon/pqclean_falcon-512_clean/api.h"
#include "sig/falcon/pqclean_falcon-512_clean/inner.h"

#if defined(GEOCOIN_FALCON512_AVX2)
int PQCLEAN_FALCON512_AVX2_crypto_sign_signature(
    uint8_t* sig, size_t* siglen, const uint8_t* m, size_t mlen, const uint8_t* sk);
int PQCLEAN_FALCON512_AVX2_crypto_sign_verify(
    const uint8_t* sig, size_t siglen, const uint8_t* m, size_t mlen, const uint8_t* pk);
#endif
}

namespace geocoin::crypto::falcon512 {

namespace {

constexpr unsigned kLogN = 9; // Falcon-512

bool falcon_use_avx2() {
#if defined(GEOCOIN_FALCON512_AVX2) && (defined(__GNUC__) || defined(__clang__))
    static const bool enabled = __builtin_cpu_supports("avx2");
    return enabled;
#else
    return false;
#endif
}

int falcon_sign_signature(
    uint8_t* sig,
    size_t* siglen,
    const uint8_t* message,
    size_t message_len,
    const uint8_t* secret_key
) {
#if defined(GEOCOIN_FALCON512_AVX2)
    if (falcon_use_avx2()) {
        return PQCLEAN_FALCON512_AVX2_crypto_sign_signature(
            sig, siglen, message, message_len, secret_key);
    }
#endif
    return PQCLEAN_FALCON512_CLEAN_crypto_sign_signature(
        sig, siglen, message, message_len, secret_key);
}

int falcon_verify_signature(
    const uint8_t* signature,
    size_t signature_len,
    const uint8_t* message,
    size_t message_len,
    const uint8_t* public_key
) {
#if defined(GEOCOIN_FALCON512_AVX2)
    if (falcon_use_avx2()) {
        return PQCLEAN_FALCON512_AVX2_crypto_sign_verify(
            signature, signature_len, message, message_len, public_key);
    }
#endif
    return PQCLEAN_FALCON512_CLEAN_crypto_sign_verify(
        signature, signature_len, message, message_len, public_key);
}

class ScopedRawCleanser final {
public:
    ScopedRawCleanser(void* data, size_t size) noexcept : data_(data), size_(size) {}
    ~ScopedRawCleanser() { OPENSSL_cleanse(data_, size_); }

    ScopedRawCleanser(const ScopedRawCleanser&) = delete;
    ScopedRawCleanser& operator=(const ScopedRawCleanser&) = delete;

private:
    void* data_;
    size_t size_;
};

// Детерминированно выводит Falcon keypair из 32-байтового seed, без
// обращения к глобальному RNG liboqs (без randombytes/OQS_randombytes_custom_algorithm).
bool keypair_from_seed32(const bytes& seed32, bytes& out_pk, bytes& out_sk) {
    if (seed32.size() != 32) {
        return false;
    }

    out_pk.assign(PQCLEAN_FALCON512_CLEAN_CRYPTO_PUBLICKEYBYTES, 0);
    out_sk.assign(PQCLEAN_FALCON512_CLEAN_CRYPTO_SECRETKEYBYTES, 0);

    union {
        uint8_t b[FALCON_KEYGEN_TEMP_9];
        uint64_t dummy_u64;
        fpr dummy_fpr;
    } tmp;

    int8_t f[512];
    int8_t g[512];
    int8_t F[512];
    uint16_t h[512];

    // Сохраняем структуру "seed -> SHAKE -> keygen", как в PQClean wrapper'ах,
    // но сидим детерминированно из переданного seed32.
    uint8_t seed48[48];
    inner_shake256_context expander;
    ScopedRawCleanser cleanse_tmp(&tmp, sizeof(tmp));
    ScopedRawCleanser cleanse_f(f, sizeof(f));
    ScopedRawCleanser cleanse_g(g, sizeof(g));
    ScopedRawCleanser cleanse_F(F, sizeof(F));
    ScopedRawCleanser cleanse_h(h, sizeof(h));
    ScopedRawCleanser cleanse_seed(seed48, sizeof(seed48));
    ScopedRawCleanser cleanse_expander(&expander, sizeof(expander));
    inner_shake256_init(&expander);
    inner_shake256_inject(&expander, seed32.data(), seed32.size());
    inner_shake256_flip(&expander);
    inner_shake256_extract(&expander, seed48, sizeof(seed48));
    inner_shake256_ctx_release(&expander);

    inner_shake256_context rng;
    ScopedRawCleanser cleanse_rng(&rng, sizeof(rng));
    inner_shake256_init(&rng);
    inner_shake256_inject(&rng, seed48, sizeof(seed48));
    inner_shake256_flip(&rng);
    PQCLEAN_FALCON512_CLEAN_keygen(&rng, f, g, F, nullptr, h, kLogN, tmp.b);
    inner_shake256_ctx_release(&rng);

    // Кодируем secret key (тот же формат, что и в PQClean).
    out_sk[0] = static_cast<byte>(0x50 + kLogN);
    size_t u = 1;
    size_t v = PQCLEAN_FALCON512_CLEAN_trim_i8_encode(
        out_sk.data() + u, out_sk.size() - u,
        f, kLogN, PQCLEAN_FALCON512_CLEAN_max_fg_bits[kLogN]
    );
    if (v == 0) {
        return false;
    }
    u += v;
    v = PQCLEAN_FALCON512_CLEAN_trim_i8_encode(
        out_sk.data() + u, out_sk.size() - u,
        g, kLogN, PQCLEAN_FALCON512_CLEAN_max_fg_bits[kLogN]
    );
    if (v == 0) {
        return false;
    }
    u += v;
    v = PQCLEAN_FALCON512_CLEAN_trim_i8_encode(
        out_sk.data() + u, out_sk.size() - u,
        F, kLogN, PQCLEAN_FALCON512_CLEAN_max_FG_bits[kLogN]
    );
    if (v == 0) {
        return false;
    }
    u += v;
    if (u != out_sk.size()) {
        return false;
    }

    // Кодируем public key (тот же формат, что и в PQClean).
    out_pk[0] = static_cast<byte>(0x00 + kLogN);
    v = PQCLEAN_FALCON512_CLEAN_modq_encode(
        out_pk.data() + 1, out_pk.size() - 1, h, kLogN
    );
    if (v != out_pk.size() - 1) {
        return false;
    }

    return true;
}

} // namespace

bool available() {
    return true;
}

bool sign_digest32(
    const Hash256& msg32,
    const bytes& private_key_seed,
    bytes& out_public_key,
    bytes& out_signature
) {
    out_public_key.clear();
    out_signature.clear();

    bytes pk;
    bytes sk;
    utils::ScopedContainerCleanser<bytes> cleanse_sk(sk);
    if (!keypair_from_seed32(private_key_seed, pk, sk)) {
        return false;
    }

    bytes sig(PQCLEAN_FALCON512_CLEAN_CRYPTO_BYTES, 0);
    size_t siglen = 0;
    if (falcon_sign_signature(
            sig.data(), &siglen,
            msg32.data(), msg32.size(),
            sk.data()) != 0) {
        return false;
    }
    if (siglen == 0 || siglen > sig.size()) {
        return false;
    }

    sig.resize(siglen);
    out_public_key = std::move(pk);
    out_signature = std::move(sig);
    return true;
}

bool public_key_from_seed32(
    const bytes& private_key_seed,
    bytes& out_public_key
) {
    out_public_key.clear();

    bytes pk;
    bytes sk;
    utils::ScopedContainerCleanser<bytes> cleanse_sk(sk);
    if (!keypair_from_seed32(private_key_seed, pk, sk)) {
        return false;
    }

    out_public_key = std::move(pk);
    return true;
}

bool verify_digest32(
    const Hash256& msg32,
    const bytes& public_key,
    const bytes& signature
) {
    if (public_key.size() != PQCLEAN_FALCON512_CLEAN_CRYPTO_PUBLICKEYBYTES) {
        return false;
    }
    if (signature.empty() || signature.size() > PQCLEAN_FALCON512_CLEAN_CRYPTO_BYTES) {
        return false;
    }
    return falcon_verify_signature(
               signature.data(), signature.size(),
               msg32.data(), msg32.size(),
               public_key.data()) == 0;
}

} // namespace geocoin::crypto::falcon512
