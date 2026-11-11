#include "geocoin/storage/backend_support.hpp"
#include "storage_internal.hpp"

#include <utility>
#include <rocksdb/write_buffer_manager.h>

namespace geocoin::storage::backend {

size_t mb_to_bytes(size_t mb) { return internal::mb_to_bytes(mb); }

std::string normalize_storage_value(std::string value) {
    return internal::normalize_storage_value(std::move(value));
}

void apply_block_cache_options(rocksdb::Options& options, size_t cache_mb) {
    internal::apply_block_cache_options(options, cache_mb);
}

void configure_shared_resources(StorageOptions& options) {
    if (options.cache_mb == 0) options.cache_mb = options.profile == StorageProfile::SAFE ? 128 : 256;
    if (options.write_buffer_mb == 0) options.write_buffer_mb = options.profile == StorageProfile::SAFE ? 64 : 256;
    options.shared_cache = rocksdb::NewLRUCache(mb_to_bytes(options.cache_mb));
    options.shared_write_buffer = std::make_shared<rocksdb::WriteBufferManager>(
        mb_to_bytes(options.write_buffer_mb), nullptr, true);
}

void apply_shared_resources(rocksdb::Options& options, const StorageOptions& config) {
    if (config.shared_cache) {
        rocksdb::BlockBasedTableOptions table;
        table.block_cache = config.shared_cache;
        table.cache_index_and_filter_blocks = true;
        options.table_factory.reset(rocksdb::NewBlockBasedTableFactory(table));
    } else {
        apply_block_cache_options(options, config.cache_mb);
    }
    if (config.shared_write_buffer) options.write_buffer_manager = config.shared_write_buffer;
}

std::string format_identity(const StorageOptions& options) {
    return std::string(kFormatPrefix) + options.network + "\n" + options.genesis_hex + "\n";
}

bool ensure_format(rocksdb::DB* db, const StorageOptions& options,
                   const std::vector<rocksdb::ColumnFamilyHandle*>& families) {
    const auto expected = format_identity(options);
    std::string actual;
    auto status = db->Get(rocksdb::ReadOptions(), kFormatKey, &actual);
    if (status.ok()) {
        if (actual == expected) return true;
        GEO_LOG_ERROR("storage", "Storage format, network or genesis mismatch");
        return false;
    }
    if (!status.IsNotFound()) return false;
    auto handles = families;
    if (handles.empty()) handles.push_back(db->DefaultColumnFamily());
    for (auto* cf : handles) {
        std::unique_ptr<rocksdb::Iterator> it(db->NewIterator(rocksdb::ReadOptions(), cf));
        it->SeekToFirst();
        if (it->Valid() || !it->status().ok()) {
            GEO_LOG_ERROR("storage", "Existing database has no supported format identifier; use a fresh data directory");
            return false;
        }
    }
    rocksdb::WriteOptions write;
    write.sync = true;
    return db->Put(write, kFormatKey, expected).ok();
}

bool ensure_backend_marker(const std::string& path, const char* backend, const char* label) {
    return internal::ensure_backend_marker(path, backend, label);
}

rocksdb::Status open_rocksdb(const rocksdb::Options& options, const std::string& path,
                           std::unique_ptr<rocksdb::DB>& db) {
    return internal::open_rocksdb(options, path, db);
}

} // namespace geocoin::storage::backend
