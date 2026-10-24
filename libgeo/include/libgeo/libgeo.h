#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
  #ifdef LIBGEO_EXPORTS
    #define LIBGEO_API __declspec(dllexport)
  #else
    #define LIBGEO_API __declspec(dllimport)
  #endif
#else
  #define LIBGEO_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define LIBGEO_API_VERSION 1
#define LIBGEO_ADDRESS_MAX_LEN 128

typedef enum geo_status_t {
    GEO_OK = 0,
    GEO_ERR_INVALID_ARG = 1,
    GEO_ERR_BUFFER_TOO_SMALL = 2,
    GEO_ERR_CRYPTO = 3,
    GEO_ERR_INVALID_ADDRESS = 4,
    GEO_ERR_INTERNAL = 255
} geo_status_t;

typedef struct geo_tx_t geo_tx_t;

LIBGEO_API uint32_t geo_api_version(void);
LIBGEO_API const char* geo_last_error(void);

LIBGEO_API geo_status_t geo_verify_address(const char* address, int* is_valid);

LIBGEO_API geo_status_t geo_derive_address_from_private_key(
    const uint8_t* private_key,
    size_t private_key_len,
    char* address_out,
    size_t address_out_len
);

LIBGEO_API geo_status_t geo_sign_digest32_recoverable(
    const uint8_t msg32[32],
    const uint8_t* private_key,
    size_t private_key_len,
    uint8_t* signature_out,
    size_t signature_out_len,
    size_t* signature_written
);

LIBGEO_API geo_status_t geo_verify_signed_message(
    const uint8_t msg32[32],
    const uint8_t* signature,
    size_t signature_len,
    int* is_valid
);

LIBGEO_API geo_status_t geo_recover_signer_address(
    const uint8_t msg32[32],
    const uint8_t* signature,
    size_t signature_len,
    char* address_out,
    size_t address_out_len
);

LIBGEO_API geo_status_t geo_multisig_create(
    uint8_t m,
    uint8_t n,
    const char* const* participant_addresses,
    char* multisig_address_out,
    size_t multisig_address_out_len
);

LIBGEO_API geo_status_t geo_multisig_verify(
    const char* multisig_address,
    int* is_valid
);

// === Transaction crypto API ===

LIBGEO_API geo_status_t geo_tx_hash_from_raw(
    const uint8_t* raw_tx,
    size_t raw_tx_len,
    uint8_t out_tx_hash32[32]
);

LIBGEO_API geo_status_t geo_tx_sign_input(
    const uint8_t tx_hash32[32],
    uint64_t input_index,
    const uint8_t* private_key,
    size_t private_key_len,
    uint8_t* signature_out,
    size_t signature_out_len,
    size_t* signature_written
);

LIBGEO_API geo_status_t geo_tx_verify_input_signature(
    const uint8_t tx_hash32[32],
    uint64_t input_index,
    const uint8_t* signature,
    size_t signature_len,
    int* is_valid
);

LIBGEO_API geo_status_t geo_tx_recover_input_signer_address(
    const uint8_t tx_hash32[32],
    uint64_t input_index,
    const uint8_t* signature,
    size_t signature_len,
    char* address_out,
    size_t address_out_len
);

// === Transaction object API ===

LIBGEO_API geo_status_t geo_tx_new(geo_tx_t** tx_out);
LIBGEO_API void geo_tx_free(geo_tx_t* tx);

LIBGEO_API geo_status_t geo_tx_deserialize(
    const uint8_t* raw_tx,
    size_t raw_tx_len,
    geo_tx_t** tx_out
);

LIBGEO_API geo_status_t geo_tx_serialize(
    const geo_tx_t* tx,
    uint8_t* out_raw_tx,
    size_t out_raw_tx_len,
    size_t* out_written
);

LIBGEO_API geo_status_t geo_tx_set_header(
    geo_tx_t* tx,
    uint32_t version,
    uint8_t type,
    uint64_t timestamp,
    uint32_t lock_time
);

LIBGEO_API geo_status_t geo_tx_add_input(
    geo_tx_t* tx,
    const uint8_t prev_tx_hash32[32],
    uint32_t vout,
    uint32_t sequence
);

LIBGEO_API geo_status_t geo_tx_add_output(
    geo_tx_t* tx,
    int64_t value,
    const char* address
);

LIBGEO_API geo_status_t geo_tx_set_input_signature(
    geo_tx_t* tx,
    uint64_t input_index,
    const uint8_t* signature,
    size_t signature_len
);

LIBGEO_API geo_status_t geo_tx_sign_input_inplace(
    geo_tx_t* tx,
    uint64_t input_index,
    const uint8_t* private_key,
    size_t private_key_len
);

LIBGEO_API geo_status_t geo_tx_hash(
    const geo_tx_t* tx,
    uint8_t out_tx_hash32[32]
);

LIBGEO_API geo_status_t geo_tx_verify(
    const geo_tx_t* tx,
    int* is_valid
);

LIBGEO_API geo_status_t geo_tx_verify_input(
    const geo_tx_t* tx,
    uint64_t input_index,
    int* is_valid
);

#ifdef __cplusplus
}
#endif
