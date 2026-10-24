#pragma once

#include "test_utils.hpp"
#include "geocoin/state/state.hpp"
#include "geocoin/state/records.hpp"
#include "geocoin/consensus/consensus.hpp"
#include "geocoin/utils/bytes.hpp"

namespace geocoin::test {

inline const std::vector<std::string> sidechain_damage_cases = {
    "missing-deposit", "owner-mismatch", "invalid-owner", "zero-unlock", "future-checkpoint",
    "missing-checkpoint-height", "missing-sidechain-height", "large-sidechain-height", "null-checkpoint",
    "zero-threshold", "excess-threshold", "committee-count", "committee-id", "duplicate-member",
    "unsorted-members", "invalid-member", "low-multiplier", "reversed-multiplier", "large-multiplier",
    "large-fee", "financial-policy", "financial-version", "empty-symbol", "bad-symbol",
    "future-decommission", "bad-namespace", "duplicate-namespace", "null-id", "missing-registry"
};
inline const std::vector<std::string> sidechain_valid_cases = {
    "valid", "accounting", "unregistered", "slashed", "decommissioned"
};

struct SidechainRegistryFixture {
    Hash256 id = hash_from_seed(110);
    state::SidechainDeposit deposit;
    state::SidechainRegistryEntry registry;

    SidechainRegistryFixture() {
        deposit.owner = address_from_seed(111);
        deposit.amount = 10 * COIN;
        deposit.unlock_height = 100;
        deposit.checkpoint = hash_from_seed(112);
        deposit.last_checkpoint_height = 2;
        deposit.last_sidechain_checkpoint_height = 120;
        registry.owner = deposit.owner;
        registry.multiplier = consensus::params::MIN_SIDECHAIN_MULTIPLIER;
        registry.max_multiplier = registry.multiplier * 2;
        registry.fee_bps = 100;
        registry.token_symbol = "TST";
        // Fees may have reduced the deposit since the limit was established.
        registry.issuance_limit = 12 * COIN * registry.multiplier;
        registry.total_issued = 5 * COIN;
        registry.committee_m = registry.committee_n = 1;
        registry.committee_members = {registry.owner};
        registry.committee_id = consensus::compute_sidechain_committee_id(1, 1, registry.committee_members);
    }

    std::map<std::string, bytes> records(const std::string& mode) const {
        auto d = deposit; auto r = registry; auto key_id = id;
        if (mode == "accounting" || mode == "bad-namespace" || mode == "duplicate-namespace") {
            r.kind = core::SidechainKind::ACCOUNTING;
            r.issuance_limit = r.total_issued = 0;
            r.multiplier = r.max_multiplier = r.fee_bps = 0;
            r.token_symbol.clear(); r.accounting_namespace = "warehouse";
            r.accounting_schema_hash = hash_from_seed(113);
            r.checkpoint_policy_id = consensus::params::ACCOUNTING_CHECKPOINT_POLICY_ID;
            r.checkpoint_version = consensus::params::ACCOUNTING_CHECKPOINT_VERSION;
        }
        if (mode == "slashed" || mode == "decommissioned") {
            d.amount = 0; r.issuance_limit = 0;
            if (mode == "decommissioned") {
                r.status = state::SidechainStatus::DECOMMISSIONING; r.decommission_height = 3;
            }
        }
        if (mode == "unregistered") d.last_checkpoint_height = d.last_sidechain_checkpoint_height = 0;
        else if (mode == "owner-mismatch") r.owner = address_from_seed(114);
        else if (mode == "invalid-owner") r.owner = d.owner = Address("invalid");
        else if (mode == "zero-unlock") d.unlock_height = 0;
        else if (mode == "future-checkpoint") d.last_checkpoint_height = 4;
        else if (mode == "missing-checkpoint-height") d.last_checkpoint_height = 0;
        else if (mode == "missing-sidechain-height") d.last_sidechain_checkpoint_height = 0;
        else if (mode == "large-sidechain-height") d.last_sidechain_checkpoint_height = consensus::params::MAX_SIDECHAIN_CHECKPOINT_HEIGHT + 1;
        else if (mode == "null-checkpoint") d.checkpoint = {};
        else if (mode == "zero-threshold") r.committee_m = 0;
        else if (mode == "excess-threshold") r.committee_m = 2;
        else if (mode == "committee-count") r.committee_n = 2;
        else if (mode == "committee-id") r.committee_id = hash_from_seed(114);
        else if (mode == "duplicate-member") { r.committee_members.push_back(r.owner); r.committee_n = 2; }
        else if (mode == "unsorted-members") {
            r.committee_members.push_back(address_from_seed(114)); r.committee_n = 2;
            std::sort(r.committee_members.begin(), r.committee_members.end(), [](const Address& a, const Address& b) { return a.str() > b.str(); });
        } else if (mode == "invalid-member") r.committee_members.front() = Address("invalid");
        else if (mode == "low-multiplier") r.multiplier = consensus::params::MIN_SIDECHAIN_MULTIPLIER - 1;
        else if (mode == "reversed-multiplier") r.max_multiplier = r.multiplier - 1;
        else if (mode == "large-multiplier") r.max_multiplier = consensus::params::MAX_SIDECHAIN_MULTIPLIER + 1;
        else if (mode == "large-fee") r.fee_bps = consensus::params::MAX_SIDECHAIN_FEE_BPS + 1;
        else if (mode == "financial-policy") r.checkpoint_policy_id = 1;
        else if (mode == "financial-version") r.checkpoint_version = 1;
        else if (mode == "empty-symbol") r.token_symbol.clear();
        else if (mode == "bad-symbol") r.token_symbol = "T\nST";
        else if (mode == "future-decommission") { r.status = state::SidechainStatus::DECOMMISSIONING; r.decommission_height = 4; }
        else if (mode == "bad-namespace") r.accounting_namespace = "bad\nnamespace";
        else if (mode == "null-id") key_id = {};
        std::map<std::string, bytes> result;
        auto add = [&](const Hash256& id) {
            const auto hex = utils::hash256_to_hex(id);
            if (mode != "missing-deposit") result["scd:" + hex] = state::records::serialize_sidechain_deposit(d);
            if (mode != "unregistered" && mode != "missing-registry") result["sc:reg:" + hex] = state::records::serialize_sidechain_registry(r);
        };
        add(key_id);
        if (mode == "duplicate-namespace") add(hash_from_seed(115));
        return result;
    }
};

} // namespace geocoin::test
