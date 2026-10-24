#include "geocoin/b2b/corporate_policy.hpp"

#include "geocoin/crypto/addresses.hpp"
#include "geocoin/crypto/geo_area.hpp"
#include "geocoin/crypto/signatures.hpp"
#include "geocoin/utils/varint.hpp"

#include <algorithm>
#include <set>

namespace geocoin::b2b {
namespace {

constexpr uint8_t kPolicyVersion = 1;
constexpr size_t kMaxPolicyMembers = 64;
constexpr size_t kMaxAuthorizationSignatures = 128;
constexpr size_t kMaxOperationSize = 128;
constexpr size_t kMaxIdentifierSize = 256;

void append_u32_le(bytes& out, uint32_t value) {
    for (size_t index = 0; index < 4; ++index) {
        out.push_back(static_cast<byte>((value >> (index * 8)) & 0xffu));
    }
}

void append_u64_le(bytes& out, uint64_t value) {
    for (size_t index = 0; index < 8; ++index) {
        out.push_back(static_cast<byte>((value >> (index * 8)) & 0xffu));
    }
}

bool append_string(bytes& out, const std::string& value, size_t max_size) {
    if (value.empty() || value.size() > max_size) {
        return false;
    }
    const bytes length = utils::encode_varint(value.size());
    out.insert(out.end(), length.begin(), length.end());
    out.insert(out.end(), value.begin(), value.end());
    return true;
}

void append_quorum_policy(bytes& out, const QuorumPolicy& policy) {
    out.push_back(policy.quorum);
    const bytes count = utils::encode_varint(policy.members.size());
    out.insert(out.end(), count.begin(), count.end());
    for (const auto& member : policy.members) {
        const std::string address = member.str();
        const bytes length = utils::encode_varint(address.size());
        out.insert(out.end(), length.begin(), length.end());
        out.insert(out.end(), address.begin(), address.end());
    }
}

const QuorumPolicy* select_role_policy(const CorporateIssuerPolicy& policy, CorporateRole role) {
    switch (role) {
        case CorporateRole::MINT:
            return &policy.mint;
        case CorporateRole::FREEZE:
            return &policy.freeze;
        case CorporateRole::OPERATIONS:
            return &policy.operations;
        case CorporateRole::RECOVERY:
            return &policy.recovery;
        default:
            return nullptr;
    }
}

bytes serialize_policy(const CorporateIssuerPolicy& policy) {
    bytes out;
    static constexpr char domain[] = "geocoin:b2b:issuer-policy:v1";
    out.insert(out.end(), domain, domain + sizeof(domain) - 1);
    out.push_back(policy.version);
    append_quorum_policy(out, policy.mint);
    append_quorum_policy(out, policy.freeze);
    append_quorum_policy(out, policy.operations);
    append_quorum_policy(out, policy.recovery);
    return out;
}

} // namespace

bool validate_quorum_policy(const QuorumPolicy& policy) {
    if (policy.quorum == 0 || policy.members.empty() ||
        policy.members.size() > kMaxPolicyMembers || policy.quorum > policy.members.size()) {
        return false;
    }

    std::string previous;
    for (const auto& member : policy.members) {
        const std::string current = member.str();
        if (!crypto::verify_address(member) || (!previous.empty() && current <= previous)) {
            return false;
        }
        previous = current;
    }
    return true;
}

bool validate_corporate_issuer_policy(const CorporateIssuerPolicy& policy) {
    return policy.version == kPolicyVersion &&
           validate_quorum_policy(policy.mint) &&
           validate_quorum_policy(policy.freeze) &&
           validate_quorum_policy(policy.operations) &&
           validate_quorum_policy(policy.recovery);
}

Hash256 compute_corporate_issuer_policy_id(const CorporateIssuerPolicy& policy) {
    if (!validate_corporate_issuer_policy(policy)) {
        return {};
    }
    return crypto::sha256(serialize_policy(policy));
}

bytes build_corporate_authorization_payload(
    const Hash256& policy_id,
    CorporateRole role,
    const std::string& operation,
    const Hash256& object_id,
    uint64_t nonce,
    uint64_t expires_at
) {
    if (role < CorporateRole::MINT || role > CorporateRole::RECOVERY ||
        operation.empty() || operation.size() > kMaxOperationSize) {
        return {};
    }

    bytes out;
    static constexpr char domain[] = "geocoin:b2b:authorization:v1";
    out.insert(out.end(), domain, domain + sizeof(domain) - 1);
    out.insert(out.end(), policy_id.begin(), policy_id.end());
    out.push_back(static_cast<byte>(role));
    if (!append_string(out, operation, kMaxOperationSize)) {
        return {};
    }
    out.insert(out.end(), object_id.begin(), object_id.end());
    append_u64_le(out, nonce);
    append_u64_le(out, expires_at);
    return out;
}

bool verify_corporate_authorizations(
    const CorporateIssuerPolicy& policy,
    CorporateRole role,
    const bytes& payload,
    const std::vector<Signature>& signatures
) {
    const QuorumPolicy* role_policy = select_role_policy(policy, role);
    if (!validate_corporate_issuer_policy(policy) || role_policy == nullptr || payload.empty() ||
        signatures.size() < role_policy->quorum || signatures.size() > kMaxAuthorizationSignatures) {
        return false;
    }

    const Hash256 message = crypto::sha256(payload);
    std::set<std::string> allowed_members;
    for (const auto& member : role_policy->members) {
        allowed_members.insert(member.str());
    }

    std::set<std::string> approved_members;
    for (const auto& signature : signatures) {
        Address signer;
        if (!crypto::recover_signer_address_from_signed_message(message, signature, signer)) {
            continue;
        }
        const std::string address = signer.str();
        if (allowed_members.count(address) != 0) {
            approved_members.insert(address);
        }
    }
    return approved_members.size() >= role_policy->quorum;
}

Hash256 derive_scope_id(
    const std::string& network_id,
    const std::string& tenant_id,
    const std::string& product_id,
    const std::string& scope_name
) {
    bytes out;
    static constexpr char domain[] = "geocoin:b2b:scope:v1";
    out.insert(out.end(), domain, domain + sizeof(domain) - 1);
    if (!append_string(out, network_id, kMaxIdentifierSize) ||
        !append_string(out, tenant_id, kMaxIdentifierSize) ||
        !append_string(out, product_id, kMaxIdentifierSize) ||
        !append_string(out, scope_name, kMaxIdentifierSize)) {
        return {};
    }
    return crypto::sha256(out);
}

Hash256 compute_metadata_hash(
    const std::string& network_id,
    const std::string& tenant_id,
    const std::string& object_type,
    const std::string& schema_id,
    uint32_t schema_version,
    const bytes& canonical_payload
) {
    if (canonical_payload.empty()) {
        return {};
    }

    bytes out;
    static constexpr char domain[] = "geocoin:b2b:metadata:v1";
    out.insert(out.end(), domain, domain + sizeof(domain) - 1);
    if (!append_string(out, network_id, kMaxIdentifierSize) ||
        !append_string(out, tenant_id, kMaxIdentifierSize) ||
        !append_string(out, object_type, kMaxIdentifierSize) ||
        !append_string(out, schema_id, kMaxIdentifierSize)) {
        return {};
    }
    append_u32_le(out, schema_version);
    const bytes payload_length = utils::encode_varint(canonical_payload.size());
    out.insert(out.end(), payload_length.begin(), payload_length.end());
    out.insert(out.end(), canonical_payload.begin(), canonical_payload.end());
    return crypto::sha256(out);
}

} // namespace geocoin::b2b
