#include "database_snapshot_fixture.hpp"
#include <chrono>
#include <functional>
#include <iostream>

using namespace geocoin;
using test::expect;

namespace {
struct Scratch {
    std::filesystem::path path = test::make_temp_dir("geocoin-transaction-lookup");
    ~Scratch() { std::error_code error; std::filesystem::remove_all(path, error); }
};

std::unique_ptr<state::Storage> backend(int mode, const Scratch& scratch) {
    if (mode == 0) return std::make_unique<state::MemoryStorage>();
    auto db = mode == 1 ? std::make_unique<state::RocksDBStorage>((scratch.path / "state").string())
                        : std::make_unique<state::RocksDBStorage>((scratch.path / "state").string(), (scratch.path / "addresses").string());
    expect(db->open(), "open lookup database");
    return db;
}

core::Block stored_block(uint8_t seed) {
    core::Block block;
    block.header.version = 1;
    block.header.timestamp = seed;
    auto tx = core::Transaction::create_coinbase(seed, test::address_from_seed(seed), COIN);
    tx.timestamp = seed;
    block.transactions.push_back(tx);
    block.calculate_merkle_root();
    block.header.witness_commitment = block.calculate_witness_commitment();
    return block;
}

template<class Operation>
void expect_corrupt(Operation&& operation, const char* message) {
    bool corrupt = false;
    try { operation(); }
    catch (const storage::ReadError& error) { corrupt = error.status() == storage::ReadStatus::CORRUPT; }
    expect(corrupt, message);
}

void wrong_write() {
    const auto original = stored_block(41).transactions.front();
    const auto replacement = stored_block(42).transactions.front();
    for (int mode = 0; mode < 3; ++mode) {
        Scratch scratch;
        auto db = backend(mode, scratch);
        const auto hash = original.get_hash();
        expect(db->put_transaction(hash, original), "seed correct typed record");
        for (const bool batch : {false, true}) {
            if (batch) db->begin_batch();
            expect(!db->put_transaction(hash, replacement), "typed write rejects a key that differs from the object's hash");
            core::Transaction loaded;
            expect(db->get_transaction(hash, loaded) && loaded.serialize() == original.serialize(), "rejected write preserves transaction");
            if (batch) expect(db->put_metadata("probe", {1}) && db->commit_batch(), "rejected write preserves other batch work");
        }
    }
}

void wrong_read() {
    const auto original = stored_block(41).transactions.front();
    const auto replacement = stored_block(42).transactions.front();
    const auto sentinel = stored_block(43).transactions.front();
    for (int mode = 1; mode < 3; ++mode) {
        Scratch scratch;
        auto db = backend(mode, scratch);
        const auto hash = original.get_hash();
        expect(db->put_transaction(hash, original), "seed typed read record");
        db.reset();
        {
            test::SnapshotDatabase raw(scratch.path / "state");
            raw.put("tx:" + utils::hash256_to_hex(hash), test::snapshot_raw(replacement.serialize()));
        }
        db = backend(mode, scratch);
        auto out = sentinel;
        expect_corrupt([&] { db->get_transaction(hash, out); }, "wrong stored transaction identity is corruption");
        expect(out.serialize() == sentinel.serialize(), "failed read preserves output");
    }
}

void malformed_membership(const std::string& kind) {
    for (int mode = 0; mode < 3; ++mode) {
        Scratch scratch;
        test::TestChain fixture(backend(mode, scratch));
        const auto block = fixture.make_block();
        fixture.append(block);
        const auto original_hash = block.transactions.front().get_hash();
        auto txid = original_hash;
        storage::ActiveTransactionRecord record{1, block.get_hash(), 1};
        if (kind == "zero-binding") record.block_hash = {};
        else if (kind == "wrong-count") record.transaction_count = 2;
        else txid = stored_block(43).transactions.front().get_hash();
        const auto key = "atx:" + utils::hash256_to_hex(txid);
        const auto raw = record.serialize();
        expect(fixture.state->get_storage()->put_metadata(key, raw), "alter membership record");
        expect_corrupt([&] { fixture.state->get_transaction(txid); }, "inconsistent membership is not a normal lookup miss or success");
        bytes retained;
        expect(fixture.state->get_storage()->get_metadata(key, retained) && retained == raw, "failed read preserves diagnostic record");
        if (txid != original_hash) expect(fixture.state->get_storage()->delete_metadata(key), "remove artificial membership");
        else expect(fixture.state->get_storage()->put_metadata(key, storage::ActiveTransactionRecord{1, block.get_hash(), 1}.serialize()), "restore correct membership");
        expect(fixture.state->get_transaction(original_hash) != nullptr && !fixture.state->get_transaction(test::hash_from_seed(99)),
               "correct membership succeeds and unknown transaction remains absent");
    }
}

void missing_archive() {
    for (int mode = 0; mode < 3; ++mode) {
        Scratch scratch;
        test::TestChain fixture(backend(mode, scratch));
        const auto first = fixture.make_block(); fixture.append(first);
        fixture.append(fixture.make_block()); fixture.append(fixture.make_block());
        const auto txid = first.transactions.front().get_hash();
        expect(fixture.chain->prune_history(2, true), "prune old body and navigation indexes");
        expect(fixture.state->get_transaction(txid) != nullptr, "pruned history initially resolves archived transaction");
        expect(fixture.state->get_storage()->delete_transaction(txid), "remove required archived transaction");
        expect_corrupt([&] { fixture.state->get_transaction(txid); }, "missing archive for an active transaction is corruption");
        if (mode != 0) {
            fixture.chain.reset(); fixture.state.reset();
            fixture.state = std::make_shared<state::ChainState>(backend(mode, scratch));
            fixture.state->set_block_storage(fixture.blocks);
            expect_corrupt([&] { fixture.state->get_transaction(txid); }, "missing archive remains detectable after reopening RocksDB");
        }
        expect(fixture.state->get_storage()->put_transaction(txid, first.transactions.front()), "restore archived transaction");
        expect(fixture.state->get_transaction(txid) != nullptr, "repaired archive is readable without changing membership");
    }
}

void reorg_lookup() {
    for (int mode = 0; mode < 3; ++mode) {
        Scratch scratch;
        test::TestChain fixture(backend(mode, scratch));
        const auto original = fixture.make_block(); fixture.append(original);
        auto* genesis = fixture.chain->get_tip()->prev;
        auto alternate = fixture.make_block(genesis);
        alternate.transactions.front().outputs.front().address = test::address_from_seed(43);
        test::TestChain::seal(alternate, genesis);
        fixture.accept(alternate);
        expect(fixture.state->revert_block(original) && fixture.state->apply_block(alternate), "apply replacement state before publishing navigation index");
        Hash256 still_old{};
        expect(fixture.blocks->get_active_block_hash(1, still_old) && still_old == original.get_hash(), "navigation index still describes old branch during transition");
        const auto txid = alternate.transactions.front().get_hash();
        const auto found = fixture.state->get_transaction(txid);
        expect(found && found->get_hash() == txid, "transaction lookup follows the block bound to current state");
        expect(!fixture.state->get_transaction(original.transactions.front().get_hash()), "old branch membership was removed");
        expect(fixture.state->revert_block(alternate) && fixture.state->apply_block(original), "restore state before complete chain switch");
        consensus::ValidationState validation;
        expect(fixture.chain->switch_to_tip(alternate.get_hash(), validation), "complete real chain switch");
        expect(fixture.state->get_transaction(txid) != nullptr && !fixture.state->get_transaction(original.transactions.front().get_hash()),
               "completed reorg preserves active lookup and hides orphaned transaction");
        // A state without Storage can rebuild a read index but cannot apply
        // non-genesis blocks, which require durable undo records.
        state::ChainState reader;
        expect(reader.set_height(1) && reader.set_tip_hash(alternate.get_hash()), "set read-only chain position");
        reader.set_block_storage(fixture.blocks);
        expect(reader.get_transaction(txid) != nullptr && !reader.get_transaction(original.transactions.front().get_hash()),
               "rebuilt in-memory membership follows the completed branch switch");
    }
}

void benchmark() {
    for (int mode : {0, 2}) {
        Scratch scratch;
        test::TestChain fixture(backend(mode, scratch));
        const auto first = fixture.make_block(); fixture.append(first);
        fixture.append(fixture.make_block()); fixture.append(fixture.make_block());
        const auto txid = first.transactions.front().get_hash();
        for (const bool archived : {false, true}) {
            if (archived) expect(fixture.chain->prune_history(2, true), "prepare archive benchmark");
            const auto begin = std::chrono::steady_clock::now();
            size_t checksum = 0;
            for (size_t i = 0; i < 2000; ++i) {
                const auto tx = fixture.state->get_transaction(txid);
                expect(tx != nullptr, "benchmark transaction is readable");
                checksum += tx->outputs.size();
            }
            const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - begin).count();
            std::cout << "LOOKUP backend=" << mode << " archived=" << archived << " iterations=2000 us=" << elapsed << " checksum=" << checksum << '\n';
        }
    }
}
}

int main(int argc, char** argv) {
    try {
        consensus::SelectParams(consensus::Network::LOADNET);
        const std::map<std::string, std::function<void()>> cases{
            {"write-tx", wrong_write},
            {"read-tx", wrong_read},
            {"zero-binding", [] { malformed_membership("zero-binding"); }},
            {"wrong-count", [] { malformed_membership("wrong-count"); }},
            {"phantom", [] { malformed_membership("phantom"); }},
            {"missing-archive", missing_archive}, {"reorg", reorg_lookup}
        };
        if (argc == 2 && std::string(argv[1]) == "--benchmark") { benchmark(); return 0; }
        if (argc == 2) cases.at(argv[1])();
        else { expect(argc == 1, "optional mode selects a lookup scenario"); for (const auto& [name, run] : cases) run(); }
        std::cout << "Transaction lookup regressions passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
