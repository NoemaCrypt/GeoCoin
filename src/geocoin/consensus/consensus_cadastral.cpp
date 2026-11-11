#include "geocoin/consensus/consensus_internal.hpp"

#include "geocoin/consensus/consensus.hpp"
#include "geocoin/crypto/addresses.hpp"

#include <set>
#include <utility>

namespace geocoin::consensus {

bool check_cadastral_transaction(const core::Transaction& tx, ValidationState& state) {
    if (tx.type != core::TransactionType::CADASTRAL_DOC) {
        return true;
    }

    if (tx.inputs.empty()) {
        return state.invalid(RejectCode::INVALID, "bad-cadastral-doc",
            "CADASTRAL_DOC must have inputs");
    }

    bool is_null_hash = true;
    for (auto byte : tx.cadastral_doc_hash) {
        if (byte != 0) {
            is_null_hash = false;
            break;
        }
    }
    if (is_null_hash || tx.cadastral_nft_id == Hash256{} || !crypto::verify_address(tx.cadastral_owner)) {
        return state.invalid(RejectCode::INVALID, "bad-cadastral-doc",
            "CADASTRAL_DOC requires a document hash, parcel NFT id and valid owner");
    }

    std::set<std::pair<Hash256, uint32_t>> seen_inputs;
    for (const auto& input : tx.inputs) {
        auto key = std::make_pair(input.prev_tx_hash, input.vout);
        if (seen_inputs.count(key)) {
            return state.invalid(RejectCode::INVALID, "bad-txns-inputs-duplicate", "duplicate inputs");
        }
        seen_inputs.insert(key);
    }
    return true;
}

} // namespace geocoin::consensus
