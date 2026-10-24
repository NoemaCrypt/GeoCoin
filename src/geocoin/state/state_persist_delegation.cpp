#include "geocoin/state/records.hpp"
#include "geocoin/state/state_serialization_internal.hpp"
#include "geocoin/utils/bytes.hpp"
#include "geocoin/utils/logger.hpp"

namespace geocoin::state {

bytes records::serialize_delegation(const DelegationState& entry) {
    const auto& address = entry.primary_address.str();
    if (address.empty() || address.size() > consensus::params::MAX_ADDRESS_SIZE || entry.total_spent < 0) {
        throw std::runtime_error("Invalid delegation state");
    }
    bytes raw{'D', 'G', 'R', '1'};
    const auto append = [&](uint64_t value, size_t size) {
        for (size_t i = 0; i < size; ++i) raw.push_back(static_cast<byte>(value >> (8 * i)));
    };
    append(address.size(), 4);
    raw.insert(raw.end(), address.begin(), address.end());
    append(entry.nonce, 4);
    append(entry.total_spent, 8);
    raw.insert(raw.end(), entry.proof_key.begin(), entry.proof_key.end());
    return raw;
}

bool records::deserialize_delegation(const bytes& raw, DelegationState& out) {
    try {
        ByteReader reader(raw, "DelegationState");
        if (reader.read_string(4, "magic") != "DGR1") return false;
        const auto length = reader.read_u32_le("address length");
        if (length == 0 || length > consensus::params::MAX_ADDRESS_SIZE) return false;
        DelegationState entry;
        entry.primary_address = Address(reader.read_string(length, "primary address"));
        entry.nonce = reader.read_u32_le("nonce");
        entry.total_spent = reader.read_amount("spent total");
        entry.proof_key = reader.read_hash256("proof key");
        if (!reader.empty()) return false;
        out = std::move(entry);
        return true;
    } catch (const std::runtime_error&) { return false; }
}

bool ChainState::persist_delegation_(const std::string& txid) {
    if (!storage_) return true;
    return record_persistence_result_(storage_->put_metadata("delegation:" + txid,
        records::serialize_delegation(delegations_.at(txid))), "persist delegation state");
}

void ChainState::load_delegations_from_storage_() {
    delegations_.clear();
    used_delegation_nonces_.clear();
    if (!storage_) return;
    const auto prefix = "idx:" + idx_epoch_ + ":dp:";
    size_t seen = 0;
    bool corrupt = false;
    std::string after;
    for (;;) {
        const auto keys = storage_->get_keys_page(prefix, 0, 1024, nullptr, after);
        for (const auto& key : keys) {
            bytes id, raw;
            DelegationState entry;
            if (!storage_->get_metadata(key, id) || id.size() != 32) { corrupt = true; continue; }
            const auto txid = utils::bytes_to_hex(id.data(), id.size());
            if (!storage_->get_metadata("delegation:" + txid, raw) ||
                !records::deserialize_delegation(raw, entry) ||
                key != prefix + hash256_to_hex(entry.proof_key) ||
                !delegations_.emplace(txid, entry).second ||
                !used_delegation_nonces_[entry.primary_address.str()].insert(entry.nonce).second) {
                corrupt = true;
                continue;
            }
            ++seen;
        }
        if (keys.size() < 1024) break;
        after = keys.back();
    }
    size_t records = 0;
    storage_->get_keys_page("delegation:", 0, 0, &records);
    if (corrupt || records != seen) {
        derived_state_storage_valid_ = false;
        GEO_LOG_ERROR("chainstate", "Incomplete or ambiguous delegation snapshot; rebuild required");
    }
}
}
