#pragma once

#include "geocoin/node/node.hpp"

#include <string>

namespace geocoin::node {

const char* node_type_name(NodeType type);
void write_default_node_config_if_missing(const std::string& config_file);
void apply_storage_config_from_file(NodeConfig& config, const std::string& config_file);
void apply_runtime_config_from_file(NodeConfig& config, const std::string& config_file);
bool validate_node_config(NodeConfig& config, std::string& error);
std::string resolve_activation_manifest_path(const NodeConfig& config);

}  // namespace geocoin::node
