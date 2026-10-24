#include "geocoin/node/node_config_loader.hpp"

#include "geocoin/utils/config.hpp"
#include "geocoin/utils/logging.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <vector>

using geocoin::utils::LogPrintf;

namespace geocoin::node {
namespace {

using ConfigMap = std::map<std::string, std::vector<std::string>>;

const std::vector<std::string>* find_config_values(const ConfigMap& config_map, const std::string& key);
std::string trim_config_value(const std::string& value);

const std::set<std::string> kSupportedConfigKeys = {
    "config-version",
    "db-backend",
    "db-profile",
    "db-write-buffer-mb",
    "db-cache-mb",
    "db-max-background-jobs",
    "db-unsafe-disable-wal-during-ibd",
    "node-type",
    "pruned-blocks",
    "pruned-delete-old-blocks",
    "pruned-keep-old-blocks",
    "pruned-delete-old-indexes",
    "pruned-keep-old-indexes",
    "addnode",
    "archive",
    "no-auto-connect",
    "activation-manifest",
    "policy-checkpoint-interval",
    "policy-checkpoint-confirmations",
    "allow-deep-reorg",
    "rpc-rate-limit",
    "rpc-max-connections",
    "rpc-max-connections-per-ip",
};

// Published v1 examples historically contained these runtime-only keys even
// though the node never applied them from geocoin.conf. Keep one transition
// path so upgrades start safely, but make the no-op explicit in the log.
const std::set<std::string> kLegacyIgnoredConfigKeys = {
    "network",
    "data-dir",
    "rpc",
    "rpcbind",
    "rpcport",
    "rpcuser",
    "rpcpassword",
    "listen",
    "port",
};

void validate_config_schema(const ConfigMap& config_map) {
    for (const auto& [key, values] : config_map) {
        const bool legacy_ignored = kLegacyIgnoredConfigKeys.count(key) != 0;
        if (kSupportedConfigKeys.count(key) == 0 && !legacy_ignored) {
            throw std::runtime_error(
                "Unsupported geocoin.conf option: " + key +
                ". Runtime-only options must be passed as documented CLI flags."
            );
        }
        if (legacy_ignored) {
            LogPrintf(
                "WARNING: legacy geocoin.conf option '%s' is ignored; use the documented CLI/service flag\n",
                key.c_str()
            );
        }
        if (key != "addnode" && values.size() > 1) {
            throw std::runtime_error("Duplicate geocoin.conf option: " + key);
        }
    }

    if (const auto* versions = find_config_values(config_map, "config-version")) {
        if (versions->size() != 1 || trim_config_value(versions->front()) != "1") {
            throw std::runtime_error("Unsupported geocoin.conf config-version (expected 1)");
        }
    }
}

const std::vector<std::string>* find_config_values(const ConfigMap& config_map, const std::string& key) {
    const auto it = config_map.find(key);
    return it == config_map.end() ? nullptr : &it->second;
}

std::string normalize_config_value(std::string value) {
    value.erase(std::remove_if(value.begin(), value.end(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    }), value.end());
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool parse_config_bool(const std::string& value, bool& out) {
    const std::string normalized = normalize_config_value(value);
    if (normalized == "1" || normalized == "true" || normalized == "yes" || normalized == "on") {
        out = true;
        return true;
    }
    if (normalized == "0" || normalized == "false" || normalized == "no" || normalized == "off") {
        out = false;
        return true;
    }
    return false;
}

std::string trim_config_value(const std::string& value) {
    size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
        ++begin;
    }

    size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }

    return value.substr(begin, end - begin);
}

bool parse_config_size(const std::string& value, size_t& out) {
    const std::string normalized = trim_config_value(value);
    if (normalized.empty()) {
        return false;
    }

    uint64_t parsed = 0;
    for (unsigned char c : normalized) {
        if (!std::isdigit(c)) {
            return false;
        }
        const uint64_t digit = static_cast<uint64_t>(c - '0');
        if (parsed > (std::numeric_limits<uint64_t>::max() - digit) / 10) {
            return false;
        }
        parsed = parsed * 10 + digit;
    }
    if (parsed > static_cast<uint64_t>((std::numeric_limits<size_t>::max)())) {
        return false;
    }
    out = static_cast<size_t>(parsed);
    return true;
}

bool parse_config_int(const std::string& value, int& out) {
    const std::string normalized = trim_config_value(value);
    if (normalized.empty()) {
        return false;
    }

    size_t pos = 0;
    bool negative = false;
    if (normalized[pos] == '+' || normalized[pos] == '-') {
        negative = normalized[pos] == '-';
        ++pos;
    }
    if (pos >= normalized.size()) {
        return false;
    }

    uint64_t magnitude = 0;
    for (; pos < normalized.size(); ++pos) {
        const unsigned char c = static_cast<unsigned char>(normalized[pos]);
        if (!std::isdigit(c)) {
            return false;
        }
        const uint64_t digit = static_cast<uint64_t>(c - '0');
        if (magnitude > (std::numeric_limits<uint64_t>::max() - digit) / 10) {
            return false;
        }
        magnitude = magnitude * 10 + digit;
    }

    const uint64_t max_positive = static_cast<uint64_t>((std::numeric_limits<int>::max)());
    const uint64_t max_negative = max_positive + 1ULL;
    if ((!negative && magnitude > max_positive) || (negative && magnitude > max_negative)) {
        return false;
    }
    out = negative
        ? static_cast<int>(-static_cast<int64_t>(magnitude))
        : static_cast<int>(magnitude);
    return true;
}

bool parse_config_height(const std::string& value, Height& out) {
    size_t parsed = 0;
    if (!parse_config_size(value, parsed)) {
        return false;
    }
    if (parsed > static_cast<size_t>((std::numeric_limits<Height>::max)())) {
        return false;
    }
    out = static_cast<Height>(parsed);
    return true;
}

bool parse_config_u32(const std::string& value, uint32_t& out) {
    size_t parsed = 0;
    if (!parse_config_size(value, parsed)) {
        return false;
    }
    if (parsed > static_cast<size_t>((std::numeric_limits<uint32_t>::max)())) {
        return false;
    }
    out = static_cast<uint32_t>(parsed);
    return true;
}

std::string normalize_config_token(const std::string& value) {
    std::string normalized;
    normalized.reserve(value.size());
    for (unsigned char c : value) {
        if (std::isspace(c) == 0) {
            normalized.push_back(static_cast<char>(std::tolower(c)));
        }
    }
    return normalized;
}

bool parse_node_type_value(const std::string& value, NodeType& out) {
    const std::string normalized = normalize_config_token(value);
    if (normalized == "full") {
        out = NodeType::FULL;
        return true;
    }
    if (normalized == "pruned") {
        out = NodeType::PRUNED;
        return true;
    }
    return false;
}

template <typename T, typename Parser>
bool apply_config_value(
    const ConfigMap& config_map,
    const std::string& key,
    T& out_value,
    Parser parse_value
) {
    const auto* values = find_config_values(config_map, key);
    if (values == nullptr) {
        return false;
    }

    for (const auto& value : *values) {
        T parsed{};
        if (parse_value(value, parsed)) {
            out_value = parsed;
        } else {
            throw std::runtime_error("Invalid value for " + key + ": " + value);
        }
    }
    return true;
}

bool apply_first_non_empty_config_value(
    const ConfigMap& config_map,
    const std::string& key,
    std::string& out_value
) {
    const auto* values = find_config_values(config_map, key);
    if (values == nullptr) {
        return false;
    }

    const std::string& value = values->front();
    if (trim_config_value(value).empty()) {
        throw std::runtime_error("Empty value for " + key);
    }
    out_value = value;
    return true;
}

void apply_bool_config_value(
    const ConfigMap& config_map,
    const std::string& key,
    bool& out_value
) {
    (void)apply_config_value(config_map, key, out_value, parse_config_bool);
}

void apply_height_config_value(
    const ConfigMap& config_map,
    const std::string& key,
    Height& out_value
) {
    (void)apply_config_value(config_map, key, out_value, parse_config_height);
}

void validate_storage_config_values(const ConfigMap& config_map)
{
    if (const auto* values = find_config_values(config_map, "db-backend")) {
        const std::string token = normalize_config_token(values->front());
        if (token != "rocksdb" && token != "rocks") {
            throw std::runtime_error(
                "Invalid db-backend=" + values->front() + "; GeoCoin supports only rocksdb"
            );
        }
    }
    if (const auto* values = find_config_values(config_map, "db-profile")) {
        const std::string token = normalize_config_token(values->front());
        if (token != "safe" && token != "fast-sync" && token != "fastsync" && token != "fast" &&
            token != "unsafe-benchmark" && token != "unsafe" && token != "benchmark") {
            throw std::runtime_error("Invalid value for db-profile: " + values->front());
        }
    }

    size_t size_value = 0;
    int int_value = 0;
    bool bool_value = false;
    Height height_value = 0;
    (void)apply_config_value(config_map, "db-write-buffer-mb", size_value, parse_config_size);
    (void)apply_config_value(config_map, "db-cache-mb", size_value, parse_config_size);
    if (apply_config_value(
            config_map,
            "db-max-background-jobs",
            int_value,
            parse_config_int
        ) && int_value < 0) {
        throw std::runtime_error("db-max-background-jobs must be non-negative");
    }
    (void)apply_config_value(
        config_map,
        "db-unsafe-disable-wal-during-ibd",
        bool_value,
        parse_config_bool
    );
    if (const auto* values = find_config_values(config_map, "node-type")) {
        NodeType node_type = NodeType::FULL;
        if (!parse_node_type_value(values->front(), node_type)) {
            throw std::runtime_error("Invalid node-type in config: " + values->front());
        }
    }
    (void)apply_config_value(config_map, "pruned-blocks", height_value, parse_config_height);
    (void)apply_config_value(
        config_map,
        "pruned-delete-old-blocks",
        bool_value,
        parse_config_bool
    );
    (void)apply_config_value(
        config_map,
        "pruned-keep-old-blocks",
        bool_value,
        parse_config_bool
    );
    (void)apply_config_value(
        config_map,
        "pruned-delete-old-indexes",
        bool_value,
        parse_config_bool
    );
    (void)apply_config_value(
        config_map,
        "pruned-keep-old-indexes",
        bool_value,
        parse_config_bool
    );
}

void validate_runtime_config_values(const ConfigMap& config_map)
{
    bool bool_value = false;
    Height height_value = 0;
    (void)apply_config_value(config_map, "archive", bool_value, parse_config_bool);
    (void)apply_config_value(config_map, "no-auto-connect", bool_value, parse_config_bool);
    (void)apply_config_value(config_map, "allow-deep-reorg", bool_value, parse_config_bool);
    (void)apply_config_value(
        config_map,
        "policy-checkpoint-interval",
        height_value,
        parse_config_height
    );
    (void)apply_config_value(
        config_map,
        "policy-checkpoint-confirmations",
        height_value,
        parse_config_height
    );
    uint32_t u32_value = 0;
    size_t size_value = 0;
    (void)apply_config_value(config_map, "rpc-rate-limit", u32_value, parse_config_u32);
    (void)apply_config_value(config_map, "rpc-max-connections", size_value, parse_config_size);
    (void)apply_config_value(
        config_map,
        "rpc-max-connections-per-ip",
        size_value,
        parse_config_size
    );
    if (const auto* values = find_config_values(config_map, "activation-manifest")) {
        if (trim_config_value(values->front()).empty()) {
            throw std::runtime_error("Empty value for activation-manifest");
        }
    }
    if (const auto* values = find_config_values(config_map, "addnode")) {
        for (const auto& value : *values) {
            if (trim_config_value(value).empty()) {
                throw std::runtime_error("Empty value for addnode");
            }
        }
    }
}

}  // namespace

void apply_storage_config_from_file(NodeConfig& config, const std::string& config_file) {
    const auto config_map = utils::read_config_file(config_file);
    validate_config_schema(config_map);
    validate_storage_config_values(config_map);

    config.db_backend = "rocksdb";
    if (config_map.count("db-backend") > 0) {
        for (const auto& v : config_map.at("db-backend")) {
            if (!v.empty()) {
                const auto normalized = normalize_config_token(v);
                if (normalized != "rocksdb" && normalized != "rocks") {
                    throw std::runtime_error(
                        "Invalid db-backend=" + v + "; GeoCoin supports only rocksdb"
                    );
                }
                break;
            }
        }
    }

    if (!config.db_profile_explicit) {
        (void)apply_first_non_empty_config_value(config_map, "db-profile", config.db_profile);
    }

    if (!config.db_write_buffer_explicit) {
        (void)apply_config_value(
            config_map,
            "db-write-buffer-mb",
            config.db_write_buffer_mb,
            parse_config_size
        );
    }

    if (!config.db_cache_explicit) {
        (void)apply_config_value(config_map, "db-cache-mb", config.db_cache_mb, parse_config_size);
    }

    if (!config.db_max_background_jobs_explicit) {
        (void)apply_config_value(
            config_map,
            "db-max-background-jobs",
            config.db_max_background_jobs,
            parse_config_int
        );
    }

    if (!config.db_unsafe_wal_explicit) {
        (void)apply_config_value(
            config_map,
            "db-unsafe-disable-wal-during-ibd",
            config.db_unsafe_disable_wal_during_ibd,
            parse_config_bool
        );
    }

    if (!config.node_type_explicit && config_map.count("node-type") > 0) {
        for (const auto& v : config_map.at("node-type")) {
            if (v.empty()) {
                continue;
            }
            NodeType parsed = NodeType::FULL;
            if (parse_node_type_value(v, parsed)) {
                config.node_type = parsed;
                break;
            }
            throw std::runtime_error("Invalid node-type in config: " + v + " (valid: full|pruned)");
        }
    }

    if (!config.pruned_block_retention_explicit) {
        (void)apply_config_value(
            config_map,
            "pruned-blocks",
            config.pruned_block_retention,
            parse_config_height
        );
    }

    if (!config.pruned_delete_old_blocks_explicit) {
        (void)apply_config_value(
            config_map,
            "pruned-delete-old-blocks",
            config.pruned_delete_old_blocks,
            parse_config_bool
        );
        bool keep_old_blocks = false;
        if (apply_config_value(config_map, "pruned-keep-old-blocks", keep_old_blocks, parse_config_bool)) {
            config.pruned_delete_old_blocks = !keep_old_blocks;
        }
    }

    if (!config.pruned_delete_old_indexes_explicit) {
        (void)apply_config_value(
            config_map,
            "pruned-delete-old-indexes",
            config.pruned_delete_old_indexes,
            parse_config_bool
        );
        bool keep_old_indexes = false;
        if (apply_config_value(config_map, "pruned-keep-old-indexes", keep_old_indexes, parse_config_bool)) {
            config.pruned_delete_old_indexes = !keep_old_indexes;
        }
    }
}

void apply_runtime_config_from_file(NodeConfig& config, const std::string& config_file) {
    const auto config_map = utils::read_config_file(config_file);
    validate_config_schema(config_map);
    validate_runtime_config_values(config_map);

    if (const auto* values = find_config_values(config_map, "addnode")) {
        for (const auto& node : *values) {
            if (trim_config_value(node).empty()) {
                throw std::runtime_error("Empty value for addnode");
            }
            config.seed_nodes.push_back(node);
            LogPrintf("Added node from config: %s\n", node.c_str());
        }
    }

    if (!config.archive) {
        bool archive = false;
        apply_bool_config_value(config_map, "archive", archive);
        config.archive = archive;
    }

    if (config.enable_auto_connect) {
        bool no_auto_connect = false;
        apply_bool_config_value(config_map, "no-auto-connect", no_auto_connect);
        if (no_auto_connect) {
            config.enable_auto_connect = false;
        }
    }

    if (config.activation_manifest_path.empty()) {
        (void)apply_first_non_empty_config_value(
            config_map,
            "activation-manifest",
            config.activation_manifest_path
        );
    }

    apply_height_config_value(config_map, "policy-checkpoint-interval", config.policy_checkpoint_interval);
    apply_height_config_value(
        config_map,
        "policy-checkpoint-confirmations",
        config.policy_checkpoint_confirmations
    );
    apply_bool_config_value(config_map, "allow-deep-reorg", config.allow_deep_reorg);

    if (!config.rpc_rate_limit_explicit) {
        (void)apply_config_value(
            config_map,
            "rpc-rate-limit",
            config.rpc_rate_limit_max_requests_per_window,
            parse_config_u32
        );
    }
    if (!config.rpc_max_connections_explicit) {
        (void)apply_config_value(
            config_map,
            "rpc-max-connections",
            config.rpc_max_connections,
            parse_config_size
        );
    }
    if (!config.rpc_max_connections_per_ip_explicit) {
        (void)apply_config_value(
            config_map,
            "rpc-max-connections-per-ip",
            config.rpc_max_connections_per_ip,
            parse_config_size
        );
    }
}

}  // namespace geocoin::node
