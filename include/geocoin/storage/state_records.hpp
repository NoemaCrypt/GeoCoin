#pragma once

#include "geocoin/types.hpp"
#include <algorithm>
#include <span>
#include <limits>
#include <stdexcept>

namespace geocoin::storage {

// The current on-disk record has no optional fields: 8-byte height,
// 32-byte block hash and 8-byte transaction count, all integers little endian.
struct ActiveTransactionRecord {
    Height height = 0;
    Hash256 block_hash{};
    uint64_t transaction_count = 0;

    bytes serialize() const {
        bytes result(48);
        for (size_t i = 0; i < 8; ++i) {
            result[i] = static_cast<byte>(height >> (8 * i));
            result[40 + i] = static_cast<byte>(transaction_count >> (8 * i));
        }
        std::copy(block_hash.begin(), block_hash.end(), result.begin() + 8);
        return result;
    }

    static bool decode(std::span<const byte> data, ActiveTransactionRecord& out) {
        if (data.size() != 48) return false;
        ActiveTransactionRecord record;
        for (size_t i = 0; i < 8; ++i) {
            record.height |= static_cast<uint64_t>(data[i]) << (8 * i);
            record.transaction_count |= static_cast<uint64_t>(data[40 + i]) << (8 * i);
        }
        if (record.transaction_count == 0) return false;
        std::copy(data.begin() + 8, data.begin() + 40, record.block_hash.begin());
        out = record;
        return true;
    }
};

// One compact reverse index per transaction. Keys are sorted, unique forward
// qaddr:/qnft: references; lengths are uint32 little endian, without padding.
struct QueryReferenceList {
    static bool valid_keys(const std::vector<std::string>& keys) {
        return !keys.empty() && std::is_sorted(keys.begin(), keys.end()) &&
            std::adjacent_find(keys.begin(), keys.end()) == keys.end() &&
            std::all_of(keys.begin(), keys.end(), [](const std::string& key) {
                return key.starts_with("qaddr:") || key.starts_with("qnft:");
            });
    }
    static bytes serialize(const std::vector<std::string>& keys) {
        if (!valid_keys(keys)) throw std::invalid_argument("Invalid query reference list");
        bytes out;
        for (const auto& key : keys) {
            if (key.size() > std::numeric_limits<uint32_t>::max())
                throw std::length_error("Query reference key is too large");
            const auto length = static_cast<uint32_t>(key.size());
            for (size_t i = 0; i < 4; ++i) out.push_back(static_cast<byte>(length >> (8 * i)));
            out.insert(out.end(), key.begin(), key.end());
        }
        return out;
    }
    static bool decode(std::span<const byte> data, std::vector<std::string>& out) {
        out.clear();
        std::vector<std::string> keys;
        size_t offset = 0;
        while (offset < data.size()) {
            if (data.size() - offset < 4) return false;
            uint32_t length = 0;
            for (size_t i = 0; i < 4; ++i) length |= uint32_t(data[offset++]) << (8 * i);
            if (length == 0 || length > data.size() - offset) return false;
            keys.emplace_back(reinterpret_cast<const char*>(data.data() + offset), length);
            offset += length;
        }
        if (!valid_keys(keys)) return false;
        out = std::move(keys);
        return true;
    }
};

} // namespace geocoin::storage
