#pragma once

#include "geocoin/core/transaction.hpp"
#include "geocoin/consensus/consensus.hpp"
#include "geocoin/utils/bytes.hpp"
#include "geocoin/utils/varint.hpp"

#include <stdexcept>

namespace geocoin::core::tx_codec {

inline void append_varbytes(bytes& out, const std::string& value) {
    const auto size = utils::encode_varint(value.size());
    out.insert(out.end(), size.begin(), size.end());
    out.insert(out.end(), value.begin(), value.end());
}

inline void append_len_prefixed_bytes(bytes& out, const bytes& value) {
    const auto size = utils::encode_varint(value.size());
    out.insert(out.end(), size.begin(), size.end());
    out.insert(out.end(), value.begin(), value.end());
}

inline void append_committee_config(
    bytes& out,
    uint8_t m,
    uint8_t n,
    const std::vector<Address>& members
) {
    out.push_back(m);
    out.push_back(n);
    const auto count = utils::encode_varint(members.size());
    out.insert(out.end(), count.begin(), count.end());
    for (const auto& a : members) {
        append_varbytes(out, a.str());
    }
}

inline void append_signature_list(bytes& out, const std::vector<Signature>& signatures) {
    const auto sig_count = utils::encode_varint(signatures.size());
    out.insert(out.end(), sig_count.begin(), sig_count.end());
    for (const auto& sig : signatures) {
        const auto slen = utils::encode_varint(sig.size());
        out.insert(out.end(), slen.begin(), slen.end());
        if (!sig.empty()) {
            out.insert(out.end(), sig.begin(), sig.end());
        }
    }
}

inline void require_remaining(const bytes& data, size_t offset, uint64_t len) {
    if (len > data.size() - offset) {
        throw std::runtime_error("Invalid transaction data");
    }
}

inline uint64_t read_varint(const bytes& data, size_t& offset) {
    return utils::decode_varint(data, offset);
}

void append_card_payload(bytes& data, const Transaction& tx);
void read_card_payload(Transaction& tx, const bytes& data, size_t& offset);
void append_sidechain_payload(bytes& data, const Transaction& tx);
void read_sidechain_payload(Transaction& tx, const bytes& data, size_t& offset);
void append_nft_payload(bytes& data, const Transaction& tx);
void read_nft_payload(Transaction& tx, const bytes& data, size_t& offset);
void append_foundation_payload(bytes& data, const Transaction& tx);
void read_foundation_payload(Transaction& tx, const bytes& data, size_t& offset);
void append_multisig_payload(bytes& data, const Transaction& tx);
void read_multisig_payload(Transaction& tx, const bytes& data, size_t& offset);
void append_cadastral_payload(bytes& data, const Transaction& tx);
void read_cadastral_payload(Transaction& tx, const bytes& data, size_t& offset);

} // namespace geocoin::core::tx_codec
