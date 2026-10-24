#pragma once

#include "geocoin/state/state.hpp"
#include "geocoin/consensus/consensus.hpp"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

namespace geocoin::state {

constexpr uint8_t kBlockUndoInterestPoolTagLocal = 0xE1;
constexpr uint8_t kTxUndoDepositBatchTagLocal = 0xBC;
constexpr uint8_t kTxUndoCardTagLocal = 0xC4;
constexpr uint8_t kTxUndoWitnessPubkeyTagLocal = 0xD1;
constexpr uint8_t kTxUndoSidechainPolicyTagLocal = 0xD2;
constexpr uint8_t kTxUndoSidechainRegistryMetaTagLocal = 0xD3;
constexpr uint8_t kTxUndoSidechainRegistryMetaVersionLocal = 1;

class ByteReader {
public:
    ByteReader(const bytes& data, const char* label)
        : data_(data)
        , label_(label)
    {
    }

    bool empty() const {
        return offset_ >= data_.size();
    }

    size_t remaining() const {
        return data_.size() - offset_;
    }

    uint8_t peek_u8(const char* field) const {
        require(1, field);
        return data_[offset_];
    }

    uint8_t peek_u8_at(size_t relative_offset, const char* field) const {
        if (relative_offset >= remaining()) {
            throw std::runtime_error(
                std::string(label_) + ": invalid " + field
            );
        }
        return data_[offset_ + relative_offset];
    }

    uint8_t read_u8(const char* field) {
        require(1, field);
        return data_[offset_++];
    }

    uint16_t read_u16_le(const char* field) {
        require(2, field);
        uint16_t value = static_cast<uint16_t>(data_[offset_]) |
            (static_cast<uint16_t>(data_[offset_ + 1]) << 8);
        offset_ += 2;
        return value;
    }

    uint32_t read_u32_le(const char* field) {
        require(4, field);
        uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            value |= static_cast<uint32_t>(data_[offset_++]) << (i * 8);
        }
        return value;
    }

    uint64_t read_u64_le(const char* field) {
        require(8, field);
        uint64_t value = 0;
        for (int i = 0; i < 8; ++i) {
            value |= static_cast<uint64_t>(data_[offset_++]) << (i * 8);
        }
        return value;
    }

    Amount read_amount(const char* field) {
        const uint64_t value = read_u64_le(field);
        if (value > static_cast<uint64_t>((std::numeric_limits<Amount>::max)())) {
            throw std::runtime_error(
                std::string(label_) + ": invalid " + field
            );
        }
        return static_cast<Amount>(value);
    }

    Amount read_i64_le(const char* field) {
        static_assert(sizeof(Amount) == sizeof(uint64_t));
        return std::bit_cast<Amount>(read_u64_le(field));
    }

    Hash256 read_hash256(const char* field) {
        require(32, field);
        Hash256 value{};
        std::copy(data_.begin() + static_cast<std::ptrdiff_t>(offset_),
                  data_.begin() + static_cast<std::ptrdiff_t>(offset_ + 32),
                  value.begin());
        offset_ += 32;
        return value;
    }

    bytes read_bytes(size_t len, const char* field) {
        require(len, field);
        bytes value(
            data_.begin() + static_cast<std::ptrdiff_t>(offset_),
            data_.begin() + static_cast<std::ptrdiff_t>(offset_ + len)
        );
        offset_ += len;
        return value;
    }

    std::string read_string(size_t len, const char* field) {
        require(len, field);
        std::string value(
            data_.begin() + static_cast<std::ptrdiff_t>(offset_),
            data_.begin() + static_cast<std::ptrdiff_t>(offset_ + len)
        );
        offset_ += len;
        return value;
    }

private:
    void require(size_t len, const char* field) const {
        if (len > remaining()) {
            throw std::runtime_error(
                std::string(label_) + ": invalid " + field
            );
        }
    }

    const bytes& data_;
    const char* label_;
    size_t offset_ = 0;
};

inline void append_u32(bytes& data, uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        data.push_back((value >> (i * 8)) & 0xFF);
    }
}

inline void append_u64(bytes& data, uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        data.push_back((value >> (i * 8)) & 0xFF);
    }
}

inline void append_encoded_string(bytes& data, const std::string& value) {
    append_u32(data, static_cast<uint32_t>(value.size()));
    data.insert(data.end(), value.begin(), value.end());
}

inline void append_card_authority(bytes& data, const core::CardAuthority& authority) {
    data.push_back(authority.quorum);
    append_u32(data, static_cast<uint32_t>(authority.members.size()));
    for (const auto& member : authority.members) {
        append_encoded_string(data, member.str());
    }
}

inline void append_card_program(bytes& data, const CardProgramEntry& program) {
    append_encoded_string(data, program.issuer.str());
    append_encoded_string(data, program.chip_vendor);
    append_encoded_string(data, program.chip_os);
    append_encoded_string(data, program.applet_aid);
    append_u32(data, program.applet_version);
    data.insert(data.end(), program.applet_package_hash.begin(), program.applet_package_hash.end());
    append_u32(data, program.policy_id);
    data.insert(data.end(), program.scope_id.begin(), program.scope_id.end());
    append_u64(data, program.created_height);
    data.push_back(program.corporate_policy.version);
    if (program.corporate_policy.version != 0) {
        append_card_authority(data, program.corporate_policy.mint);
        append_card_authority(data, program.corporate_policy.freeze);
        append_card_authority(data, program.corporate_policy.operations);
        append_card_authority(data, program.corporate_policy.recovery);
        data.insert(
            data.end(),
            program.corporate_policy.compliance_policy_hash.begin(),
            program.corporate_policy.compliance_policy_hash.end()
        );
        append_u32(data, program.corporate_policy.max_active_cards);
    }
}

void read_deposit_undo_payload(
    ByteReader& reader,
    TxUndo::DepositUndo& out,
    const char* label,
    bool optional_checkpoint_heights
);
core::CardAuthority read_card_authority(ByteReader& reader);
CardProgramEntry read_card_program(ByteReader& reader);

} // namespace geocoin::state
