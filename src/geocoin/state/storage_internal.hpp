#pragma once

#include <string>

namespace geocoin::state {

// Written into both split RocksDB instances before a dual Write. Direct Get on
// each DB detects a crash between the two commits.
inline constexpr const char* kSplitCommitPendingKey = "split_commit_pending";

inline bool is_utxo_metadata_key(const std::string& key) {
    return key.starts_with("utxo:");
}

} // namespace geocoin::state
