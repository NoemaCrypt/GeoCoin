#pragma once

#include "geocoin/core/transaction.hpp"

namespace geocoin::core {

bool verify_all_inputs(const Transaction& tx);
bool verify_angle_commitment_transaction(const Transaction& tx);
bool verify_nft_transaction(const Transaction& tx);
bool verify_cadastral_transaction(const Transaction& tx);
bool verify_foundation_transaction(const Transaction& tx);
bool verify_sidechain_owner_transaction(const Transaction& tx);

} // namespace geocoin::core
