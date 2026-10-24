#pragma once

#include "geocoin/types.hpp"

#include <cstdint>
#include <string>
#include <vector>

// Canonical B2B identifier and authorization helpers.
// L1 consensus does not call verify_corporate_authorizations or consume nonce/
// expires_at. Chainstate authorizes CARD_* by counting signed UTXO inputs whose
// addresses sit in the on-chain role quorum. The authorization payload is for
// product backends (HSM signing, replay protection). See adr/0001 and adr/0002.

namespace geocoin::b2b {

enum class CorporateRole : uint8_t {
    MINT = 1,
    FREEZE = 2,
    OPERATIONS = 3,
    RECOVERY = 4,
};

struct QuorumPolicy {
    uint8_t quorum = 0;
    std::vector<Address> members;
};

struct CorporateIssuerPolicy {
    uint8_t version = 1;
    QuorumPolicy mint;
    QuorumPolicy freeze;
    QuorumPolicy operations;
    QuorumPolicy recovery;
};

bool validate_quorum_policy(const QuorumPolicy& policy);
bool validate_corporate_issuer_policy(const CorporateIssuerPolicy& policy);

Hash256 compute_corporate_issuer_policy_id(const CorporateIssuerPolicy& policy);

bytes build_corporate_authorization_payload(
    const Hash256& policy_id,
    CorporateRole role,
    const std::string& operation,
    const Hash256& object_id,
    uint64_t nonce,
    uint64_t expires_at
);

bool verify_corporate_authorizations(
    const CorporateIssuerPolicy& policy,
    CorporateRole role,
    const bytes& payload,
    const std::vector<Signature>& signatures
);

Hash256 derive_scope_id(
    const std::string& network_id,
    const std::string& tenant_id,
    const std::string& product_id,
    const std::string& scope_name
);

Hash256 compute_metadata_hash(
    const std::string& network_id,
    const std::string& tenant_id,
    const std::string& object_type,
    const std::string& schema_id,
    uint32_t schema_version,
    const bytes& canonical_payload
);

} // namespace geocoin::b2b
