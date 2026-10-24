#pragma once

#include "geocoin/network/secure_channel.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>

namespace geocoin::test {

struct SecureChannelFixture {
    network::SecureChannelKeyMaterial peer_keys;
    std::unique_ptr<network::SecureChannel> remote_channel;
    Hash256 channel_binding{};
};

/**
 * Build deterministic, paired keys for socket-based unit tests.
 *
 * The Peer under test owns peer_keys. The socket harness owns remote_channel,
 * whose send/receive directions are the inverse of the Peer's directions.
 * These keys are deliberately test-only and never enter production code.
 */
inline SecureChannelFixture make_secure_channel_fixture(byte discriminator = 0)
{
    network::SecureChannelKeyMaterial peer_keys;
    network::SecureChannelKeyMaterial remote_keys;

    for (size_t i = 0; i < peer_keys.send_key.size(); ++i) {
        const byte peer_send = static_cast<byte>(0x42U ^ discriminator ^ static_cast<byte>(i));
        const byte remote_send = static_cast<byte>(0x91U ^ discriminator ^ static_cast<byte>(i));
        peer_keys.send_key[i] = peer_send;
        remote_keys.receive_key[i] = peer_send;
        peer_keys.receive_key[i] = remote_send;
        remote_keys.send_key[i] = remote_send;
    }

    std::fill(
        peer_keys.send_nonce_prefix.begin(),
        peer_keys.send_nonce_prefix.end(),
        static_cast<byte>(0x44U ^ discriminator)
    );
    std::fill(
        remote_keys.receive_nonce_prefix.begin(),
        remote_keys.receive_nonce_prefix.end(),
        static_cast<byte>(0x44U ^ discriminator)
    );
    std::fill(
        peer_keys.receive_nonce_prefix.begin(),
        peer_keys.receive_nonce_prefix.end(),
        static_cast<byte>(0x73U ^ discriminator)
    );
    std::fill(
        remote_keys.send_nonce_prefix.begin(),
        remote_keys.send_nonce_prefix.end(),
        static_cast<byte>(0x73U ^ discriminator)
    );

    Hash256 binding{};
    for (size_t i = 0; i < binding.size(); ++i) {
        binding[i] = static_cast<byte>(0xa5U ^ discriminator ^ static_cast<byte>(i));
    }
    peer_keys.channel_binding = binding;
    remote_keys.channel_binding = binding;

    SecureChannelFixture fixture;
    fixture.peer_keys = std::move(peer_keys);
    fixture.remote_channel =
        std::make_unique<network::SecureChannel>(std::move(remote_keys));
    fixture.channel_binding = binding;
    return fixture;
}

} // namespace geocoin::test
