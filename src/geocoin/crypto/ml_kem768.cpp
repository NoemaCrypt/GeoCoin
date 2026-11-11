#include "geocoin/crypto/ml_kem768.hpp"

#include "geocoin/utils/secure_memory.hpp"

extern "C" {
#include <oqs/kem_ml_kem.h>
}

namespace geocoin::crypto::ml_kem768 {

namespace {

static_assert(kPublicKeySize == OQS_KEM_ml_kem_768_length_public_key);
static_assert(kSecretKeySize == OQS_KEM_ml_kem_768_length_secret_key);
static_assert(kCiphertextSize == OQS_KEM_ml_kem_768_length_ciphertext);
static_assert(kSharedSecretSize == OQS_KEM_ml_kem_768_length_shared_secret);

} // namespace

bool generate_keypair(KeyPair& out) {
    out.public_key.assign(kPublicKeySize, 0);
    out.secret_key.assign(kSecretKeySize, 0);
    if (OQS_KEM_ml_kem_768_keypair(out.public_key.data(), out.secret_key.data()) != OQS_SUCCESS) {
        out.public_key.clear();
        utils::cleanse_container(out.secret_key);
        return false;
    }
    return true;
}

bool encapsulate(
    const bytes& public_key,
    bytes& ciphertext,
    secure_bytes& shared_secret
) {
    ciphertext.clear();
    utils::cleanse_container(shared_secret);
    if (public_key.size() != kPublicKeySize) {
        return false;
    }

    ciphertext.assign(kCiphertextSize, 0);
    shared_secret.assign(kSharedSecretSize, 0);
    if (OQS_KEM_ml_kem_768_encaps(
            ciphertext.data(),
            shared_secret.data(),
            public_key.data()) != OQS_SUCCESS) {
        ciphertext.clear();
        utils::cleanse_container(shared_secret);
        return false;
    }
    return true;
}

bool decapsulate(
    const bytes& ciphertext,
    const secure_bytes& secret_key,
    secure_bytes& shared_secret
) {
    utils::cleanse_container(shared_secret);
    if (ciphertext.size() != kCiphertextSize || secret_key.size() != kSecretKeySize) {
        return false;
    }

    shared_secret.assign(kSharedSecretSize, 0);
    if (OQS_KEM_ml_kem_768_decaps(
            shared_secret.data(),
            ciphertext.data(),
            secret_key.data()) != OQS_SUCCESS) {
        utils::cleanse_container(shared_secret);
        return false;
    }
    return true;
}

} // namespace geocoin::crypto::ml_kem768
