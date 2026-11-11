#include "geocoin/state/state.hpp"
#include "geocoin/state/records.hpp"
#include "geocoin/state/state_persist_internal.hpp"

#include "geocoin/consensus/consensus.hpp"
#include "geocoin/consensus/card_rules.hpp"
#include "geocoin/consensus/sidechain_rules.hpp"
#include "geocoin/crypto/addresses.hpp"
#include "geocoin/utils/bytes.hpp"
#include "geocoin/utils/logger.hpp"

#include <string>

namespace geocoin::state {
using namespace records;

bool records::validate_card_program(const CardProgramEntry& entry, Height tip_height) {
    auto valid_text = [](const std::string& value, size_t max_size) {
        return !value.empty() && value.size() <= max_size && consensus::is_printable_ascii(value);
    };
    return crypto::verify_address(entry.issuer) && entry.created_height <= tip_height &&
           valid_text(entry.chip_vendor, consensus::params::MAX_CARD_CHIP_VENDOR) &&
           valid_text(entry.chip_os, consensus::params::MAX_CARD_CHIP_OS) &&
           valid_text(entry.applet_aid, consensus::params::MAX_CARD_APPLET_AID) &&
           entry.applet_version > 0 && !utils::is_null_hash256(entry.applet_package_hash) &&
           !utils::is_null_hash256(entry.scope_id) && consensus::valid_card_corporate_policy(entry.corporate_policy);
}

bool records::validate_card_instance(const CardInstanceEntry& entry, const CardProgramEntry& program, Height tip_height) {
    return !utils::is_null_hash256(entry.program_id) && !utils::is_null_hash256(entry.uid_hash) &&
           !utils::is_null_hash256(entry.pubkey_hash) && !utils::is_null_hash256(entry.metadata_hash) &&
           entry.issuer == program.issuer && entry.status <= 2 &&
           consensus::valid_card_validity(entry.valid_from, entry.valid_to) &&
           program.created_height <= entry.issued_height && entry.issued_height <= entry.updated_height &&
           entry.updated_height <= tip_height;
}

bytes records::serialize_card_program(const CardProgramEntry& entry) {
    bytes data;
    const std::string issuer = entry.issuer.str();
    uint32_t issuer_len = static_cast<uint32_t>(issuer.size());
    for (int i = 0; i < 4; i++) {
        data.push_back((issuer_len >> (i * 8)) & 0xFF);
    }
    data.insert(data.end(), issuer.begin(), issuer.end());

    auto append_string = [&data](const std::string& s) {
        uint32_t len = static_cast<uint32_t>(s.size());
        for (int i = 0; i < 4; i++) {
            data.push_back((len >> (i * 8)) & 0xFF);
        }
        data.insert(data.end(), s.begin(), s.end());
    };
    append_string(entry.chip_vendor);
    append_string(entry.chip_os);
    append_string(entry.applet_aid);

    for (int i = 0; i < 4; i++) {
        data.push_back((entry.applet_version >> (i * 8)) & 0xFF);
    }
    data.insert(data.end(), entry.applet_package_hash.begin(), entry.applet_package_hash.end());
    for (int i = 0; i < 4; i++) {
        data.push_back((entry.policy_id >> (i * 8)) & 0xFF);
    }
    data.insert(data.end(), entry.scope_id.begin(), entry.scope_id.end());
    for (int i = 0; i < 8; i++) {
        data.push_back((static_cast<uint64_t>(entry.created_height) >> (i * 8)) & 0xFF);
    }
    data.push_back(entry.corporate_policy.version);
    if (entry.corporate_policy.version != 0) {
        auto append_authority = [&data](const core::CardAuthority& authority) {
            data.push_back(authority.quorum);
            const uint32_t count = static_cast<uint32_t>(authority.members.size());
            for (int i = 0; i < 4; i++) {
                data.push_back((count >> (i * 8)) & 0xFF);
            }
            for (const auto& member : authority.members) {
                const std::string address = member.str();
                const uint32_t length = static_cast<uint32_t>(address.size());
                for (int i = 0; i < 4; i++) {
                    data.push_back((length >> (i * 8)) & 0xFF);
                }
                data.insert(data.end(), address.begin(), address.end());
            }
        };
        append_authority(entry.corporate_policy.mint);
        append_authority(entry.corporate_policy.freeze);
        append_authority(entry.corporate_policy.operations);
        append_authority(entry.corporate_policy.recovery);
        data.insert(
            data.end(),
            entry.corporate_policy.compliance_policy_hash.begin(),
            entry.corporate_policy.compliance_policy_hash.end()
        );
        for (int i = 0; i < 4; i++) {
            data.push_back((entry.corporate_policy.max_active_cards >> (i * 8)) & 0xFF);
        }
    }
    return data;
}

bool records::deserialize_card_program(const bytes& data, CardProgramEntry& out) {
    size_t offset = 0;
    auto read_u32 = [&data, &offset](uint32_t& out_v) -> bool {
        if (offset + 4 > data.size()) {
            return false;
        }
        out_v = 0;
        for (int i = 0; i < 4; i++) {
            out_v |= static_cast<uint32_t>(data[offset++]) << (i * 8);
        }
        return true;
    };
    auto read_u64 = [&data, &offset](uint64_t& out_v) -> bool {
        if (offset + 8 > data.size()) {
            return false;
        }
        out_v = 0;
        for (int i = 0; i < 8; i++) {
            out_v |= static_cast<uint64_t>(data[offset++]) << (i * 8);
        }
        return true;
    };
    auto read_string = [&data, &offset, &read_u32](std::string& out_s) -> bool {
        uint32_t len = 0;
        if (!read_u32(len) || len > data.size() - offset) {
            return false;
        }
        out_s.assign(data.begin() + offset, data.begin() + offset + len);
        offset += len;
        return true;
    };

    std::string issuer;
    if (!read_string(issuer)) {
        return false;
    }
    out.issuer = Address(issuer);
    if (!read_string(out.chip_vendor) ||
        !read_string(out.chip_os) ||
        !read_string(out.applet_aid)) {
        return false;
    }

    if (!read_u32(out.applet_version)) {
        return false;
    }
    if (offset + 32 > data.size()) {
        return false;
    }
    std::copy(data.begin() + offset, data.begin() + offset + 32, out.applet_package_hash.begin());
    offset += 32;

    if (!read_u32(out.policy_id)) {
        return false;
    }
    if (offset + 32 > data.size()) {
        return false;
    }
    std::copy(data.begin() + offset, data.begin() + offset + 32, out.scope_id.begin());
    offset += 32;

    uint64_t created = 0;
    if (!read_u64(created)) {
        return false;
    }
    out.created_height = static_cast<Height>(created);
    if (offset == data.size()) return false;
    out.corporate_policy = {};
    out.corporate_policy.version = data[offset++];
    if (out.corporate_policy.version == 0) return offset == data.size();
    if (out.corporate_policy.version != 1) {
        return false;
    }
    auto read_authority = [&data, &offset, &read_u32, &read_string](core::CardAuthority& authority) -> bool {
        if (offset >= data.size()) {
            return false;
        }
        authority.quorum = data[offset++];
        uint32_t count = 0;
        if (!read_u32(count) || count == 0 || count > consensus::params::MAX_CARD_AUTHORITY_MEMBERS) {
            return false;
        }
        authority.members.clear();
        authority.members.reserve(count);
        for (uint32_t index = 0; index < count; ++index) {
            std::string address;
            if (!read_string(address) || address.size() > consensus::params::MAX_ADDRESS_SIZE) {
                return false;
            }
            authority.members.emplace_back(Address(address));
        }
        return true;
    };
    if (!read_authority(out.corporate_policy.mint) ||
        !read_authority(out.corporate_policy.freeze) ||
        !read_authority(out.corporate_policy.operations) ||
        !read_authority(out.corporate_policy.recovery) ||
        offset + 32 + 4 > data.size()) {
        return false;
    }
    std::copy(
        data.begin() + offset,
        data.begin() + offset + 32,
        out.corporate_policy.compliance_policy_hash.begin()
    );
    offset += 32;
    for (int i = 0; i < 4; i++) {
        out.corporate_policy.max_active_cards |= static_cast<uint32_t>(data[offset++]) << (i * 8);
    }
    return offset == data.size();
}

bytes records::serialize_card_instance(const CardInstanceEntry& entry) {
    bytes data;
    data.insert(data.end(), entry.program_id.begin(), entry.program_id.end());

    const std::string issuer = entry.issuer.str();
    uint32_t issuer_len = static_cast<uint32_t>(issuer.size());
    for (int i = 0; i < 4; i++) {
        data.push_back((issuer_len >> (i * 8)) & 0xFF);
    }
    data.insert(data.end(), issuer.begin(), issuer.end());

    data.insert(data.end(), entry.uid_hash.begin(), entry.uid_hash.end());
    data.insert(data.end(), entry.pubkey_hash.begin(), entry.pubkey_hash.end());

    for (int i = 0; i < 8; i++) {
        data.push_back((entry.valid_from >> (i * 8)) & 0xFF);
    }
    for (int i = 0; i < 8; i++) {
        data.push_back((entry.valid_to >> (i * 8)) & 0xFF);
    }
    data.push_back(entry.status);
    data.insert(data.end(), entry.metadata_hash.begin(), entry.metadata_hash.end());
    for (int i = 0; i < 8; i++) {
        data.push_back((static_cast<uint64_t>(entry.issued_height) >> (i * 8)) & 0xFF);
    }
    for (int i = 0; i < 8; i++) {
        data.push_back((static_cast<uint64_t>(entry.updated_height) >> (i * 8)) & 0xFF);
    }
    return data;
}

bool records::deserialize_card_instance(const bytes& data, CardInstanceEntry& out) {
    size_t offset = 0;
    if (offset + 32 > data.size()) {
        return false;
    }
    std::copy(data.begin() + offset, data.begin() + offset + 32, out.program_id.begin());
    offset += 32;

    if (offset + 4 > data.size()) {
        return false;
    }
    uint32_t issuer_len = 0;
    for (int i = 0; i < 4; i++) {
        issuer_len |= static_cast<uint32_t>(data[offset++]) << (i * 8);
    }
    if (issuer_len > consensus::params::MAX_ADDRESS_SIZE ||
        issuer_len > data.size() - offset) {
        return false;
    }
    out.issuer = Address(std::string(data.begin() + offset, data.begin() + offset + issuer_len));
    offset += issuer_len;

    if (offset + 32 > data.size()) {
        return false;
    }
    std::copy(data.begin() + offset, data.begin() + offset + 32, out.uid_hash.begin());
    offset += 32;

    if (offset + 32 > data.size()) {
        return false;
    }
    std::copy(data.begin() + offset, data.begin() + offset + 32, out.pubkey_hash.begin());
    offset += 32;

    if (offset + 8 > data.size()) {
        return false;
    }
    out.valid_from = 0;
    for (int i = 0; i < 8; i++) {
        out.valid_from |= static_cast<uint64_t>(data[offset++]) << (i * 8);
    }

    if (offset + 8 > data.size()) {
        return false;
    }
    out.valid_to = 0;
    for (int i = 0; i < 8; i++) {
        out.valid_to |= static_cast<uint64_t>(data[offset++]) << (i * 8);
    }

    if (offset + 1 > data.size()) {
        return false;
    }
    out.status = data[offset++];

    if (offset + 32 > data.size()) {
        return false;
    }
    std::copy(data.begin() + offset, data.begin() + offset + 32, out.metadata_hash.begin());
    offset += 32;

    if (offset + 8 > data.size()) {
        return false;
    }
    out.issued_height = 0;
    for (int i = 0; i < 8; i++) {
        out.issued_height |= static_cast<uint64_t>(data[offset++]) << (i * 8);
    }

    if (offset + 8 > data.size()) {
        return false;
    }
    out.updated_height = 0;
    for (int i = 0; i < 8; i++) {
        out.updated_height |= static_cast<uint64_t>(data[offset++]) << (i * 8);
    }

    return offset == data.size();
}

void ChainState::persist_card_program_to_storage(
    const Hash256& card_program_id,
    const CardProgramEntry& entry
) {
    if (!storage_) {
        return;
    }
    const std::string id_hex = hash256_to_hex(card_program_id);
    record_persistence_result_(
        storage_->put_metadata("card:prog:" + id_hex, serialize_card_program(entry)),
        "persist card program"
    );
}

void ChainState::delete_card_program_from_storage(const Hash256& card_program_id) {
    if (!storage_) {
        return;
    }
    const std::string id_hex = hash256_to_hex(card_program_id);
    record_persistence_result_(
        storage_->delete_metadata("card:prog:" + id_hex),
        "delete card program"
    );
}

void ChainState::persist_card_instance_to_storage(
    const Hash256& card_uid_hash,
    const CardInstanceEntry& entry
) {
    if (!storage_) {
        return;
    }
    const std::string uid_hex = hash256_to_hex(card_uid_hash);
    record_persistence_result_(
        storage_->put_metadata("card:uid:" + uid_hex, serialize_card_instance(entry)),
        "persist card instance"
    );
}

void ChainState::delete_card_instance_from_storage(const Hash256& card_uid_hash) {
    if (!storage_) {
        return;
    }
    const std::string uid_hex = hash256_to_hex(card_uid_hash);
    record_persistence_result_(
        storage_->delete_metadata("card:uid:" + uid_hex),
        "delete card instance"
    );
}

void ChainState::load_card_registry_from_storage() {
    card_programs_.clear();
    card_instances_.clear();
    card_pubkey_to_uid_.clear();
    card_active_counts_.clear();

    if (!storage_) {
        return;
    }

    const bool programs_valid = internal::load_hash_metadata_records(*storage_, "card:prog:", [&](const std::string& id_hex, const Hash256&, const bytes& raw) {
        CardProgramEntry entry;
        if (!deserialize_card_program(raw, entry) || !validate_card_program(entry, current_height_)) return false;
        return card_programs_.emplace(id_hex, std::move(entry)).second;
    });

    const bool cards_valid = internal::load_hash_metadata_records(*storage_, "card:uid:", [&](const std::string&, const Hash256& uid, const bytes& raw) {
        CardInstanceEntry entry;
        if (!deserialize_card_instance(raw, entry) || entry.uid_hash != uid) return false;
        const auto program = card_programs_.find(hash256_to_hex(entry.program_id));
        if (program == card_programs_.end() || !validate_card_instance(entry, program->second, current_height_)) return false;
        const std::string pubkey_hex = hash256_to_hex(entry.pubkey_hash);
        if (card_pubkey_to_uid_.count(pubkey_hex) > 0) {
            GEO_LOG_WARN(
                "chainstate",
                "Duplicate card_pubkey_hash in storage: " << pubkey_hex.substr(0, 16) << "..."
            );
            return false;
        }
        cache_card_instance_(entry);
        return true;
    });
    if (!programs_valid || !cards_valid) {
        derived_state_storage_valid_ = false;
        GEO_LOG_ERROR("chainstate", "Corrupt card registry snapshot; rebuild required");
    }
}

} // namespace geocoin::state
