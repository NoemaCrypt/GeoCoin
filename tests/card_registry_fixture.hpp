#pragma once

#include "test_utils.hpp"
#include "geocoin/state/state.hpp"
#include "geocoin/state/records.hpp"
#include "geocoin/utils/bytes.hpp"

namespace geocoin::test {

inline const std::vector<std::string> card_damage_cases = {
    "orphan-program", "issuer-mismatch", "invalid-issuer", "invalid-status",
    "zero-valid-from", "equal-validity", "reversed-validity", "future-program",
    "early-issue", "reversed-heights", "future-update", "null-program-id",
    "null-pubkey", "null-metadata", "empty-vendor", "invalid-os", "zero-applet-version",
    "null-package", "null-scope", "zero-quorum", "excess-quorum", "duplicate-member",
    "unsorted-members", "invalid-member", "duplicate-pubkey"
};

// A projection at height 3, used by runtime and offline snapshot tests.
struct CardRegistryFixture {
    Hash256 program_id = hash_from_seed(80);
    state::CardProgramEntry program;
    state::CardInstanceEntry card;

    CardRegistryFixture() {
        program.issuer = address_from_seed(81);
        program.chip_vendor = "NXP"; program.chip_os = "JCOP4";
        program.applet_aid = "A000000001"; program.applet_version = 1;
        program.applet_package_hash = hash_from_seed(82);
        program.scope_id = hash_from_seed(83); program.created_height = 1;
        program.policy_id = 7;
        program.corporate_policy.version = 1;
        core::CardAuthority role; role.quorum = 1; role.members = {program.issuer};
        program.corporate_policy.mint = program.corporate_policy.freeze =
            program.corporate_policy.operations = program.corporate_policy.recovery = role;
        program.corporate_policy.compliance_policy_hash = hash_from_seed(84);
        program.corporate_policy.max_active_cards = 5000;
        card.program_id = program_id; card.issuer = program.issuer;
        card.uid_hash = hash_from_seed(85); card.pubkey_hash = hash_from_seed(86);
        card.metadata_hash = hash_from_seed(87);
        card.valid_from = 100; card.valid_to = 200;
        card.issued_height = 2; card.updated_height = 3;
    }

    std::map<std::string, bytes> records(const std::string& damage = {}) const {
        auto p = program; auto c = card; auto id = program_id;
        auto& role = p.corporate_policy.mint;
        if (damage == "issuer-mismatch") c.issuer = address_from_seed(88);
        else if (damage == "invalid-issuer") p.issuer = c.issuer = Address("invalid");
        else if (damage == "invalid-status") c.status = 3;
        else if (damage == "zero-valid-from") c.valid_from = 0;
        else if (damage == "equal-validity") c.valid_to = c.valid_from;
        else if (damage == "reversed-validity") c.valid_to = c.valid_from - 1;
        else if (damage == "future-program") p.created_height = 4;
        else if (damage == "early-issue") p.created_height = 3;
        else if (damage == "reversed-heights") c.issued_height = 3, c.updated_height = 2;
        else if (damage == "future-update") c.updated_height = 4;
        else if (damage == "null-program-id") id = c.program_id = {};
        else if (damage == "null-pubkey") c.pubkey_hash = {};
        else if (damage == "null-metadata") c.metadata_hash = {};
        else if (damage == "empty-vendor") p.chip_vendor.clear();
        else if (damage == "invalid-os") p.chip_os = "bad\nos";
        else if (damage == "zero-applet-version") p.applet_version = 0;
        else if (damage == "null-package") p.applet_package_hash = {};
        else if (damage == "null-scope") p.scope_id = {};
        else if (damage == "zero-quorum") role.quorum = 0;
        else if (damage == "excess-quorum") role.quorum = 2;
        else if (damage == "duplicate-member") role.members.push_back(role.members.front());
        else if (damage == "unsorted-members") {
            role.members.push_back(address_from_seed(88));
            std::sort(role.members.begin(), role.members.end(), [](const Address& a, const Address& b) { return a.str() > b.str(); });
        } else if (damage == "invalid-member") role.members.front() = Address("invalid");
        std::map<std::string, bytes> result;
        if (damage != "orphan-program")
            result["card:prog:" + utils::hash256_to_hex(id)] = state::records::serialize_card_program(p);
        result["card:uid:" + utils::hash256_to_hex(c.uid_hash)] = state::records::serialize_card_instance(c);
        if (damage == "duplicate-pubkey") {
            c.uid_hash = hash_from_seed(89);
            result["card:uid:" + utils::hash256_to_hex(c.uid_hash)] = state::records::serialize_card_instance(c);
        }
        return result;
    }
};

} // namespace geocoin::test
