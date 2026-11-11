#pragma once

#include "geocoin/network/secure_channel.hpp"
#include "geocoin/types.hpp"

#include <memory>

namespace geocoin::network {

struct SecureHelloData {
    bytes x25519_public_key;
    bytes ml_kem_public_key;
    bytes nonce;
};

struct SecureReplyData {
    bytes x25519_public_key;
    bytes ml_kem_ciphertext;
    bytes nonce;
};

/** Hybrid ephemeral pre-handshake. No node identity is exposed here. */
class SecureHandshake final {
public:
    ~SecureHandshake();

    SecureHandshake(const SecureHandshake&) = delete;
    SecureHandshake& operator=(const SecureHandshake&) = delete;

    static std::unique_ptr<SecureHandshake> create_initiator();
    static std::unique_ptr<SecureHandshake> create_responder(const SecureHelloData& hello);

    bool is_initiator() const noexcept { return initiator_; }
    const SecureHelloData& hello() const noexcept { return hello_; }
    const SecureReplyData& reply() const noexcept { return reply_; }

    bool complete_initiator(
        const SecureReplyData& reply,
        uint32_t network_magic,
        const bytes& hello_payload,
        const bytes& reply_payload
    );

    bool complete_responder(
        uint32_t network_magic,
        const bytes& hello_payload,
        const bytes& reply_payload
    );

    bool is_complete() const noexcept { return complete_; }
    const Hash256& channel_binding() const noexcept { return channel_binding_; }
    bool take_key_material(SecureChannelKeyMaterial& out);

private:
    explicit SecureHandshake(bool initiator);

    bool initialize_ephemeral_x25519();
    bool derive_keys(
        uint32_t network_magic,
        const bytes& hello_payload,
        const bytes& reply_payload
    );

    bool initiator_ = false;
    bool complete_ = false;
    bool keys_taken_ = false;
    SecureHelloData hello_;
    SecureReplyData reply_;
    secure_bytes x25519_private_key_;
    secure_bytes ml_kem_secret_key_;
    secure_bytes ml_kem_shared_secret_;
    bytes remote_x25519_public_key_;
    Hash256 channel_binding_{};
    SecureChannelKeyMaterial key_material_{};
};

} // namespace geocoin::network
