#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>
#include "geocoin/storage/db_options.hpp"

namespace rocksdb {
class DB;
struct Options;
class Status;
class ColumnFamilyHandle;
}

namespace geocoin::storage::backend {

// Shared storage setup API. RocksDB version handling and filesystem checks
// stay in geocoin_storage instead of leaking its private implementation headers.
size_t mb_to_bytes(size_t mb);
std::string normalize_storage_value(std::string value);
void apply_block_cache_options(rocksdb::Options& options, size_t cache_mb);
void configure_shared_resources(StorageOptions& options);
void apply_shared_resources(rocksdb::Options& options, const StorageOptions& config);
inline constexpr const char* kFormatKey = "__geocoin_format";
inline constexpr unsigned kFormatVersion = 2;
inline constexpr const char* kFormatPrefix = "geocoin-storage/2\n";
std::string format_identity(const StorageOptions& options);
bool ensure_format(rocksdb::DB* db, const StorageOptions& options,
                   const std::vector<rocksdb::ColumnFamilyHandle*>& families = {});
bool ensure_backend_marker(const std::string& path, const char* backend, const char* label);
rocksdb::Status open_rocksdb(const rocksdb::Options& options, const std::string& path,
                           std::unique_ptr<rocksdb::DB>& db);

} // namespace geocoin::storage::backend
