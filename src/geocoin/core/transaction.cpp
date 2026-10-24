#include "geocoin/core/transaction.hpp"
#include "geocoin/crypto/geo_area.hpp"
#include <stdexcept>

namespace geocoin::core {

bytes serialize_protocol_authorization(const Transaction& tx) {
    tx.require_canonical_payload();
    Transaction unsigned_tx = tx;
    unsigned_tx.sidechain_signature.clear();
    unsigned_tx.sidechain_committee_signatures.clear();
    unsigned_tx.foundation_signatures.clear();
    for (auto& input : unsigned_tx.inputs) input.signature_hash = Hash256{};
    return unsigned_tx.serialize_without_witness();
}

std::optional<Hash256> Transaction::get_protocol_authorization_hash() const {
    switch (type) {
        case TransactionType::SPEND_DEPOSIT:
        case TransactionType::UNLOCK_DEPOSIT:
        case TransactionType::ISSUE_SIDECHAIN:
        case TransactionType::REGISTER_ACCOUNTING_SIDECHAIN:
        case TransactionType::MINT_SIDECHAIN:
        case TransactionType::REDEEM_SIDECHAIN:
        case TransactionType::UPDATE_SIDECHAIN_COMMITTEE:
        case TransactionType::SLASH_DEPOSIT:
        case TransactionType::DECOMMISSION_SIDECHAIN:
        case TransactionType::UPDATE_FOUNDATION_ADDRESS:
            return crypto::sha256(serialize_protocol_authorization(*this));
        default:
            // UTXO spends and checkpoint heights already provide uniqueness.
            return std::nullopt;
    }
}

Hash256 Transaction::get_hash() const {
    auto data = serialize_without_witness();
    return crypto::double_sha256(data);
}

Transaction Transaction::deserialize_canonical(const bytes& data) {
    size_t offset = 0;
    Transaction tx = Transaction::deserialize(data, offset);
    if (offset != data.size()) {
        throw std::runtime_error("trailing bytes after transaction deserialization");
    }
    if (tx.serialize() != data) {
        throw std::runtime_error("non-canonical transaction encoding");
    }
    return tx;
}

Hash256 Transaction::canonical_hash_from_serialized(const bytes& data) {
    return deserialize_canonical(data).get_hash();
}

size_t Transaction::get_size() const {
    return serialize().size();
}

} // namespace geocoin::core
