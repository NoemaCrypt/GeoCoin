#include "geocoin/network/secure_handshake.hpp"

#include "geocoin/crypto/geo_area.hpp"
#include "geocoin/crypto/ml_kem768.hpp"
#include "geocoin/utils/secure_memory.hpp"

#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/rand.h>
#include <openssl/crypto.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <memory>

namespace geocoin::network {

namespace {

constexpr char kTranscriptDomain[] = "GEOCOIN-P2P-HYBRID-HANDSHAKE-v2";
constexpr char kKeyDomain[] = "GEOCOIN-P2P-CHANNEL-KEYS-v2";
constexpr size_t kHandshakeNonceSize = 32;
constexpr size_t kX25519KeySize = 32;
constexpr size_t kExpandedKeySize = 72;

using Pkey = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using PkeyContext = std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>;

void append_le32(bytes& out, uint32_t value) {
    out.push_back(static_cast<byte>(value & 0xffU));
    out.push_back(static_cast<byte>((value >> 8U) & 0xffU));
    out.push_back(static_cast<byte>((value >> 16U) & 0xffU));
    out.push_back(static_cast<byte>((value >> 24U) & 0xffU));
}

bool generate_x25519(bytes& public_key, secure_bytes& private_key) {
    public_key.clear();
    utils::cleanse_container(private_key);

    PkeyContext context(EVP_PKEY_CTX_new_id(EVP_PKEY_X25519, nullptr), &EVP_PKEY_CTX_free);
    EVP_PKEY* generated_raw = nullptr;
    if (!context || EVP_PKEY_keygen_init(context.get()) != 1 ||
        EVP_PKEY_keygen(context.get(), &generated_raw) != 1) {
        return false;
    }
    Pkey generated(generated_raw, &EVP_PKEY_free);

    public_key.assign(kX25519KeySize, 0);
    private_key.assign(kX25519KeySize, 0);
    size_t public_size = public_key.size();
    size_t private_size = private_key.size();
    if (EVP_PKEY_get_raw_public_key(
            generated.get(), public_key.data(), &public_size) != 1 ||
        EVP_PKEY_get_raw_private_key(
            generated.get(), private_key.data(), &private_size) != 1 ||
        public_size != kX25519KeySize || private_size != kX25519KeySize) {
        public_key.clear();
        utils::cleanse_container(private_key);
        return false;
    }
    return true;
}

bool derive_x25519(
    const secure_bytes& private_key,
    const bytes& remote_public_key,
    secure_bytes& shared_secret
) {
    utils::cleanse_container(shared_secret);
    if (private_key.size() != kX25519KeySize ||
        remote_public_key.size() != kX25519KeySize) {
        return false;
    }

    Pkey local(
        EVP_PKEY_new_raw_private_key(
            EVP_PKEY_X25519, nullptr, private_key.data(), private_key.size()),
        &EVP_PKEY_free
    );
    Pkey remote(
        EVP_PKEY_new_raw_public_key(
            EVP_PKEY_X25519, nullptr, remote_public_key.data(), remote_public_key.size()),
        &EVP_PKEY_free
    );
    PkeyContext context(local ? EVP_PKEY_CTX_new(local.get(), nullptr) : nullptr, &EVP_PKEY_CTX_free);
    if (!local || !remote || !context ||
        EVP_PKEY_derive_init(context.get()) != 1 ||
        EVP_PKEY_derive_set_peer(context.get(), remote.get()) != 1) {
        return false;
    }

    size_t shared_size = 0;
    if (EVP_PKEY_derive(context.get(), nullptr, &shared_size) != 1 ||
        shared_size != kX25519KeySize) {
        return false;
    }
    shared_secret.assign(shared_size, 0);
    if (EVP_PKEY_derive(context.get(), shared_secret.data(), &shared_size) != 1 ||
        shared_size != kX25519KeySize) {
        utils::cleanse_container(shared_secret);
        return false;
    }

    // Reject an all-zero shared secret (low-order remote public key). Some
    // OpenSSL builds still return success here, which would silently drop the
    // X25519 contribution and leave the session relying on ML-KEM alone despite
    // the advertised hybrid. Compare in constant time before use.
    unsigned char zero_flag = 0;
    for (const auto byte_value : shared_secret) {
        zero_flag |= static_cast<unsigned char>(byte_value);
    }
    if (zero_flag == 0) {
        utils::cleanse_container(shared_secret);
        return false;
    }
    return true;
}

Hash256 transcript_hash(
    uint32_t network_magic,
    const bytes& hello_payload,
    const bytes& reply_payload
) {
    bytes transcript;
    transcript.reserve(
        sizeof(kTranscriptDomain) - 1 + 12 + hello_payload.size() + reply_payload.size()
    );
    transcript.insert(
        transcript.end(),
        kTranscriptDomain,
        kTranscriptDomain + sizeof(kTranscriptDomain) - 1
    );
    append_le32(transcript, network_magic);
    append_le32(transcript, static_cast<uint32_t>(hello_payload.size()));
    transcript.insert(transcript.end(), hello_payload.begin(), hello_payload.end());
    append_le32(transcript, static_cast<uint32_t>(reply_payload.size()));
    transcript.insert(transcript.end(), reply_payload.begin(), reply_payload.end());
    return crypto::sha256(transcript);
}

bool hkdf_expand(
    const secure_bytes& input_key_material,
    const Hash256& salt,
    secure_bytes& output
) {
    utils::cleanse_container(output);
    output.assign(kExpandedKeySize, 0);
    PkeyContext context(EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr), &EVP_PKEY_CTX_free);
    if (!context ||
        EVP_PKEY_derive_init(context.get()) != 1 ||
        EVP_PKEY_CTX_set_hkdf_md(context.get(), EVP_sha256()) != 1 ||
        EVP_PKEY_CTX_set1_hkdf_salt(
            context.get(), salt.data(), static_cast<int>(salt.size())) != 1 ||
        EVP_PKEY_CTX_set1_hkdf_key(
            context.get(),
            input_key_material.data(),
            static_cast<int>(input_key_material.size())) != 1 ||
        EVP_PKEY_CTX_add1_hkdf_info(
            context.get(),
            reinterpret_cast<const unsigned char*>(kKeyDomain),
            static_cast<int>(sizeof(kKeyDomain) - 1)) != 1) {
        utils::cleanse_container(output);
        return false;
    }

    size_t output_size = output.size();
    if (EVP_PKEY_derive(context.get(), output.data(), &output_size) != 1 ||
        output_size != kExpandedKeySize) {
        utils::cleanse_container(output);
        return false;
    }
    return true;
}

bool valid_hello(const SecureHelloData& hello) {
    return hello.x25519_public_key.size() == kX25519KeySize &&
           hello.ml_kem_public_key.size() == crypto::ml_kem768::kPublicKeySize &&
           hello.nonce.size() == kHandshakeNonceSize;
}

bool valid_reply(const SecureReplyData& reply) {
    return reply.x25519_public_key.size() == kX25519KeySize &&
           reply.ml_kem_ciphertext.size() == crypto::ml_kem768::kCiphertextSize &&
           reply.nonce.size() == kHandshakeNonceSize;
}

} // namespace

SecureHandshake::SecureHandshake(bool initiator) : initiator_(initiator) {}

SecureHandshake::~SecureHandshake() {
    utils::cleanse_container(x25519_private_key_);
    utils::cleanse_container(ml_kem_secret_key_);
    utils::cleanse_container(ml_kem_shared_secret_);
}

std::unique_ptr<SecureHandshake> SecureHandshake::create_initiator() {
    auto handshake = std::unique_ptr<SecureHandshake>(new SecureHandshake(true));
    if (!handshake->initialize_ephemeral_x25519()) {
        return nullptr;
    }

    crypto::ml_kem768::KeyPair kem_keypair;
    if (!crypto::ml_kem768::generate_keypair(kem_keypair)) {
        return nullptr;
    }
    handshake->hello_.ml_kem_public_key = std::move(kem_keypair.public_key);
    handshake->ml_kem_secret_key_ = std::move(kem_keypair.secret_key);
    handshake->hello_.nonce.assign(kHandshakeNonceSize, 0);
    if (RAND_bytes(
            handshake->hello_.nonce.data(),
            static_cast<int>(handshake->hello_.nonce.size())) != 1) {
        return nullptr;
    }
    return handshake;
}

std::unique_ptr<SecureHandshake> SecureHandshake::create_responder(
    const SecureHelloData& hello
) {
    if (!valid_hello(hello)) {
        return nullptr;
    }
    auto handshake = std::unique_ptr<SecureHandshake>(new SecureHandshake(false));
    handshake->hello_ = hello;
    handshake->remote_x25519_public_key_ = hello.x25519_public_key;
    if (!handshake->initialize_ephemeral_x25519()) {
        return nullptr;
    }
    if (!crypto::ml_kem768::encapsulate(
            hello.ml_kem_public_key,
            handshake->reply_.ml_kem_ciphertext,
            handshake->ml_kem_shared_secret_)) {
        return nullptr;
    }
    handshake->reply_.nonce.assign(kHandshakeNonceSize, 0);
    if (RAND_bytes(
            handshake->reply_.nonce.data(),
            static_cast<int>(handshake->reply_.nonce.size())) != 1) {
        return nullptr;
    }
    return handshake;
}

bool SecureHandshake::initialize_ephemeral_x25519() {
    bytes public_key;
    if (!generate_x25519(public_key, x25519_private_key_)) {
        return false;
    }
    if (initiator_) {
        hello_.x25519_public_key = std::move(public_key);
    } else {
        reply_.x25519_public_key = std::move(public_key);
    }
    return true;
}

bool SecureHandshake::complete_initiator(
    const SecureReplyData& reply,
    uint32_t network_magic,
    const bytes& hello_payload,
    const bytes& reply_payload
) {
    if (!initiator_ || complete_ || !valid_reply(reply)) {
        return false;
    }
    reply_ = reply;
    remote_x25519_public_key_ = reply.x25519_public_key;
    if (!crypto::ml_kem768::decapsulate(
            reply.ml_kem_ciphertext,
            ml_kem_secret_key_,
            ml_kem_shared_secret_)) {
        return false;
    }
    return derive_keys(network_magic, hello_payload, reply_payload);
}

bool SecureHandshake::complete_responder(
    uint32_t network_magic,
    const bytes& hello_payload,
    const bytes& reply_payload
) {
    if (initiator_ || complete_ || !valid_hello(hello_) || !valid_reply(reply_)) {
        return false;
    }
    return derive_keys(network_magic, hello_payload, reply_payload);
}

bool SecureHandshake::derive_keys(
    uint32_t network_magic,
    const bytes& hello_payload,
    const bytes& reply_payload
) {
    if (hello_payload.empty() || reply_payload.empty() ||
        hello_payload.size() > static_cast<size_t>((std::numeric_limits<uint32_t>::max)()) ||
        reply_payload.size() > static_cast<size_t>((std::numeric_limits<uint32_t>::max)())) {
        return false;
    }

    secure_bytes x25519_shared;
    utils::ScopedContainerCleanser<secure_bytes> x25519_cleanser(x25519_shared);
    if (!derive_x25519(
            x25519_private_key_, remote_x25519_public_key_, x25519_shared) ||
        ml_kem_shared_secret_.size() != crypto::ml_kem768::kSharedSecretSize) {
        return false;
    }

    secure_bytes hybrid_secret;
    utils::ScopedContainerCleanser<secure_bytes> hybrid_cleanser(hybrid_secret);
    hybrid_secret.reserve(x25519_shared.size() + ml_kem_shared_secret_.size());
    hybrid_secret.insert(hybrid_secret.end(), x25519_shared.begin(), x25519_shared.end());
    hybrid_secret.insert(
        hybrid_secret.end(), ml_kem_shared_secret_.begin(), ml_kem_shared_secret_.end()
    );

    channel_binding_ = transcript_hash(network_magic, hello_payload, reply_payload);
    secure_bytes expanded;
    utils::ScopedContainerCleanser<secure_bytes> expanded_cleanser(expanded);
    if (!hkdf_expand(hybrid_secret, channel_binding_, expanded)) {
        return false;
    }

    const auto initiator_to_responder_key = expanded.begin();
    const auto responder_to_initiator_key = expanded.begin() + 32;
    const auto initiator_nonce = expanded.begin() + 64;
    const auto responder_nonce = expanded.begin() + 68;
    if (initiator_) {
        std::copy_n(initiator_to_responder_key, 32, key_material_.send_key.begin());
        std::copy_n(responder_to_initiator_key, 32, key_material_.receive_key.begin());
        std::copy_n(initiator_nonce, 4, key_material_.send_nonce_prefix.begin());
        std::copy_n(responder_nonce, 4, key_material_.receive_nonce_prefix.begin());
    } else {
        std::copy_n(responder_to_initiator_key, 32, key_material_.send_key.begin());
        std::copy_n(initiator_to_responder_key, 32, key_material_.receive_key.begin());
        std::copy_n(responder_nonce, 4, key_material_.send_nonce_prefix.begin());
        std::copy_n(initiator_nonce, 4, key_material_.receive_nonce_prefix.begin());
    }
    key_material_.channel_binding = channel_binding_;
    complete_ = true;

    utils::cleanse_container(x25519_private_key_);
    utils::cleanse_container(ml_kem_secret_key_);
    utils::cleanse_container(ml_kem_shared_secret_);
    return true;
}

bool SecureHandshake::take_key_material(SecureChannelKeyMaterial& out) {
    if (!complete_ || keys_taken_) {
        return false;
    }
    out = std::move(key_material_);
    keys_taken_ = true;
    return true;
}

} // namespace geocoin::network
