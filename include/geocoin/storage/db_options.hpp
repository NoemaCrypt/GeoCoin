#pragma once

#include <cstddef>
#include <memory>
#include <string>

namespace rocksdb { class Cache; class WriteBufferManager; }

namespace geocoin::storage {

enum class StorageBackend {
    ROCKSDB
};

enum class StorageProfile {
    SAFE,
    FAST_SYNC,
    UNSAFE_BENCHMARK
};

struct StorageOptions {
    StorageProfile profile = StorageProfile::SAFE;
    size_t write_buffer_mb = 0;
    size_t cache_mb = 0;
    int max_background_jobs = 0;
    bool disable_wal_during_ibd = false;
    std::string network = "standalone";
    std::string genesis_hex = std::string(64, '0');
    std::shared_ptr<rocksdb::Cache> shared_cache;
    std::shared_ptr<rocksdb::WriteBufferManager> shared_write_buffer;
};

} // namespace geocoin::storage
