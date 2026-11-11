#include "geocoin/core/transaction_codec.hpp"
#include "geocoin/core/transaction_internal.hpp"

#include "geocoin/crypto/geo_area.hpp"
#include "geocoin/crypto/signatures.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace geocoin::core::tx_codec {

void append_foundation_payload(bytes& data, const Transaction& tx) {
    append_varbytes(data, tx.foundation_old_address.str());
    append_varbytes(data, tx.foundation_new_address.str());
    append_committee_config(
        data,
        tx.foundation_m,
        tx.foundation_n,
        tx.foundation_participants
    );
    append_committee_config(
        data,
        tx.foundation_new_m,
        tx.foundation_new_n,
        tx.foundation_new_participants
    );
    append_signature_list(data, tx.foundation_signatures);
}

void read_foundation_payload(Transaction& tx, const bytes& data, size_t& offset) {
    uint64_t old_len = read_varint(data, offset);
    if (old_len > consensus::params::MAX_ADDRESS_SIZE) {
        throw std::runtime_error("Invalid transaction data: foundation old address too long");
    }
    require_remaining(data, offset, old_len);
    if (old_len > 0) {
        std::string old_str(data.begin() + offset, data.begin() + offset + old_len);
        tx.foundation_old_address = Address(old_str);
        offset += static_cast<size_t>(old_len);
    }

    uint64_t new_len = read_varint(data, offset);
    if (new_len > consensus::params::MAX_ADDRESS_SIZE) {
        throw std::runtime_error("Invalid transaction data: foundation new address too long");
    }
    require_remaining(data, offset, new_len);
    if (new_len > 0) {
        std::string new_str(data.begin() + offset, data.begin() + offset + new_len);
        tx.foundation_new_address = Address(new_str);
        offset += static_cast<size_t>(new_len);
    }

    require_remaining(data, offset, 2);
    tx.foundation_m = data[offset++];
    tx.foundation_n = data[offset++];

    uint64_t cur_count = read_varint(data, offset);
    if (cur_count > 255) {
        throw std::runtime_error("Invalid transaction data: foundation participant count too large");
    }
    tx.foundation_participants.clear();
    for (uint64_t i = 0; i < cur_count; i++) {
        uint64_t alen = read_varint(data, offset);
        if (alen > consensus::params::MAX_ADDRESS_SIZE) {
            throw std::runtime_error("Invalid transaction data: foundation participant address too long");
        }
        require_remaining(data, offset, alen);
        std::string a(data.begin() + offset, data.begin() + offset + alen);
        tx.foundation_participants.emplace_back(a);
        offset += static_cast<size_t>(alen);
    }

    require_remaining(data, offset, 2);
    tx.foundation_new_m = data[offset++];
    tx.foundation_new_n = data[offset++];

    uint64_t new_count = read_varint(data, offset);
    if (new_count > 255) {
        throw std::runtime_error("Invalid transaction data: foundation new participant count too large");
    }
    tx.foundation_new_participants.clear();
    for (uint64_t i = 0; i < new_count; i++) {
        uint64_t alen = read_varint(data, offset);
        if (alen > consensus::params::MAX_ADDRESS_SIZE) {
            throw std::runtime_error("Invalid transaction data: foundation new participant address too long");
        }
        require_remaining(data, offset, alen);
        std::string a(data.begin() + offset, data.begin() + offset + alen);
        tx.foundation_new_participants.emplace_back(a);
        offset += static_cast<size_t>(alen);
    }

    uint64_t sig_count = read_varint(data, offset);
    if (sig_count > 255) {
        throw std::runtime_error("Invalid transaction data: foundation signature count too large");
    }
    tx.foundation_signatures.clear();
    for (uint64_t i = 0; i < sig_count; i++) {
        uint64_t slen = read_varint(data, offset);
        if (slen > consensus::params::MAX_SIGNATURE_SIZE) {
            throw std::runtime_error("Invalid transaction data: foundation signature too large");
        }
        require_remaining(data, offset, slen);
        Signature sig;
        sig.resize(static_cast<size_t>(slen));
        if (slen > 0) {
            std::copy(data.begin() + offset, data.begin() + offset + slen, sig.begin());
            offset += static_cast<size_t>(slen);
        }
        tx.foundation_signatures.push_back(sig);
    }
}

} // namespace geocoin::core::tx_codec

namespace geocoin::core {

bytes build_foundation_update_payload_for_signing(const Transaction& tx) {
    return serialize_protocol_authorization(tx);
}

bool verify_foundation_transaction(const Transaction& tx) {
    if (tx.foundation_signatures.empty()) {
        return false;
    }
    bytes payload = build_foundation_update_payload_for_signing(tx);
    Hash256 msg32 = crypto::sha256(payload);
    for (const auto& sig : tx.foundation_signatures) {
        Address signer;
        if (!crypto::recover_signer_address_from_signed_message(msg32, sig, signer)) {
            return false;
        }
    }
    return true;
}

} // namespace geocoin::core
