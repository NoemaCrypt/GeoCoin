#include "rocksdb_store.hpp"
#include "block_storage_keys.hpp"
#include "storage_internal.hpp"
#include "geocoin/storage/backend_support.hpp"
#include "geocoin/storage/schema.hpp"
#include "geocoin/storage/read_status.hpp"
#include <rocksdb/iterator.h>
#include <rocksdb/write_batch.h>
#include <algorithm>
#include <filesystem>
#include <set>
#include <thread>

namespace geocoin::storage {
using namespace internal::block_keys;
using internal::mb_to_bytes;

RocksDBIterator::RocksDBIterator(
    rocksdb::DB* db,
    const std::vector<rocksdb::ColumnFamilyHandle*>& column_families
)
{
    if (!db) {
        return;
    }
    its_.reserve(column_families.size());
    for (auto* cf : column_families) {
        if (!cf) {
            continue;
        }
        its_.emplace_back(db->NewIterator(rocksdb::ReadOptions(), cf));
    }
}

RocksDBIterator::~RocksDBIterator() = default;

bool RocksDBIterator::valid() const {
    for (const auto& it : its_) {
        if (!it) throw ReadError(ReadStatus::IO_ERROR, "Cannot create block storage iterator");
        const auto status = it->status();
        if (!status.ok()) throw ReadError(status.IsCorruption() ? ReadStatus::CORRUPT : ReadStatus::IO_ERROR,
                                           status.ToString());
    }
    return current_valid_;
}

void RocksDBIterator::seek(const std::string& key) {
    for (auto& it : its_) {
        if (it) {
            it->Seek(key);
        }
    }
    select_min();
}

void RocksDBIterator::next() {
    if (!current_valid_) {
        return;
    }
    for (auto& it : its_) {
        if (it && it->Valid() && it->key().compare(current_key_) == 0) {
            it->Next();
        }
    }
    select_min();
}

std::string RocksDBIterator::key() const {
    return current_valid_ ? current_key_ : std::string{};
}

void RocksDBIterator::select_min() {
    current_valid_ = false;
    current_key_.clear();
    for (auto& it : its_) {
        if (!it || !it->Valid()) {
            continue;
        }
        const auto key = it->key();
        if (!current_valid_ || key.compare(current_key_) < 0) {
            current_valid_ = true;
            current_key_.assign(key.data(), key.size());
        }
    }
}

RocksDBStore::RocksDBStore(BlockStoreRole role)
    : role_(role)
{
}

RocksDBStore::~RocksDBStore() {
    close();
}

bool RocksDBStore::open(const std::string& path, const StorageOptions& options) {
    rocksdb::Options db_options;
    db_options.create_if_missing = true;
    db_options.error_if_exists = false;
    db_options.create_missing_column_families = true;

    const unsigned int hw_threads = std::max(1u, std::thread::hardware_concurrency());
    db_options.IncreaseParallelism(static_cast<int>(hw_threads));
    db_options.OptimizeLevelStyleCompaction();
    db_options.write_buffer_size = mb_to_bytes(
        options.write_buffer_mb > 0
            ? options.write_buffer_mb
            : (options.profile == StorageProfile::SAFE ? 64 : 256)
    );
    db_options.max_write_buffer_number =
        (options.profile == StorageProfile::SAFE) ? 3 : 6;
    db_options.min_write_buffer_number_to_merge =
        (options.profile == StorageProfile::SAFE) ? 1 : 2;
    db_options.max_background_jobs = options.max_background_jobs > 0
        ? options.max_background_jobs
        : std::max(4, static_cast<int>(hw_threads / 2));
    db_options.target_file_size_base = mb_to_bytes(64);
    db_options.max_bytes_for_level_base = mb_to_bytes(
        options.profile == StorageProfile::SAFE ? 256 : 1024
    );
    db_options.bytes_per_sync = mb_to_bytes(1);
    db_options.wal_bytes_per_sync = mb_to_bytes(1);
    db_options.compaction_readahead_size = mb_to_bytes(2);
    db_options.compression = (options.profile == StorageProfile::UNSAFE_BENCHMARK)
        ? rocksdb::kNoCompression
        : rocksdb::kSnappyCompression;
    backend::apply_shared_resources(db_options, options);

    rocksdb::ColumnFamilyOptions cf_options(db_options);
    std::vector<std::string> cf_names;
    rocksdb::Status status = rocksdb::DB::ListColumnFamilies(db_options, path, &cf_names);
    const auto required_cfs = required_column_families();
    std::set<std::string> expected(required_cfs.begin(), required_cfs.end());
    expected.insert(rocksdb::kDefaultColumnFamilyName);
    if (status.ok()) {
        if (std::set<std::string>(cf_names.begin(), cf_names.end()) != expected) {
            GEO_LOG_ERROR("storage", "Unsupported column family layout; use a fresh data directory");
            return false;
        }
    } else {
        std::error_code error;
        if (std::filesystem::exists(std::filesystem::path(path) / "CURRENT", error) || error) {
            GEO_LOG_ERROR("storage", "Cannot read existing database layout: " << status.ToString());
            return false;
        }
        cf_names.assign(expected.begin(), expected.end());
    }

    std::vector<rocksdb::ColumnFamilyDescriptor> descriptors;
    descriptors.reserve(cf_names.size());
    for (const auto& name : cf_names) {
        descriptors.emplace_back(name, cf_options);
    }

    std::vector<rocksdb::ColumnFamilyHandle*> raw_handles;
    status = internal::open_rocksdb_with_column_families(
        db_options,
        path,
        descriptors,
        &raw_handles,
        db_
    );
    if (!status.ok()) {
        GEO_LOG_ERROR("storage", "Failed to open RocksDB block storage: " << status.ToString());
        return false;
    }

    for (size_t i = 0; i < raw_handles.size(); ++i) {
        cf_handles_[cf_names[i]] = raw_handles[i];
    }

    if (!backend::ensure_format(db_.get(), options, raw_handles)) {
        close();
        return false;
    }

    GEO_LOG_INFO(
        "storage",
        "Opened RocksDB " << role_name() << " block storage with "
            << cf_handles_.size() << " column families"
    );
    return true;
}

void RocksDBStore::close() {
    std::lock_guard<std::mutex> lock(write_mutex_);
    if (db_) {
        for (auto& [name, handle] : cf_handles_) {
            (void)name;
            if (handle) {
                db_->DestroyColumnFamilyHandle(handle);
            }
        }
    }
    cf_handles_.clear();
    db_.reset();
    has_unlogged_writes_ = false;
}

bool RocksDBStore::get(const std::string& key, std::string& value) const {
    if (!db_) throw ReadError(ReadStatus::IO_ERROR, "Block storage is closed");
    const auto status = db_->Get(rocksdb::ReadOptions(), handle_for_key(key), key, &value);
    if (status.IsNotFound()) return false;
    if (!status.ok()) throw ReadError(status.IsCorruption() ? ReadStatus::CORRUPT : ReadStatus::IO_ERROR,
                                       "Read " + key + ": " + status.ToString());
    return true;
}

bool RocksDBStore::put(const std::string& key, const std::string& value, bool sync, bool disable_wal) {
    std::lock_guard<std::mutex> lock(write_mutex_);
    if (!db_) return false;
    rocksdb::WriteOptions options;
    options.sync = sync;
    options.disableWAL = disable_wal && !sync;
    rocksdb::ColumnFamilyHandle* handle = handle_for_key(key);
    const auto status = db_->Put(options, handle, key, rocksdb::Slice(value.data(), value.size()));
    return finish_write(status, options.disableWAL, sync);
}

bool RocksDBStore::delete_key(const std::string& key, bool sync, bool disable_wal) {
    std::lock_guard<std::mutex> lock(write_mutex_);
    if (!db_) return false;
    rocksdb::WriteOptions options;
    options.sync = sync;
    options.disableWAL = disable_wal && !sync;
    rocksdb::ColumnFamilyHandle* handle = handle_for_key(key);
    const auto status = db_->Delete(options, handle, key);
    return finish_write(status, options.disableWAL, sync);
}

bool RocksDBStore::write_batch(const std::vector<KVWrite>& writes, bool sync, bool disable_wal) {
    std::lock_guard<std::mutex> lock(write_mutex_);
    if (!db_) return false;
    rocksdb::WriteBatch batch;
    for (const auto& write : writes) {
        rocksdb::ColumnFamilyHandle* handle = handle_for_key(write.key);
        if (write.type == KVWrite::Type::PUT) {
            batch.Put(handle, write.key, rocksdb::Slice(write.value.data(), write.value.size()));
        } else {
            batch.Delete(handle, write.key);
        }
    }
    rocksdb::WriteOptions options;
    options.sync = sync;
    options.disableWAL = disable_wal && !sync;
    rocksdb::Status status = db_->Write(options, &batch);
    return finish_write(status, options.disableWAL, sync);
}

bool RocksDBStore::sync() {
    std::lock_guard<std::mutex> lock(write_mutex_);
    if (!db_) {
        return false;
    }

    rocksdb::Status status = db_->SyncWAL();
    if (!status.ok()) {
        return false;
    }
    if (has_unlogged_writes_) {
        status = flush_all_column_families();
        if (!status.ok()) {
            return false;
        }
        has_unlogged_writes_ = false;
    }
    return true;
}

std::unique_ptr<RocksDBIterator> RocksDBStore::new_iterator() const {
    if (!db_) return nullptr;
    return std::make_unique<RocksDBIterator>(db_.get(), all_column_family_handles());
}

bool RocksDBStore::finish_write(
    const rocksdb::Status& write_status,
    bool wrote_without_wal,
    bool sync_write
) {
    if (!write_status.ok()) {
        return false;
    }
    has_unlogged_writes_ = has_unlogged_writes_ || wrote_without_wal;
    if (sync_write && has_unlogged_writes_) {
        const rocksdb::Status flush_status = flush_all_column_families();
        if (!flush_status.ok()) {
            return false;
        }
        has_unlogged_writes_ = false;
    }
    return true;
}

std::vector<std::string> RocksDBStore::required_column_families() const {
    auto names = schema::columns(role_ == BlockStoreRole::CHAIN_DATA ? schema::Store::BLOCKS : schema::Store::INDEX);
    names.erase("default");
    return {names.begin(), names.end()};
}

const char* RocksDBStore::column_family_for_key(const std::string& key) {
    if (const auto* record = schema::find(key)) return record->column;
    return rocksdb::kDefaultColumnFamilyName.c_str();
}

rocksdb::ColumnFamilyHandle* RocksDBStore::default_cf() const {
    auto it = cf_handles_.find(rocksdb::kDefaultColumnFamilyName);
    return (it != cf_handles_.end()) ? it->second : db_->DefaultColumnFamily();
}

rocksdb::ColumnFamilyHandle* RocksDBStore::handle_for_key(const std::string& key) const {
    const char* cf_name = column_family_for_key(key);
    auto it = cf_handles_.find(cf_name);
    if (it != cf_handles_.end()) {
        return it->second;
    }
    return default_cf();
}

const char* RocksDBStore::role_name() const {
    switch (role_) {
        case BlockStoreRole::CHAIN_DATA:
            return "chaindata";
        case BlockStoreRole::INDEX:
            return "index";
        default:
            return "unknown";
    }
}

std::vector<rocksdb::ColumnFamilyHandle*> RocksDBStore::all_column_family_handles() const {
    std::vector<rocksdb::ColumnFamilyHandle*> handles;
    handles.reserve(cf_handles_.size());
    for (const auto& [name, handle] : cf_handles_) {
        (void)name;
        if (handle) {
            handles.push_back(handle);
        }
    }
    return handles;
}

rocksdb::Status RocksDBStore::flush_all_column_families() {
    rocksdb::FlushOptions flush_options;
    flush_options.wait = true;
    rocksdb::Status status;
    for (auto* handle : all_column_family_handles()) {
        status = db_->Flush(flush_options, handle);
        if (!status.ok()) {
            return status;
        }
    }
    return status;
}

} // namespace geocoin::storage
