#pragma once

#include "geocoin/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace geocoin::network {

inline constexpr uint16_t kSecureWireVersion = 2;
inline constexpr uint16_t kSecureSuiteHybridMlKem768X25519ChaCha20Poly1305 = 1;
inline constexpr size_t kSecureRecordPrefixSize = 4;
inline constexpr size_t kSecureRecordTagSize = 16;
inline constexpr size_t kSecureRecordMinPlaintext = 13;
inline constexpr size_t kSecureRecordMaxPlaintext = (4 * 1024 * 1024) + 13;

struct SecureChannelKeyMaterial {
    std::array<byte, 32> send_key{};
    std::array<byte, 32> receive_key{};
    std::array<byte, 4> send_nonce_prefix{};
    std::array<byte, 4> receive_nonce_prefix{};
    Hash256 channel_binding{};

    SecureChannelKeyMaterial() = default;
    ~SecureChannelKeyMaterial();
    SecureChannelKeyMaterial(const SecureChannelKeyMaterial&) = delete;
    SecureChannelKeyMaterial& operator=(const SecureChannelKeyMaterial&) = delete;
    SecureChannelKeyMaterial(SecureChannelKeyMaterial&& other) noexcept;
    SecureChannelKeyMaterial& operator=(SecureChannelKeyMaterial&& other) noexcept;
};

enum class SecureRecordSizeStatus {
    INCOMPLETE,
    READY,
    INVALID
};

/**
 * Ordered ChaCha20-Poly1305 record layer for one TCP peer session.
 *
 * Nonces are implicit (per-direction prefix + monotonic 64-bit counter), so
 * they consume no wire bytes and cannot be supplied by an attacker. One
 * encrypted record contains exactly one legacy P2P frame.
 */
class SecureChannel final {
public:
    explicit SecureChannel(SecureChannelKeyMaterial key_material);
    ~SecureChannel();

    SecureChannel(const SecureChannel&) = delete;
    SecureChannel& operator=(const SecureChannel&) = delete;

    bool protect(const bytes& plaintext_frame, bytes& record);
    bool unprotect(const bytes& record, bytes& plaintext_frame);

    uint64_t sent_records() const noexcept { return send_counter_; }
    uint64_t received_records() const noexcept { return receive_counter_; }
    const Hash256& channel_binding() const noexcept { return channel_binding_; }

    static SecureRecordSizeStatus inspect_record_size(
        const byte* data,
        size_t available,
        size_t& total_record_size
    ) noexcept;

private:
    std::array<byte, 32> send_key_{};
    std::array<byte, 32> receive_key_{};
    std::array<byte, 4> send_nonce_prefix_{};
    std::array<byte, 4> receive_nonce_prefix_{};
    Hash256 channel_binding_{};
    uint64_t send_counter_ = 0;
    uint64_t receive_counter_ = 0;
};

} // namespace geocoin::network
