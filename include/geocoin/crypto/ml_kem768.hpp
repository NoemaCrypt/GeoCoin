#pragma once

#include "geocoin/types.hpp"

#include <cstddef>

namespace geocoin::crypto::ml_kem768 {

inline constexpr size_t kPublicKeySize = 1184;
inline constexpr size_t kSecretKeySize = 2400;
inline constexpr size_t kCiphertextSize = 1088;
inline constexpr size_t kSharedSecretSize = 32;

struct KeyPair {
    bytes public_key;
    secure_bytes secret_key;
};

/** Generate a fresh FIPS 203 ML-KEM-768 key pair. */
bool generate_keypair(KeyPair& out);

/** Encapsulate to a peer public key. */
bool encapsulate(
    const bytes& public_key,
    bytes& ciphertext,
    secure_bytes& shared_secret
);

/** Decapsulate a ciphertext with the matching secret key. */
bool decapsulate(
    const bytes& ciphertext,
    const secure_bytes& secret_key,
    secure_bytes& shared_secret
);

} // namespace geocoin::crypto::ml_kem768
