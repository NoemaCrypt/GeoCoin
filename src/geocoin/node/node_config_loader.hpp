#pragma once

#include "geocoin/node/node.hpp"

#include <filesystem>
#include <string>

namespace geocoin::node {

const char* node_type_name(NodeType type);
void write_default_node_config_if_missing(const std::string& config_file);
void apply_storage_config_from_file(NodeConfig& config, const std::string& config_file);
void apply_runtime_config_from_file(NodeConfig& config, const std::string& config_file);
bool validate_node_config(NodeConfig& config, std::string& error);
std::string resolve_activation_manifest_path(const NodeConfig& config);

/**
 * Resolve the per-network data directory from the configured base directory.
 *
 * The network id is appended unless the configured path already names it as
 * its last component. Trailing separators and `.` segments are normalized
 * first, so "<dir>/degree/" resolves to "<dir>/degree" rather than nesting a
 * second "degree" directory. An empty configured path means "./data".
 */
std::filesystem::path resolve_network_data_dir(
    const std::string& configured_data_dir,
    const std::string& network_id
);

}  // namespace geocoin::node
