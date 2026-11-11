#include "test_utils.hpp"
#include "geocoin/core/block.hpp"
#include "geocoin/core/transaction.hpp"
#include "geocoin/state/storage.hpp"
#include "geocoin/storage/block_storage.hpp"
#include "geocoin/storage/rocksdb_store.hpp"

#include <rocksdb/utilities/stackable_db.h>
#include <atomic>
#include <barrier>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace geocoin::storage {
struct BlockStorageTestAccess {
    static void wrap(BlockStorage& storage, bool chain,
                     const std::function<rocksdb::DB*(rocksdb::DB*)>& factory) {
        auto& store = chain ? storage.chain_db_ : storage.index_db_;
        store->db_.reset(factory(store->db_.release()));
    }
    static bool marker(const BlockStorage& storage) {
        std::string value;
        return storage.index_db_->get("split_write_pending", value);
    }
    static bool indexed(const BlockStorage& storage, const Hash256& hash) {
        std::string value;
        return storage.db_get(storage.block_index_key(hash), value);
    }
};
}

namespace geocoin::state {
struct RocksDBStorageTestAccess {
    static bool flush_one(RocksDBStorage& storage, bool addresses) {
        rocksdb::FlushOptions options;
        options.wait = true;
        return (addresses ? storage.address_db_ : storage.db_)->Flush(options).ok();
    }
};
}

namespace {
using namespace geocoin;
using test::expect;
using Access = storage::BlockStorageTestAccess;

struct Scratch {
    std::filesystem::path path = test::make_temp_dir("geocoin-split-write-failure");
    ~Scratch() { std::error_code error; std::filesystem::remove_all(path, error); }
};

struct Probe {
    bool fail_next_batch = false;
    bool fail_after_batch = false;
    bool fail_marker = false;
    bool fail_after_marker = false;
    std::function<void()> before_batch;
    std::function<void()> after_batch;
};

// Intercept actual RocksDB calls in tests; the production backend stays concrete.
class ObservedDB final : public rocksdb::StackableDB {
public:
    ObservedDB(rocksdb::DB* db, std::shared_ptr<Probe> probe)
        : StackableDB(db), probe_(std::move(probe)) {}
    rocksdb::Status Write(const rocksdb::WriteOptions& options, rocksdb::WriteBatch* batch) override {
        if (probe_->before_batch) probe_->before_batch();
        if (probe_->fail_next_batch) {
            probe_->fail_next_batch = false;
            return rocksdb::Status::IOError("injected index write failure");
        }
        auto result = GetBaseDB()->Write(options, batch);
        if (result.ok() && probe_->after_batch) probe_->after_batch();
        if (result.ok() && probe_->fail_after_batch) {
            probe_->fail_after_batch = false;
            return rocksdb::Status::IOError("injected uncertain write result");
        }
        return result;
    }
    using StackableDB::Put;
    rocksdb::Status Put(const rocksdb::WriteOptions& options, rocksdb::ColumnFamilyHandle* family,
                        const rocksdb::Slice& key, const rocksdb::Slice& value) override {
        if (key == "split_write_pending" && probe_->fail_marker) {
            probe_->fail_marker = false;
            return rocksdb::Status::IOError("injected marker write failure");
        }
        auto result = GetBaseDB()->Put(options, family, key, value);
        if (result.ok() && key == "split_write_pending" && probe_->fail_after_marker) {
            probe_->fail_after_marker = false;
            return rocksdb::Status::IOError("injected uncertain marker result");
        }
        return result;
    }
private:
    std::shared_ptr<Probe> probe_;
};

void observe(storage::BlockStorage& db, bool chain, const std::shared_ptr<Probe>& probe) {
    Access::wrap(db, chain, [&](auto* base) { return new ObservedDB(base, probe); });
}

core::Block block_at(Height height) {
    core::Block block;
    block.header.timestamp = 1700000000 + static_cast<uint32_t>(height);
    block.header.nonce = height;
    block.transactions.push_back(core::Transaction::create_coinbase(height, test::address_from_seed(42), COIN));
    // Parent and child reconstruct the same block across wall-clock seconds.
    block.transactions.front().timestamp = block.header.timestamp;
    block.header.merkle_root = block.calculate_merkle_root();
    block.header.witness_commitment = block.calculate_witness_commitment();
    return block;
}

storage::StorageOptions ibd_options() {
    storage::StorageOptions options;
    options.disable_wal_during_ibd = true;
    options.write_buffer_mb = 64;
    return options;
}

void marker_ordering() {
    Scratch scratch;
    storage::BlockStorage db(scratch.path.string());
    auto probe = std::make_shared<Probe>();
    bool marked_before_chain_write = false;
    probe->before_batch = [&] { marked_before_chain_write = Access::marker(db); };
    observe(db, true, probe);
    expect(db.save_block(block_at(1), 1, false), "write completes");
    expect(marked_before_chain_write, "pending marker must exist before the first database changes");
    expect(!db.has_incomplete_split_write(), "completed write clears its marker");
}

void failed_write_retry() {
    Scratch scratch;
    storage::BlockStorage db(scratch.path.string());
    auto probe = std::make_shared<Probe>();
    probe->fail_next_batch = true;
    observe(db, false, probe);
    const auto first = block_at(1), second = block_at(2);
    expect(!db.save_block(first, 1, false), "index failure reaches the caller");
    expect(Access::marker(db), "partial write retains its marker");
    expect(db.has_block(first.get_hash()) && !Access::indexed(db, first.get_hash()),
           "fixture interrupted a real split write");
    expect(!db.save_block(second, 2, false), "a later write cannot conceal an unfinished split write");
    expect(!db.has_block(second.get_hash()) && Access::marker(db), "later write changes neither body nor marker");
    expect(!db.set_policy_checkpoint(second.get_hash(), 2), "single-record writes also preserve the failed operation");
    db.close();
    expect(db.open(scratch.path.string()) && db.has_incomplete_split_write(), "reopening retains the unfinished marker");
    expect(!db.save_block(second, 2, false), "reopening does not erase the pending operation through a new write");
}

void uncertain_write_phase(int phase, bool applied) {
    Scratch scratch;
    storage::BlockStorage db(scratch.path.string());
    auto probe = std::make_shared<Probe>();
    if (phase == 0) {
        probe->fail_marker = !applied;
        probe->fail_after_marker = applied;
    } else {
        probe->fail_next_batch = !applied;
        probe->fail_after_batch = applied;
    }
    observe(db, phase == 1, probe);
    const auto block = block_at(1);
    expect(!db.save_block(block, 1, false), "each uncertain write phase reports failure");
    expect(db.has_incomplete_split_write(), "uncertain result remains visible in the running instance");
    expect(!db.set_best_block(block.get_hash(), 1), "uncertain operation cannot be overwritten in memory");
    const bool has_body = phase == 2 || (phase == 1 && applied);
    const bool has_index = phase == 2 && applied;
    expect(db.has_block(block.get_hash()) == has_body && Access::indexed(db, block.get_hash()) == has_index,
           "failure preserves the actual committed write phase");
    db.close();
    expect(db.open(scratch.path.string()), "reopen after uncertain result");
    const bool incomplete = !(phase == 0 && !applied) && !has_index;
    expect(db.has_incomplete_split_write() == incomplete,
           "reopening distinguishes pending writes from a completed final commit");
    expect(db.set_witness_pruned_height(0) != incomplete,
           "only a known completed or untouched database accepts new writes");
}

void concurrent_writers() {
    Scratch scratch;
    storage::BlockStorage db(scratch.path.string());
    std::vector<core::Block> blocks;
    for (Height h = 1; h <= 4; ++h) blocks.push_back(block_at(h));
    std::barrier ready(4);
    std::atomic<unsigned> failed{0};
    std::vector<std::thread> writers;
    for (size_t i = 0; i < blocks.size(); ++i) {
        writers.emplace_back([&, i] {
            ready.arrive_and_wait();
            if (!db.save_block(blocks[i], i + 1, false)) ++failed;
        });
    }
    for (auto& writer : writers) writer.join();
    expect(failed == 0 && !db.has_incomplete_split_write(), "healthy concurrent writes serialize without spurious failures");
    expect(db.get_best_height() == 4, "concurrent lower blocks cannot overwrite the highest saved block");
    for (const auto& block : blocks) {
        expect(db.has_block(block.get_hash()) && Access::indexed(db, block.get_hash()),
               "every concurrent body has its index");
    }
}

#ifndef _WIN32
int child(const std::string& mode, const std::filesystem::path& root) {
    if (mode == "interrupted" || mode == "prune-interrupted" || mode == "block-ibd") {
        storage::BlockStorage db(root.string(), storage::StorageBackend::ROCKSDB, ibd_options());
        expect(db.is_open(), "child opens block storage");
        if (mode == "prune-interrupted") {
            expect(db.save_block(block_at(1), 1, false) && db.set_active_block_and_tip(1, block_at(1).get_hash()),
                   "seed active body before pruning interruption");
        }
        if (mode != "block-ibd") {
            auto probe = std::make_shared<Probe>();
            probe->after_batch = [] { ::_exit(42); };
            observe(db, true, probe);
        } else {
            db.set_ibd_flush_interval(10000);
            db.set_ibd_mode(true);
        }
        expect(mode == "prune-interrupted" ? db.prune_block_body_at_height(1)
                                            : db.save_block(block_at(1), 1, false), "child writes block data");
        expect(!db.has_incomplete_split_write(), "child cleared the completed marker");
        ::_exit(0);
    }
    state::RocksDBStorage db((root / "state").string(), (root / "addresses").string(), ibd_options());
    expect(db.open(), "child opens split state");
    expect(db.put_metadata("chain:probe", {1}) && db.put_metadata("addr:probe", {1}), "seed durable state pair");
    db.set_ibd_flush_interval(10000);
    db.set_ibd_mode(true);
    db.begin_batch();
    expect(db.put_metadata("chain:probe", {2}) && db.put_metadata("addr:probe", {2}) && db.commit_batch(),
           "child commits state pair");
    expect(!db.has_incomplete_split_commit(), "child cleared state markers");
    // Independent DBs can flush at different times before a process stops.
    expect(state::RocksDBStorageTestAccess::flush_one(db, mode == "state-addresses"), "flush one physical DB");
    ::_exit(0);
}

void crash_case(const std::string& self, const std::string& mode) {
    Scratch scratch;
    const pid_t pid = ::fork();
    expect(pid >= 0, "fork succeeds");
    if (pid == 0) {
        ::execl(self.c_str(), self.c_str(), "--child", mode.c_str(), scratch.path.c_str(), static_cast<char*>(nullptr));
        ::_exit(127);
    }
    int status = 0;
    expect(::waitpid(pid, &status, 0) == pid, "child is reaped");
    const bool interrupted = mode == "interrupted" || mode == "prune-interrupted";
    expect(WIFEXITED(status) && WEXITSTATUS(status) == (interrupted ? 42 : 0),
           "child stopped at the selected write boundary");
    if (interrupted || mode == "block-ibd") {
        storage::BlockStorage db(scratch.path.string());
        expect(db.is_open(), "block storage reopens");
        if (interrupted) {
            expect(db.has_block(block_at(1).get_hash()) == (mode == "interrupted") &&
                   Access::indexed(db, block_at(1).get_hash()) == (mode == "prune-interrupted"),
                   "process stopped between the two database writes");
            expect(db.has_incomplete_split_write(), "interruption between database writes must be detectable after restart");
        } else {
            core::Block loaded;
            expect(db.get_block(block_at(1).get_hash(), loaded, true) && Access::indexed(db, loaded.get_hash()),
                   "clearing the IBD marker requires durable bodies and indexes");
            expect(!db.has_incomplete_split_write(), "complete IBD write reopens without a pending marker");
        }
    } else {
        state::RocksDBStorage db((scratch.path / "state").string(), (scratch.path / "addresses").string());
        expect(db.open(), "state reopens");
        bytes primary, addresses;
        expect(db.get_metadata("chain:probe", primary) && db.get_metadata("addr:probe", addresses), "read both state parts");
        expect(db.has_incomplete_split_commit() || primary == addresses,
               "independent flushes must not expose mismatched state without a pending marker");
        expect(primary == bytes{2} && addresses == bytes{2} && !db.has_incomplete_split_commit(),
               "completed split commit survives a process stop");
    }
}
#endif
}

int main(int argc, char** argv) {
    try {
#ifndef _WIN32
        if (argc == 4 && std::string(argv[1]) == "--child") return child(argv[2], argv[3]);
#endif
        const std::string mode = argc == 2 ? argv[1] : "all";
        if (mode == "all" || mode == "ordering") marker_ordering();
        if (mode == "all" || mode == "retry") failed_write_retry();
        if (mode == "all" || mode == "phases") {
            for (int phase = 0; phase < 3; ++phase) {
                for (bool applied : {false, true}) uncertain_write_phase(phase, applied);
            }
        }
        if (mode == "all" || mode == "concurrent") concurrent_writers();
#ifndef _WIN32
        const auto self = std::filesystem::absolute(argv[0]).string();
        for (const auto* scenario : {"interrupted", "prune-interrupted", "block-ibd", "state-addresses", "state-primary"}) {
            if (mode == "all" || mode == scenario) crash_case(self, scenario);
        }
#endif
        std::cout << "Split-write failure regressions passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
