#pragma once

#include "geocoin/core/transaction.hpp"
#include "geocoin/types.hpp"

#include <cstdint>
#include <vector>

namespace geocoin::consensus::sidechain_checkpoint {

Hash256 build_financial_supply_leaf_v2(
    const Hash256& sidechain_id,
    Height checkpoint_height,
    uint64_t total_supply
);

Hash256 build_financial_commitment_v2(
    const Hash256& sidechain_id,
    const Hash256& state_root,
    Height checkpoint_height,
    uint64_t total_supply
);

bytes build_financial_proof_v2(
    const Hash256& state_root,
    Height checkpoint_height,
    uint64_t total_supply,
    uint32_t leaf_index,
    const std::vector<Hash256>& merkle_path
);

bool verify_financial_v2(
    const core::BatchCheckpointItem& item,
    Amount issuance_limit,
    Amount total_issued
);

Hash256 build_accounting_commitment_v1(
    const Hash256& sidechain_id,
    const Hash256& state_root,
    Height checkpoint_height,
    const Hash256& schema_hash
);

bytes build_accounting_proof_v1(
    const Hash256& state_root,
    Height checkpoint_height,
    const Hash256& schema_hash
);

bool verify_accounting_v1(
    const core::BatchCheckpointItem& item,
    const Hash256& schema_hash
);

} // namespace geocoin::consensus::sidechain_checkpoint
