#pragma once

#include "geocoin/types.hpp"

#include <string>
#include <vector>

namespace geocoin::consensus {

bool is_printable_ascii(const std::string& value);
bool is_sorted_unique_addresses(const std::vector<Address>& members);

bool sidechain_committee_config_valid(
    uint8_t m,
    uint8_t n,
    const std::vector<Address>& members
);

bool sidechain_committee_id_matches(
    uint8_t m,
    uint8_t n,
    const std::vector<Address>& members,
    const Hash256& committee_id,
    bool allow_null
);

bool mint_fits_issuance_capacity(
    Amount issuance_limit,
    Amount total_issued,
    Amount amount
);

bool financial_issuance_limit_from_deposit(
    uint32_t multiplier,
    Amount deposit_amount,
    Amount& out
);

} // namespace geocoin::consensus
