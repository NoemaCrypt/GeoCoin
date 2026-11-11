#include "geocoin/crypto/encryption.hpp"
#include "geocoin/crypto/geo_area.hpp"
#include "geocoin/crypto/signatures.hpp"
#include "geocoin/utils/secure_memory.hpp"
#include <openssl/evp.h>
#include <openssl/aes.h>
#include <openssl/rand.h>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>

namespace geocoin::crypto {

namespace {
    constexpr size_t kSaltSize = 16;
    constexpr size_t kIvSize = 12;   // GCM стандартный IV
    constexpr size_t kTagSize = 16;
}

bool derive_key_pbkdf2(
    const std::string& password,
    const bytes& salt,
    int iterations,
    bytes& key
) {
    utils::cleanse_container(key);
    if (iterations <= 0 ||
        password.size() > static_cast<size_t>((std::numeric_limits<int>::max)()) ||
        salt.size() > static_cast<size_t>((std::numeric_limits<int>::max)())) {
        return false;
    }

    key.resize(32); // AES-256 key size

    if (PKCS5_PBKDF2_HMAC(
        password.c_str(), static_cast<int>(password.length()),
        salt.data(), static_cast<int>(salt.size()),
        iterations,
        EVP_sha256(),
        static_cast<int>(key.size()), key.data()
    ) != 1) {
        utils::cleanse_container(key);
        return false;
    }

    return true;
}

bool encrypt_aes256(
    const bytes& data,
    const std::string& password,
    bytes& encrypted
) {
    return encrypt_aes256(data, password, encrypted, kPbkdf2Iterations);
}

bool encrypt_aes256(
    const bytes& data,
    const std::string& password,
    bytes& encrypted,
    int iterations
) {
    auto fail = [&encrypted]() {
        utils::cleanse_container(encrypted);
        return false;
    };
    if (data.size() > static_cast<size_t>((std::numeric_limits<int>::max)())) {
        return fail();
    }

    // Generate random salt and IV
    bytes salt(kSaltSize);
    bytes iv(kIvSize);

    if (RAND_bytes(salt.data(), static_cast<int>(salt.size())) != 1 ||
        RAND_bytes(iv.data(), static_cast<int>(iv.size())) != 1) {
        return fail();
    }

    // Derive key from password
    bytes key;
    utils::ScopedContainerCleanser<bytes> key_cleanser(key);
    if (!derive_key_pbkdf2(password, salt, iterations, key)) {
        return fail();
    }

    // Create cipher context
    std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> ctx(
        EVP_CIPHER_CTX_new(),
        &EVP_CIPHER_CTX_free
    );
    if (!ctx) {
        return fail();
    }

    // Initialize encryption (AES-256-GCM)
    if (EVP_EncryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1) {
        return fail();
    }
    if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(iv.size()), nullptr) != 1) {
        return fail();
    }
    if (EVP_EncryptInit_ex(ctx.get(), nullptr, nullptr, key.data(), iv.data()) != 1) {
        return fail();
    }

    // Build in a temporary buffer so callers never observe partial output.
    bytes result(salt.size() + iv.size() + data.size() + kTagSize);

    // Copy salt and IV to output
    std::memcpy(result.data(), salt.data(), salt.size());
    std::memcpy(result.data() + salt.size(), iv.data(), iv.size());

    // Encrypt data
    int len = 0;
    int ciphertext_len = 0;

    if (EVP_EncryptUpdate(ctx.get(), result.data() + salt.size() + iv.size(),
                          &len, data.data(), static_cast<int>(data.size())) != 1) {
        return fail();
    }
    ciphertext_len = len;

    // Finalize encryption
    if (EVP_EncryptFinal_ex(ctx.get(), result.data() + salt.size() + iv.size() + len, &len) != 1) {
        return fail();
    }
    ciphertext_len += len;

    // Get GCM tag
    bytes tag(kTagSize);
    if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_GET_TAG, static_cast<int>(tag.size()), tag.data()) != 1) {
        return fail();
    }

    // Resize to actual size and append tag
    result.resize(salt.size() + iv.size() + ciphertext_len + tag.size());
    std::memcpy(result.data() + salt.size() + iv.size() + ciphertext_len, tag.data(), tag.size());

    encrypted = std::move(result);
    return true;
}

bool decrypt_aes256(
    const bytes& encrypted,
    const std::string& password,
    bytes& data
) {
    return decrypt_aes256(encrypted, password, data, kPbkdf2Iterations);
}

bool decrypt_aes256(
    const bytes& encrypted,
    const std::string& password,
    bytes& data,
    int iterations
) {
    // Support in-place use without letting output cleanup destroy ciphertext.
    bytes encrypted_copy;
    const bytes* encrypted_input = &encrypted;
    if (std::addressof(encrypted) == std::addressof(data)) {
        encrypted_copy = encrypted;
        encrypted_input = &encrypted_copy;
    }
    utils::cleanse_container(data);
    const bytes& input = *encrypted_input;

    // Check minimum size (salt + IV + tag)
    if (input.size() < kSaltSize + kIvSize + kTagSize) {
        return false;
    }

    // Extract salt and IV
    bytes salt(input.begin(), input.begin() + kSaltSize);
    bytes iv(input.begin() + kSaltSize, input.begin() + kSaltSize + kIvSize);
    const size_t tag_offset = input.size() - kTagSize;
    bytes tag(input.begin() + tag_offset, input.end());

    // Derive key from password
    bytes key;
    utils::ScopedContainerCleanser<bytes> key_cleanser(key);
    if (!derive_key_pbkdf2(password, salt, iterations, key)) {
        return false;
    }

    // Create cipher context
    std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> ctx(
        EVP_CIPHER_CTX_new(),
        &EVP_CIPHER_CTX_free
    );
    if (!ctx) {
        return false;
    }

    // Initialize decryption (AES-256-GCM)
    if (EVP_DecryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1) {
        return false;
    }
    if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(iv.size()), nullptr) != 1) {
        return false;
    }
    if (EVP_DecryptInit_ex(ctx.get(), nullptr, nullptr, key.data(), iv.data()) != 1) {
        return false;
    }

    // Allocate output buffer
    if (tag_offset < (kSaltSize + kIvSize)) {
        return false;
    }
    const size_t ciphertext_size = tag_offset - (kSaltSize + kIvSize);
    if (ciphertext_size > static_cast<size_t>((std::numeric_limits<int>::max)())) {
        return false;
    }
    // GCM plaintext is unauthenticated until DecryptFinal succeeds. Keep it in
    // a cleansed temporary and publish it only after tag verification.
    bytes plaintext(ciphertext_size);
    utils::ScopedContainerCleanser<bytes> plaintext_cleanser(plaintext);

    // Decrypt data
    int len = 0;
    int plaintext_len = 0;

    if (EVP_DecryptUpdate(ctx.get(), plaintext.data(), &len,
                          input.data() + kSaltSize + kIvSize,
                          static_cast<int>(ciphertext_size)) != 1) {
        return false;
    }
    plaintext_len = len;

    // Set expected tag before finalizing
    if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_TAG, static_cast<int>(tag.size()), tag.data()) != 1) {
        return false;
    }

    // Finalize decryption (verifies tag)
    if (EVP_DecryptFinal_ex(ctx.get(), plaintext.data() + len, &len) != 1) {
        return false;
    }
    plaintext_len += len;

    // Resize to actual size
    plaintext.resize(plaintext_len);
    data = std::move(plaintext);
    return true;
}

} // namespace geocoin::crypto
