#include "geocoin/crypto/signatures.hpp"
#include "geocoin/crypto/addresses.hpp"
#include "geocoin/crypto/geo_area.hpp"
#include "geocoin/crypto/falcon512.hpp"
#include <array>
#include <cstring>
#include <stdexcept>

namespace geocoin::crypto {

namespace {

constexpr std::array<uint8_t, 4> kFalconMagic{{'F', '5', '1', '2'}};
constexpr size_t kFalconSigHeaderSize = 4 + 2 + 2; // magic + pub_len + sig_len
constexpr size_t kFalconSigMinSize = kFalconSigHeaderSize + 2;
constexpr size_t kFalconSigMaxSize = 4096;

inline void write_u16_be(uint8_t out[2], uint16_t v) {
    out[0] = static_cast<uint8_t>((v >> 8) & 0xFF);
    out[1] = static_cast<uint8_t>(v & 0xFF);
}

inline uint16_t read_u16_be(const uint8_t in[2]) {
    return static_cast<uint16_t>((static_cast<uint16_t>(in[0]) << 8) | in[1]);
}

Address address_from_falcon_public_key(const bytes& falcon_public_key) {
    Hash256 s = sha256(falcon_public_key);
    bytes s_bytes(s.begin(), s.end());
    Hash160 r = ripemd160(s_bytes);

    bytes version_hash;
    version_hash.push_back(0x00);
    version_hash.insert(version_hash.end(), r.begin(), r.end());

    Hash256 checksum_hash = double_sha256(version_hash);

    bytes full = version_hash;
    full.insert(full.end(), checksum_hash.begin(), checksum_hash.begin() + 4);

    return Address("GEO" + base58_encode(full));
}

Signature pack_falcon_signature(
    const bytes& public_key,
    const bytes& signature
) {
    if (public_key.size() > 0xFFFF || signature.size() > 0xFFFF) {
        return {};
    }

    Signature out(kFalconSigHeaderSize + public_key.size() + signature.size(), 0);
    size_t off = 0;

    std::memcpy(out.data() + off, kFalconMagic.data(), kFalconMagic.size());
    off += kFalconMagic.size();

    uint8_t pub_len_be[2];
    write_u16_be(pub_len_be, static_cast<uint16_t>(public_key.size()));
    std::memcpy(out.data() + off, pub_len_be, 2);
    off += 2;

    uint8_t sig_len_be[2];
    write_u16_be(sig_len_be, static_cast<uint16_t>(signature.size()));
    std::memcpy(out.data() + off, sig_len_be, 2);
    off += 2;

    std::memcpy(out.data() + off, public_key.data(), public_key.size());
    off += public_key.size();
    std::memcpy(out.data() + off, signature.data(), signature.size());

    return out;
}

bool unpack_falcon_signature(
    const Signature& payload,
    bytes& out_public_key,
    bytes& out_signature
) {
    out_public_key.clear();
    out_signature.clear();

    if (payload.size() < kFalconSigMinSize || payload.size() > kFalconSigMaxSize) {
        return false;
    }

    size_t off = 0;
    if (!std::equal(kFalconMagic.begin(), kFalconMagic.end(), payload.begin())) {
        return false;
    }
    off += kFalconMagic.size();

    const uint16_t pub_len = read_u16_be(payload.data() + off);
    off += 2;
    const uint16_t sig_len = read_u16_be(payload.data() + off);
    off += 2;

    if (pub_len == 0 || sig_len == 0) {
        return false;
    }
    if (off + pub_len + sig_len != payload.size()) {
        return false;
    }

    out_public_key.assign(payload.begin() + static_cast<ptrdiff_t>(off),
                          payload.begin() + static_cast<ptrdiff_t>(off + pub_len));
    off += pub_len;
    out_signature.assign(payload.begin() + static_cast<ptrdiff_t>(off),
                         payload.begin() + static_cast<ptrdiff_t>(off + sig_len));

    return true;
}

bool sign_with_msg32(
    const Hash256& msg32,
    const bytes& private_key,
    Signature& out
) {
    bytes pub;
    bytes sig;
    if (!falcon512::sign_digest32(msg32, private_key, pub, sig)) {
        return false;
    }
    out = pack_falcon_signature(pub, sig);
    return !out.empty();
}

bool verify_with_msg32(const Hash256& msg32, const Signature& signature) {
    bytes pub;
    bytes sig;
    if (!unpack_falcon_signature(signature, pub, sig)) {
        return false;
    }
    return falcon512::verify_digest32(msg32, pub, sig);
}

bool recover_with_msg32(
    const Hash256& msg32,
    const Signature& signature,
    Address& out_address
) {
    bytes pub;
    bytes sig;
    if (!unpack_falcon_signature(signature, pub, sig)) {
        return false;
    }
    if (!falcon512::verify_digest32(msg32, pub, sig)) {
        return false;
    }
    out_address = address_from_falcon_public_key(pub);
    return true;
}

Hash256 standard_input_message_hash(
    const Hash256& tx_hash,
    size_t input_index
) {
    bytes data;
    data.reserve(32 + 8);
    data.insert(data.end(), tx_hash.begin(), tx_hash.end());

    const uint64_t idx = static_cast<uint64_t>(input_index);
    for (int i = 0; i < 8; ++i) {
        data.push_back(static_cast<byte>((idx >> (i * 8)) & 0xFF));
    }

    return sha256(data);
}

} // namespace

bool has_canonical_signature_encoding(const Signature& signature) {
    if (signature.empty()) {
        return true;
    }
    if (signature.size() >= kFalconMagic.size() &&
        std::equal(kFalconMagic.begin(), kFalconMagic.end(), signature.begin())) {
        bytes pub;
        bytes sig;
        return unpack_falcon_signature(signature, pub, sig) &&
               falcon512::is_canonical_signature_encoding(sig);
    }
    return falcon512::is_canonical_signature_encoding(signature);
}

Signature sign_transaction_input(
    const Hash256& tx_hash,
    size_t input_index,
    const bytes& private_key
) {
    const Hash256 msg = standard_input_message_hash(tx_hash, input_index);

    Signature out;
    if (!sign_with_msg32(msg, private_key, out)) {
        throw std::runtime_error("Не удалось создать подпись Falcon-512 для входа транзакции");
    }
    return out;
}

bool sign_transaction_input_detached(
    const Hash256& tx_hash,
    size_t input_index,
    const bytes& private_key,
    bytes& out_public_key,
    Signature& out_signature
) {
    out_public_key.clear();
    out_signature.clear();

    const Hash256 msg = standard_input_message_hash(tx_hash, input_index);

    bytes pub;
    bytes sig;
    if (!falcon512::sign_digest32(msg, private_key, pub, sig)) {
        return false;
    }

    out_public_key = std::move(pub);
    out_signature = std::move(sig);
    return !out_public_key.empty() && !out_signature.empty();
}

Signature sign_recoverable_message(
    const Hash256& msg32,
    const bytes& private_key
) {
    Signature out;
    if (!sign_with_msg32(msg32, private_key, out)) {
        throw std::runtime_error("Не удалось создать подпись Falcon-512 для сообщения");
    }
    return out;
}

Signature sign_angle_commitment(
    const Hash256& spending_tx_hash,
    const bytes& private_key
) {
    Signature out;
    if (!sign_with_msg32(spending_tx_hash, private_key, out)) {
        throw std::runtime_error("Не удалось создать подпись Falcon-512 для angle commitment");
    }
    return out;
}

bool verify_transaction_input(
    const Hash256& tx_hash,
    size_t input_index,
    const Signature& signature
) {
    const Hash256 msg = standard_input_message_hash(tx_hash, input_index);
    return verify_with_msg32(msg, signature);
}

bool verify_transaction_input_detached(
    const Hash256& tx_hash,
    size_t input_index,
    const bytes& public_key,
    const Signature& signature
) {
    const Hash256 msg = standard_input_message_hash(tx_hash, input_index);
    return falcon512::verify_digest32(msg, public_key, signature);
}

bool verify_angle_commitment_signature(
    const Hash256& spending_tx_hash,
    const Signature& signature
) {
    return verify_with_msg32(spending_tx_hash, signature);
}

bool verify_signed_message(
    const Hash256& msg32,
    const Signature& signature
) {
    return verify_with_msg32(msg32, signature);
}

bool recover_signer_address_from_signed_message(
    const Hash256& msg32,
    const Signature& signature,
    Address& out_address
) {
    return recover_with_msg32(msg32, signature, out_address);
}

bool recover_signer_address_from_transaction_input(
    const Hash256& tx_hash,
    size_t input_index,
    const Signature& signature,
    Address& out_address
) {
    const Hash256 msg = standard_input_message_hash(tx_hash, input_index);
    return recover_with_msg32(msg, signature, out_address);
}

bool address_from_public_key(
    const bytes& public_key,
    Address& out_address
) {
    if (public_key.empty()) {
        return false;
    }
    out_address = address_from_falcon_public_key(public_key);
    return true;
}

bool derive_identity_address_from_private_key(
    const bytes& private_key,
    Address& out_address
) {
    bytes pub;
    if (!falcon512::public_key_from_seed32(private_key, pub)) {
        return false;
    }
    out_address = address_from_falcon_public_key(pub);
    return true;
}

bool recover_signer_address_from_angle_commitment(
    const Hash256& spending_tx_hash,
    const Signature& signature,
    Address& out_address
) {
    return recover_with_msg32(spending_tx_hash, signature, out_address);
}

} // namespace geocoin::crypto
