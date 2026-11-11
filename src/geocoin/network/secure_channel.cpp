#include "geocoin/network/secure_channel.hpp"

#include "geocoin/utils/secure_memory.hpp"

#include <openssl/crypto.h>
#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <memory>

namespace geocoin::network {

namespace {

constexpr char kAadDomain[] = "GEOCOIN-P2P-RECORD-v2";

using CipherContext = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>;

void write_le32(byte* out, uint32_t value) noexcept {
    out[0] = static_cast<byte>(value & 0xffU);
    out[1] = static_cast<byte>((value >> 8U) & 0xffU);
    out[2] = static_cast<byte>((value >> 16U) & 0xffU);
    out[3] = static_cast<byte>((value >> 24U) & 0xffU);
}

uint32_t read_le32(const byte* in) noexcept {
    return static_cast<uint32_t>(in[0]) |
           (static_cast<uint32_t>(in[1]) << 8U) |
           (static_cast<uint32_t>(in[2]) << 16U) |
           (static_cast<uint32_t>(in[3]) << 24U);
}

std::array<byte, 12> make_nonce(const std::array<byte, 4>& prefix, uint64_t counter) {
    std::array<byte, 12> nonce{};
    std::copy(prefix.begin(), prefix.end(), nonce.begin());
    for (size_t i = 0; i < 8; ++i) {
        nonce[4 + i] = static_cast<byte>((counter >> ((7U - i) * 8U)) & 0xffU);
    }
    return nonce;
}

bytes make_aad(
    const Hash256& channel_binding,
    const byte* length_prefix,
    uint64_t counter
) {
    bytes aad;
    aad.reserve(sizeof(kAadDomain) - 1 + channel_binding.size() + 4 + 8);
    aad.insert(aad.end(), kAadDomain, kAadDomain + sizeof(kAadDomain) - 1);
    aad.insert(aad.end(), channel_binding.begin(), channel_binding.end());
    aad.insert(aad.end(), length_prefix, length_prefix + 4);
    for (size_t i = 0; i < 8; ++i) {
        aad.push_back(static_cast<byte>((counter >> ((7U - i) * 8U)) & 0xffU));
    }
    return aad;
}

void cleanse_key_material(SecureChannelKeyMaterial& material) noexcept {
    OPENSSL_cleanse(material.send_key.data(), material.send_key.size());
    OPENSSL_cleanse(material.receive_key.data(), material.receive_key.size());
    OPENSSL_cleanse(material.send_nonce_prefix.data(), material.send_nonce_prefix.size());
    OPENSSL_cleanse(material.receive_nonce_prefix.data(), material.receive_nonce_prefix.size());
}

} // namespace

SecureChannelKeyMaterial::~SecureChannelKeyMaterial() {
    cleanse_key_material(*this);
}

SecureChannelKeyMaterial::SecureChannelKeyMaterial(
    SecureChannelKeyMaterial&& other
) noexcept
    : send_key(other.send_key)
    , receive_key(other.receive_key)
    , send_nonce_prefix(other.send_nonce_prefix)
    , receive_nonce_prefix(other.receive_nonce_prefix)
    , channel_binding(other.channel_binding) {
    cleanse_key_material(other);
}

SecureChannelKeyMaterial& SecureChannelKeyMaterial::operator=(
    SecureChannelKeyMaterial&& other
) noexcept {
    if (this != &other) {
        cleanse_key_material(*this);
        send_key = other.send_key;
        receive_key = other.receive_key;
        send_nonce_prefix = other.send_nonce_prefix;
        receive_nonce_prefix = other.receive_nonce_prefix;
        channel_binding = other.channel_binding;
        cleanse_key_material(other);
    }
    return *this;
}

SecureChannel::SecureChannel(SecureChannelKeyMaterial key_material)
    : send_key_(key_material.send_key)
    , receive_key_(key_material.receive_key)
    , send_nonce_prefix_(key_material.send_nonce_prefix)
    , receive_nonce_prefix_(key_material.receive_nonce_prefix)
    , channel_binding_(key_material.channel_binding) {}

SecureChannel::~SecureChannel() {
    OPENSSL_cleanse(send_key_.data(), send_key_.size());
    OPENSSL_cleanse(receive_key_.data(), receive_key_.size());
    OPENSSL_cleanse(send_nonce_prefix_.data(), send_nonce_prefix_.size());
    OPENSSL_cleanse(receive_nonce_prefix_.data(), receive_nonce_prefix_.size());
}

bool SecureChannel::protect(const bytes& plaintext_frame, bytes& record) {
    record.clear();
    if (plaintext_frame.size() < kSecureRecordMinPlaintext ||
        plaintext_frame.size() > kSecureRecordMaxPlaintext ||
        plaintext_frame.size() > static_cast<size_t>((std::numeric_limits<int>::max)()) ||
        send_counter_ == (std::numeric_limits<uint64_t>::max)()) {
        return false;
    }

    const size_t protected_size = plaintext_frame.size() + kSecureRecordTagSize;
    if (protected_size > static_cast<size_t>((std::numeric_limits<uint32_t>::max)())) {
        return false;
    }

    bytes output(kSecureRecordPrefixSize + protected_size, 0);
    write_le32(output.data(), static_cast<uint32_t>(protected_size));
    const auto nonce = make_nonce(send_nonce_prefix_, send_counter_);
    const bytes aad = make_aad(channel_binding_, output.data(), send_counter_);

    CipherContext context(EVP_CIPHER_CTX_new(), &EVP_CIPHER_CTX_free);
    if (!context ||
        EVP_EncryptInit_ex(context.get(), EVP_chacha20_poly1305(), nullptr, nullptr, nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(
            context.get(), EVP_CTRL_AEAD_SET_IVLEN, static_cast<int>(nonce.size()), nullptr) != 1 ||
        EVP_EncryptInit_ex(
            context.get(), nullptr, nullptr, send_key_.data(), nonce.data()) != 1) {
        return false;
    }

    int written = 0;
    if (EVP_EncryptUpdate(
            context.get(), nullptr, &written, aad.data(), static_cast<int>(aad.size())) != 1) {
        return false;
    }

    int ciphertext_size = 0;
    if (EVP_EncryptUpdate(
            context.get(),
            output.data() + kSecureRecordPrefixSize,
            &written,
            plaintext_frame.data(),
            static_cast<int>(plaintext_frame.size())) != 1) {
        return false;
    }
    ciphertext_size = written;
    if (EVP_EncryptFinal_ex(
            context.get(),
            output.data() + kSecureRecordPrefixSize + ciphertext_size,
            &written) != 1) {
        return false;
    }
    ciphertext_size += written;
    if (static_cast<size_t>(ciphertext_size) != plaintext_frame.size()) {
        return false;
    }

    if (EVP_CIPHER_CTX_ctrl(
            context.get(),
            EVP_CTRL_AEAD_GET_TAG,
            static_cast<int>(kSecureRecordTagSize),
            output.data() + kSecureRecordPrefixSize + ciphertext_size) != 1) {
        return false;
    }

    ++send_counter_;
    record = std::move(output);
    return true;
}

bool SecureChannel::unprotect(const bytes& record, bytes& plaintext_frame) {
    utils::cleanse_container(plaintext_frame);
    size_t expected_size = 0;
    if (inspect_record_size(record.data(), record.size(), expected_size) !=
            SecureRecordSizeStatus::READY ||
        expected_size != record.size() ||
        receive_counter_ == (std::numeric_limits<uint64_t>::max)()) {
        return false;
    }

    const size_t protected_size = record.size() - kSecureRecordPrefixSize;
    const size_t ciphertext_size = protected_size - kSecureRecordTagSize;
    if (ciphertext_size > static_cast<size_t>((std::numeric_limits<int>::max)())) {
        return false;
    }

    const auto nonce = make_nonce(receive_nonce_prefix_, receive_counter_);
    const bytes aad = make_aad(channel_binding_, record.data(), receive_counter_);
    CipherContext context(EVP_CIPHER_CTX_new(), &EVP_CIPHER_CTX_free);
    if (!context ||
        EVP_DecryptInit_ex(context.get(), EVP_chacha20_poly1305(), nullptr, nullptr, nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(
            context.get(), EVP_CTRL_AEAD_SET_IVLEN, static_cast<int>(nonce.size()), nullptr) != 1 ||
        EVP_DecryptInit_ex(
            context.get(), nullptr, nullptr, receive_key_.data(), nonce.data()) != 1) {
        return false;
    }

    int written = 0;
    if (EVP_DecryptUpdate(
            context.get(), nullptr, &written, aad.data(), static_cast<int>(aad.size())) != 1) {
        return false;
    }

    bytes plaintext(ciphertext_size, 0);
    utils::ScopedContainerCleanser<bytes> plaintext_cleanser(plaintext);
    int plaintext_size = 0;
    if (EVP_DecryptUpdate(
            context.get(),
            plaintext.data(),
            &written,
            record.data() + kSecureRecordPrefixSize,
            static_cast<int>(ciphertext_size)) != 1) {
        return false;
    }
    plaintext_size = written;

    void* tag = const_cast<byte*>(
        record.data() + kSecureRecordPrefixSize + ciphertext_size
    );
    if (EVP_CIPHER_CTX_ctrl(
            context.get(),
            EVP_CTRL_AEAD_SET_TAG,
            static_cast<int>(kSecureRecordTagSize),
            tag) != 1 ||
        EVP_DecryptFinal_ex(context.get(), plaintext.data() + plaintext_size, &written) != 1) {
        return false;
    }
    plaintext_size += written;
    if (plaintext_size < 0 || static_cast<size_t>(plaintext_size) != ciphertext_size) {
        return false;
    }

    ++receive_counter_;
    plaintext.resize(static_cast<size_t>(plaintext_size));
    plaintext_frame = std::move(plaintext);
    return true;
}

SecureRecordSizeStatus SecureChannel::inspect_record_size(
    const byte* data,
    size_t available,
    size_t& total_record_size
) noexcept {
    total_record_size = 0;
    if (available < kSecureRecordPrefixSize) {
        return SecureRecordSizeStatus::INCOMPLETE;
    }
    if (data == nullptr) {
        return SecureRecordSizeStatus::INVALID;
    }

    const uint32_t protected_size = read_le32(data);
    if (protected_size < kSecureRecordTagSize + kSecureRecordMinPlaintext ||
        protected_size > kSecureRecordMaxPlaintext + kSecureRecordTagSize) {
        return SecureRecordSizeStatus::INVALID;
    }
    total_record_size = kSecureRecordPrefixSize + static_cast<size_t>(protected_size);
    return available < total_record_size
        ? SecureRecordSizeStatus::INCOMPLETE
        : SecureRecordSizeStatus::READY;
}

} // namespace geocoin::network
