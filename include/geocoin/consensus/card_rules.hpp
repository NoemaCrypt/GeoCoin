#pragma once

#include "geocoin/types.hpp"

namespace geocoin::core {
struct CardCorporatePolicy;
}

namespace geocoin::consensus {

// Shared by transaction validation and the persisted card registry.
bool valid_card_corporate_policy(const core::CardCorporatePolicy& policy);
bool valid_card_validity(uint64_t valid_from, uint64_t valid_to);

} // namespace geocoin::consensus
