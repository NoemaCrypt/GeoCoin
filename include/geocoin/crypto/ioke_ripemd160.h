#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IOKE_RIPEMD160_DIGEST_LENGTH 20
#define IOKE_RIPEMD160_BLOCK_LENGTH 64

typedef struct {
    uint32_t state[5];
    uint64_t total_len;
    uint8_t buffer[IOKE_RIPEMD160_BLOCK_LENGTH];
    uint32_t buffer_len;
} IOKE_RIPEMD160_CTX;

int IOKE_RIPEMD160_Init(IOKE_RIPEMD160_CTX* ctx);
int IOKE_RIPEMD160_Update(IOKE_RIPEMD160_CTX* ctx, const void* data, size_t len);
int IOKE_RIPEMD160_Final(unsigned char md[IOKE_RIPEMD160_DIGEST_LENGTH], IOKE_RIPEMD160_CTX* ctx);

#ifdef __cplusplus
}
#endif
