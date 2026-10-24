#include "geocoin/b2b/corporate_policy.hpp"
#include "geocoin/crypto/addresses.hpp"
#include "geocoin/crypto/geo_area.hpp"
#include "geocoin/crypto/signatures.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace {

void expect(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

geocoin::bytes key_from_seed(uint8_t seed) {
    geocoin::bytes key(32);
    for (size_t index = 0; index < key.size(); ++index) {
        key[index] = static_cast<geocoin::byte>(seed + index);
    }
    return key;
}

geocoin::Address address_from_key(const geocoin::bytes& key) {
    geocoin::Address address;
    if (!geocoin::crypto::derive_identity_address_from_private_key(key, address)) {
        throw std::runtime_error("address derivation failed");
    }
    return address;
}

geocoin::b2b::QuorumPolicy make_quorum(
    uint8_t quorum,
    const std::vector<geocoin::Address>& members
) {
    geocoin::b2b::QuorumPolicy policy;
    policy.quorum = quorum;
    policy.members = members;
    std::sort(policy.members.begin(), policy.members.end(), [](const auto& left, const auto& right) {
        return left.str() < right.str();
    });
    return policy;
}

} // namespace

int main() {
    using namespace geocoin;

    try {
        const bytes key_a = key_from_seed(11);
        const bytes key_b = key_from_seed(31);
        const bytes key_c = key_from_seed(51);
        const Address address_a = address_from_key(key_a);
        const Address address_b = address_from_key(key_b);
        const Address address_c = address_from_key(key_c);

        b2b::CorporateIssuerPolicy policy;
        policy.mint = make_quorum(2, {address_a, address_b, address_c});
        policy.freeze = make_quorum(2, {address_a, address_b, address_c});
        policy.operations = make_quorum(1, {address_a, address_b});
        policy.recovery = make_quorum(2, {address_a, address_b, address_c});
        expect(b2b::validate_corporate_issuer_policy(policy), "valid issuer policy rejected");

        const Hash256 policy_id = b2b::compute_corporate_issuer_policy_id(policy);
        expect(policy_id != Hash256{}, "policy id is empty");

        const Hash256 object_id = crypto::sha256(bytes{'c', 'a', 'r', 'd', '-', '1'});
        const bytes payload = b2b::build_corporate_authorization_payload(
            policy_id,
            b2b::CorporateRole::MINT,
            "card.issue",
            object_id,
            7,
            9999999999ULL
        );
        expect(!payload.empty(), "authorization payload is empty");
        const Hash256 message = crypto::sha256(payload);
        const std::vector<Signature> signatures{
            crypto::sign_recoverable_message(message, key_a),
            crypto::sign_recoverable_message(message, key_b),
        };
        expect(b2b::verify_corporate_authorizations(
                   policy, b2b::CorporateRole::MINT, payload, signatures),
               "mint quorum rejected");
        expect(!b2b::verify_corporate_authorizations(
                   policy, b2b::CorporateRole::MINT, payload, {signatures.front()}),
               "partial mint quorum accepted");

        const Hash256 first_scope = b2b::derive_scope_id("degree", "tenant-a", "access", "staff");
        const Hash256 second_scope = b2b::derive_scope_id("degree", "tenant-b", "access", "staff");
        expect(first_scope != Hash256{} && first_scope != second_scope, "scope namespace is not isolated");

        const bytes metadata{'{', '"', 'v', '"', ':', '1', '}'};
        const Hash256 metadata_hash = b2b::compute_metadata_hash(
            "degree",
            "tenant-a",
            "card.instance",
            "com.geocoin.card.metadata",
            1,
            metadata
        );
        expect(metadata_hash != Hash256{}, "metadata hash is empty");
        expect(
            metadata_hash != b2b::compute_metadata_hash(
                "degree", "tenant-a", "card.instance", "com.geocoin.card.metadata", 2, metadata),
            "metadata version is not committed");
        expect(
            metadata_hash != b2b::compute_metadata_hash(
                "testnet", "tenant-a", "card.instance", "com.geocoin.card.metadata", 1, metadata),
            "metadata network is not isolated");
        expect(
            metadata_hash != b2b::compute_metadata_hash(
                "degree", "tenant-b", "card.instance", "com.geocoin.card.metadata", 1, metadata),
            "metadata tenant is not isolated");
        expect(
            metadata_hash != b2b::compute_metadata_hash(
                "degree", "tenant-a", "card.program", "com.geocoin.card.metadata", 1, metadata),
            "metadata object type is not isolated");
        expect(
            b2b::compute_metadata_hash(
                "", "tenant-a", "card.instance", "com.geocoin.card.metadata", 1, metadata) == Hash256{},
            "empty metadata network is accepted");

        std::cout << "test_b2b_corporate_policy: ok\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "test_b2b_corporate_policy failed: " << error.what() << '\n';
        return 1;
    }
}
