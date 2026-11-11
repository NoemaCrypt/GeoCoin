#pragma once

#include "geocoin/consensus/consensus.hpp"
#include "geocoin/core/transaction.hpp"
#include "geocoin/consensus/validation.hpp"

namespace geocoin::consensus {

bool check_card_transaction(const core::Transaction& tx, ValidationState& state);
bool check_nft_transaction(const core::Transaction& tx, ValidationState& state);
bool check_foundation_transaction(const core::Transaction& tx, ValidationState& state);
bool check_cadastral_transaction(const core::Transaction& tx, ValidationState& state);
bool check_multisig_transaction(
    const core::Transaction& tx,
    ValidationState& state,
    bool& complete
);
bool check_sidechain_transaction(
    const core::Transaction& tx,
    ValidationState& state,
    bool& complete
);
bool check_common_utxo_transaction(
    const core::Transaction& tx,
    ValidationState& state
);
bool contextual_check_nft_transaction(
    const core::Transaction& tx,
    ValidationState& state,
    const ContextualTxLookup& lookup
);
bool contextual_check_multisig_transaction(
    const core::Transaction& tx,
    ValidationState& state,
    const ContextualTxLookup& lookup
);
bool check_block_angle_package(const core::Block& block, ValidationState& state);

} // namespace geocoin::consensus
