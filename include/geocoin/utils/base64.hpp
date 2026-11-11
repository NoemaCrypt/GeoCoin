#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace geocoin::utils {

inline size_t base64_decoded_size(size_t encoded_size) {
    return encoded_size / 4 * 3;
}

inline size_t base64_encoded_size(size_t raw_size) {
    return (raw_size + 2) / 3 * 4;
}

inline size_t base64_encode(char* output, const void* input, size_t input_size) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const auto* raw = static_cast<const uint8_t*>(input);
    size_t written = 0;
    size_t i = 0;
    while (i + 3 <= input_size) {
        const uint32_t value = (uint32_t(raw[i]) << 16) | (uint32_t(raw[i + 1]) << 8) | raw[i + 2];
        output[written++] = kAlphabet[(value >> 18) & 63];
        output[written++] = kAlphabet[(value >> 12) & 63];
        output[written++] = kAlphabet[(value >> 6) & 63];
        output[written++] = kAlphabet[value & 63];
        i += 3;
    }
    if (i < input_size) {
        uint32_t value = uint32_t(raw[i]) << 16;
        output[written++] = kAlphabet[(value >> 18) & 63];
        if (i + 1 < input_size) {
            value |= uint32_t(raw[i + 1]) << 8;
            output[written++] = kAlphabet[(value >> 12) & 63];
            output[written++] = kAlphabet[(value >> 6) & 63];
            output[written++] = '=';
        } else {
            output[written++] = kAlphabet[(value >> 12) & 63];
            output[written++] = '=';
            output[written++] = '=';
        }
    }
    return written;
}

// Returns {decoded bytes, consumed input}. A failed decode returns {0, 0}.
inline std::pair<size_t, size_t> base64_decode(void* output, const char* input, size_t input_size) {
    auto value_of = [](unsigned char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };

    auto* raw = static_cast<uint8_t*>(output);
    size_t written = 0;
    size_t i = 0;
    while (i < input_size) {
        if (input[i] == '=') break;
        if (i + 2 > input_size) return {0, 0};
        const int a = value_of(static_cast<unsigned char>(input[i]));
        const int b = value_of(static_cast<unsigned char>(input[i + 1]));
        if (a < 0 || b < 0) return {0, 0};
        if (i + 2 == input_size || input[i + 2] == '=') {
            if ((b & 0x0f) != 0) return {0, 0};
            raw[written++] = static_cast<uint8_t>((a << 2) | (b >> 4));
            i += 2;
            break;
        }
        const int c = value_of(static_cast<unsigned char>(input[i + 2]));
        if (c < 0) return {0, 0};
        if (i + 3 == input_size || input[i + 3] == '=') {
            if ((c & 0x03) != 0) return {0, 0};
            raw[written++] = static_cast<uint8_t>((a << 2) | (b >> 4));
            raw[written++] = static_cast<uint8_t>((b << 4) | (c >> 2));
            i += 3;
            break;
        }
        const int d = value_of(static_cast<unsigned char>(input[i + 3]));
        if (d < 0) return {0, 0};
        raw[written++] = static_cast<uint8_t>((a << 2) | (b >> 4));
        raw[written++] = static_cast<uint8_t>((b << 4) | (c >> 2));
        raw[written++] = static_cast<uint8_t>((c << 6) | d);
        i += 4;
    }
    while (i < input_size) {
        if (input[i] != '=') return {0, 0};
        ++i;
    }
    return {written, input_size};
}

inline std::string base64_encode(const std::string& input) {
    std::string output(base64_encoded_size(input.size()), '\0');
    const size_t written = base64_encode(output.data(), input.data(), input.size());
    output.resize(written);
    return output;
}

} // namespace geocoin::utils
