#include "geocoin/core/transaction_codec.hpp"
#include "geocoin/core/transaction_internal.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace geocoin::core::tx_codec {

void append_cadastral_payload(bytes& data, const Transaction& tx) {
    data.insert(data.end(), tx.cadastral_doc_hash.begin(), tx.cadastral_doc_hash.end());
    append_varbytes(data, tx.cadastral_number);
    append_varbytes(data, tx.cadastral_owner.str());
    data.insert(data.end(), tx.cadastral_nft_id.begin(), tx.cadastral_nft_id.end());
}

void read_cadastral_payload(Transaction& tx, const bytes& data, size_t& offset) {
    if (offset + 32 > data.size()) {
        throw std::runtime_error("Invalid transaction data");
    }
    std::copy(
        data.begin() + offset,
        data.begin() + offset + 32,
        tx.cadastral_doc_hash.begin()
    );
    offset += 32;

    uint64_t num_len = read_varint(data, offset);
    if (num_len > consensus::params::MAX_CADASTRAL_NUMBER) {
        throw std::runtime_error("Invalid transaction data: cadastral number too long");
    }
    require_remaining(data, offset, num_len);
    if (num_len > 0) {
        tx.cadastral_number = std::string(
            data.begin() + offset,
            data.begin() + offset + static_cast<size_t>(num_len)
        );
        offset += static_cast<size_t>(num_len);
    }

    uint64_t owner_len = read_varint(data, offset);
    if (owner_len > consensus::params::MAX_ADDRESS_SIZE) {
        throw std::runtime_error("Invalid transaction data: owner address too long");
    }
    require_remaining(data, offset, owner_len);
    if (owner_len > 0) {
        std::string owner_str(
            data.begin() + offset,
            data.begin() + offset + static_cast<size_t>(owner_len)
        );
        tx.cadastral_owner = Address(owner_str);
        offset += static_cast<size_t>(owner_len);
    }

    require_remaining(data, offset, 32);
    std::copy_n(data.begin() + offset, 32, tx.cadastral_nft_id.begin());
    offset += 32;
}

} // namespace geocoin::core::tx_codec

namespace geocoin::core {

bool verify_cadastral_transaction(const Transaction& tx) {
    if (!verify_all_inputs(tx)) {
        return false;
    }
    return true;
}

} // namespace geocoin::core
