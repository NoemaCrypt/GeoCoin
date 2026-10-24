#include "geocoin/core/transaction_codec.hpp"
#include "geocoin/core/transaction_internal.hpp"

#include "geocoin/crypto/geo_area.hpp"
#include "geocoin/crypto/signatures.hpp"
#include "geocoin/nft/nft_types.hpp"

#include <stdexcept>

namespace geocoin::core::tx_codec {

void append_nft_payload(bytes& data, const Transaction& tx) {
    bytes payload;
    if (tx.type == TransactionType::NFT_CREATE) {
        if (!tx.nft_create.has_value()) {
            throw std::runtime_error("NFT_CREATE missing payload");
        }
        payload = tx.nft_create->serialize();
    } else if (tx.type == TransactionType::NFT_TRANSFER) {
        if (!tx.nft_transfer.has_value()) {
            throw std::runtime_error("NFT_TRANSFER missing payload");
        }
        payload = tx.nft_transfer->serialize();
    } else if (tx.type == TransactionType::NFT_BURN) {
        if (!tx.nft_burn.has_value()) {
            throw std::runtime_error("NFT_BURN missing payload");
        }
        payload = tx.nft_burn->serialize();
    } else {
        throw std::runtime_error("NFT payload requested for non-NFT transaction");
    }
    auto len = utils::encode_varint(payload.size());
    data.insert(data.end(), len.begin(), len.end());
    data.insert(data.end(), payload.begin(), payload.end());
}

void read_nft_payload(Transaction& tx, const bytes& data, size_t& offset) {
    const uint64_t payload_len = read_varint(data, offset);
    if (payload_len > consensus::params::MAX_TX_SIZE) {
        throw std::runtime_error("Invalid transaction data: NFT payload too large");
    }
    require_remaining(data, offset, payload_len);
    bytes payload(data.begin() + offset, data.begin() + offset + payload_len);
    offset += static_cast<size_t>(payload_len);

    if (tx.type == TransactionType::NFT_CREATE) {
        tx.nft_create = nft::NFTCreationTransaction::deserialize(payload);
    } else if (tx.type == TransactionType::NFT_TRANSFER) {
        tx.nft_transfer = nft::NFTTransferTransaction::deserialize(payload);
    } else if (tx.type == TransactionType::NFT_BURN) {
        tx.nft_burn = nft::NFTBurnTransaction::deserialize(payload);
    } else {
        throw std::runtime_error("NFT payload requested for non-NFT transaction");
    }
}

} // namespace geocoin::core::tx_codec

namespace geocoin::core {
namespace {

bool verify_nft_payload_signature(
    const bytes& unsigned_payload,
    const Signature& signature,
    const Address& expected_signer
) {
    Hash256 msg32 = crypto::sha256(unsigned_payload);

    Address recovered;
    if (!crypto::recover_signer_address_from_signed_message(msg32, signature, recovered)) {
        return false;
    }

    if (expected_signer.str().empty()) {
        return false;
    }
    return recovered.str() == expected_signer.str();
}

} // namespace

bool verify_nft_transaction(const Transaction& tx) {
    if (tx.type == TransactionType::NFT_CREATE) {
        if (!tx.nft_create.has_value()) {
            return false;
        }
        if (!verify_all_inputs(tx)) {
            return false;
        }

        // Обычный и PoL варианты используют один полный канонический payload.
        if (tx.nft_create->pol_data.has_value()) {
            const auto& pol = tx.nft_create->pol_data.value();
            if (!pol.is_valid_accuracy() ||
                pol.source_type > static_cast<uint8_t>(nft::PoLSourceType::HYBRID)) {
                return false;
            }
        }

        const bytes unsigned_payload = tx.nft_create->serialize_for_signing();
        return verify_nft_payload_signature(
            unsigned_payload,
            tx.nft_create->signature,
            tx.nft_create->creator_address
        );
    }

    if (tx.type == TransactionType::NFT_TRANSFER) {
        if (!tx.nft_transfer.has_value()) {
            return false;
        }
        if (!verify_all_inputs(tx)) {
            return false;
        }

        const bytes unsigned_payload = tx.nft_transfer->serialize_for_signing();
        return verify_nft_payload_signature(
            unsigned_payload,
            tx.nft_transfer->signature,
            tx.nft_transfer->from
        );
    }

    if (tx.type == TransactionType::NFT_BURN) {
        if (!tx.nft_burn.has_value()) {
            return false;
        }
        if (!verify_all_inputs(tx)) {
            return false;
        }

        const bytes unsigned_payload = tx.nft_burn->serialize_for_signing();
        return verify_nft_payload_signature(
            unsigned_payload,
            tx.nft_burn->signature,
            tx.nft_burn->owner
        );
    }

    return false;
}

} // namespace geocoin::core
