#include "geocoin/consensus/sidechain_rules.hpp"

#include "geocoin/consensus/consensus.hpp"
#include "geocoin/crypto/addresses.hpp"
#include "geocoin/utils/bytes.hpp"

namespace geocoin::consensus {

bool is_printable_ascii(const std::string& value) {
    for (unsigned char c : value) {
        if (c < 0x20 || c > 0x7E) {
            return false;
        }
    }
    return true;
}

bool is_sorted_unique_addresses(const std::vector<Address>& members) {
    std::string prev;
    for (const auto& a : members) {
        const std::string cur = a.str();
        if (cur.empty() || !crypto::verify_address(a)) {
            return false;
        }
        if (!prev.empty() && cur <= prev) {
            return false;
        }
        prev = cur;
    }
    return true;
}

bool sidechain_committee_config_valid(
    uint8_t m,
    uint8_t n,
    const std::vector<Address>& members
) {
    if (m == 0 || n == 0 || m > n) {
        return false;
    }
    if (members.size() != n ||
        members.size() > params::MAX_SIDECHAIN_COMMITTEE_MEMBERS) {
        return false;
    }
    return is_sorted_unique_addresses(members);
}

bool sidechain_committee_id_matches(
    uint8_t m,
    uint8_t n,
    const std::vector<Address>& members,
    const Hash256& committee_id,
    bool allow_null
) {
    if (!sidechain_committee_config_valid(m, n, members)) {
        return false;
    }
    if (allow_null && utils::is_null_hash256(committee_id)) {
        return true;
    }
    return committee_id == compute_sidechain_committee_id(m, n, members);
}

bool mint_fits_issuance_capacity(
    Amount issuance_limit,
    Amount total_issued,
    Amount amount
) {
    Amount remaining_capacity = 0;
    return amount > 0 &&
           checked_sub_amount(issuance_limit, amount, remaining_capacity) &&
           total_issued <= remaining_capacity;
}

bool financial_issuance_limit_from_deposit(
    uint32_t multiplier,
    Amount deposit_amount,
    Amount& out
) {
    return checked_mul_amount(deposit_amount, multiplier, out);
}

} // namespace geocoin::consensus
