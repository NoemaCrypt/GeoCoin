#pragma once

#include "geocoin/storage/db_options.hpp"
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace rocksdb { class DB; class Iterator; class ColumnFamilyHandle; class Status; }

namespace geocoin::storage {

struct KVWrite {
    enum class Type {
        PUT,
        DELETE_KEY
    };

    Type type = Type::PUT;
    std::string key;
    std::string value;
};

enum class BlockStoreRole { CHAIN_DATA, INDEX };

class RocksDBIterator {
public:
    RocksDBIterator(rocksdb::DB* db, const std::vector<rocksdb::ColumnFamilyHandle*>& column_families);
    ~RocksDBIterator();
    bool valid() const;
    void seek(const std::string& key);
    void next();
    std::string key() const;
private:
    void select_min();
    std::vector<std::unique_ptr<rocksdb::Iterator>> its_;
    std::string current_key_;
    bool current_valid_ = false;
};

// One concrete backend. RocksDB options, CF routing, iterators and durability
// live here; BlockStorage owns chain records and transitions.
class RocksDBStore {
public:
    explicit RocksDBStore(BlockStoreRole role);
    ~RocksDBStore();
    bool open(const std::string& path, const StorageOptions& options);
    bool get(const std::string& key, std::string& value) const;
    bool put(const std::string& key, const std::string& value, bool sync, bool disable_wal);
    bool delete_key(const std::string& key, bool sync, bool disable_wal);
    bool write_batch(const std::vector<KVWrite>& writes, bool sync, bool disable_wal);
    bool sync();
    std::unique_ptr<RocksDBIterator> new_iterator() const;
private:
#ifdef GEOCOIN_ENABLE_BLOCK_STORAGE_TEST_ACCESS
    friend struct BlockStorageTestAccess;
#endif
    void close();
    bool finish_write(const rocksdb::Status& status, bool wrote_without_wal, bool sync_write);
    std::vector<std::string> required_column_families() const;
    static const char* column_family_for_key(const std::string& key);
    rocksdb::ColumnFamilyHandle* default_cf() const;
    rocksdb::ColumnFamilyHandle* handle_for_key(const std::string& key) const;
    const char* role_name() const;
    std::vector<rocksdb::ColumnFamilyHandle*> all_column_family_handles() const;
    rocksdb::Status flush_all_column_families();

    std::unique_ptr<rocksdb::DB> db_;
    BlockStoreRole role_;
    std::unordered_map<std::string, rocksdb::ColumnFamilyHandle*> cf_handles_;
    std::mutex write_mutex_;
    bool has_unlogged_writes_ = false;
};

} // namespace geocoin::storage
