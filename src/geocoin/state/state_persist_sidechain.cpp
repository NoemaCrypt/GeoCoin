#include "geocoin/state/state.hpp"
#include "geocoin/state/records.hpp"
#include "geocoin/state/state_persist_internal.hpp"

#include "geocoin/consensus/consensus.hpp"
#include "geocoin/consensus/sidechain_rules.hpp"
#include "geocoin/crypto/addresses.hpp"
#include "geocoin/utils/bytes.hpp"
#include "geocoin/utils/logger.hpp"

#include <cstdint>
#include <set>
#include <string>

namespace geocoin::state {
using namespace records;

namespace {
constexpr uint8_t kSidechainRegistryExtensionTagLocal = 0xA3;
constexpr uint8_t kSidechainRegistryExtensionVersionLocal = 1;
}

bool records::validate_sidechain_deposit(const SidechainDeposit& deposit, Height tip_height) {
    return crypto::verify_address(deposit.owner) && deposit.amount >= 0 && deposit.unlock_height > 0 &&
           deposit.last_checkpoint_height <= tip_height &&
           deposit.last_sidechain_checkpoint_height <= consensus::params::MAX_SIDECHAIN_CHECKPOINT_HEIGHT &&
           (deposit.last_checkpoint_height == 0) == (deposit.last_sidechain_checkpoint_height == 0) &&
           (deposit.last_checkpoint_height == 0 || !utils::is_null_hash256(deposit.checkpoint));
}

bool records::validate_sidechain_registry(const SidechainRegistryEntry& entry, Height tip_height) {
    if (!crypto::verify_address(entry.owner) || entry.issuance_limit < 0 || entry.total_issued < 0 ||
        !consensus::sidechain_committee_id_matches(entry.committee_m, entry.committee_n,
            entry.committee_members, entry.committee_id, false)) return false;
    if (entry.status == SidechainStatus::ACTIVE) {
        if (entry.decommission_height != 0) return false;
    } else if (entry.status == SidechainStatus::DECOMMISSIONING) {
        if (entry.decommission_height == 0 || entry.decommission_height > tip_height) return false;
    } else return false;
    if (entry.kind == core::SidechainKind::ACCOUNTING) {
        return entry.issuance_limit == 0 && entry.total_issued == 0 && entry.multiplier == 0 &&
               entry.max_multiplier == 0 && entry.fee_bps == 0 && entry.token_symbol.empty() &&
               !entry.accounting_namespace.empty() &&
               entry.accounting_namespace.size() <= consensus::params::MAX_SIDECHAIN_NAMESPACE &&
               consensus::is_printable_ascii(entry.accounting_namespace) &&
               !utils::is_null_hash256(entry.accounting_schema_hash) &&
               entry.checkpoint_policy_id == consensus::params::ACCOUNTING_CHECKPOINT_POLICY_ID &&
               entry.checkpoint_version == consensus::params::ACCOUNTING_CHECKPOINT_VERSION;
    }
    // Slashing can lower the limit below outstanding issuance. Fees and
    // redemptions also change the deposit without recomputing this limit.
    return entry.kind == core::SidechainKind::FINANCIAL &&
           entry.multiplier >= consensus::params::MIN_SIDECHAIN_MULTIPLIER &&
           entry.multiplier <= entry.max_multiplier && entry.max_multiplier <= consensus::params::MAX_SIDECHAIN_MULTIPLIER &&
           entry.fee_bps <= consensus::params::MAX_SIDECHAIN_FEE_BPS && !entry.token_symbol.empty() &&
           entry.token_symbol.size() <= consensus::params::MAX_TOKEN_SYMBOL && consensus::is_printable_ascii(entry.token_symbol) &&
           entry.accounting_namespace.empty() && utils::is_null_hash256(entry.accounting_schema_hash) &&
           entry.checkpoint_policy_id == consensus::params::FINANCIAL_CHECKPOINT_POLICY_ID &&
           entry.checkpoint_version == consensus::params::FINANCIAL_CHECKPOINT_VERSION;
}

bytes records::serialize_sidechain_deposit(const SidechainDeposit& deposit) {
    bytes data;
    const std::string owner_str = deposit.owner.str();
    uint32_t owner_len = static_cast<uint32_t>(owner_str.size());
    for (int i = 0; i < 4; i++) {
        data.push_back((owner_len >> (i * 8)) & 0xFF);
    }
    data.insert(data.end(), owner_str.begin(), owner_str.end());

    for (int i = 0; i < 8; i++) {
        data.push_back((static_cast<uint64_t>(deposit.amount) >> (i * 8)) & 0xFF);
    }

    for (int i = 0; i < 8; i++) {
        data.push_back((static_cast<uint64_t>(deposit.unlock_height) >> (i * 8)) & 0xFF);
    }

    data.insert(data.end(), deposit.checkpoint.begin(), deposit.checkpoint.end());

    for (int i = 0; i < 8; i++) {
        data.push_back((static_cast<uint64_t>(deposit.last_checkpoint_height) >> (i * 8)) & 0xFF);
    }
    for (int i = 0; i < 8; i++) {
        data.push_back((static_cast<uint64_t>(deposit.last_sidechain_checkpoint_height) >> (i * 8)) & 0xFF);
    }
    return data;
}

bool records::deserialize_sidechain_deposit(const bytes& data, SidechainDeposit& out) {
    size_t offset = 0;
    if (offset + 4 > data.size()) {
        return false;
    }
    uint32_t owner_len = 0;
    for (int i = 0; i < 4; i++) {
        owner_len |= (uint32_t)data[offset++] << (i * 8);
    }
    if (owner_len > consensus::params::MAX_ADDRESS_SIZE ||
        owner_len > data.size() - offset) {
        return false;
    }
    const std::string owner_str(data.begin() + offset, data.begin() + offset + owner_len);
    out.owner = Address(owner_str);
    offset += owner_len;

    if (offset + 8 > data.size()) {
        return false;
    }
    uint64_t raw_amount = 0;
    for (int i = 0; i < 8; i++) {
        raw_amount |= static_cast<uint64_t>(data[offset++]) << (i * 8);
    }
    if (raw_amount > static_cast<uint64_t>((std::numeric_limits<Amount>::max)())) {
        return false;
    }
    out.amount = static_cast<Amount>(raw_amount);

    if (offset + 8 > data.size()) {
        return false;
    }
    out.unlock_height = 0;
    for (int i = 0; i < 8; i++) {
        out.unlock_height |= (uint64_t)data[offset++] << (i * 8);
    }

    if (offset + 32 > data.size()) {
        return false;
    }
    std::copy(data.begin() + offset, data.begin() + offset + 32, out.checkpoint.begin());
    offset += 32;

    if (data.size() - offset != 16) return false;
    out.last_checkpoint_height = 0;
    for (int i = 0; i < 8; i++) {
        out.last_checkpoint_height |= (uint64_t)data[offset++] << (i * 8);
    }
    out.last_sidechain_checkpoint_height = 0;
    for (int i = 0; i < 8; i++) {
        out.last_sidechain_checkpoint_height |= (uint64_t)data[offset++] << (i * 8);
    }

    return offset == data.size();
}

bytes records::serialize_sidechain_registry(const SidechainRegistryEntry& entry) {
    bytes data;
    const std::string owner_str = entry.owner.str();
    uint32_t owner_len = static_cast<uint32_t>(owner_str.size());
    for (int i = 0; i < 4; i++) {
        data.push_back((owner_len >> (i * 8)) & 0xFF);
    }
    data.insert(data.end(), owner_str.begin(), owner_str.end());

    for (int i = 0; i < 8; i++) {
        data.push_back((static_cast<uint64_t>(entry.issuance_limit) >> (i * 8)) & 0xFF);
    }
    for (int i = 0; i < 8; i++) {
        data.push_back((static_cast<uint64_t>(entry.total_issued) >> (i * 8)) & 0xFF);
    }
    for (int i = 0; i < 4; i++) {
        data.push_back((static_cast<uint32_t>(entry.multiplier) >> (i * 8)) & 0xFF);
    }
    for (int i = 0; i < 4; i++) {
        data.push_back((static_cast<uint32_t>(entry.fee_bps) >> (i * 8)) & 0xFF);
    }
    for (int i = 0; i < 4; i++) {
        data.push_back((static_cast<uint32_t>(entry.max_multiplier) >> (i * 8)) & 0xFF);
    }
    data.push_back(entry.checkpoint_policy_id);
    data.push_back(entry.checkpoint_version);

    uint32_t sym_len = static_cast<uint32_t>(entry.token_symbol.size());
    for (int i = 0; i < 4; i++) {
        data.push_back((sym_len >> (i * 8)) & 0xFF);
    }
    data.insert(data.end(), entry.token_symbol.begin(), entry.token_symbol.end());

    data.push_back(entry.committee_m);
    data.push_back(entry.committee_n);
    uint32_t count = static_cast<uint32_t>(entry.committee_members.size());
    for (int i = 0; i < 4; i++) {
        data.push_back((count >> (i * 8)) & 0xFF);
    }
    for (const auto& p : entry.committee_members) {
        const std::string s = p.str();
        uint32_t len = static_cast<uint32_t>(s.size());
        for (int i = 0; i < 4; i++) {
            data.push_back((len >> (i * 8)) & 0xFF);
        }
        data.insert(data.end(), s.begin(), s.end());
    }
    data.insert(data.end(), entry.committee_id.begin(), entry.committee_id.end());
    data.push_back(kSidechainRegistryExtensionTagLocal);
    data.push_back(kSidechainRegistryExtensionVersionLocal);
    data.push_back(static_cast<uint8_t>(entry.kind));
    data.push_back(static_cast<uint8_t>(entry.status));
    for (int i = 0; i < 8; ++i) {
        data.push_back(
            (static_cast<uint64_t>(entry.decommission_height) >> (i * 8)) & 0xFF
        );
    }
    const uint32_t namespace_len =
        static_cast<uint32_t>(entry.accounting_namespace.size());
    for (int i = 0; i < 4; ++i) {
        data.push_back((namespace_len >> (i * 8)) & 0xFF);
    }
    data.insert(
        data.end(),
        entry.accounting_namespace.begin(),
        entry.accounting_namespace.end()
    );
    data.insert(
        data.end(),
        entry.accounting_schema_hash.begin(),
        entry.accounting_schema_hash.end()
    );
    return data;
}

bool records::deserialize_sidechain_registry(const bytes& data, SidechainRegistryEntry& out) {
    size_t offset = 0;
    if (offset + 4 > data.size()) {
        return false;
    }
    uint32_t owner_len = 0;
    for (int i = 0; i < 4; i++) {
        owner_len |= (uint32_t)data[offset++] << (i * 8);
    }
    if (owner_len > consensus::params::MAX_ADDRESS_SIZE ||
        owner_len > data.size() - offset) {
        return false;
    }
    const std::string owner_str(data.begin() + offset, data.begin() + offset + owner_len);
    out.owner = Address(owner_str);
    offset += owner_len;

    if (offset + 8 > data.size()) {
        return false;
    }
    uint64_t raw_issuance_limit = 0;
    for (int i = 0; i < 8; i++) {
        raw_issuance_limit |= static_cast<uint64_t>(data[offset++]) << (i * 8);
    }
    if (raw_issuance_limit > static_cast<uint64_t>((std::numeric_limits<Amount>::max)())) {
        return false;
    }
    out.issuance_limit = static_cast<Amount>(raw_issuance_limit);

    if (offset + 8 > data.size()) {
        return false;
    }
    uint64_t raw_total_issued = 0;
    for (int i = 0; i < 8; i++) {
        raw_total_issued |= static_cast<uint64_t>(data[offset++]) << (i * 8);
    }
    if (raw_total_issued > static_cast<uint64_t>((std::numeric_limits<Amount>::max)())) {
        return false;
    }
    out.total_issued = static_cast<Amount>(raw_total_issued);

    if (offset + 4 > data.size()) {
        return false;
    }
    out.multiplier = 0;
    for (int i = 0; i < 4; i++) {
        out.multiplier |= (uint32_t)data[offset++] << (i * 8);
    }

    if (offset + 4 > data.size()) {
        return false;
    }
    out.fee_bps = 0;
    for (int i = 0; i < 4; i++) {
        out.fee_bps |= (uint32_t)data[offset++] << (i * 8);
    }

    if (offset + 4 > data.size()) {
        return false;
    }
    out.max_multiplier = 0;
    for (int i = 0; i < 4; i++) {
        out.max_multiplier |= (uint32_t)data[offset++] << (i * 8);
    }
    if (offset + 2 > data.size()) return false;
    out.checkpoint_policy_id = data[offset++];
    out.checkpoint_version = data[offset++];

    if (offset + 4 > data.size()) {
        return false;
    }
    uint32_t sym_len = 0;
    for (int i = 0; i < 4; i++) {
        sym_len |= (uint32_t)data[offset++] << (i * 8);
    }
    if (sym_len > consensus::params::MAX_TOKEN_SYMBOL ||
        sym_len > data.size() - offset) {
        return false;
    }
    out.token_symbol.assign(data.begin() + offset, data.begin() + offset + sym_len);
    offset += sym_len;

    if (offset + 2 > data.size()) {
        return false;
    }
    out.committee_m = data[offset++];
    out.committee_n = data[offset++];

    if (offset + 4 > data.size()) {
        return false;
    }
    uint32_t count = 0;
    for (int i = 0; i < 4; i++) {
        count |= (uint32_t)data[offset++] << (i * 8);
    }
    if (count > consensus::params::MAX_SIDECHAIN_COMMITTEE_MEMBERS) {
        return false;
    }
    out.committee_members.clear();
    for (uint32_t i = 0; i < count; i++) {
        if (offset + 4 > data.size()) {
            return false;
        }
        uint32_t len = 0;
        for (int j = 0; j < 4; j++) {
            len |= (uint32_t)data[offset++] << (j * 8);
        }
        if (len > consensus::params::MAX_ADDRESS_SIZE || len > data.size() - offset) {
            return false;
        }
        out.committee_members.emplace_back(
            Address(std::string(data.begin() + offset, data.begin() + offset + len)));
        offset += len;
    }
    if (offset + 32 > data.size()) {
        return false;
    }
    std::copy(data.begin() + offset, data.begin() + offset + 32, out.committee_id.begin());
    offset += 32;

    out.kind = core::SidechainKind::FINANCIAL;
    out.status = SidechainStatus::ACTIVE;
    out.decommission_height = 0;
    out.accounting_namespace.clear();
    out.accounting_schema_hash = {};
    if (offset + 4 + 8 + 4 + 32 > data.size() ||
        data[offset++] != kSidechainRegistryExtensionTagLocal ||
        data[offset++] != kSidechainRegistryExtensionVersionLocal) {
        return false;
    }
    const uint8_t kind_byte = data[offset++];
    const uint8_t status_byte = data[offset++];
    if (kind_byte > static_cast<uint8_t>(core::SidechainKind::ACCOUNTING) ||
        status_byte > static_cast<uint8_t>(SidechainStatus::DECOMMISSIONING)) {
        return false;
    }
    out.kind = static_cast<core::SidechainKind>(kind_byte);
    out.status = static_cast<SidechainStatus>(status_byte);
    out.decommission_height = 0;
    for (int i = 0; i < 8; ++i) {
        out.decommission_height |= static_cast<uint64_t>(data[offset++]) << (i * 8);
    }
    uint32_t namespace_len = 0;
    for (int i = 0; i < 4; ++i) {
        namespace_len |= static_cast<uint32_t>(data[offset++]) << (i * 8);
    }
    if (namespace_len > consensus::params::MAX_SIDECHAIN_NAMESPACE ||
        namespace_len > data.size() - offset ||
        data.size() - offset - namespace_len < 32) {
        return false;
    }
    out.accounting_namespace.assign(
        data.begin() + offset,
        data.begin() + offset + namespace_len
    );
    offset += namespace_len;
    std::copy(
        data.begin() + offset,
        data.begin() + offset + 32,
        out.accounting_schema_hash.begin()
    );
    offset += 32;
    if (offset != data.size()) {
        return false;
    }
    if ((out.status == SidechainStatus::ACTIVE && out.decommission_height != 0) ||
        (out.status == SidechainStatus::DECOMMISSIONING && out.decommission_height == 0)) {
        return false;
    }
    if (out.kind == core::SidechainKind::ACCOUNTING) {
        return out.issuance_limit == 0 && out.total_issued == 0 &&
               out.multiplier == 0 && out.fee_bps == 0 && out.max_multiplier == 0 &&
               out.token_symbol.empty() && !out.accounting_namespace.empty() &&
               out.accounting_schema_hash != Hash256{} &&
               out.checkpoint_policy_id ==
                   consensus::params::ACCOUNTING_CHECKPOINT_POLICY_ID &&
               out.checkpoint_version ==
                   consensus::params::ACCOUNTING_CHECKPOINT_VERSION;
    }
    return out.accounting_namespace.empty() &&
           out.accounting_schema_hash == Hash256{};
}

bool ChainState::get_sidechain_deposit(const Hash256& sidechain_id, SidechainDeposit& out) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const std::string hex = hash256_to_hex(sidechain_id);
    auto it = sidechain_deposits_.find(hex);
    if (it != sidechain_deposits_.end()) {
        out = it->second;
        return true;
    }
    if (storage_) {
        bytes raw;
        if (storage_->get_metadata("scd:" + hex, raw)) {
            SidechainDeposit tmp;
            if (!utils::is_null_hash256(sidechain_id) && deserialize_sidechain_deposit(raw, tmp) &&
                validate_sidechain_deposit(tmp, current_height_)) {
                out = tmp;
                return true;
            }
            throw storage::ReadError(storage::ReadStatus::CORRUPT, "Invalid sidechain deposit record");
        }
    }
    return false;
}

bool ChainState::get_sidechain_registry(const Hash256& sidechain_id, SidechainRegistryEntry& out) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const std::string hex = hash256_to_hex(sidechain_id);
    auto it = sidechain_registry_.find(hex);
    if (it != sidechain_registry_.end()) {
        out = it->second;
        return true;
    }
    if (storage_) {
        bytes raw;
        if (storage_->get_metadata("sc:reg:" + hex, raw)) {
            SidechainRegistryEntry tmp;
            SidechainDeposit deposit;
            if (!utils::is_null_hash256(sidechain_id) && deserialize_sidechain_registry(raw, tmp) &&
                validate_sidechain_registry(tmp, current_height_) && get_sidechain_deposit(sidechain_id, deposit) &&
                tmp.owner == deposit.owner &&
                (tmp.accounting_namespace.empty() || !has_accounting_namespace(tmp.accounting_namespace))) {
                out = tmp;
                return true;
            }
            throw storage::ReadError(storage::ReadStatus::CORRUPT, "Invalid sidechain registry record");
        }
    }
    return false;
}

void ChainState::persist_sidechain_deposit_to_storage(
    const Hash256& sidechain_id,
    const SidechainDeposit& deposit
) {
    if (!storage_) {
        return;
    }
    const std::string id_hex = hash256_to_hex(sidechain_id);
    record_persistence_result_(
        storage_->put_metadata("scd:" + id_hex, serialize_sidechain_deposit(deposit)),
        "persist sidechain deposit"
    );
}

void ChainState::delete_sidechain_deposit_from_storage(const Hash256& sidechain_id) {
    if (!storage_) {
        return;
    }
    const std::string id_hex = hash256_to_hex(sidechain_id);
    record_persistence_result_(
        storage_->delete_metadata("scd:" + id_hex),
        "delete sidechain deposit"
    );
}

void ChainState::load_sidechain_deposits_from_storage() {
    sidechain_deposits_.clear();
    if (!storage_) {
        return;
    }
    const bool valid = internal::load_hash_metadata_records(*storage_, "scd:",
        [&](const std::string& id_hex, const Hash256&, const bytes& raw) {
            SidechainDeposit deposit;
            if (!deserialize_sidechain_deposit(raw, deposit) || !validate_sidechain_deposit(deposit, current_height_)) return false;
            return sidechain_deposits_.emplace(id_hex, std::move(deposit)).second;
        });
    if (!valid) {
        derived_state_storage_valid_ = false;
        GEO_LOG_ERROR("chainstate", "Corrupt sidechain deposit snapshot; rebuild required");
    }
}

void ChainState::persist_sidechain_registry_to_storage(
    const Hash256& sidechain_id,
    const SidechainRegistryEntry& entry
) {
    if (!storage_) {
        return;
    }
    const std::string id_hex = hash256_to_hex(sidechain_id);
    record_persistence_result_(
        storage_->put_metadata("sc:reg:" + id_hex, serialize_sidechain_registry(entry)),
        "persist sidechain registry"
    );
}

void ChainState::delete_sidechain_registry_from_storage(const Hash256& sidechain_id) {
    if (!storage_) {
        return;
    }
    const std::string id_hex = hash256_to_hex(sidechain_id);
    record_persistence_result_(
        storage_->delete_metadata("sc:reg:" + id_hex),
        "delete sidechain registry"
    );
}

void ChainState::load_sidechain_registry_from_storage() {
    sidechain_registry_.clear();
    if (!storage_) {
        return;
    }
    std::set<std::string> namespaces;
    bool valid = internal::load_hash_metadata_records(*storage_, "sc:reg:",
        [&](const std::string& id_hex, const Hash256&, const bytes& raw) {
            SidechainRegistryEntry entry;
            const auto deposit = sidechain_deposits_.find(id_hex);
            if (!deserialize_sidechain_registry(raw, entry) || !validate_sidechain_registry(entry, current_height_) ||
                deposit == sidechain_deposits_.end() || entry.owner != deposit->second.owner ||
                (!entry.accounting_namespace.empty() && !namespaces.insert(entry.accounting_namespace).second)) return false;
            return sidechain_registry_.emplace(id_hex, std::move(entry)).second;
        });
    for (const auto& [id, deposit] : sidechain_deposits_) {
        if (deposit.last_checkpoint_height > 0 && !sidechain_registry_.contains(id)) valid = false;
    }
    if (!valid) {
        derived_state_storage_valid_ = false;
        GEO_LOG_ERROR("chainstate", "Corrupt sidechain registry snapshot; rebuild required");
    }
}

} // namespace geocoin::state
