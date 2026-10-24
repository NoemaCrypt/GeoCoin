#include "geocoin/storage/witness_payload.hpp"
#include "geocoin/core/block.hpp"
#include "geocoin/consensus/consensus.hpp"
#include "geocoin/crypto/geo_area.hpp"
#include "geocoin/utils/bytes.hpp"
#include "geocoin/utils/varint.hpp"

#include <algorithm>
#include <exception>

namespace geocoin::storage {

bytes serialize_witness_payload(const core::Block& block) {
    bytes out;
    auto count_bytes = utils::encode_varint(block.transactions.size());
    out.insert(out.end(), count_bytes.begin(), count_bytes.end());
    for (const auto& tx : block.transactions) {
        tx.append_witness(out);
    }
    return out;
}

bool has_material_witness(const core::Transaction& tx) {
    for (const auto& w : tx.witnesses) {
        if (!w.signature.empty() || !w.public_key.empty()) {
            return true;
        }
    }
    return false;
}

bool apply_witness_payload(core::Block& block, const bytes& payload) {
    try {
        size_t offset = 0;
        uint64_t tx_count = utils::decode_varint(payload, offset);
        if (tx_count != block.transactions.size()) {
            return false;
        }
        // Stage only witness data; a rejected payload must leave the block intact.
        std::vector<std::vector<core::TxWitness>> restored(block.transactions.size());
        for (size_t i = 0; i < block.transactions.size(); ++i) {
            if (offset + 1 > payload.size()) return false;
            const uint8_t witness_version = payload[offset++];
            if (witness_version != consensus::params::WITNESS_FORMAT_VERSION_CURRENT) return false;

            uint64_t wcount = utils::decode_varint(payload, offset);
            const core::Transaction& tx = block.transactions[i];
            if (wcount != tx.inputs.size()) {
                return false;
            }
            restored[i].reserve(wcount);
            for (uint64_t w = 0; w < wcount; ++w) {
                core::TxWitness wit;
                if (offset + 32 > payload.size()) return false;
                std::copy(payload.begin() + offset, payload.begin() + offset + 32, wit.public_key_hash.begin());
                offset += 32;
                if (offset + 1 > payload.size()) return false;
                const uint8_t flags = payload[offset++];
                if ((flags & 0xFEu) != 0) return false;
                if ((flags & 0x01u) != 0) {
                    if (offset + consensus::params::WITNESS_PUBKEY_SIZE > payload.size()) return false;
                    wit.public_key.assign(
                        payload.begin() + offset,
                        payload.begin() + offset + consensus::params::WITNESS_PUBKEY_SIZE
                    );
                    offset += consensus::params::WITNESS_PUBKEY_SIZE;
                    if (crypto::sha256(wit.public_key) != wit.public_key_hash) {
                        return false;
                    }
                }
                uint64_t sig_len = utils::decode_varint(payload, offset);
                if (sig_len > consensus::params::MAX_WITNESS_SIG_SIZE) {
                    return false;
                }
                if (offset + sig_len > payload.size()) return false;
                wit.signature.assign(payload.begin() + offset, payload.begin() + offset + sig_len);
                offset += sig_len;

                if (tx.type == core::TransactionType::ANGLE_COMMITMENT) {
                    // ANGLE carries its signature in the transaction body;
                    // its witness slot is always empty.
                    if (!utils::is_null_hash256(wit.public_key_hash) ||
                        !wit.public_key.empty() || !wit.signature.empty()) {
                        return false;
                    }
                } else if (!tx.is_coinbase()) {
                    const auto& expected_sig_hash = tx.inputs[w].signature_hash;
                    if (!utils::is_null_hash256(expected_sig_hash) &&
                        crypto::sha256(wit.signature) != expected_sig_hash) {
                        return false;
                    }
                }

                restored[i].push_back(std::move(wit));
            }
        }
        if (offset != payload.size()) return false;
        for (size_t i = 0; i < block.transactions.size(); ++i) {
            block.transactions[i].witness_version = consensus::params::WITNESS_FORMAT_VERSION_CURRENT;
            block.transactions[i].witnesses = std::move(restored[i]);
        }
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

} // namespace geocoin::storage
