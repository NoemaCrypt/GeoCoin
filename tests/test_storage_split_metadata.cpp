#include "geocoin/state/storage.hpp"

#include <chrono>
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

using geocoin::bytes;

bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << message << "\n";
    }
    return condition;
}

bytes value_bytes(const char* value) {
    return bytes(value, value + std::char_traits<char>::length(value));
}

geocoin::core::Transaction make_memory_tx(uint8_t seed) {
    geocoin::core::Transaction tx;
    tx.version = 1;
    tx.timestamp = 1700000100 + seed;
    tx.outputs.push_back({
        geocoin::COIN + seed,
        geocoin::Address("GEOmemory-tx-" + std::to_string(seed))
    });
    return tx;
}

std::filesystem::path unique_test_dir() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
        ("geocoin_storage_split_metadata_" + std::to_string(now));
}

bool seed_fallback_metadata(const std::filesystem::path& chain_path, const std::string& key, const bytes& value) {
    geocoin::state::RocksDBStorage storage(chain_path.string());
    return storage.open() && storage.put_metadata(key, value);
}

bool fallback_has_metadata(const std::filesystem::path& chain_path, const std::string& key) {
    geocoin::state::RocksDBStorage storage(chain_path.string());
    if (!storage.open()) {
        return false;
    }
    bytes value;
    return storage.get_metadata(key, value);
}

bool contains_key(const std::vector<std::string>& keys, const std::string& key) {
    return std::find(keys.begin(), keys.end(), key) != keys.end();
}

void test_memory_storage_batch_abort(bool& ok) {
    geocoin::state::MemoryStorage storage;
    const std::string stable_key = "memory:stable";
    const std::string transient_key = "memory:transient";

    ok &= expect(storage.put_metadata(stable_key, value_bytes("old")),
                 "MemoryStorage initial put_metadata failed");

    storage.begin_batch();
    ok &= expect(storage.put_metadata(stable_key, value_bytes("new")),
                 "MemoryStorage batch put_metadata failed");
    bytes value;
    ok &= expect(storage.get_metadata(stable_key, value) && value == value_bytes("new"),
                 "MemoryStorage batch put should be visible inside batch");
    storage.abort_batch();

    ok &= expect(storage.get_metadata(stable_key, value) && value == value_bytes("old"),
                 "MemoryStorage abort_batch must restore previous metadata");

    storage.begin_batch();
    ok &= expect(storage.delete_metadata(stable_key),
                 "MemoryStorage batch delete_metadata failed");
    ok &= expect(!storage.has_metadata(stable_key),
                 "MemoryStorage batch delete should hide metadata inside batch");
    storage.abort_batch();

    ok &= expect(storage.has_metadata(stable_key),
                 "MemoryStorage abort_batch must restore deleted metadata");

    storage.begin_batch();
    ok &= expect(storage.put_metadata(transient_key, value_bytes("temp")),
                 "MemoryStorage transient batch put failed");
    storage.abort_batch();

    ok &= expect(!storage.has_metadata(transient_key),
                 "MemoryStorage abort_batch must discard new metadata");
}

void test_memory_storage_tx_batches(bool& ok) {
    geocoin::state::MemoryStorage storage;

    const auto tx_a = make_memory_tx(1);
    const auto tx_b = make_memory_tx(2);
    const geocoin::Hash256 tx_a_hash = tx_a.get_hash();
    const geocoin::Hash256 tx_b_hash = tx_b.get_hash();

    ok &= expect(!storage.commit_batch(), "MemoryStorage commit without batch should fail");

    ok &= expect(storage.put_transaction(tx_a_hash, tx_a), "MemoryStorage put_transaction failed");
    ok &= expect(storage.has_transaction(tx_a_hash), "MemoryStorage has_transaction sees committed tx");

    storage.begin_batch();
    ok &= expect(storage.put_transaction(tx_b_hash, tx_b), "MemoryStorage batch put_transaction failed");
    ok &= expect(storage.delete_transaction(tx_a_hash), "MemoryStorage batch delete_transaction failed");
    ok &= expect(!storage.has_transaction(tx_a_hash), "MemoryStorage batch delete hides tx");
    ok &= expect(storage.has_transaction(tx_b_hash), "MemoryStorage batch put exposes tx");

    geocoin::core::Transaction loaded_tx;
    ok &= expect(storage.get_transaction(tx_b_hash, loaded_tx) && loaded_tx.get_hash() == tx_b_hash,
                 "MemoryStorage batch get_transaction returns staged tx");
    ok &= expect(!storage.get_transaction(tx_a_hash, loaded_tx),
                 "MemoryStorage batch get_transaction hides deleted tx");
    storage.abort_batch();

    ok &= expect(storage.has_transaction(tx_a_hash), "MemoryStorage abort restores committed tx");
    ok &= expect(!storage.has_transaction(tx_b_hash), "MemoryStorage abort discards staged tx");

    storage.begin_batch();
    ok &= expect(storage.delete_transaction(tx_a_hash), "MemoryStorage commit delete_transaction failed");
    ok &= expect(storage.put_transaction(tx_b_hash, tx_b), "MemoryStorage commit put_transaction failed");
    ok &= expect(storage.commit_batch(), "MemoryStorage commit_batch failed");

    ok &= expect(!storage.has_transaction(tx_a_hash), "MemoryStorage commit removes deleted tx");
    ok &= expect(storage.get_transaction(tx_b_hash, loaded_tx) && loaded_tx.get_hash() == tx_b_hash,
                 "MemoryStorage commit persists tx");

    ok &= expect(storage.delete_transaction(tx_b_hash), "MemoryStorage non-batch delete_transaction failed");
    ok &= expect(!storage.has_transaction(tx_b_hash), "MemoryStorage non-batch delete removes tx");
}

void test_memory_storage_key_listing_batches(bool& ok) {
    geocoin::state::MemoryStorage storage;
    ok &= expect(storage.put_metadata("memory:key:a", value_bytes("a")),
                 "MemoryStorage key listing seed A failed");
    ok &= expect(storage.put_metadata("memory:key:b", value_bytes("b")),
                 "MemoryStorage key listing seed B failed");

    storage.begin_batch();
    ok &= expect(storage.delete_metadata("memory:key:a"),
                 "MemoryStorage key listing batch delete failed");
    ok &= expect(storage.put_metadata("memory:key:c", value_bytes("c")),
                 "MemoryStorage key listing batch put failed");
    const auto staged_keys = storage.get_all_keys();
    const auto staged_prefix = storage.get_keys_with_prefix("memory:key:");
    ok &= expect(!contains_key(staged_keys, "memory:key:a"),
                 "MemoryStorage get_all_keys hides batch-deleted key");
    ok &= expect(contains_key(staged_keys, "memory:key:b"),
                 "MemoryStorage get_all_keys keeps committed key");
    ok &= expect(contains_key(staged_keys, "memory:key:c"),
                 "MemoryStorage get_all_keys includes staged key");
    ok &= expect(!contains_key(staged_prefix, "memory:key:a") &&
                     contains_key(staged_prefix, "memory:key:b") &&
                     contains_key(staged_prefix, "memory:key:c"),
                 "MemoryStorage prefix scan observes the staged batch view");
    ok &= expect(storage.get_keys_with_prefix("unrelated:").empty(),
                 "MemoryStorage prefix scan excludes unrelated metadata");
    ok &= expect(storage.commit_batch(), "MemoryStorage key listing commit failed");

    const auto committed_keys = storage.get_all_keys();
    ok &= expect(!contains_key(committed_keys, "memory:key:a"),
                 "MemoryStorage get_all_keys persists batch delete");
    ok &= expect(contains_key(committed_keys, "memory:key:c"),
                 "MemoryStorage get_all_keys persists batch put");

    storage.begin_batch();
    ok &= expect(storage.delete_metadata("memory:key:c"),
                 "MemoryStorage key listing abort delete setup failed");
    ok &= expect(storage.put_metadata("memory:key:d", value_bytes("d")),
                 "MemoryStorage key listing abort put setup failed");
    storage.abort_batch();

    const auto aborted_keys = storage.get_all_keys();
    ok &= expect(contains_key(aborted_keys, "memory:key:c"),
                 "MemoryStorage get_all_keys restores aborted delete");
    ok &= expect(!contains_key(aborted_keys, "memory:key:d"),
                 "MemoryStorage get_all_keys drops aborted put");
}

void test_rocksdb_closed_guards(bool& ok, const std::filesystem::path& db_path) {
    geocoin::state::RocksDBStorage storage(db_path.string());
    const auto tx = make_memory_tx(9);
    const auto tx_hash = tx.get_hash();

    geocoin::core::Transaction loaded_tx;
    bytes value;
    ok &= expect(!storage.is_open(), "RocksDBStorage starts closed");
    const auto expect_io = [&](auto read) {
        bool failed = false;
        try { read(); } catch (const geocoin::storage::ReadError& e) {
            failed = e.status() == geocoin::storage::ReadStatus::IO_ERROR;
        }
        ok &= expect(failed, "closed storage must report I/O failure");
    };
    ok &= expect(!storage.put_transaction(tx_hash, tx), "closed RocksDBStorage rejects put_transaction");
    expect_io([&] { storage.get_transaction(tx_hash, loaded_tx); });
    expect_io([&] { storage.has_transaction(tx_hash); });
    ok &= expect(!storage.delete_transaction(tx_hash), "closed RocksDBStorage rejects delete_transaction");
    ok &= expect(!storage.put_metadata("closed:meta", value_bytes("meta")),
                 "closed RocksDBStorage rejects put_metadata");
    expect_io([&] { storage.get_metadata("closed:meta", value); });
    expect_io([&] { storage.has_metadata("closed:meta"); });
    ok &= expect(!storage.delete_metadata("closed:meta"), "closed RocksDBStorage rejects delete_metadata");
    expect_io([&] { storage.get_all_keys(); });
    expect_io([&] { storage.begin_batch(); });
    ok &= expect(!storage.commit_batch(), "closed RocksDBStorage commit_batch fails");
    storage.abort_batch();
}

void test_rocksdb_tx_batches_and_ibd(bool& ok, const std::filesystem::path& db_path) {
    geocoin::storage::StorageOptions options;
    options.disable_wal_during_ibd = true;
    options.profile = geocoin::storage::StorageProfile::FAST_SYNC;

    geocoin::state::RocksDBStorage storage(db_path.string(), options);
    ok &= expect(storage.open(), "RocksDBStorage tx test opens");
    ok &= expect(storage.open(), "RocksDBStorage second open is a no-op");

    const auto tx_a = make_memory_tx(21);
    const auto tx_b = make_memory_tx(22);
    const auto tx_a_hash = tx_a.get_hash();
    const auto tx_b_hash = tx_b.get_hash();

    geocoin::core::Transaction loaded_tx;
    ok &= expect(!storage.get_transaction(tx_a_hash, loaded_tx), "missing RocksDB tx is absent");

    ok &= expect(storage.put_transaction(tx_a_hash, tx_a), "RocksDBStorage put_transaction succeeds");
    ok &= expect(storage.has_transaction(tx_a_hash), "RocksDBStorage has_transaction sees committed tx");
    ok &= expect(storage.get_transaction(tx_a_hash, loaded_tx) && loaded_tx.get_hash() == tx_a_hash,
                 "RocksDBStorage get_transaction loads committed tx");

    storage.begin_batch();
    ok &= expect(storage.put_transaction(tx_b_hash, tx_b), "RocksDBStorage batch put_transaction succeeds");
    ok &= expect(storage.delete_transaction(tx_a_hash), "RocksDBStorage batch delete_transaction succeeds");
    ok &= expect(storage.commit_batch(), "RocksDBStorage tx batch commit succeeds");

    ok &= expect(!storage.has_transaction(tx_a_hash), "RocksDBStorage committed batch delete removes tx");
    ok &= expect(storage.get_transaction(tx_b_hash, loaded_tx) && loaded_tx.get_hash() == tx_b_hash,
                 "RocksDBStorage committed batch put stores tx");

    storage.begin_batch();
    ok &= expect(storage.delete_transaction(tx_b_hash), "RocksDBStorage abort delete tx setup succeeds");
    ok &= expect(storage.put_transaction(tx_a_hash, tx_a), "RocksDBStorage abort put tx setup succeeds");
    storage.abort_batch();

    ok &= expect(storage.has_transaction(tx_b_hash), "RocksDBStorage abort preserves committed tx");
    ok &= expect(!storage.has_transaction(tx_a_hash), "RocksDBStorage abort drops staged tx");

    storage.set_ibd_flush_interval(2);
    storage.set_ibd_mode(true);
    storage.begin_batch();
    ok &= expect(storage.put_metadata("ibd:one", value_bytes("1")),
                 "RocksDBStorage IBD first metadata put succeeds");
    ok &= expect(storage.commit_batch(), "RocksDBStorage IBD first commit succeeds");
    storage.begin_batch();
    ok &= expect(storage.put_metadata("ibd:two", value_bytes("2")),
                 "RocksDBStorage IBD second metadata put succeeds");
    ok &= expect(storage.commit_batch(), "RocksDBStorage IBD flush-interval commit succeeds");
    storage.begin_batch();
    ok &= expect(storage.put_metadata("ibd:three", value_bytes("3")),
                 "RocksDBStorage IBD pending metadata put succeeds");
    ok &= expect(storage.commit_batch(), "RocksDBStorage IBD pending commit succeeds");
    storage.set_ibd_mode(false);
    storage.begin_batch();
    ok &= expect(storage.put_metadata("ibd:after", value_bytes("4")),
                 "RocksDBStorage post-IBD metadata put succeeds");
    ok &= expect(storage.commit_batch(), "RocksDBStorage post-IBD forced sync commit succeeds");

    bytes value;
    ok &= expect(storage.get_metadata("ibd:one", value) && value == value_bytes("1"),
                 "RocksDBStorage IBD first value is readable");
    ok &= expect(storage.get_metadata("ibd:after", value) && value == value_bytes("4"),
                 "RocksDBStorage post-IBD value is readable");

    ok &= expect(storage.delete_transaction(tx_b_hash), "RocksDBStorage non-batch delete_transaction succeeds");
    ok &= expect(!storage.has_transaction(tx_b_hash), "RocksDBStorage non-batch delete removes tx");
    storage.close();
}

void test_storage_factory_helpers(bool& ok, const std::filesystem::path& root) {
    using geocoin::state::StorageFactory;
    using geocoin::storage::StorageProfile;

    ok &= expect(StorageFactory::parse_type(" memory ") == StorageFactory::Type::MEMORY,
                 "StorageFactory parses memory backend");
    ok &= expect(StorageFactory::parse_type("ROCKS") == StorageFactory::Type::ROCKSDB,
                 "StorageFactory parses rocks alias");
    ok &= expect(!StorageFactory::parse_type("unknown").has_value(),
                 "StorageFactory rejects unknown backend");
    ok &= expect(StorageFactory::parse_profile("fastsync") == StorageProfile::FAST_SYNC,
                 "StorageFactory parses fastsync profile");
    ok &= expect(StorageFactory::parse_profile("benchmark") == StorageProfile::UNSAFE_BENCHMARK,
                 "StorageFactory parses benchmark profile");
    ok &= expect(!StorageFactory::parse_profile("slow").has_value(),
                 "StorageFactory rejects unknown profile");
    ok &= expect(std::string(StorageFactory::type_name(StorageFactory::Type::MEMORY)) == "memory",
                 "StorageFactory names memory backend");
    ok &= expect(std::string(StorageFactory::type_name(StorageFactory::Type::ROCKSDB)) == "rocksdb",
                 "StorageFactory names rocksdb backend");
    ok &= expect(std::string(StorageFactory::type_name(static_cast<StorageFactory::Type>(99))) == "unknown",
                 "StorageFactory names unknown backend");
    ok &= expect(std::string(StorageFactory::profile_name(StorageProfile::SAFE)) == "safe",
                 "StorageFactory names safe profile");
    ok &= expect(std::string(StorageFactory::profile_name(StorageProfile::FAST_SYNC)) == "fast-sync",
                 "StorageFactory names fast profile");
    ok &= expect(std::string(StorageFactory::profile_name(StorageProfile::UNSAFE_BENCHMARK)) == "unsafe-benchmark",
                 "StorageFactory names unsafe profile");
    ok &= expect(std::string(StorageFactory::profile_name(static_cast<StorageProfile>(99))) == "unknown",
                 "StorageFactory names unknown profile");
    ok &= expect(StorageFactory::rocksdb_available(), "StorageFactory reports RocksDB availability");

    auto memory = StorageFactory::create(StorageFactory::Type::MEMORY);
    ok &= expect(memory != nullptr, "StorageFactory creates memory storage");

    auto split_memory = StorageFactory::create_split(
        StorageFactory::Type::MEMORY,
        (root / "factory-memory-chain").string(),
        (root / "factory-memory-address").string()
    );
    ok &= expect(split_memory != nullptr, "StorageFactory creates split memory storage");

    auto rocks = StorageFactory::create(
        StorageFactory::Type::ROCKSDB,
        (root / "factory" / "chaindata").string()
    );
    ok &= expect(rocks != nullptr, "StorageFactory creates RocksDB storage");
}

void test_split_commit_pending_marker(
    bool& ok,
    const std::filesystem::path& chain_path,
    const std::filesystem::path& address_path
) {
    {
        geocoin::state::RocksDBStorage storage(chain_path.string(), address_path.string());
        ok &= expect(storage.open(), "pending-marker test opens split storage");
        ok &= expect(!storage.has_incomplete_split_commit(),
                     "fresh split storage has no pending marker");
        ok &= expect(!geocoin::state::MemoryStorage().has_incomplete_split_commit(),
                     "MemoryStorage never reports a split-commit pending marker");

        storage.begin_batch();
        ok &= expect(storage.put_metadata("addr:pending-probe", value_bytes("addr")),
                     "pending-marker batch address put failed");
        ok &= expect(storage.put_metadata("chain:pending-probe", value_bytes("chain")),
                     "pending-marker batch chain put failed");
        ok &= expect(storage.commit_batch(), "pending-marker split commit failed");
        ok &= expect(storage.last_batch_commit_was_durable(),
                     "successful split commit is durable");
        ok &= expect(!storage.has_incomplete_split_commit(),
                     "successful split commit clears the pending marker");
        ok &= expect(!storage.has_metadata("split_commit_pending"),
                     "successful split commit does not leave pending metadata");
    }

    {
        geocoin::state::RocksDBStorage storage(chain_path.string(), address_path.string());
        ok &= expect(storage.open(), "pending-marker test reopens split storage");
        ok &= expect(storage.put_metadata("split_commit_pending", {1}),
                     "seeding chaindata pending marker failed");
        ok &= expect(storage.has_incomplete_split_commit(),
                     "pending marker on chaindata is detected by direct Get");
    }

    {
        geocoin::state::RocksDBStorage addr_only(address_path.string());
        ok &= expect(addr_only.open(), "pending-marker test opens addresses DB");
        ok &= expect(addr_only.put_metadata("split_commit_pending", {1}),
                     "seeding addresses-only pending marker failed");
    }

    {
        geocoin::state::RocksDBStorage storage(chain_path.string(), address_path.string());
        ok &= expect(storage.open(), "pending-marker test reopens after addresses-only seed");
        ok &= expect(storage.has_incomplete_split_commit(),
                     "pending marker on addresses is detected without has_metadata fallback");
    }
}

} // namespace

int main() {
    const std::filesystem::path root = unique_test_dir();
    const std::filesystem::path chain_path = root / "chaindata";
    const std::filesystem::path address_path = root / "addresses";

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);

    const std::string non_batch_key = "addr:split-non-batch";
    const std::string batch_put_key = "addr:split-batch-put";
    const std::string batch_delete_key = "addr:split-batch-delete";
    const std::string batch_abort_key = "addr:split-batch-abort";
    const std::string chain_key = "chain:split-visible";

    bool ok = true;
    test_memory_storage_batch_abort(ok);
    test_memory_storage_tx_batches(ok);
    test_memory_storage_key_listing_batches(ok);
    test_rocksdb_closed_guards(ok, root / "closed-rocksdb");
    test_rocksdb_tx_batches_and_ibd(ok, root / "rocksdb-block-tx");
    test_storage_factory_helpers(ok, root);

    ok &= expect(seed_fallback_metadata(address_path, non_batch_key, value_bytes("old")),
                 "failed to seed non-batch fallback metadata");
    ok &= expect(seed_fallback_metadata(address_path, batch_put_key, value_bytes("old")),
                 "failed to seed batch-put fallback metadata");
    ok &= expect(seed_fallback_metadata(address_path, batch_delete_key, value_bytes("old")),
                 "failed to seed batch-delete fallback metadata");

    {
        geocoin::state::RocksDBStorage storage(chain_path.string(), address_path.string());
        ok &= expect(storage.open(), "failed to open split RocksDBStorage");

        bytes value;
        ok &= expect(storage.get_metadata(non_batch_key, value) && value == value_bytes("old"),
                     "split storage did not read seeded fallback metadata");

        ok &= expect(storage.put_metadata(non_batch_key, value_bytes("new")),
                     "non-batch put_metadata failed");
        ok &= expect(storage.get_metadata(non_batch_key, value) && value == value_bytes("new"),
                     "split storage did not read non-batch address metadata");
    }

    {
        geocoin::state::RocksDBStorage storage(chain_path.string(), address_path.string());
        ok &= expect(storage.open(), "failed to reopen split RocksDBStorage for batch put");
        storage.begin_batch();
        ok &= expect(storage.put_metadata(batch_put_key, value_bytes("new")),
                     "batch put_metadata failed");
        ok &= expect(storage.put_metadata(chain_key, value_bytes("chain")),
                     "batch put_metadata failed for chain metadata");
        ok &= expect(storage.commit_batch(), "batch put commit failed");
    }

    ok &= expect(!fallback_has_metadata(chain_path, non_batch_key),
                 "non-batch put left stale fallback metadata in chaindata");
    ok &= expect(!fallback_has_metadata(chain_path, batch_put_key),
                 "batch put left stale fallback metadata in chaindata");

    {
        geocoin::state::RocksDBStorage storage(chain_path.string(), address_path.string());
        ok &= expect(storage.open(), "failed to reopen split RocksDBStorage for batch abort");
        storage.begin_batch();
        ok &= expect(storage.put_metadata(batch_abort_key, value_bytes("temp")),
                     "batch abort put_metadata failed");
        ok &= expect(storage.delete_metadata(non_batch_key),
                     "batch abort delete_metadata failed");
        storage.abort_batch();

        bytes value;
        ok &= expect(storage.get_metadata(non_batch_key, value) && value == value_bytes("new"),
                     "RocksDBStorage abort_batch must preserve previous metadata");
        ok &= expect(!storage.has_metadata(batch_abort_key),
                     "RocksDBStorage abort_batch must discard new metadata");

        const auto keys = storage.get_all_keys();
        const auto address_keys = storage.get_keys_with_prefix("addr:");
        ok &= expect(contains_key(keys, non_batch_key),
                     "split storage get_all_keys should include address metadata");
        ok &= expect(contains_key(keys, chain_key),
                     "split storage get_all_keys should include chain metadata");
        ok &= expect(contains_key(address_keys, non_batch_key) &&
                         !contains_key(address_keys, chain_key),
                     "split RocksDB prefix seek spans the address store without unrelated keys");
    }

    {
        geocoin::state::RocksDBStorage storage(chain_path.string(), address_path.string());
        ok &= expect(storage.open(), "failed to reopen split RocksDBStorage for batch delete");
        storage.begin_batch();
        ok &= expect(storage.put_metadata(batch_delete_key, value_bytes("new")),
                     "batch delete setup put_metadata failed");
        ok &= expect(storage.delete_metadata(batch_delete_key),
                     "batch delete_metadata failed");
        ok &= expect(storage.commit_batch(), "batch delete commit failed");

        bytes value;
        ok &= expect(!storage.get_metadata(batch_delete_key, value),
                     "batch delete left metadata visible through split storage");
        ok &= expect(!storage.has_incomplete_split_commit(),
                     "successful split batch delete leaves no pending marker");
    }

    test_split_commit_pending_marker(ok, root / "pending-chain", root / "pending-addr");

    std::filesystem::remove_all(root, ec);
    return ok ? 0 : 1;
}
