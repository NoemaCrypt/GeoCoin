#include "geocoin/crypto/geo_area.hpp"
#include "geocoin/crypto/signatures.hpp"
#define IOKE_SHA256_NO_OPENSSL_COMPAT_NAMES
#include "geocoin/crypto/ioke_sha256.h"
#include "geocoin/crypto/ioke_ripemd160.h"
#include "geocoin/utils/bytes.hpp"
#include "geocoin/utils/secure_memory.hpp"
#include <openssl/rand.h>
#include <stdexcept>

namespace geocoin::crypto {

bool generate_private_key(bytes& private_key) {
    private_key.assign(32, 0);
    if (RAND_priv_bytes(private_key.data(), static_cast<int>(private_key.size())) != 1) {
        utils::cleanse_container(private_key);
        return false;
    }
    return true;
}

Hash256 sha256(const uint8_t* data, size_t size) {
    Hash256 hash{};
    const unsigned char* input = size == 0 ? nullptr : data;
    // AUTO selects SHA-NI on x86 and the ARM crypto extension when the CPU has them.
    if (IOKE_SHA256(input, size, hash.data()) == nullptr) {
        throw std::runtime_error("SHA-256 digest failed");
    }
    return hash;
}

Hash256 sha256(const bytes& data) {
    return sha256(data.empty() ? nullptr : data.data(), data.size());
}

Hash160 ripemd160(const bytes& data) {
    Hash160 hash;
    IOKE_RIPEMD160_CTX ctx;
    IOKE_RIPEMD160_Init(&ctx);
    IOKE_RIPEMD160_Update(&ctx, data.data(), data.size());
    IOKE_RIPEMD160_Final(hash.data(), &ctx);
    return hash;
}

Hash256 double_sha256(const uint8_t* data, size_t size) {
    const Hash256 first = sha256(data, size);
    return sha256(first.data(), first.size());
}

Hash256 double_sha256(const bytes& data) {
    return double_sha256(data.empty() ? nullptr : data.data(), data.size());
}

bool compute_address_from_private_key(const bytes& private_key, Address& address) {
    if (private_key.size() != 32) {
        return false;
    }

    return derive_identity_address_from_private_key(private_key, address);
}

} // namespace geocoin::crypto

namespace geocoin::crypto {

std::string bytes_to_hex(const bytes& data) {
    return utils::bytes_to_hex(data);
}

bytes hex_to_bytes(const std::string& hex) {
    std::string clean_hex;
    for (char c : hex) {
        if (c != ' ' && c != '\t' && c != '\n') {
            clean_hex.push_back(c);
        }
    }
    return utils::hex_to_bytes(clean_hex);
}

} // namespace geocoin::crypto
