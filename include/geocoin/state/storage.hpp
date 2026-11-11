#pragma once

#include "geocoin/types.hpp"
#include "geocoin/core/transaction.hpp"
#include "geocoin/storage/db_options.hpp"
#include "geocoin/storage/read_status.hpp"
#include <algorithm>
#include <atomic>
#include <memory>
#include <string>
#include <optional>
#include <map>  // Используем map вместо unordered_map для Hash256
#include <mutex>
#include <vector>

namespace rocksdb {
    class DB;
    class WriteBatch;
}

namespace geocoin::state {

using storage::StorageOptions;
using storage::StorageProfile;

/**
 * @brief Хранилище состояния и архивных транзакций; блоками владеет BlockStorage
 */
class Storage {
public:
    virtual ~Storage() = default;

    // Typed writes reject a key that differs from the object's hash without
    // changing stored or pending data. Successful typed reads return that exact
    // identity; mismatched records throw CORRUPT and leave the output unchanged.

    // Транзакции
    virtual bool put_transaction(const Hash256& hash, const core::Transaction& tx) = 0;
    virtual bool get_transaction(const Hash256& hash, core::Transaction& tx) const = 0;
    virtual bool has_transaction(const Hash256& hash) const = 0;
    virtual bool delete_transaction(const Hash256& hash) = 0;

    // Reads return false only for an absent key; corrupt data and I/O failures
    // throw storage::ReadError. Scans must either complete or throw, never return
    // a partial result. Callers serialize access, including the batch lifetime.
    // All reads during a batch see its pending puts and deletes.
    // Метаданные
    virtual bool put_metadata(const std::string& key, const bytes& value) = 0;
    virtual bool get_metadata(const std::string& key, bytes& value) const = 0;
    virtual bool has_metadata(const std::string& key) const = 0;
    virtual bool delete_metadata(const std::string& key) = 0;

    storage::ReadResult read_metadata(const std::string& key, bytes& value) const {
        try {
            return {get_metadata(key, value) ? storage::ReadStatus::FOUND
                                             : storage::ReadStatus::NOT_FOUND, {}};
        } catch (const storage::ReadError& error) {
            return {error.status(), error.what()};
        }
    }

    // Получить все ключи метаданных
    virtual std::vector<std::string> get_all_keys() const { return {}; };
    // Bounded metadata scan. Backends should implement this with a prefix seek;
    // the default keeps third-party/test implementations source compatible.
    virtual std::vector<std::string> get_keys_with_prefix(const std::string& prefix) const {
        std::vector<std::string> result;
        for (const auto& key : get_all_keys()) {
            if (key.rfind(prefix, 0) == 0) result.push_back(key);
        }
        return result;
    }
    virtual std::vector<std::string> get_keys_page(const std::string& prefix, size_t offset,
                                                  size_t limit, size_t* total = nullptr, const std::string& after = {}) const {
        auto keys = get_keys_with_prefix(prefix);
        if (!after.empty()) keys.erase(keys.begin(), std::upper_bound(keys.begin(), keys.end(), after));
        if (total) *total = keys.size();
        if (offset >= keys.size()) return {};
        const auto count = std::min(limit, keys.size() - offset);
        return {keys.begin() + offset, keys.begin() + offset + count};
    }

    // Batch-операции
    virtual void begin_batch() = 0;
    virtual bool commit_batch() = 0;
    virtual void abort_batch() = 0;
    virtual bool batch_active() const { return false; }

    // A successful commit may still be buffered during IBD. Recovery markers
    // must only be removed after every backing store is crash-durable.
    virtual bool last_batch_commit_was_durable() const { return true; }

    // True when a split chaindata/addresses commit wrote one DB and not the other.
    // Implementations must probe each backing store directly (no cross-DB fallback).
    virtual bool has_incomplete_split_commit() const { return false; }

    // Called with exclusive access before rebuilding persistent state. Disk
    // backends retain a checkpoint until the replacement has been verified.
    virtual bool create_recovery_checkpoint() { return true; }
    virtual void discard_recovery_checkpoint() {}

    // Политика записи в IBD (опционально для бэкендов хранения)
    virtual void set_ibd_mode(bool enabled) { (void)enabled; }
    virtual void set_ibd_flush_interval(size_t operations) { (void)operations; }
};

/**
 * @brief Реализация Storage в памяти (для тестов)
 */
class MemoryStorage : public Storage {
public:
    MemoryStorage() = default;
    ~MemoryStorage() override = default;


    // Транзакции
    bool put_transaction(const Hash256& hash, const core::Transaction& tx) override;
    bool get_transaction(const Hash256& hash, core::Transaction& tx) const override;
    bool has_transaction(const Hash256& hash) const override;
    bool delete_transaction(const Hash256& hash) override;

    // Метаданные
    bool put_metadata(const std::string& key, const bytes& value) override;
    bool get_metadata(const std::string& key, bytes& value) const override;
    bool has_metadata(const std::string& key) const override;
    bool delete_metadata(const std::string& key) override;

    // Batch-операции
    void begin_batch() override;
    bool commit_batch() override;
    void abort_batch() override;
    bool batch_active() const override { return in_batch_; }

    std::vector<std::string> get_all_keys() const override;
    std::vector<std::string> get_keys_with_prefix(const std::string& prefix) const override;
    std::vector<std::string> get_keys_page(const std::string& prefix, size_t offset,
                                         size_t limit, size_t* total = nullptr, const std::string& after = {}) const override;

private:
    std::map<Hash256, core::Transaction> transactions_;
    std::map<std::string, bytes> metadata_;

    // Batch-состояние
    bool in_batch_ = false;
    std::map<Hash256, std::optional<core::Transaction>> batch_transactions_;
    std::map<std::string, std::optional<bytes>> batch_metadata_;
};

/**
 * @brief RocksDB реализация Storage
 */
class RocksDBStorage : public Storage {
public:
    explicit RocksDBStorage(const std::string& db_path, StorageOptions options = {});
    RocksDBStorage(
        const std::string& db_path,
        const std::string& address_db_path,
        StorageOptions options = {}
    );
    ~RocksDBStorage() override;

    bool open();
    void close();
    bool is_open() const { return db_ != nullptr; }


    bool put_transaction(const Hash256& hash, const core::Transaction& tx) override;
    bool get_transaction(const Hash256& hash, core::Transaction& tx) const override;
    bool has_transaction(const Hash256& hash) const override;
    bool delete_transaction(const Hash256& hash) override;

    bool put_metadata(const std::string& key, const bytes& value) override;
    bool get_metadata(const std::string& key, bytes& value) const override;
    bool has_metadata(const std::string& key) const override;
    bool delete_metadata(const std::string& key) override;

    void begin_batch() override;
    bool commit_batch() override;
    void abort_batch() override;
    bool batch_active() const override { return in_batch_; }
    bool last_batch_commit_was_durable() const override;
    bool has_incomplete_split_commit() const override;
    bool create_recovery_checkpoint() override;
    void discard_recovery_checkpoint() override;

    void set_ibd_mode(bool enabled) override;
    void set_ibd_flush_interval(size_t operations) override;

    std::vector<std::string> get_all_keys() const override;
    std::vector<std::string> get_keys_with_prefix(const std::string& prefix) const override;

    std::vector<std::string> get_keys_page(const std::string& prefix, size_t offset,
                                          size_t limit, size_t* total = nullptr, const std::string& after = {}) const override;

private:
#ifdef GEOCOIN_ENABLE_STATE_STORAGE_TEST_ACCESS
    friend struct RocksDBStorageTestAccess;
#endif
    std::string db_path_;
    std::string address_db_path_;
    StorageOptions options_;
    std::unique_ptr<rocksdb::DB> db_;
    std::unique_ptr<rocksdb::DB> address_db_;
    std::unique_ptr<rocksdb::WriteBatch> batch_;
    std::unique_ptr<rocksdb::WriteBatch> address_batch_;
    std::map<std::string, std::optional<bytes>> batch_values_;
    std::string recovery_checkpoint_path_;
    bool in_batch_ = false;

    mutable std::mutex write_mode_mutex_;
    bool ibd_mode_ = false;
    size_t ibd_pending_commits_ = 0;
    size_t ibd_flush_interval_ = 1000;
    bool force_sync_next_commit_ = false;
    bool has_unlogged_writes_ = false;
    std::atomic<bool> last_batch_commit_durable_{true};

    std::string tx_key(const Hash256& hash) const;
    std::string meta_key(const std::string& key) const;
    bool is_address_metadata_key(const std::string& key) const;
    rocksdb::DB* db_for_metadata_key(const std::string& key) const;
    bool read_value(rocksdb::DB* db, const std::string& key, bytes& value) const;
};

/**
 * @brief Фабрика для создания Storage
 */
class StorageFactory {
public:
    enum class Type {
        MEMORY,
        ROCKSDB
    };

    static std::unique_ptr<Storage> create(
        Type type,
        const std::string& path = "",
        StorageOptions options = {}
    );
    static std::unique_ptr<Storage> create_split(
        Type type,
        const std::string& chaindata_path,
        const std::string& addresses_path,
        StorageOptions options = {}
    );
    static std::optional<Type> parse_type(const std::string& value);
    static std::optional<StorageProfile> parse_profile(const std::string& value);
    static const char* type_name(Type type);
    static const char* profile_name(StorageProfile profile);
    static bool rocksdb_available();
};

} // namespace geocoin::state
