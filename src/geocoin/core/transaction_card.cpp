#include "geocoin/core/transaction_codec.hpp"

#include <algorithm>
#include <stdexcept>

namespace geocoin::core::tx_codec {
namespace {

void append_card_authority(bytes& out, const CardAuthority& authority) {
    out.push_back(authority.quorum);
    const auto count = utils::encode_varint(authority.members.size());
    out.insert(out.end(), count.begin(), count.end());
    for (const auto& member : authority.members) {
        append_varbytes(out, member.str());
    }
}

void append_card_corporate_policy(bytes& out, const CardCorporatePolicy& policy) {
    out.push_back(policy.version);
    append_card_authority(out, policy.mint);
    append_card_authority(out, policy.freeze);
    append_card_authority(out, policy.operations);
    append_card_authority(out, policy.recovery);
    out.insert(out.end(), policy.compliance_policy_hash.begin(), policy.compliance_policy_hash.end());
    for (int i = 0; i < 4; i++) {
        out.push_back((policy.max_active_cards >> (i * 8)) & 0xFF);
    }
}

CardAuthority read_card_authority(const bytes& data, size_t& offset) {
    if (offset >= data.size()) {
        throw std::runtime_error("Invalid transaction data: missing card authority quorum");
    }
    CardAuthority authority;
    authority.quorum = data[offset++];
    const uint64_t member_count = utils::decode_varint(data, offset);
    if (member_count > consensus::params::MAX_CARD_AUTHORITY_MEMBERS) {
        throw std::runtime_error("Invalid transaction data: too many card authority members");
    }
    authority.members.reserve(static_cast<size_t>(member_count));
    for (uint64_t index = 0; index < member_count; ++index) {
        const uint64_t address_length = utils::decode_varint(data, offset);
        if (address_length > consensus::params::MAX_ADDRESS_SIZE ||
            address_length > data.size() - offset) {
            throw std::runtime_error("Invalid transaction data: invalid card authority address");
        }
        authority.members.emplace_back(
            Address(std::string(data.begin() + offset, data.begin() + offset + address_length))
        );
        offset += static_cast<size_t>(address_length);
    }
    return authority;
}

CardCorporatePolicy read_card_corporate_policy(const bytes& data, size_t& offset) {
    if (offset >= data.size()) {
        throw std::runtime_error("Invalid transaction data: missing card corporate policy");
    }
    CardCorporatePolicy policy;
    policy.version = data[offset++];
    policy.mint = read_card_authority(data, offset);
    policy.freeze = read_card_authority(data, offset);
    policy.operations = read_card_authority(data, offset);
    policy.recovery = read_card_authority(data, offset);
    if (offset + 32 + 4 > data.size()) {
        throw std::runtime_error("Invalid transaction data: truncated card corporate policy");
    }
    std::copy(
        data.begin() + offset,
        data.begin() + offset + 32,
        policy.compliance_policy_hash.begin()
    );
    offset += 32;
    for (int i = 0; i < 4; i++) {
        policy.max_active_cards |= static_cast<uint32_t>(data[offset++]) << (i * 8);
    }
    return policy;
}

} // namespace

void append_card_payload(bytes& data, const Transaction& tx) {
    if (tx.type == TransactionType::CARD_PROGRAM_DEFINE) {
        data.insert(data.end(), tx.card_program_id.begin(), tx.card_program_id.end());

        const std::string issuer = tx.card_issuer.str();
        auto issuer_len = utils::encode_varint(issuer.size());
        data.insert(data.end(), issuer_len.begin(), issuer_len.end());
        data.insert(data.end(), issuer.begin(), issuer.end());

        auto vendor_len = utils::encode_varint(tx.card_chip_vendor.size());
        data.insert(data.end(), vendor_len.begin(), vendor_len.end());
        data.insert(data.end(), tx.card_chip_vendor.begin(), tx.card_chip_vendor.end());

        auto os_len = utils::encode_varint(tx.card_chip_os.size());
        data.insert(data.end(), os_len.begin(), os_len.end());
        data.insert(data.end(), tx.card_chip_os.begin(), tx.card_chip_os.end());

        auto aid_len = utils::encode_varint(tx.card_applet_aid.size());
        data.insert(data.end(), aid_len.begin(), aid_len.end());
        data.insert(data.end(), tx.card_applet_aid.begin(), tx.card_applet_aid.end());

        for (int i = 0; i < 4; i++) {
            data.push_back((tx.card_applet_version >> (i * 8)) & 0xFF);
        }

        data.insert(data.end(), tx.card_applet_package_hash.begin(), tx.card_applet_package_hash.end());

        for (int i = 0; i < 4; i++) {
            data.push_back((tx.card_policy_id >> (i * 8)) & 0xFF);
        }

        data.insert(data.end(), tx.card_scope_id.begin(), tx.card_scope_id.end());
        append_card_corporate_policy(data, tx.card_corporate_policy);
        return;
    }

    if (tx.type == TransactionType::CARD_ISSUE) {
        data.insert(data.end(), tx.card_program_id.begin(), tx.card_program_id.end());

        const std::string issuer = tx.card_issuer.str();
        auto issuer_len = utils::encode_varint(issuer.size());
        data.insert(data.end(), issuer_len.begin(), issuer_len.end());
        data.insert(data.end(), issuer.begin(), issuer.end());

        for (int i = 0; i < 4; i++) {
            data.push_back((tx.card_policy_id >> (i * 8)) & 0xFF);
        }

        data.insert(data.end(), tx.card_scope_id.begin(), tx.card_scope_id.end());
        data.insert(data.end(), tx.card_uid_hash.begin(), tx.card_uid_hash.end());
        data.insert(data.end(), tx.card_pubkey_hash.begin(), tx.card_pubkey_hash.end());

        for (int i = 0; i < 8; i++) {
            data.push_back((tx.card_valid_from >> (i * 8)) & 0xFF);
        }
        for (int i = 0; i < 8; i++) {
            data.push_back((tx.card_valid_to >> (i * 8)) & 0xFF);
        }

        data.push_back(tx.card_status);
        data.insert(data.end(), tx.card_metadata_hash.begin(), tx.card_metadata_hash.end());
        return;
    }

    if (tx.type == TransactionType::CARD_UPDATE_STATUS) {
        data.insert(data.end(), tx.card_program_id.begin(), tx.card_program_id.end());

        const std::string issuer = tx.card_issuer.str();
        auto issuer_len = utils::encode_varint(issuer.size());
        data.insert(data.end(), issuer_len.begin(), issuer_len.end());
        data.insert(data.end(), issuer.begin(), issuer.end());

        data.insert(data.end(), tx.card_uid_hash.begin(), tx.card_uid_hash.end());
        data.push_back(tx.card_status);
        data.insert(data.end(), tx.card_metadata_hash.begin(), tx.card_metadata_hash.end());
        return;
    }

    if (tx.type == TransactionType::CARD_UPDATE_POLICY) {
        data.insert(data.end(), tx.card_program_id.begin(), tx.card_program_id.end());
        append_varbytes(data, tx.card_issuer.str());
        for (int i = 0; i < 4; i++) {
            data.push_back((tx.card_policy_id >> (i * 8)) & 0xFF);
        }
        data.insert(data.end(), tx.card_scope_id.begin(), tx.card_scope_id.end());
        append_card_corporate_policy(data, tx.card_corporate_policy);
    }
}

void read_card_payload(Transaction& tx, const bytes& data, size_t& offset) {
    auto require = [&](uint64_t len) { require_remaining(data, offset, len); };
    auto read_vi = [&]() { return read_varint(data, offset); };

    if (tx.type == TransactionType::CARD_PROGRAM_DEFINE) {
        if (offset + 32 > data.size()) throw std::runtime_error("Invalid transaction data");
        std::copy(data.begin() + offset, data.begin() + offset + 32, tx.card_program_id.begin());
        offset += 32;

        uint64_t issuer_len = read_vi();
        if (issuer_len > consensus::params::MAX_ADDRESS_SIZE) {
            throw std::runtime_error("Invalid transaction data: card issuer too long");
        }
        require(issuer_len);
        tx.card_issuer = Address(std::string(data.begin() + offset, data.begin() + offset + issuer_len));
        offset += issuer_len;

        uint64_t vendor_len = read_vi();
        if (vendor_len > consensus::params::MAX_CARD_CHIP_VENDOR) {
            throw std::runtime_error("Invalid transaction data: card chip vendor too long");
        }
        require(vendor_len);
        tx.card_chip_vendor.assign(data.begin() + offset, data.begin() + offset + vendor_len);
        offset += vendor_len;

        uint64_t os_len = read_vi();
        if (os_len > consensus::params::MAX_CARD_CHIP_OS) {
            throw std::runtime_error("Invalid transaction data: card chip os too long");
        }
        require(os_len);
        tx.card_chip_os.assign(data.begin() + offset, data.begin() + offset + os_len);
        offset += os_len;

        uint64_t aid_len = read_vi();
        if (aid_len > consensus::params::MAX_CARD_APPLET_AID) {
            throw std::runtime_error("Invalid transaction data: card applet aid too long");
        }
        require(aid_len);
        tx.card_applet_aid.assign(data.begin() + offset, data.begin() + offset + aid_len);
        offset += aid_len;

        if (offset + 4 > data.size()) throw std::runtime_error("Invalid transaction data");
        tx.card_applet_version = 0;
        for (int i = 0; i < 4; i++) {
            tx.card_applet_version |= (uint32_t)data[offset++] << (i * 8);
        }

        if (offset + 32 > data.size()) throw std::runtime_error("Invalid transaction data");
        std::copy(
            data.begin() + offset,
            data.begin() + offset + 32,
            tx.card_applet_package_hash.begin()
        );
        offset += 32;

        if (offset + 4 > data.size()) throw std::runtime_error("Invalid transaction data");
        tx.card_policy_id = 0;
        for (int i = 0; i < 4; i++) {
            tx.card_policy_id |= (uint32_t)data[offset++] << (i * 8);
        }

        if (offset + 32 > data.size()) throw std::runtime_error("Invalid transaction data");
        std::copy(data.begin() + offset, data.begin() + offset + 32, tx.card_scope_id.begin());
        offset += 32;
        tx.card_corporate_policy = read_card_corporate_policy(data, offset);
        return;
    }

    if (tx.type == TransactionType::CARD_ISSUE) {
        if (offset + 32 > data.size()) throw std::runtime_error("Invalid transaction data");
        std::copy(data.begin() + offset, data.begin() + offset + 32, tx.card_program_id.begin());
        offset += 32;

        uint64_t issuer_len = read_vi();
        if (issuer_len > consensus::params::MAX_ADDRESS_SIZE) {
            throw std::runtime_error("Invalid transaction data: card issuer too long");
        }
        require(issuer_len);
        tx.card_issuer = Address(std::string(data.begin() + offset, data.begin() + offset + issuer_len));
        offset += issuer_len;

        if (offset + 4 > data.size()) throw std::runtime_error("Invalid transaction data");
        tx.card_policy_id = 0;
        for (int i = 0; i < 4; i++) {
            tx.card_policy_id |= (uint32_t)data[offset++] << (i * 8);
        }

        if (offset + 32 > data.size()) throw std::runtime_error("Invalid transaction data");
        std::copy(data.begin() + offset, data.begin() + offset + 32, tx.card_scope_id.begin());
        offset += 32;

        if (offset + 32 > data.size()) throw std::runtime_error("Invalid transaction data");
        std::copy(data.begin() + offset, data.begin() + offset + 32, tx.card_uid_hash.begin());
        offset += 32;

        if (offset + 32 > data.size()) throw std::runtime_error("Invalid transaction data");
        std::copy(data.begin() + offset, data.begin() + offset + 32, tx.card_pubkey_hash.begin());
        offset += 32;

        if (offset + 8 > data.size()) throw std::runtime_error("Invalid transaction data");
        tx.card_valid_from = 0;
        for (int i = 0; i < 8; i++) {
            tx.card_valid_from |= (uint64_t)data[offset++] << (i * 8);
        }

        if (offset + 8 > data.size()) throw std::runtime_error("Invalid transaction data");
        tx.card_valid_to = 0;
        for (int i = 0; i < 8; i++) {
            tx.card_valid_to |= (uint64_t)data[offset++] << (i * 8);
        }

        if (offset + 1 > data.size()) throw std::runtime_error("Invalid transaction data");
        tx.card_status = data[offset++];

        if (offset + 32 > data.size()) throw std::runtime_error("Invalid transaction data");
        std::copy(data.begin() + offset, data.begin() + offset + 32, tx.card_metadata_hash.begin());
        offset += 32;
        return;
    }

    if (tx.type == TransactionType::CARD_UPDATE_STATUS) {
        if (offset + 32 > data.size()) throw std::runtime_error("Invalid transaction data");
        std::copy(data.begin() + offset, data.begin() + offset + 32, tx.card_program_id.begin());
        offset += 32;

        uint64_t issuer_len = read_vi();
        if (issuer_len > consensus::params::MAX_ADDRESS_SIZE) {
            throw std::runtime_error("Invalid transaction data: card issuer too long");
        }
        require(issuer_len);
        tx.card_issuer = Address(std::string(data.begin() + offset, data.begin() + offset + issuer_len));
        offset += issuer_len;

        if (offset + 32 > data.size()) throw std::runtime_error("Invalid transaction data");
        std::copy(data.begin() + offset, data.begin() + offset + 32, tx.card_uid_hash.begin());
        offset += 32;

        if (offset + 1 > data.size()) throw std::runtime_error("Invalid transaction data");
        tx.card_status = data[offset++];

        if (offset + 32 > data.size()) throw std::runtime_error("Invalid transaction data");
        std::copy(data.begin() + offset, data.begin() + offset + 32, tx.card_metadata_hash.begin());
        offset += 32;
        return;
    }

    if (tx.type == TransactionType::CARD_UPDATE_POLICY) {
        if (offset + 32 > data.size()) throw std::runtime_error("Invalid transaction data");
        std::copy(data.begin() + offset, data.begin() + offset + 32, tx.card_program_id.begin());
        offset += 32;

        uint64_t issuer_len = read_vi();
        if (issuer_len > consensus::params::MAX_ADDRESS_SIZE) {
            throw std::runtime_error("Invalid transaction data: card issuer too long");
        }
        require(issuer_len);
        tx.card_issuer = Address(std::string(data.begin() + offset, data.begin() + offset + issuer_len));
        offset += issuer_len;

        if (offset + 4 > data.size()) throw std::runtime_error("Invalid transaction data");
        tx.card_policy_id = 0;
        for (int i = 0; i < 4; i++) {
            tx.card_policy_id |= static_cast<uint32_t>(data[offset++]) << (i * 8);
        }

        if (offset + 32 > data.size()) throw std::runtime_error("Invalid transaction data");
        std::copy(data.begin() + offset, data.begin() + offset + 32, tx.card_scope_id.begin());
        offset += 32;
        tx.card_corporate_policy = read_card_corporate_policy(data, offset);
    }
}

} // namespace geocoin::core::tx_codec
