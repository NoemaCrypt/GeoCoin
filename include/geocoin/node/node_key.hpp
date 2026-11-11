#pragma once

#include "geocoin/types.hpp"

#include <filesystem>

namespace geocoin::node {

/**
 * Load a 32-byte node identity key, or create it atomically with owner-only
 * permissions. Existing symlinks, non-regular files and malformed keys are
 * rejected instead of being overwritten.
 */
bytes load_or_create_node_key(const std::filesystem::path& data_dir);

} // namespace geocoin::node
