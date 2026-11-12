#pragma once

#include <string>
#include <vector>

namespace geocoin::network {

/**
 * @brief Порты P2P по умолчанию
 */
static constexpr int DEFAULT_P2P_PORT = 3030;
static constexpr int DEFAULT_TESTNET_P2P_PORT = 13030;
inline constexpr const char* DEFAULT_MAINNET_SEED_NODE = "45.12.114.4:3030";

/**
 * @brief Shared DNS seed configuration for discovery and node startup.
 */
inline const std::vector<std::string>& get_dns_seed_hostnames(bool use_testnet) {
    // No operational DNS seeds are configured. Mainnet uses the fixed endpoint
    // below; testnet peers must be supplied explicitly.
    static const std::vector<std::string> kMainnetSeeds;
    static const std::vector<std::string> kTestnetSeeds;
    return use_testnet ? kTestnetSeeds : kMainnetSeeds;
}

/**
 * @brief Резервные seed endpoints.
 *
 * Mainnet includes a literal IP endpoint so bootstrap works without DNS.
 * Configured DNS hostnames, when available, are additional endpoints.
 */
inline std::vector<std::string> get_hardcoded_seed_nodes(bool use_testnet) {
    const auto& hosts = get_dns_seed_hostnames(use_testnet);
    const int port = use_testnet ? DEFAULT_TESTNET_P2P_PORT : DEFAULT_P2P_PORT;

    std::vector<std::string> seeds;
    seeds.reserve(hosts.size() + (use_testnet ? 0 : 1));
    if (!use_testnet) {
        seeds.emplace_back(DEFAULT_MAINNET_SEED_NODE);
    }
    for (const auto& host : hosts) {
        seeds.push_back(host + ":" + std::to_string(port));
    }
    return seeds;
}

} // namespace geocoin::network
