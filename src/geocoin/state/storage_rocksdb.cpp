#include "geocoin/state/storage.hpp"
#include "geocoin/state/storage_internal.hpp"
#include "storage_scan.hpp"
#include "geocoin/core/block.hpp"
#include "geocoin/core/transaction.hpp"
#include "geocoin/utils/logger.hpp"
#include "geocoin/utils/bytes.hpp"
#include "geocoin/storage/backend_support.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <limits>
#include <thread>

#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <rocksdb/slice.h>
#include <rocksdb/version.h>
#include <rocksdb/write_batch.h>
#include <rocksdb/utilities/checkpoint.h>
#include <chrono>

namespace geocoin::state {
namespace {
// Keep at most two RocksDB cursors; pending writes are merged by the common
// metadata pager instead of materializing all matching keys during a batch.
class MetadataCursor {
public:
    MetadataCursor(rocksdb::DB* primary, rocksdb::DB* addresses,
                   const std::string& start, const std::string& after) {
        for (auto* db : {primary, addresses}) {
            if (!db) continue;
            auto& iterator = iterators_.emplace_back(db->NewIterator(rocksdb::ReadOptions()));
            if (!iterator) throw storage::ReadError(storage::ReadStatus::IO_ERROR, "Cannot create storage iterator");
            iterator->Seek(start);
            if (!after.empty() && iterator->Valid() && iterator->key().compare(after) == 0) iterator->Next();
        }
        select_next();
    }
    bool valid() const { return valid_; }
    std::string_view key() const { return key_; }
    void next() {
        for (auto& iterator : iterators_) {
            if (iterator->Valid() && iterator->key().compare(key_) == 0) iterator->Next();
        }
        select_next();
    }

private:
    void select_next() {
        valid_ = false;
        for (const auto& iterator : iterators_) {
            const auto status = iterator->status();
            if (!status.ok()) throw storage::ReadError(status.IsCorruption() ? storage::ReadStatus::CORRUPT
                                                                            : storage::ReadStatus::IO_ERROR, status.ToString());
            if (iterator->Valid() && (!valid_ || iterator->key().compare(key_) < 0)) {
                key_.assign(iterator->key().data(), iterator->key().size());
                valid_ = true;
            }
        }
    }
    std::vector<std::unique_ptr<rocksdb::Iterator>> iterators_;
    std::string key_;
    bool valid_ = false;
};
} // namespace


RocksDBStorage::RocksDBStorage(const std::string& db_path, StorageOptions options)
    : db_path_(db_path)
    , options_(options)
    , db_(nullptr)
    , address_db_(nullptr)
    , batch_(nullptr)
    , address_batch_(nullptr)
{
}

RocksDBStorage::RocksDBStorage(
    const std::string& db_path,
    const std::string& address_db_path,
    StorageOptions options
)
    : db_path_(db_path)
    , address_db_path_(address_db_path)
    , options_(options)
    , db_(nullptr)
    , address_db_(nullptr)
    , batch_(nullptr)
    , address_batch_(nullptr)
{
}

RocksDBStorage::~RocksDBStorage() {
    close();
}

bool RocksDBStorage::open() {
    if (db_) {
        return true;
    }

    if (!storage::backend::ensure_backend_marker(db_path_, "rocksdb", "state storage")) {
        return false;
    }

    rocksdb::Options options;
    options.create_if_missing = true;
    options.error_if_exists = false;

    const unsigned int hw_threads = std::max(1u, std::thread::hardware_concurrency());
    options.IncreaseParallelism(static_cast<int>(hw_threads));
    options.OptimizeLevelStyleCompaction();

    const size_t default_write_buffer_mb =
        (options_.profile == StorageProfile::SAFE) ? 64 : 256;
    options.write_buffer_size = storage::backend::mb_to_bytes(
        options_.write_buffer_mb > 0 ? options_.write_buffer_mb : default_write_buffer_mb
    );
    options.max_write_buffer_number =
        (options_.profile == StorageProfile::SAFE) ? 3 : 6;
    options.min_write_buffer_number_to_merge =
        (options_.profile == StorageProfile::SAFE) ? 1 : 2;
    options.max_background_jobs = options_.max_background_jobs > 0
        ? options_.max_background_jobs
        : std::max(4, static_cast<int>(hw_threads / 2));
    options.target_file_size_base = storage::backend::mb_to_bytes(64);
    options.max_bytes_for_level_base = storage::backend::mb_to_bytes(
        options_.profile == StorageProfile::SAFE ? 256 : 1024
    );
    options.bytes_per_sync = storage::backend::mb_to_bytes(1);
    options.wal_bytes_per_sync = storage::backend::mb_to_bytes(1);
    options.compaction_readahead_size = storage::backend::mb_to_bytes(2);
    options.compression = (options_.profile == StorageProfile::UNSAFE_BENCHMARK)
        ? rocksdb::kNoCompression
        : rocksdb::kSnappyCompression;
    storage::backend::apply_shared_resources(options, options_);

    std::unique_ptr<rocksdb::DB> db;
    rocksdb::Status status = storage::backend::open_rocksdb(options, db_path_, db);
    if (!status.ok()) {
        GEO_LOG_ERROR("storage", "Failed to open RocksDB: " << status.ToString());
        return false;
    }

    db_ = std::move(db);
    if (!address_db_path_.empty()) {
        if (!storage::backend::ensure_backend_marker(address_db_path_, "rocksdb", "address storage")) {
            db_.reset();
            return false;
        }

        std::unique_ptr<rocksdb::DB> address_db;
        status = storage::backend::open_rocksdb(options, address_db_path_, address_db);
        if (!status.ok()) {
            GEO_LOG_ERROR("storage", "Failed to open RocksDB address storage: " << status.ToString());
            db_.reset();
            return false;
        }
        address_db_ = std::move(address_db);
    }

    if (!storage::backend::ensure_format(db_.get(), options_) ||
        (address_db_ && !storage::backend::ensure_format(address_db_.get(), options_))) {
        close();
        return false;
    }

    GEO_LOG_INFO(
        "storage",
        "Opened RocksDB state storage chaindata=" << db_path_
            << (address_db_path_.empty() ? "" : " addresses=" + address_db_path_)
    );
    return true;
}

void RocksDBStorage::close() {
    if (batch_) {
        abort_batch();
    }
    address_batch_.reset();
    address_db_.reset();
    db_.reset();
}

bool RocksDBStorage::create_recovery_checkpoint() {
    if (!db_ || in_batch_) return false;
    const auto stamp = std::chrono::system_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::path(db_path_ + ".recovery") / std::to_string(stamp);
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    if (ec) return false;
    const auto copy = [&](rocksdb::DB* db, const char* name) {
        rocksdb::Checkpoint* raw = nullptr;
        auto status = rocksdb::Checkpoint::Create(db, &raw);
        std::unique_ptr<rocksdb::Checkpoint> checkpoint(raw);
        if (status.ok()) status = checkpoint->CreateCheckpoint((root / name).string());
        if (!status.ok()) GEO_LOG_ERROR("storage", "Recovery checkpoint: " << status.ToString());
        return status.ok();
    };
    if (!copy(db_.get(), "chain") || (address_db_ && !copy(address_db_.get(), "addresses"))) return false;
    recovery_checkpoint_path_ = root.string();
    GEO_LOG_INFO("storage", "Preserved recovery checkpoint at " << recovery_checkpoint_path_);
    return true;
}

void RocksDBStorage::discard_recovery_checkpoint() {
    if (recovery_checkpoint_path_.empty()) return;
    std::error_code ec;
    std::filesystem::remove_all(recovery_checkpoint_path_, ec);
    if (ec) GEO_LOG_WARN("storage", "Could not remove completed recovery checkpoint: " << ec.message());
    recovery_checkpoint_path_.clear();
}

std::string RocksDBStorage::tx_key(const Hash256& hash) const {
    return "tx:" + utils::hash256_to_hex(hash);
}

std::string RocksDBStorage::meta_key(const std::string& key) const {
    return "meta:" + key;
}

bool RocksDBStorage::is_address_metadata_key(const std::string& key) const {
    return is_utxo_metadata_key(key) ||
           key.rfind("addr:", 0) == 0 ||
           key.rfind("address:", 0) == 0;
}

rocksdb::DB* RocksDBStorage::db_for_metadata_key(const std::string& key) const {
    if (address_db_ && is_address_metadata_key(key)) {
        return address_db_.get();
    }
    return db_.get();
}

std::vector<std::string> RocksDBStorage::get_all_keys() const {
    return get_keys_with_prefix("");
}

std::vector<std::string> RocksDBStorage::get_keys_page(const std::string& prefix, size_t offset,
                                                    size_t limit, size_t* total, const std::string& after) const {
    if (!db_) throw storage::ReadError(storage::ReadStatus::IO_ERROR, "Storage is closed");
    if (limit == 0 && !total) return {};
    const std::string raw_prefix = meta_key(prefix);
    const std::string raw_after = after.empty() ? std::string{} : meta_key(after);
    MetadataCursor cursor(db_.get(), address_db_.get(), std::max(raw_prefix, raw_after), raw_after);
    return internal::scan_metadata_page(cursor, batch_values_, raw_prefix, offset, limit, total, raw_after, 5);
}

std::vector<std::string> RocksDBStorage::get_keys_with_prefix(const std::string& prefix) const {
    return get_keys_page(prefix, 0, (std::numeric_limits<size_t>::max)());
}

bool RocksDBStorage::put_transaction(const Hash256& hash, const core::Transaction& tx) {
    if (!db_ || tx.get_hash() != hash) return false;

    bytes data = tx.serialize();
    const std::string key = tx_key(hash);

    if (in_batch_) {
        batch_->Put(key, rocksdb::Slice(reinterpret_cast<const char*>(data.data()), data.size()));
        batch_values_[key] = data;
        return true;
    }

    rocksdb::WriteOptions options;
    options.sync = true;
    rocksdb::Status status = db_->Put(
        options,
        key,
        rocksdb::Slice(reinterpret_cast<const char*>(data.data()), data.size())
    );
    return status.ok();
}

bool RocksDBStorage::get_transaction(const Hash256& hash, core::Transaction& tx) const {
    bytes data;
    if (!read_value(db_.get(), tx_key(hash), data)) return false;
    try {
        auto decoded = core::Transaction::deserialize_canonical(data);
        if (decoded.get_hash() != hash) {
            throw std::runtime_error("Stored transaction hash does not match its key");
        }
        tx = std::move(decoded);
    } catch (const std::exception& error) {
        throw storage::ReadError(storage::ReadStatus::CORRUPT, error.what());
    }
    return true;
}

bool RocksDBStorage::has_transaction(const Hash256& hash) const {
    bytes data;
    return read_value(db_.get(), tx_key(hash), data);
}

bool RocksDBStorage::delete_transaction(const Hash256& hash) {
    if (!db_) return false;

    const std::string key = tx_key(hash);
    if (in_batch_) {
        batch_->Delete(key);
        batch_values_[key] = std::nullopt;
        return true;
    }

    rocksdb::WriteOptions options;
    options.sync = true;
    rocksdb::Status status = db_->Delete(options, key);
    return status.ok();
}

bool RocksDBStorage::put_metadata(const std::string& key, const bytes& value) {
    if (!db_) return false;
    const std::string db_key = meta_key(key);
    if (in_batch_) {
        auto* target = address_db_ && is_address_metadata_key(key) ? address_batch_.get() : batch_.get();
        target->Put(db_key, rocksdb::Slice(reinterpret_cast<const char*>(value.data()), value.size()));
        batch_values_[db_key] = value;
        return true;
    }
    rocksdb::WriteOptions options;
    options.sync = true;
    return db_for_metadata_key(key)->Put(options, db_key,
        rocksdb::Slice(reinterpret_cast<const char*>(value.data()), value.size())).ok();
}

bool RocksDBStorage::get_metadata(const std::string& key, bytes& value) const {
    return read_value(db_for_metadata_key(key), meta_key(key), value);
}

bool RocksDBStorage::has_metadata(const std::string& key) const {
    bytes value;
    return get_metadata(key, value);
}

bool RocksDBStorage::delete_metadata(const std::string& key) {
    if (!db_) return false;
    const std::string db_key = meta_key(key);
    if (in_batch_) {
        auto* target = address_db_ && is_address_metadata_key(key) ? address_batch_.get() : batch_.get();
        target->Delete(db_key);
        batch_values_[db_key] = std::nullopt;
        return true;
    }
    rocksdb::WriteOptions options;
    options.sync = true;
    // The split-commit sentinel is the only metadata deliberately present in both DBs.
    if (key == kSplitCommitPendingKey && address_db_ && !address_db_->Delete(options, db_key).ok()) return false;
    return db_for_metadata_key(key)->Delete(options, db_key).ok();
}

void RocksDBStorage::begin_batch() {
    if (!db_) throw storage::ReadError(storage::ReadStatus::IO_ERROR, "Storage is closed");
    if (in_batch_) throw std::logic_error("Storage batch is already active");
    batch_values_.clear();

    batch_ = std::make_unique<rocksdb::WriteBatch>();
    if (address_db_) {
        address_batch_ = std::make_unique<rocksdb::WriteBatch>();
    }
    in_batch_ = true;
}

bool RocksDBStorage::commit_batch() {
    last_batch_commit_durable_.store(false, std::memory_order_release);
    if (!db_ || !batch_ || !in_batch_) {
        return false;
    }

    bool sync_write = true;
    bool disable_wal = false;
    {
        std::lock_guard<std::mutex> lock(write_mode_mutex_);
        if (ibd_mode_) {
            ++ibd_pending_commits_;
            sync_write = (ibd_pending_commits_ >= ibd_flush_interval_) || force_sync_next_commit_;
            disable_wal = options_.disable_wal_during_ibd;
            if (sync_write) {
                ibd_pending_commits_ = 0;
                force_sync_next_commit_ = false;
            }
        } else {
            sync_write = true;
            ibd_pending_commits_ = 0;
            force_sync_next_commit_ = false;
        }
    }

    const bool split_commit = address_db_ && address_batch_;
    if (split_commit) {
        // The marker deletions below are durable. Both payloads must also be
        // durable before clearing them: separate DBs can flush independently.
        sync_write = true;
        disable_wal = false;
        const std::string pending_key = meta_key(kSplitCommitPendingKey);
        batch_->Put(pending_key, rocksdb::Slice("1", 1));
        address_batch_->Put(pending_key, rocksdb::Slice("1", 1));
    }

    rocksdb::WriteOptions options;
    options.sync = sync_write;
    options.disableWAL = disable_wal && !sync_write;
    rocksdb::Status status;
    bool wrote_without_wal = false;
    if (split_commit) {
        // Addresses (UTXO) first so a crash cannot publish a new height/count
        // against the previous UTXO snapshot. The pending marker on either DB
        // forces rebuild even when the UTXO count happens to match.
        status = address_db_->Write(options, address_batch_.get());
        wrote_without_wal = status.ok() && options.disableWAL;
        if (status.ok()) {
            status = db_->Write(options, batch_.get());
            wrote_without_wal = wrote_without_wal || (status.ok() && options.disableWAL);
        }
    } else {
        status = db_->Write(options, batch_.get());
        wrote_without_wal = status.ok() && options.disableWAL;
    }

    batch_.reset();
    address_batch_.reset();
    batch_values_.clear();
    in_batch_ = false;

    bool flush_unlogged_writes = false;
    {
        std::lock_guard<std::mutex> lock(write_mode_mutex_);
        has_unlogged_writes_ = has_unlogged_writes_ || wrote_without_wal;
        flush_unlogged_writes = sync_write && has_unlogged_writes_;
    }

    if (!status.ok()) {
        return false;
    }
    if (flush_unlogged_writes) {
        rocksdb::FlushOptions flush_options;
        flush_options.wait = true;
        status = db_->Flush(flush_options);
        if (status.ok() && address_db_) {
            status = address_db_->Flush(flush_options);
        }
    }
    if (!status.ok()) {
        return false;
    }

    if (split_commit) {
        rocksdb::WriteOptions clear_options;
        clear_options.sync = true;
        const std::string pending_key = meta_key(kSplitCommitPendingKey);
        status = db_->Delete(clear_options, pending_key);
        if (status.ok()) {
            status = address_db_->Delete(clear_options, pending_key);
        }
        if (!status.ok()) {
            return false;
        }
    }

    if (flush_unlogged_writes) {
        std::lock_guard<std::mutex> lock(write_mode_mutex_);
        has_unlogged_writes_ = false;
    }
    last_batch_commit_durable_.store(sync_write, std::memory_order_release);
    return true;
}

void RocksDBStorage::abort_batch() {
    batch_.reset();
    address_batch_.reset();
    batch_values_.clear();
    in_batch_ = false;
}

bool RocksDBStorage::last_batch_commit_was_durable() const {
    return last_batch_commit_durable_.load(std::memory_order_acquire);
}

bool RocksDBStorage::has_incomplete_split_commit() const {
    if (!address_db_) return false;
    // Read each physical DB; the batch overlay never contains this internal marker.
    bytes value;
    return read_value(db_.get(), meta_key(kSplitCommitPendingKey), value) ||
           read_value(address_db_.get(), meta_key(kSplitCommitPendingKey), value);
}

bool RocksDBStorage::read_value(rocksdb::DB* db, const std::string& key, bytes& value) const {
    value.clear();
    if (!db) throw storage::ReadError(storage::ReadStatus::IO_ERROR, "Storage is closed");
    if (in_batch_) {
        auto it = batch_values_.find(key);
        if (it != batch_values_.end()) {
            if (!it->second) return false;
            value = *it->second;
            return true;
        }
    }
    std::string raw;
    const auto status = db->Get(rocksdb::ReadOptions(), key, &raw);
    if (status.IsNotFound()) return false;
    if (!status.ok()) throw storage::ReadError(
        status.IsCorruption() ? storage::ReadStatus::CORRUPT : storage::ReadStatus::IO_ERROR,
        "Read " + key + ": " + status.ToString());
    value.assign(raw.begin(), raw.end());
    return true;
}

void RocksDBStorage::set_ibd_mode(bool enabled) {
    std::lock_guard<std::mutex> lock(write_mode_mutex_);
    if (ibd_mode_ == enabled) {
        return;
    }

    ibd_mode_ = enabled;
    if (!ibd_mode_ && ibd_pending_commits_ > 0) {
        force_sync_next_commit_ = true;
    }
}

void RocksDBStorage::set_ibd_flush_interval(size_t operations) {
    std::lock_guard<std::mutex> lock(write_mode_mutex_);
    ibd_flush_interval_ = (operations == 0) ? 1 : operations;
}

} // namespace geocoin::state
