#pragma once

#include "geocoin/crypto/geo_area.hpp"
#include "geocoin/types.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>

namespace geocoin::test {

inline void expect(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

inline void expect(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Fn>
void expect_throws(const char* message, Fn&& fn)
{
    try {
        std::forward<Fn>(fn)();
    } catch (const std::exception&) {
        return;
    }
    throw std::runtime_error(message);
}

template <typename Fn>
void expect_throws(Fn&& fn, const char* message)
{
    expect_throws(message, std::forward<Fn>(fn));
}

inline bytes make_key(uint8_t seed)
{
    bytes key(32);
    for (size_t i = 0; i < key.size(); ++i) {
        key[i] = static_cast<byte>(seed + i);
    }
    return key;
}

inline Hash256 hash_from_seed(uint8_t seed, uint8_t step = 17)
{
    Hash256 hash{};
    for (size_t i = 0; i < hash.size(); ++i) {
        hash[i] = static_cast<byte>(seed + i * step);
    }
    return hash;
}

inline Address address_from_key(const bytes& key)
{
    Address address;
    expect(crypto::compute_address_from_private_key(key, address), "failed to derive address");
    return address;
}

inline Address address_from_key(uint8_t seed)
{
    return address_from_key(make_key(seed));
}

inline Address address_from_seed(uint8_t seed)
{
    return address_from_key(seed);
}

inline std::filesystem::path make_temp_dir(const std::string& name)
{
    const auto path = std::filesystem::temp_directory_path() /
        (name + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
    return path;
}

} // namespace geocoin::test
