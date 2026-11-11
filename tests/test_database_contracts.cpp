#include "chain_test_fixture.hpp"
#include "database_snapshot_fixture.hpp"
#include "geocoin/utils/bytes.hpp"
#include "geocoin/storage/backend_support.hpp"
#include "geocoin/storage/state_records.hpp"
#include "geocoin/storage/witness_payload.hpp"
#include "geocoin/wallet/wallet.hpp"
#include "../src/geocoin/state/storage_scan.hpp"
#include <iostream>
#include <limits>
#include <thread>

using namespace geocoin;
using test::expect;

namespace {
struct Scratch {
    std::filesystem::path path = test::make_temp_dir("geocoin-db-contract");
    ~Scratch() { std::error_code ec; std::filesystem::remove_all(path, ec); }
};

struct CountingRocksStorage : state::RocksDBStorage {
    using RocksDBStorage::RocksDBStorage;
    inline static size_t transaction_reads = 0;
    bool get_transaction(const Hash256& hash, core::Transaction& tx) const override {
        ++transaction_reads;
        return RocksDBStorage::get_transaction(hash, tx);
    }
};

std::unique_ptr<state::RocksDBStorage> persistent(const Scratch& scratch, bool split = true) {
    auto db = split
        ? std::make_unique<CountingRocksStorage>((scratch.path / "chain").string(), (scratch.path / "addresses").string())
        : std::make_unique<CountingRocksStorage>((scratch.path / "single").string());
    expect(db->open(), "open state database");
    return db;
}

void batch_contract(state::Storage& db) {
    const std::vector<std::string> prefixes{"probe:", "addr:probe:"};
    for (const auto& prefix : prefixes) {
        expect(db.put_metadata(prefix + "old", {1}), "seed old value");
        db.begin_batch();
        expect(db.put_metadata(prefix + "old", {2}) && db.put_metadata(prefix + "new", {3}), "stage values");
        bytes value;
        expect(db.get_metadata(prefix + "old", value) && value == bytes{2}, "read pending replacement");
        expect(db.has_metadata(prefix + "new"), "has pending insertion");
        expect(db.get_keys_with_prefix(prefix).size() == 2, "scan sees pending insertion");
        bool nested_rejected = false;
        try { db.begin_batch(); } catch (const std::logic_error&) { nested_rejected = true; }
        expect(nested_rejected, "nested begin rejects without discarding pending writes");
        expect(db.delete_metadata(prefix + "old"), "stage removal");
        expect(!db.get_metadata(prefix + "old", value), "get hides pending deletion");
        expect(value.empty(), "missing metadata clears the output on every backend");
        expect(db.get_keys_with_prefix(prefix) == std::vector<std::string>{prefix + "new"}, "scan hides pending deletion");
        db.abort_batch();
        expect(db.get_metadata(prefix + "old", value) && value == bytes{1}, "abort restores old value");
        expect(!db.has_metadata(prefix + "new"), "abort removes pending insertion");
        db.begin_batch();
        expect(db.delete_metadata(prefix + "old") && db.put_metadata(prefix + "old", {4}), "delete then replace");
        expect(db.get_metadata(prefix + "old", value) && value == bytes{4}, "last write wins");
        expect(db.commit_batch(), "commit pending replacement");
        expect(db.get_metadata(prefix + "old", value) && value == bytes{4}, "committed replacement visible");
    }
    const auto tx = core::Transaction::create_coinbase(1, test::address_from_seed(42), COIN);
    const auto hash = tx.get_hash();
    core::Transaction loaded;
    db.begin_batch();
    expect(db.put_transaction(hash, tx) && db.has_transaction(hash) && db.get_transaction(hash, loaded), "pending transaction readable");
    expect(loaded.get_hash() == hash, "pending transaction identity");
    expect(db.delete_transaction(hash) && !db.has_transaction(hash), "pending transaction removed");
    db.abort_batch();
}

// A scan that fails after returning a valid record must never look complete.
struct InterruptedCursor {
    bool valid() const { return true; }
    std::string_view key() const { return "probe:a"; }
    void next() { throw storage::ReadError(storage::ReadStatus::IO_ERROR, "injected read failure"); }
};

void read_failure_contract() {
    bool threw = false;
    try {
        InterruptedCursor cursor;
        const std::map<std::string, std::optional<bytes>> updates;
        (void)state::internal::scan_metadata_page(cursor, updates, "probe:", 0, 1, nullptr, "");
    } catch (const storage::ReadError& error) { threw = error.status() == storage::ReadStatus::IO_ERROR; }
    expect(threw, "interrupted scan reports I/O failure instead of partial keys");
    Scratch scratch;
    state::RocksDBStorage closed(scratch.path.string());
    bytes value;
    expect(closed.read_metadata("missing", value).status == storage::ReadStatus::IO_ERROR, "closed database is not an absent record");
    auto opened = persistent(scratch);
    expect(opened->read_metadata("missing", value).status == storage::ReadStatus::NOT_FOUND, "absent key distinguished");
}

void active_record_contract() {
    storage::ActiveTransactionRecord record{42, {}, 2};
    record.block_hash.fill(0x5a);
    bytes expected(48, 0);
    expected[0] = 42;
    std::fill(expected.begin() + 8, expected.begin() + 40, 0x5a);
    expected[40] = 2;
    expect(record.serialize() == expected, "active transaction record preserves current disk bytes");
    storage::ActiveTransactionRecord decoded;
    expect(storage::ActiveTransactionRecord::decode(expected, decoded) && decoded.height == 42 &&
           decoded.block_hash == record.block_hash && decoded.transaction_count == 2, "decode mandatory marker fields");
    for (const size_t length : {8u, 40u, 47u, 49u}) {
        expect(!storage::ActiveTransactionRecord::decode(bytes(length), decoded), "reject incomplete or oversized marker");
    }
    expected[40] = 0;
    expect(!storage::ActiveTransactionRecord::decode(expected, decoded), "reject empty transaction count");
}

void query_reference_contract() {
    using storage::QueryReferenceList;
    const std::string suffix = ":00000000000000000001:" + utils::hash256_to_hex(test::hash_from_seed(91));
    const std::vector<std::string> keys{"qaddr:61" + suffix, "qaddr:62" + suffix};
    const auto encoded = QueryReferenceList::serialize(keys);
    std::vector<std::string> decoded;
    expect(QueryReferenceList::decode(encoded, decoded) && decoded == keys, "packed query references roundtrip");
    auto truncated = encoded; truncated.pop_back();
    expect(!QueryReferenceList::decode(truncated, decoded) && decoded.empty(), "truncated query list publishes no partial keys");
    auto trailing = encoded; trailing.push_back(0);
    expect(!QueryReferenceList::decode(trailing, decoded), "query list rejects trailing partial length");
    expect(!QueryReferenceList::decode({}, decoded), "an empty reference list is omitted rather than stored");
    auto duplicate = QueryReferenceList::serialize({keys.front()});
    const auto single = duplicate;
    duplicate.insert(duplicate.end(), single.begin(), single.end());
    expect(!QueryReferenceList::decode(duplicate, decoded), "packed query references must be unique");
    test::expect_throws([&] { QueryReferenceList::serialize({keys.back(), keys.front()}); }, "unordered query references rejected");
    test::expect_throws([&] { QueryReferenceList::serialize({"foundation_cfg"}); }, "reverse index cannot name unrelated state records");
}

void restart(test::TestChain& fixture, const Scratch& scratch) {
    fixture.chain.reset();
    fixture.state.reset();
    fixture.blocks->close();
    expect(fixture.blocks->open((fixture.directory / "blocks").string()), "reopen block storage");
    fixture.state = std::make_shared<state::ChainState>(persistent(scratch));
    fixture.state->set_block_storage(fixture.blocks);
    fixture.chain = std::make_shared<core::Blockchain>(fixture.state, fixture.blocks);
}

void compact(test::TestChain& fixture) {
    auto* anchor = fixture.chain->get_tip()->prev;
    for (Height h = 0; h <= anchor->height; ++h) expect(fixture.blocks->prune_block_body_at_height(h), "prune old body");
    const auto header = anchor->get_block_header();
    expect(fixture.blocks->prune_block_indexes_below_height(anchor->height + 1,
        anchor->get_block_hash(), header, anchor->chain_work), "compact old navigation indexes");
}

void pruning_and_journal() {
    Scratch scratch;
    test::TestChain fixture(persistent(scratch));
    const auto first = fixture.make_block(); fixture.append(first);
    for (int i = 0; i < 2; ++i) fixture.append(fixture.make_block());
    const auto txid = first.transactions.front().get_hash();
    expect(!fixture.state->get_storage()->has_transaction(txid), "retained block has no duplicate raw transaction");
    expect(fixture.state->get_transaction(txid) != nullptr, "recent transaction is read from its block");
    compact(fixture);
    expect(fixture.state->get_storage()->has_transaction(txid), "transaction archived before deleting its block body");
    restart(fixture, scratch);
    expect(fixture.chain->initialize(), "compacted chain restarts");
    expect(fixture.state->get_transaction(txid) != nullptr, "historical transaction remains available after restart");
    const auto address = test::address_from_seed(42);
    size_t total = 0;
    const auto page = fixture.state->get_address_txids_page(address, 1, 1, &total);
    expect(total == 3 && page.size() == 1 && page.front().height == 2, "persisted address history survives pruning and restart");
    expect(fixture.state->get_balance(address) > 0, "persisted address UTXO index survives restart");
    const auto before = fixture.state->get_utxo_count();
    bytes journal(42, 0); journal[0] = 1; journal[1] = 1; journal[2] = 3;
    const auto tip = fixture.state->get_tip_hash();
    std::copy(tip.begin(), tip.end(), journal.begin() + 10);
    expect(fixture.state->get_storage()->put_metadata("chainstate_journal", journal), "seed uncertain transition");
    restart(fixture, scratch);
    expect(!fixture.chain->initialize(), "unprovable recovery on pruned history reports failure");
    expect(fixture.state->get_utxo_count() == before, "failed pruned recovery preserves UTXOs");
    bytes retained;
    expect(fixture.state->get_storage()->get_metadata("chainstate_journal", retained) && retained == journal, "failed recovery retains journal");
}

void witness_retry_progress() {
    test::TestChain fixture;
    fixture.state->set_prune_depth(1);
    const auto first = fixture.make_block();
    fixture.append(first);
    expect(fixture.blocks->delete_block(first.get_hash()), "temporarily remove first witness source");
    fixture.append(fixture.make_block());
    Height progress = 99;
    expect(fixture.state->get_witness_pruned_height(progress) && progress == 0,
           "failed witness maintenance preserves its cursor");
    expect(fixture.blocks->save_block(first, 1, false), "restore first witness source");
    fixture.append(fixture.make_block());
    storage::BlockStorage::PruningStats stats;
    expect(fixture.state->get_pruning_stats(stats) && stats.total_blocks == 2,
           "automatic pruning retries the missed height before advancing");
    expect(fixture.state->get_witness_pruned_height(progress) && progress == 2,
           "automatic witness progress describes a completed prefix");
    fixture.state->set_archive_mode(true);
    for (int i = 0; i < 10; ++i) fixture.append(fixture.make_block());
    fixture.state->set_archive_mode(false);
    fixture.append(fixture.make_block());
    expect(fixture.state->get_witness_pruned_height(progress) && progress == 10,
           "automatic catch-up processes at most eight pending heights per block");
    fixture.append(fixture.make_block());
    expect(fixture.state->get_witness_pruned_height(progress) && progress == 14,
           "later maintenance drains the remaining backlog");
}

void witness_after_body_pruning() {
    test::TestChain fixture;
    for (int i = 0; i < 6; ++i) fixture.append(fixture.make_block());
    fixture.state->get_storage()->begin_batch();
    expect(!fixture.state->prune_witness_up_to(6), "unfinished state batch defers witness maintenance");
    fixture.state->get_storage()->abort_batch();
    expect(fixture.chain->prune_history(3, false), "archive old bodies while retaining indexes");
    expect(fixture.state->prune_witness_up_to(6),
           "witness maintenance advances past intentionally deleted bodies");
    Height progress = 0;
    expect(fixture.state->get_witness_pruned_height(progress) && progress == 6,
           "witness progress reaches retained bodies");
    storage::BlockStorage::PruningStats stats, again;
    expect(fixture.state->get_pruning_stats(stats) && fixture.state->prune_witness_up_to(6) &&
           fixture.state->get_pruning_stats(again) && again.total_blocks == stats.total_blocks,
           "completed witness range is not counted again");
}

void witness_cursor_bounds(bool wrapped) {
    test::TestChain fixture;
    for (int i = 0; i < 3; ++i) fixture.append(fixture.make_block());
    const Height maximum = (std::numeric_limits<Height>::max)();
    if (wrapped) expect(fixture.blocks->set_witness_pruned_height(maximum), "seed maximum completed cursor");
    expect(fixture.state->prune_witness_up_to(maximum, 10), "bounded maintenance request completes");
    Height progress = 0;
    expect(fixture.state->get_witness_pruned_height(progress) && progress == (wrapped ? maximum : 3),
           wrapped ? "maximum completed cursor cannot wrap to low heights"
                   : "witness progress cannot run ahead of the current chain");
}

void deep_undo_restart() {
    Scratch scratch;
    test::TestChain fixture(persistent(scratch));
    fixture.chain->set_policy_checkpoint_options(0, 0, true);
    const auto first = fixture.make_block(); fixture.append(first);
    for (int i = 1; i < 145; ++i) fixture.append(fixture.make_block());
    const Amount fee = consensus::params::MIN_TX_FEE;
    auto last = fixture.make_block(nullptr, fee);
    core::Transaction payment;
    payment.version = 2;
    payment.timestamp = last.header.timestamp;
    core::TxInput input;
    input.prev_tx_hash = first.transactions.front().get_hash();
    input.vout = 0;
    payment.inputs.push_back(input);
    payment.outputs.push_back({COIN, test::address_from_seed(43), {}});
    payment.outputs.push_back({first.transactions.front().outputs.front().value - COIN - fee,
                               test::address_from_seed(42), {}});
    payment.sign(test::make_key(42));
    last.transactions.push_back(payment);
    test::TestChain::seal(last, fixture.chain->get_tip());
    fixture.append(last);
    const auto txid = payment.get_hash();
    const auto serialized_payment = payment.serialize();
    bytes cached_witness, disk_witness;
    expect(fixture.state->get_witness_payload(last.get_hash(), cached_witness) &&
           fixture.blocks->build_witness_payload_from_block(last.get_hash(), disk_witness) && cached_witness == disk_witness,
           "state cache and disk encode the same canonical witness payload");
    expect(fixture.blocks->prune_witness_at_height(146), "relocate witness atomically");
    core::Block stripped;
    expect(fixture.blocks->get_block(last.get_hash(), stripped, false) &&
           std::all_of(stripped.transactions[1].witnesses.begin(), stripped.transactions[1].witnesses.end(),
                       [](const core::TxWitness& witness) { return witness.signature.empty() && witness.public_key.empty(); }),
           "physical block no longer contains witness");
    expect(!fixture.state->get_storage()->has_transaction(txid), "witness relocation creates no raw transaction duplicate");
    auto loaded = fixture.state->get_transaction(txid);
    expect(loaded && loaded->serialize() == serialized_payment, "transaction read reassembles full witness");
    const auto key = "undo:" + utils::hash256_to_hex(first.get_hash());
    expect(fixture.state->get_storage()->has_metadata(key), "deep undo exists before restart");
    CountingRocksStorage::transaction_reads = 0;
    const auto started = std::chrono::steady_clock::now();
    restart(fixture, scratch);
    fixture.chain->set_policy_checkpoint_options(0, 0, true);
    expect(fixture.chain->initialize(), "deep reorg chain restarts");
    expect(CountingRocksStorage::transaction_reads == 0, "startup leaves transaction history on disk");
    std::cout << "DB_STARTUP blocks=147 history_transaction_reads=0 elapsed_ms="
              << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count() << '\n';
    expect(fixture.state->get_storage()->has_metadata(key), "opening does not delete undo before policy");
    expect(!fixture.state->get_storage()->has_transaction(txid), "restart does not recreate raw transaction duplicate");
    loaded = fixture.state->get_transaction(txid);
    expect(loaded && loaded->serialize() == serialized_payment, "full witness remains readable after restart");
    expect(fixture.state->get_storage()->put_transaction(txid, stripped.transactions[1]), "seed incomplete archive entry");
    expect(!fixture.blocks->prune_block_body_at_height(146) && fixture.blocks->has_block(last.get_hash()),
           "empty witness placeholders cannot satisfy archive durability check");
    expect(fixture.state->get_storage()->delete_transaction(txid), "remove incomplete test archive");
    expect(fixture.blocks->prune_block_body_at_height(146), "archive reconstructable transactions before body removal");
    loaded = fixture.state->get_transaction(txid);
    expect(loaded && loaded->serialize() == serialized_payment, "archive preserves full witness after body removal");
}

void reject_old_keys_without_rewriting() {
    Scratch scratch;
    state::UTXO output;
    output.tx_hash.fill(0x42); output.address = test::address_from_seed(42); output.value = COIN;
    const auto prefix = "utxo:" + utils::hash256_to_hex(output.tx_hash) + ":";
    output.vout = 16; const auto a = output.serialize();
    output.vout = 22; const auto b = output.serialize();
    {
        auto db = persistent(scratch);
        expect(db->put_metadata("idx_epoch", bytes(32, 'a')), "seed epoch");
        expect(db->put_metadata(prefix + "10", a) && db->put_metadata(prefix + "16", b), "seed conflicting outpoint names");
    }
    bool rejected = false;
    try { state::ChainState state(persistent(scratch)); } catch (const std::runtime_error&) { rejected = true; }
    expect(rejected, "noncanonical outpoint rejected");
    auto db = persistent(scratch); bytes value;
    expect(db->get_metadata(prefix + "10", value) && value == a, "first source unchanged");
    expect(db->get_metadata(prefix + "16", value) && value == b, "overlapping source unchanged");
    expect(!db->has_metadata(prefix + "22"), "loader never converts keys");
}

void format_and_index_integrity() {
    Scratch scratch;
    {
        auto db = persistent(scratch);
        expect(db->put_metadata("probe:identity", {42}), "seed format identity probe");
    }
    storage::StorageOptions wrong;
    wrong.network = "another-network";
    state::RocksDBStorage rejected((scratch.path / "chain").string(), (scratch.path / "addresses").string(), wrong);
    expect(!rejected.open(), "another network cannot open the same database");
    {
        auto db = persistent(scratch); bytes value;
        expect(db->get_metadata("probe:identity", value) && value == bytes{42}, "format rejection preserves original data");
    }
    Scratch state_dir;
    test::TestChain fixture(persistent(state_dir));
    fixture.append(fixture.make_block());
    const auto keys = fixture.state->get_storage()->get_keys_with_prefix("addr:utxo:");
    expect(!keys.empty(), "address secondary indexes exist");
    expect(fixture.state->get_storage()->delete_metadata(keys.front()), "inject missing address index");
    restart(fixture, state_dir);
    expect(!fixture.state->has_derived_state_snapshot(), "missing address index invalidates snapshot");
    expect(fixture.chain->initialize(), "complete history rebuilds missing address index");
    expect(fixture.state->get_storage()->has_metadata(keys.front()), "rebuild restores address index");
    storage::StorageOptions options;
    options.cache_mb = 8; options.write_buffer_mb = 4;
    storage::backend::configure_shared_resources(options);
    const auto shared = options;
    expect(shared.shared_cache == options.shared_cache && shared.shared_write_buffer == options.shared_write_buffer,
           "storage option copies share one cache and memtable budget");
}

void archive_before_pruning() {
    struct ArchiveFailure : state::MemoryStorage {
        bool fail = false;
        bool put_transaction(const Hash256& hash, const core::Transaction& tx) override {
            return !fail && MemoryStorage::put_transaction(hash, tx);
        }
    };
    auto backend = std::make_unique<ArchiveFailure>();
    auto* storage = backend.get();
    test::TestChain fixture(std::move(backend));
    const auto block = fixture.make_block(); fixture.append(block);
    storage->fail = true;
    expect(!fixture.blocks->prune_block_body_at_height(1), "failed transaction archive prevents body deletion");
    expect(fixture.blocks->has_block(block.get_hash()), "source block remains after archive failure");
    storage->fail = false;
    expect(fixture.blocks->prune_block_body_at_height(1), "archive and pruning can be retried");
    expect(!fixture.blocks->has_block(block.get_hash()) && fixture.state->get_transaction(block.transactions[0].get_hash()),
           "archived transaction remains accessible after body deletion");
}

void concurrent_pruning_and_writes() {
    test::TestChain fixture;
    std::exception_ptr maintenance_error;
    std::jthread maintainer([&](std::stop_token stop) {
        try {
            while (!stop.stop_requested()) {
                expect(fixture.chain->prune_history(3, false), "concurrent history maintenance succeeds");
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        } catch (...) { maintenance_error = std::current_exception(); }
    });
    Hash256 first_tx{};
    for (int i = 0; i < 12; ++i) {
        const auto block = fixture.make_block();
        if (i == 0) first_tx = block.transactions.front().get_hash();
        fixture.append(block);
    }
    maintainer.request_stop(); maintainer.join();
    if (maintenance_error) std::rethrow_exception(maintenance_error);
    expect(fixture.chain->prune_history(3, false), "final history maintenance succeeds");
    expect(fixture.state->get_height() == 12 && fixture.state->has_derived_state_snapshot(), "concurrent pruning keeps valid state");
    expect(fixture.state->get_transaction(first_tx) != nullptr, "concurrent pruning preserves archived transactions");
}

core::Block alternate_block(const test::TestChain& fixture, const chain::BlockIndex* previous, uint32_t offset = 1) {
    auto block = fixture.make_block(previous);
    block.header.timestamp += offset;
    block.transactions.front().timestamp = block.header.timestamp;
    test::TestChain::seal(block, previous);
    return block;
}

void pruned_fork_restart(bool long_fork) {
    Scratch scratch;
    test::TestChain fixture(persistent(scratch));
    const auto* genesis = fixture.chain->get_tip();
    std::vector<core::Block> active;
    for (int i = 0; i < 6; ++i) {
        active.push_back(fixture.make_block());
        fixture.append(active.back());
    }
    std::vector<core::Block> discarded;
    const auto* parent = genesis;
    for (int i = 0; i < (long_fork ? 7 : 2); ++i) {
        discarded.push_back(alternate_block(fixture, parent));
        parent = fixture.accept(discarded.back());
    }
    expect(fixture.blocks->set_witness_payload(discarded.back().get_hash(),
           storage::serialize_witness_payload(discarded.back())), "seed retained fork witness payload");
    if (long_fork) {
        for (int i = 0; i < 3; ++i) expect(fixture.blocks->delete_block(discarded[i].get_hash()),
                                          "old fork ancestors already lack bodies");
    }
    const auto* anchor = fixture.chain->find_block_index(active[2].get_hash());
    const auto retained = alternate_block(fixture, anchor, 2);
    fixture.accept(retained);
    const auto first_tx = active.front().transactions.front().get_hash();
    uint64_t body_count = 0, index_count = 0;
    expect(fixture.chain->prune_history(3, true, &body_count, &index_count), "prune history containing alternate branches");
    restart(fixture, scratch);
    expect(fixture.chain->initialize(), "pruned node restarts with a connected retained block graph");
    expect(body_count == (long_fork ? 8u : 6u) && index_count == (long_fork ? 10u : 5u),
           "pruning counters include discarded fork bodies and all removed indexes");
    expect(fixture.chain->get_tip()->get_block_hash() == active.back().get_hash(), "restart preserves active tip");
    expect(fixture.state->get_transaction(first_tx) != nullptr, "active transaction archive survives fork cleanup");
    for (const auto& block : discarded) {
        expect(!fixture.blocks->has_block(block.get_hash()) && !fixture.chain->find_block_index(block.get_hash()),
               "discarded branch is absent from both body storage and loaded index");
    }
    bytes payload;
    expect(!fixture.blocks->get_witness_payload(discarded.back().get_hash(), payload), "discarded witness payload is removed");
    expect(fixture.blocks->has_block(retained.get_hash()) && fixture.chain->find_block_index(retained.get_hash()),
           "alternate branch descending from the retained anchor remains available");
    core::Block best;
    expect(fixture.blocks->get_best_block(best) && best.get_hash() == active.back().get_hash(),
           "best-body metadata does not point to the discarded higher branch");
    fixture.append(fixture.make_block());
    expect(fixture.chain->prune_history(3, true), "advance the pruning anchor");
    restart(fixture, scratch);
    expect(fixture.chain->initialize() && fixture.chain->get_height() == 7, "chain restarts after a second pruning cycle");
    expect(!fixture.blocks->has_block(retained.get_hash()) && !fixture.chain->find_block_index(retained.get_hash()),
           "formerly retained branch is removed once it diverges below the new anchor");
    expect(fixture.state->get_transaction(first_tx) != nullptr, "archived active history remains accessible");
}

void pruned_late_branch() {
    test::TestChain fixture;
    const auto* genesis = fixture.chain->get_tip();
    for (int i = 0; i < 6; ++i) fixture.append(fixture.make_block());
    const auto old_fork = alternate_block(fixture, genesis);
    auto* old_index = fixture.accept(old_fork);
    const auto late = alternate_block(fixture, old_index);
    expect(fixture.chain->prune_history(3, true), "prune before late fork delivery");
    consensus::ValidationState state;
    expect(!fixture.chain->accept_block(late, state, nullptr), "late block cannot recreate an already discarded fork");
    expect(state.get_reject_code() == consensus::RejectCode::CHECKPOINT && !old_index->is_failed(),
           "pruning boundary is local policy rather than a consensus-invalid ancestor");
    expect(!fixture.blocks->has_block(late.get_hash()) && !old_index->has_data(),
           "disk and in-memory body availability agree after pruning");
    consensus::ValidationState duplicate;
    expect(!fixture.chain->accept_block(old_fork, duplicate, nullptr), "known discarded body cannot bypass the pruning boundary");
    const auto* anchor = fixture.chain->get_tip()->get_ancestor(3);
    const auto allowed = alternate_block(fixture, anchor, 3);
    auto* allowed_tip = fixture.accept(allowed);
    expect(fixture.blocks->has_block(allowed.get_hash()), "new branch within retained history is still accepted");
    for (int i = 0; i < 3; ++i) allowed_tip = fixture.accept(alternate_block(fixture, allowed_tip));
    consensus::ValidationState transition;
    expect(fixture.chain->switch_to_tip(allowed_tip->get_block_hash(), transition) &&
           fixture.state->get_tip_hash() == allowed_tip->get_block_hash(),
           "reorganization within retained history still applies successfully");
    expect(fixture.chain->prune_history(3, true), "pruning continues after a retained-branch reorganization");
}
}

int main(int argc, char** argv) {
    try {
        consensus::SelectParams(consensus::Network::LOADNET);
        if (argc == 4 && std::string(argv[1]) == "--alter-snapshot-fixture") {
            test::alter_snapshot(argv[2], argv[3]);
            return 0;
        }
        if (argc == 2) {
            const std::string mode = argv[1];
            if (mode == "--pruned-fork-restart") { pruned_fork_restart(false); return 0; }
            if (mode == "--pruned-long-fork") { pruned_fork_restart(true); return 0; }
            if (mode == "--pruned-late-branch") { pruned_late_branch(); return 0; }
            if (mode == "--witness-retry") { witness_retry_progress(); return 0; }
            if (mode == "--witness-after-bodies") { witness_after_body_pruning(); return 0; }
            if (mode == "--witness-future-cursor") { witness_cursor_bounds(false); return 0; }
            if (mode == "--witness-wrapped-cursor") { witness_cursor_bounds(true); return 0; }
        }
        if (argc == 3 && std::string(argv[1]) == "--check-snapshot-fixture") {
            const std::filesystem::path root(argv[2]);
            auto db = std::make_unique<state::RocksDBStorage>((root / "chaindata/state").string(), (root / "addresses/state").string());
            expect(db->open(), "open restored state");
            auto restored_state = std::make_shared<state::ChainState>(std::move(db));
            auto blocks = std::make_shared<storage::BlockStorage>(root.string());
            restored_state->set_block_storage(blocks);
            core::Blockchain chain(restored_state, blocks);
            expect(chain.initialize() && restored_state->get_height() == 3, "restored chain initializes at original height");
            expect(restored_state->get_address_txids(test::address_from_seed(42)).size() == 3, "restored address history");
            return 0;
        }
        if (argc == 3 && (std::string(argv[1]) == "--snapshot-fixture" ||
                          std::string(argv[1]) == "--snapshot-witness-fixture")) {
            const std::filesystem::path root(argv[2]);
            auto db = std::make_unique<state::RocksDBStorage>((root / "chaindata/state").string(), (root / "addresses/state").string());
            expect(db->open(), "open snapshot fixture state");
            test::TestChain fixture(std::move(db));
            for (int i = 0; i < 3; ++i) {
                auto block = fixture.make_block();
                if (std::string(argv[1]) == "--snapshot-witness-fixture") {
                    block.transactions.front().witnesses.resize(1);
                    block.transactions.front().witnesses.front().signature = {1, 2, 3};
                    test::TestChain::seal(block, fixture.chain->get_tip());
                }
                fixture.append(block);
            }
            fixture.chain.reset(); fixture.state.reset(); fixture.blocks->close();
            for (const auto* name : {"chaindata/blocks", "index/blocks"}) {
                const auto source = fixture.directory / "blocks" / name;
                const auto target = root / name;
                std::filesystem::create_directories(target.parent_path());
                std::filesystem::copy(source, target, std::filesystem::copy_options::recursive);
                auto source_marker = source; source_marker.replace_extension(".backend");
                auto target_marker = target; target_marker.replace_extension(".backend");
                std::filesystem::copy_file(source_marker, target_marker);
            }
            return 0;
        }
        state::MemoryStorage memory; batch_contract(memory);
        Scratch one, two;
        auto single = persistent(one, false); batch_contract(*single);
        auto split = persistent(two); batch_contract(*split);
        read_failure_contract();
        active_record_contract();
        query_reference_contract();
        reject_old_keys_without_rewriting();
        format_and_index_integrity();
        archive_before_pruning();
        pruned_fork_restart(false);
        pruned_fork_restart(true);
        pruned_late_branch();
        witness_retry_progress();
        witness_after_body_pruning();
        witness_cursor_bounds(false);
        witness_cursor_bounds(true);
        concurrent_pruning_and_writes();
        pruning_and_journal();
        deep_undo_restart();
        std::cout << "Database contract regressions passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
