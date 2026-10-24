#include "geocoin/consensus/consensus_internal.hpp"

#include "geocoin/consensus/consensus.hpp"
#include "geocoin/nft/nft_types.hpp"

namespace geocoin::consensus {

bool check_nft_transaction(const core::Transaction& tx, ValidationState& state) {
    if (tx.type == core::TransactionType::NFT_CREATE) {
        if (!tx.nft_create.has_value()) {
            return state.invalid(RejectCode::INVALID, "bad-nft-create", "NFT_CREATE missing payload");
        }
        if (tx.nft_create->pol_data.has_value()) {
            if (!params::ENABLE_NFT_POL) {
                return state.invalid(RejectCode::INVALID, "bad-nft-pol-disabled", "PoL is disabled by consensus");
            }
            const auto& pol = tx.nft_create->pol_data.value();
            if (pol.source_type > static_cast<uint8_t>(nft::PoLSourceType::HYBRID)) {
                return state.invalid(RejectCode::INVALID, "bad-nft-pol-source", "invalid PoL source_type");
            }
            if (pol.gps_accuracy_cm == 0 || pol.gps_accuracy_cm > params::MAX_NFT_POL_ACCURACY_CM) {
                return state.invalid(RejectCode::INVALID, "bad-nft-pol-accuracy", "invalid PoL accuracy");
            }
        }
        if (tx.inputs.empty()) {
            return state.invalid(RejectCode::INVALID, "bad-nft-create", "NFT_CREATE must have inputs");
        }
        if (tx.outputs.empty()) {
            return state.invalid(RejectCode::INVALID, "bad-nft-create", "NFT_CREATE must have outputs");
        }
        return true;
    }

    if (tx.type == core::TransactionType::NFT_TRANSFER) {
        if (!tx.nft_transfer.has_value()) {
            return state.invalid(RejectCode::INVALID, "bad-nft-transfer", "NFT_TRANSFER missing payload");
        }
        if (tx.inputs.empty()) {
            return state.invalid(RejectCode::INVALID, "bad-nft-transfer", "NFT_TRANSFER must have inputs");
        }
        if (tx.outputs.empty()) {
            return state.invalid(RejectCode::INVALID, "bad-nft-transfer", "NFT_TRANSFER must have outputs");
        }
        return true;
    }

    if (tx.type == core::TransactionType::NFT_BURN) {
        if (!tx.nft_burn.has_value()) {
            return state.invalid(RejectCode::INVALID, "bad-nft-burn", "NFT_BURN missing payload");
        }
        if (tx.inputs.empty()) {
            return state.invalid(RejectCode::INVALID, "bad-nft-burn", "NFT_BURN must have inputs");
        }
        return true;
    }

    return true;
}

bool contextual_check_nft_transaction(
    const core::Transaction& tx,
    ValidationState& state,
    const ContextualTxLookup& lookup
) {
    if (tx.type != core::TransactionType::NFT_CREATE) {
        return true;
    }
    if (!tx.nft_create.has_value()) {
        return state.invalid(RejectCode::INVALID, "bad-nft-create-missing-payload",
            "NFT_CREATE missing payload");
    }

    const Hash256& nft_id = tx.nft_create->nft_id;
    if (lookup.has_nft_id_key && lookup.has_nft_id_key(nft_id)) {
        return state.invalid(RejectCode::INVALID, "bad-nft-create-duplicate",
            "NFT with this id already exists");
    }
    return true;
}

} // namespace geocoin::consensus
