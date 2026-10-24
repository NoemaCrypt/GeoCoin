#include "chain_test_fixture.hpp"
#include "geocoin/utils/bytes.hpp"

#include <chrono>
#include <iostream>
#include <set>

namespace {
using namespace geocoin;
using test::expect;
using Clock = std::chrono::steady_clock;

struct Scratch {
    std::filesystem::path path = test::make_temp_dir("geocoin-state-hot-paths");
    ~Scratch() { std::error_code error; std::filesystem::remove_all(path, error); }
};

struct ReadCounts {
    std::set<Hash256> sources;
    size_t source_reads = 0;
};

template<class Backend>
struct CountingStorage : Backend {
    template<class... Args>
    explicit CountingStorage(ReadCounts& counts, Args&&... args)
        : Backend(std::forward<Args>(args)...), counts(counts) {}
    bool get_transaction(const Hash256& hash, core::Transaction& tx) const override {
        if (counts.sources.contains(hash)) ++counts.source_reads;
        return Backend::get_transaction(hash, tx);
    }
    ReadCounts& counts;
};

std::unique_ptr<state::Storage> open_storage(const Scratch& scratch, int mode, ReadCounts& counts) {
    if (mode == 0) return std::make_unique<CountingStorage<state::MemoryStorage>>(counts);
    auto db = mode == 1
        ? std::make_unique<CountingStorage<state::RocksDBStorage>>(counts, (scratch.path / "state").string())
        : std::make_unique<CountingStorage<state::RocksDBStorage>>(
              counts, (scratch.path / "state").string(), (scratch.path / "addresses").string());
    expect(db->open(), "open hot-path test database");
    return db;
}

core::Transaction spend_all(const core::Transaction& source, uint8_t owner, uint8_t recipient,
                            Timestamp timestamp, size_t output_count = 1,
                            Amount fee = consensus::params::MIN_TX_FEE) {
    core::Transaction tx;
    tx.version = 2;
    tx.timestamp = timestamp;
    Amount value = 0;
    // Funding coinbases also have a foundation output, which belongs to another owner.
    for (size_t i = 0; i < source.outputs.size(); ++i) {
        if (source.outputs[i].address != test::address_from_seed(owner)) continue;
        core::TxInput input;
        input.prev_tx_hash = source.get_hash();
        input.vout = static_cast<uint32_t>(i);
        tx.inputs.push_back(input);
        value += source.outputs[i].value;
    }
    value -= fee;
    const Amount part = value / static_cast<Amount>(output_count);
    for (size_t i = 0; i < output_count; ++i) {
        tx.outputs.push_back({i + 1 == output_count ? value - part * static_cast<Amount>(i) : part,
                              test::address_from_seed(recipient), {}});
    }
    tx.sign(test::make_key(owner));
    expect(tx.verify(), "test payment has a valid signature");
    return tx;
}

void expect_history(const state::ChainState& state, uint8_t owner,
                    const std::vector<std::pair<Hash256, Height>>& expected) {
    size_t total = 0;
    const auto refs = state.get_address_txids_page(test::address_from_seed(owner), 0, 1000, &total);
    expect(total == expected.size() && refs.size() == expected.size(), "address history count and deduplication");
    std::set<std::pair<Hash256, Height>> actual;
    for (const auto& ref : refs) actual.emplace(ref.txid, ref.height);
    expect(actual == std::set<std::pair<Hash256, Height>>(expected.begin(), expected.end()),
           "address history preserves both incoming and outgoing transactions");
}

void chain_history(int mode, bool benchmark) {
    Scratch scratch;
    ReadCounts counts;
    test::TestChain fixture(open_storage(scratch, mode, counts));
    const auto first = fixture.make_block();
    fixture.append(first);
    for (int i = 1; i < 100; ++i) fixture.append(fixture.make_block());

    const Amount fee = consensus::params::MIN_TX_FEE;
    auto funding_block = fixture.make_block(nullptr, fee);
    const auto funding = spend_all(first.transactions[0], 42, 43, funding_block.header.timestamp, 16);
    funding_block.transactions.push_back(funding);
    test::TestChain::seal(funding_block, fixture.chain->get_tip());
    fixture.append(funding_block);
    const auto funding_hash = funding.get_hash();
    expect_history(*fixture.state, 43, {{funding_hash, 101}});

    auto block = fixture.make_block(nullptr, fee);
    const auto payment = spend_all(funding, 43, 44, block.header.timestamp);
    // A zero-fee child keeps this benchmark valid on the original implementation.
    // Positive fees on same-block spends are checked separately below.
    const auto chained = spend_all(payment, 44, 45, block.header.timestamp, 1, 0);
    block.transactions.push_back(payment);
    block.transactions.push_back(chained);
    test::TestChain::seal(block, fixture.chain->get_tip());
    const auto payment_hash = payment.get_hash();
    const auto chained_hash = chained.get_hash();
    counts.sources = {funding_hash, payment_hash};
    counts.source_reads = 0;
    const auto started = Clock::now();
    fixture.append(block);
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - started).count();
    std::cout << "STATE_INDEX backend=" << mode << " inputs=" << payment.inputs.size() + chained.inputs.size()
              << " source_transaction_reads=" << counts.source_reads << " block_apply_us=" << elapsed << '\n';
    if (!benchmark) expect(counts.source_reads == 0, "indexing resolved UTXOs does not reread source transactions");
    const auto check_history = [&] {
        expect_history(*fixture.state, 43, {{funding_hash, 101}, {payment_hash, 102}});
        expect_history(*fixture.state, 44, {{payment_hash, 102}, {chained_hash, 102}});
        expect_history(*fixture.state, 45, {{chained_hash, 102}});
    };
    check_history();
    expect(fixture.state->get_balance(test::address_from_seed(43)) == 0 &&
           fixture.state->get_balance(test::address_from_seed(44)) == 0,
           "senders without change retain outgoing history");

    auto* parent = fixture.chain->get_tip()->prev;
    auto fork = fixture.make_block(parent);
    ++fork.header.timestamp;
    test::TestChain::seal(fork, parent);
    fixture.accept(fork);
    consensus::ValidationState validation;
    expect(fixture.chain->switch_to_tip(fork.get_hash(), validation), "activate competing tip");
    expect_history(*fixture.state, 43, {{funding_hash, 101}});
    expect_history(*fixture.state, 44, {});
    expect_history(*fixture.state, 45, {});
    expect(fixture.chain->switch_to_tip(block.get_hash(), validation), "restore spending tip");
    check_history();

    // Keep the active tip body, as the node's pruning policy does.
    fixture.append(fixture.make_block());
    expect(fixture.blocks->prune_block_body_at_height(101) && fixture.blocks->prune_block_body_at_height(102),
           "archive source transactions and prune their block bodies");
    if (mode != 0) {
        fixture.chain.reset();
        fixture.state.reset();
        fixture.state = std::make_shared<state::ChainState>(open_storage(scratch, mode, counts));
        fixture.state->set_block_storage(fixture.blocks);
        fixture.chain = std::make_shared<core::Blockchain>(fixture.state, fixture.blocks);
        expect(fixture.chain->initialize(), "restart with pruned source blocks");
        check_history();
    }
    auto next = fixture.make_block(nullptr, fee);
    const auto archived_spend = spend_all(chained, 45, 46, next.header.timestamp);
    next.transactions.push_back(archived_spend);
    test::TestChain::seal(next, fixture.chain->get_tip());
    counts.sources = {chained_hash};
    counts.source_reads = 0;
    fixture.append(next);
    if (!benchmark) expect(counts.source_reads == 0, "indexing needs no archived transaction read after pruning");
    expect_history(*fixture.state, 45, {{chained_hash, 102}, {archived_spend.get_hash(), 104}});
    expect_history(*fixture.state, 46, {{archived_spend.get_hash(), 104}});

    if (!benchmark) {
        auto fee_block = fixture.make_block(nullptr, 2 * fee);
        const auto parent_payment = spend_all(archived_spend, 46, 47, fee_block.header.timestamp);
        const auto child_payment = spend_all(parent_payment, 47, 48, fee_block.header.timestamp);
        fee_block.transactions.push_back(parent_payment);
        fee_block.transactions.push_back(child_payment);
        test::TestChain::seal(fee_block, fixture.chain->get_tip());

        auto omitted_child_fee = fee_block;
        omitted_child_fee.transactions[0] = fixture.make_block(nullptr, fee).transactions[0];
        test::TestChain::seal(omitted_child_fee, fixture.chain->get_tip());
        consensus::ValidationState rejected;
        expect(!fixture.chain->process_new_block(omitted_child_fee, rejected) &&
               rejected.get_reject_reason() == "bad-cb-foundation-amount" && fixture.state->get_height() == 104,
               "coinbase cannot omit the foundation share of a same-block spend fee");
        fixture.append(fee_block);
        expect_history(*fixture.state, 47, {{parent_payment.get_hash(), 105}, {child_payment.get_hash(), 105}});
        expect_history(*fixture.state, 48, {{child_payment.get_hash(), 105}});
    }
}

void undo_batch_contract(int mode) {
    Scratch scratch;
    ReadCounts counts;
    state::ChainState state(open_storage(scratch, mode, counts));
    state::BlockUndo original{};
    original.height = 5;
    original.block_hash = test::hash_from_seed(77);
    original.tx_undos.resize(1);
    original.tx_undos[0].angle_store_preexisted = true;
    expect(state.save_block_undo(original), "save initial undo");
    state::BlockUndo loaded{};
    expect(state.load_block_undo(original.block_hash, loaded) && loaded.height == 5 &&
           !loaded.tx_undos[0].angle_store_preexisted && original.tx_undos[0].angle_store_preexisted,
           "durable undo clears runtime ownership without mutating the caller");
    state.get_storage()->begin_batch();
    auto pending = original;
    pending.height = 6;
    expect(state.save_block_undo(pending), "stage undo replacement");
    expect(state.load_block_undo(original.block_hash, loaded) && loaded.height == 6, "read pending undo replacement");
    state.get_storage()->abort_batch();
    expect(state.load_block_undo(original.block_hash, loaded) && loaded.height == 5,
           "aborting a batch cannot leave stale undo in a separate cache");
    state.get_storage()->begin_batch();
    expect(state.get_storage()->delete_metadata("undo:" + utils::hash256_to_hex(original.block_hash)), "stage undo deletion");
    expect(!state.load_block_undo(original.block_hash, loaded), "pending undo deletion is visible");
    state.get_storage()->abort_batch();
    expect(state.load_block_undo(original.block_hash, loaded) && loaded.height == 5, "aborted deletion restores undo");
}

void undo_ownership_and_failure() {
    state::BlockUndo undo{};
    undo.height = 5;
    undo.block_hash = test::hash_from_seed(79);
    undo.tx_undos.resize(1);
    undo.tx_undos[0].angle_store_preexisted = true;
    state::ChainState transient;
    state::BlockUndo loaded{};
    expect(transient.save_block_undo(undo) && transient.load_block_undo(undo.block_hash, loaded) &&
           !loaded.tx_undos[0].angle_store_preexisted && undo.tx_undos[0].angle_store_preexisted,
           "undo without Storage preserves the runtime ownership contract");

    struct RejectUndoStorage : state::MemoryStorage {
        bool reject = false;
        bool put_metadata(const std::string& key, const bytes& value) override {
            return !(reject && key.starts_with("undo:")) && MemoryStorage::put_metadata(key, value);
        }
    };
    auto backend = std::make_unique<RejectUndoStorage>();
    auto* writes = backend.get();
    state::ChainState state(std::move(backend));
    expect(state.save_block_undo(undo), "seed undo before a write failure");
    writes->reject = true;
    undo.height = 6;
    expect(!state.save_block_undo(undo) && state.load_block_undo(undo.block_hash, loaded) && loaded.height == 5,
           "failed replacement does not publish uncommitted undo");
    undo.block_hash = test::hash_from_seed(80);
    expect(!state.save_block_undo(undo) && !state.load_block_undo(undo.block_hash, loaded),
           "failed insertion cannot become visible through a cache");
}

void key_and_undo_benchmark() {
    state::UTXO utxo;
    for (size_t i = 0; i < utxo.tx_hash.size(); ++i) utxo.tx_hash[i] = static_cast<byte>(i);
    utxo.address = test::address_from_seed(42);
    utxo.value = COIN;
    const std::string prefix = "utxo:000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f:";
    for (const auto index : {0U, 1U, 10U, UINT32_MAX}) {
        utxo.vout = index;
        expect(utxo.get_key() == prefix + std::to_string(index), "UTXO key bytes preserve decimal output indices");
    }
    constexpr size_t iterations = 100000;
    size_t checksum = 0;
    const auto started = Clock::now();
    for (size_t i = 0; i < iterations; ++i) {
        utxo.vout = static_cast<uint32_t>(i);
        const auto key = utxo.get_key();
        checksum += key.size() + static_cast<unsigned char>(key.back());
    }
    std::cout << "UTXO_KEYS iterations=" << iterations << " elapsed_us="
              << std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - started).count()
              << " checksum=" << checksum << '\n';

    state::BlockUndo undo{};
    undo.block_hash = utxo.tx_hash;
    undo.height = 5;
    undo.tx_undos.resize(16);
    for (auto& tx : undo.tx_undos) tx.spent_utxos.assign(16, utxo);
    state::ChainState state(std::make_unique<state::MemoryStorage>());
    const auto undo_started = Clock::now();
    for (int i = 0; i < 200; ++i) expect(state.save_block_undo(undo), "benchmark undo writes succeed");
    std::cout << "UNDO_SAVE iterations=200 utxos=256 bytes=" << undo.serialize().size() << " elapsed_us="
              << std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - undo_started).count() << '\n';
}
}

int main(int argc, char** argv) {
    try {
        const bool benchmark = argc == 2 && std::string(argv[1]) == "--benchmark";
        const bool undo_only = argc == 2 && std::string(argv[1]) == "--undo-contract";
        expect(argc == 1 || benchmark || undo_only, "usage: test_state_hot_paths [--benchmark|--undo-contract]");
        consensus::SelectParams(consensus::Network::LOADNET);
        for (int mode = 0; mode < 3; ++mode) {
            if (!undo_only) chain_history(mode, benchmark);
            if (!benchmark) undo_batch_contract(mode);
        }
        if (!benchmark) undo_ownership_and_failure();
        if (!undo_only) key_and_undo_benchmark();
        std::cout << "State hot-path checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
