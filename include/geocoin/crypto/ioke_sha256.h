#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

    #define IOKE_SHA256_DIGEST_LENGTH 32
    #define IOKE_SHA256_BLOCK_LENGTH 64

#ifndef SHA256_DIGEST_LENGTH
    #define SHA256_DIGEST_LENGTH IOKE_SHA256_DIGEST_LENGTH
#endif
#ifndef SHA256_BLOCK_LENGTH
    #define SHA256_BLOCK_LENGTH IOKE_SHA256_BLOCK_LENGTH
#endif

    typedef struct {
        uint32_t state[8];
        uint64_t total_len;
        uint8_t buffer[IOKE_SHA256_BLOCK_LENGTH];
        uint32_t buffer_len;
    } IOKE_SHA256_CTX;

    int IOKE_SHA256_Init(IOKE_SHA256_CTX* ctx);
    int IOKE_SHA256_Update(IOKE_SHA256_CTX* ctx, const void* data, size_t len);
    int IOKE_SHA256_Final(unsigned char md[IOKE_SHA256_DIGEST_LENGTH], IOKE_SHA256_CTX* ctx);
    unsigned char* IOKE_SHA256(const unsigned char* data, size_t len, unsigned char md[IOKE_SHA256_DIGEST_LENGTH]);

	void sha256(uint8_t in[], uint32_t size, uint8_t out[]);

    void ioke_sha256_init_state(uint32_t state[8]);
    void ioke_sha256_compress(uint32_t state[8], const uint8_t* data, size_t blocks);
    void ioke_sha256_compress2_x86_sha_ni(uint32_t state_a[8], const uint8_t* data_a, uint32_t state_b[8], const uint8_t* data_b, size_t blocks);
    void ioke_sha256_compress4_x86_sha_ni(uint32_t state_a[8], const uint8_t* data_a, uint32_t state_b[8], const uint8_t* data_b, uint32_t state_c[8], const uint8_t* data_c, uint32_t state_d[8], const uint8_t* data_d, size_t blocks);
    void ioke_sha256_expand_rounds_x86_sha_ni(const uint8_t block[64], void* rounds_out);
    void ioke_sha256_compress2_scheduled_x86_sha_ni(uint32_t state_a[8], uint32_t state_b[8], const void* rounds);
    void ioke_sha256_write_hash(uint8_t out[IOKE_SHA256_DIGEST_LENGTH], const uint32_t state[8]);

    enum {
        IOKE_SHA256_BACKEND_AUTO = 0,
        IOKE_SHA256_BACKEND_LEGACY = 1,
        IOKE_SHA256_BACKEND_NATIVE = 2
    };

    void ioke_sha256_set_backend(int backend);
    int ioke_sha256_backend_available(int backend);
    const char* ioke_sha256_backend_name(void);

#if !defined(IOKE_SHA256_NO_OPENSSL_COMPAT_NAMES) && !defined(OPENSSL_SHA_H)
    typedef IOKE_SHA256_CTX SHA256_CTX;
    #define SHA256_Init IOKE_SHA256_Init
    #define SHA256_Update IOKE_SHA256_Update
    #define SHA256_Final IOKE_SHA256_Final
    #define SHA256 IOKE_SHA256
#endif

#ifdef __cplusplus
}
#endif
