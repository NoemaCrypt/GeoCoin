#pragma once

#include "geocoin/types.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>

namespace geocoin::utils {

inline int hex_digit_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return 10 + (c - 'a');
    }
    if (c >= 'A' && c <= 'F') {
        return 10 + (c - 'A');
    }
    return -1;
}

inline bool is_hex_string(std::string_view value) {
    return std::all_of(value.begin(), value.end(), [](char c) {
        return hex_digit_value(c) >= 0;
    });
}

inline std::string bytes_to_hex(const byte* data, size_t size) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.resize(size * 2);
    for (size_t i = 0; i < size; ++i) {
        const byte b = data[i];
        out[i * 2] = kHex[(b >> 4) & 0x0F];
        out[i * 2 + 1] = kHex[b & 0x0F];
    }
    return out;
}

inline std::string bytes_to_hex(const bytes& data) {
    return bytes_to_hex(data.data(), data.size());
}

inline std::string hash256_to_hex(const Hash256& hash) {
    return bytes_to_hex(hash.data(), hash.size());
}

inline bool hex_to_bytes_checked(std::string_view hex, bytes& out) {
    if (hex.size() % 2 != 0) {
        return false;
    }

    bytes parsed;
    parsed.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        const int hi = hex_digit_value(hex[i]);
        const int lo = hex_digit_value(hex[i + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        parsed.push_back(static_cast<byte>((hi << 4) | lo));
    }

    out = std::move(parsed);
    return true;
}

inline bytes hex_to_bytes(std::string_view hex) {
    bytes out;
    if (!hex_to_bytes_checked(hex, out)) {
        return {};
    }
    return out;
}

inline bool hex_to_hash256_checked(std::string_view hex, Hash256& out) {
    bytes parsed;
    if (!hex_to_bytes_checked(hex, parsed) || parsed.size() != out.size()) {
        return false;
    }
    std::copy(parsed.begin(), parsed.end(), out.begin());
    return true;
}

inline bool is_null_hash256(const Hash256& hash) {
    return std::all_of(hash.begin(), hash.end(), [](byte b) {
        return b == 0;
    });
}

inline bytes encode_u64_le(uint64_t value) {
    bytes out(8);
    for (size_t i = 0; i < 8; ++i) {
        out[i] = static_cast<byte>((value >> (i * 8)) & 0xFFu);
    }
    return out;
}

inline void append_u32_le(bytes& out, uint32_t value) {
    for (size_t i = 0; i < sizeof(value); ++i) {
        out.push_back(static_cast<byte>((value >> (i * 8)) & 0xFFu));
    }
}

inline void append_u64_le(bytes& out, uint64_t value) {
    const auto encoded = encode_u64_le(value);
    out.insert(out.end(), encoded.begin(), encoded.end());
}

inline void append_hash256(bytes& out, const Hash256& hash) {
    out.insert(out.end(), hash.begin(), hash.end());
}

inline bool decode_u64_le_exact(const bytes& raw, uint64_t& out) {
    if (raw.size() != 8) {
        return false;
    }

    uint64_t value = 0;
    for (size_t i = 0; i < 8; ++i) {
        value |= static_cast<uint64_t>(raw[i]) << (i * 8);
    }
    out = value;
    return true;
}

inline uint64_t read_u64_le_unchecked(const bytes& in, size_t offset) {
    uint64_t value = 0;
    for (size_t i = 0; i < 8; ++i) {
        value |= static_cast<uint64_t>(in[offset + i]) << (i * 8);
    }
    return value;
}

inline void write_u64_le(bytes& data, size_t offset, uint64_t value) {
    for (size_t i = 0; i < 8; ++i) {
        data[offset + i] = static_cast<byte>((value >> (i * 8)) & 0xFFu);
    }
}

inline bytes encode_height_le(Height height) {
    return encode_u64_le(static_cast<uint64_t>(height));
}

inline bool decode_height_le_exact(const bytes& raw, Height& out) {
    uint64_t value = 0;
    if (!decode_u64_le_exact(raw, value)) {
        return false;
    }
    out = static_cast<Height>(value);
    return true;
}

} // namespace geocoin::utils
