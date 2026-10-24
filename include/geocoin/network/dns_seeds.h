#pragma once

#include <string>
#include <vector>

namespace geocoin::network {

/**
 * @brief Порты P2P по умолчанию
 */
static constexpr int DEFAULT_P2P_PORT = 3030;
static constexpr int DEFAULT_TESTNET_P2P_PORT = 13030;

/**
 * @brief Shared DNS seed configuration for discovery and node startup.
 */
inline const std::vector<std::string>& get_dns_seed_hostnames(bool use_testnet) {
    static const std::vector<std::string> kMainnetSeeds = {
        "seed.geocoin.network",
        "seed1.geocoin.network",
        "seed2.geocoin.network",
    };
    static const std::vector<std::string> kTestnetSeeds = {
        "testnet-seed.geocoin.network",
        "testnet-seed1.geocoin.network",
    };
    return use_testnet ? kTestnetSeeds : kMainnetSeeds;
}

/**
 * @brief Резервные seed endpoints.
 *
 * Используем те же публичные hostnames, но уже как host:port endpoints для
 * прямых outbound attempts, если DNS batch seeding не вернул адресов.
 */
inline std::vector<std::string> get_hardcoded_seed_nodes(bool use_testnet) {
    const auto& hosts = get_dns_seed_hostnames(use_testnet);
    const int port = use_testnet ? DEFAULT_TESTNET_P2P_PORT : DEFAULT_P2P_PORT;

    std::vector<std::string> seeds;
    seeds.reserve(hosts.size());
    for (const auto& host : hosts) {
        seeds.push_back(host + ":" + std::to_string(port));
    }
    return seeds;
}

} // namespace geocoin::network
