#pragma once

#include "geocoin/types.hpp"

namespace geocoin::core { class Block; class Transaction; }

namespace geocoin::storage {

// The same payload is used by the state cache, disk pruning and restoration.
bytes serialize_witness_payload(const core::Block& block);
bool has_material_witness(const core::Transaction& tx);
// Apply all witness data only after validation; failure leaves the block intact.
bool apply_witness_payload(core::Block& block, const bytes& payload);

} // namespace geocoin::storage
