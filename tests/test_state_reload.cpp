#include "chain_test_fixture.hpp"
#include "geocoin/multisig/multisig.hpp"
#include "geocoin/utils/bytes.hpp"
#include "geocoin/state/records.hpp"
#include <functional>
#include <iostream>
#include <map>

using namespace geocoin;
using test::expect;

namespace {
struct Scratch {
    std::filesystem::path path = test::make_temp_dir("geocoin-state-reload");
    ~Scratch() { std::error_code error; std::filesystem::remove_all(path, error); }
};

std::unique_ptr<state::Storage> backend(int mode, const std::filesystem::path& root) {
    if (mode == 0) return std::make_unique<state::MemoryStorage>();
    auto db = mode == 1 ? std::make_unique<state::RocksDBStorage>((root / "state").string())
                        : std::make_unique<state::RocksDBStorage>((root / "state").string(), (root / "addresses").string());
    expect(db->open(), "open reload database");
    return db;
}

state::UTXO seed(state::Storage& db, uint8_t id) {
    const std::string epoch = "0123456789abcdef0123456789abcdef";
    expect(db.put_metadata("idx_epoch", bytes(epoch.begin(), epoch.end())), "seed epoch");
    state::FoundationConfig foundation{consensus::Params().foundation_address(), consensus::Params().foundation_m(),
        consensus::Params().foundation_n(), consensus::Params().foundation_participants()};
    expect(db.put_metadata("foundation_cfg", state::records::serialize_foundation_config(foundation)) &&
           db.put_metadata("interest_pool_balance", utils::encode_u64_le(0)), "seed required monetary state");
    state::UTXO utxo;
    utxo.tx_hash = test::hash_from_seed(id);
    utxo.vout = 0;
    utxo.value = id * COIN;
    utxo.address = test::address_from_seed(id);
    utxo.height = id;
    const auto tip = test::hash_from_seed(id + 1);
    expect(db.put_metadata(utxo.get_key(), utxo.serialize()) &&
           db.put_metadata(utxo.get_address_index_key(), {}) &&
           db.put_metadata("chainstate_height", utils::encode_u64_le(id)) &&
           db.put_metadata("chainstate_tip_hash", bytes(tip.begin(), tip.end())) &&
           db.put_metadata("chainstate_utxo_count", utils::encode_u64_le(1)), "seed complete UTXO snapshot");
    return utxo;
}

void replace_snapshot(bool detach) {
    for (int mode = 0; mode < 3; ++mode) {
        Scratch scratch;
        auto first = backend(mode, scratch.path / "first");
        const auto old = seed(*first, 41);
        state::ChainState state(std::move(first));
        expect(state.has_utxo(old.tx_hash, 0) && state.has_derived_state_snapshot(), "first snapshot loaded");
        state.set_archive_mode(true);
        state.set_prune_depth(37);
        if (detach) {
            state.set_storage(nullptr);
            expect(state.get_utxo_count() == 0 && state.get_height() == 0 && state.get_tip_hash() == Hash256{},
                   "detaching storage starts with empty derived state");
        } else {
            auto second = backend(mode, scratch.path / "second");
            const auto next = seed(*second, 42);
            state.set_storage(std::move(second));
            expect(!state.has_utxo(old.tx_hash, 0) && state.has_utxo(next.tx_hash, 0),
                   "replacement loads new UTXOs instead of retaining old snapshot");
            expect(state.get_balance(next.address) == next.value && state.get_balance(old.address) == 0,
                   "balances and address indexes belong to replacement");
            expect(state.get_height() == 42 && state.get_tip_hash() == test::hash_from_seed(43) &&
                   state.has_derived_state_snapshot(), "replacement position and validity loaded");
        }
        expect(state.is_archive_mode() && state.get_prune_depth() == 37, "reload preserves operator retention policy");
    }
}

struct InterruptedReadStorage : state::MemoryStorage {
    bool get_metadata(const std::string& key, bytes& value) const override {
        if (key == "chainstate_tip_hash") throw storage::ReadError(storage::ReadStatus::IO_ERROR, "injected snapshot read failure");
        return MemoryStorage::get_metadata(key, value);
    }
};

void interrupted_load() {
    auto first = std::make_unique<state::MemoryStorage>();
    seed(*first, 41);
    state::ChainState state(std::move(first));
    auto broken = std::make_unique<InterruptedReadStorage>();
    seed(*broken, 42);
    bool failed = false;
    try { state.set_storage(std::move(broken)); }
    catch (const storage::ReadError& error) { failed = error.status() == storage::ReadStatus::IO_ERROR; }
    expect(failed, "snapshot read failure propagates");
    expect(!state.has_derived_state_snapshot(), "failed replacement cannot advertise a valid snapshot");
    const auto coinbase = core::Transaction::create_coinbase(1, test::address_from_seed(42), COIN);
    expect(!state.apply_transaction(coinbase, 1, true), "failed replacement cannot accept a state transition");
    auto good = std::make_unique<state::MemoryStorage>();
    const auto utxo = seed(*good, 43);
    state.set_storage(std::move(good));
    expect(state.has_derived_state_snapshot() && state.has_utxo(utxo.tx_hash, 0), "complete replacement recovers after read failure");
}

void batch_replacement(bool incoming) {
    for (int mode = 0; mode < 3; ++mode) {
        Scratch scratch;
        auto first = backend(mode, scratch.path / "first");
        const auto old = seed(*first, 41);
        auto second = backend(mode, scratch.path / "second");
        seed(*second, 42);
        state::ChainState state(std::move(first));
        auto* original = state.get_storage();
        auto* pending = incoming ? second.get() : original;
        pending->begin_batch();
        expect(pending->put_metadata("pending", {9}), "stage uncommitted metadata");
        bool rejected = false;
        try { state.set_storage(std::move(second)); }
        catch (const std::logic_error&) { rejected = true; }
        expect(rejected, incoming ? "uncommitted incoming snapshot is rejected" : "unfinished current batch prevents replacement");
        expect(state.get_storage() == original && state.has_utxo(old.tx_hash, 0), "rejected replacement preserves current state");
        if (!incoming) {
            bytes value;
            expect(original->batch_active() && original->get_metadata("pending", value) && value == bytes{9},
                   "rejected replacement retains original pending batch");
            original->abort_batch();
        }
    }
}

void clear_undo() {
    state::ChainState state;
    state::BlockUndo undo;
    undo.block_hash = test::hash_from_seed(61);
    undo.height = 2;
    expect(state.save_block_undo(undo), "seed in-memory undo");
    state.clear();
    state::BlockUndo loaded;
    expect(!state.load_block_undo(undo.block_hash, loaded), "cleared state has no stale in-memory undo");
}

void clear_angle() {
    state::ChainState state;
    const auto shared = state.get_angle_store();
    core::Transaction tx;
    tx.type = core::TransactionType::ANGLE_COMMITMENT;
    tx.multisig_address = test::address_from_seed(61);
    tx.spending_tx_hash = test::hash_from_seed(62);
    shared->add_commitment(tx);
    expect(shared->has_commitment(tx.get_hash()), "seed transient verifier index");
    state.clear();
    expect(shared->size() == 0 && shared->get_by_multisig_address(tx.multisig_address).empty() &&
           shared->get_by_spending_tx(tx.spending_tx_hash).empty(), "clear resets shared verifier index and its secondary indexes");
    expect(state.get_angle_store() == shared, "clear preserves shared verifier object identity");
}

void clear_witness() {
    test::TestChain fixture;
    auto block = fixture.make_block();
    block.transactions.front().witnesses.resize(1);
    block.transactions.front().witnesses.front().signature = {1, 2, 3};
    test::TestChain::seal(block, fixture.chain->get_tip());
    fixture.append(block);
    bytes payload;
    expect(fixture.state->get_witness_payload(block.get_hash(), payload), "seed witness cache through block application");
    expect(fixture.blocks->delete_block(block.get_hash()), "remove underlying witness source");
    fixture.state->clear();
    expect(!fixture.state->get_witness_payload(block.get_hash(), payload), "clear discards cached witness from old state");
}

bytes foundation_record() {
    const auto participant = test::address_from_seed(61);
    const auto address = multisig::create_multisig_address(1, 1, {participant}).address;
    bytes data;
    const auto append_string = [&](const std::string& value) {
        const uint32_t size = value.size();
        for (int i = 0; i < 4; ++i) data.push_back(static_cast<byte>(size >> (i * 8)));
        data.insert(data.end(), value.begin(), value.end());
    };
    append_string(address);
    data.insert(data.end(), {1, 1, 1, 0, 0, 0});
    append_string(participant.str());
    return data;
}

void foundation_rebuild() {
    for (int mode = 1; mode < 3; ++mode) {
        Scratch scratch;
        test::TestChain fixture(backend(mode, scratch.path));
        fixture.append(fixture.make_block());
        fixture.append(fixture.make_block());
        const auto height = fixture.state->get_height();
        const auto tip = fixture.state->get_tip_hash();
        const auto balance = fixture.state->get_balance(test::address_from_seed(42));
        auto* db = fixture.state->get_storage();
        expect(db->put_metadata("foundation_cfg", foundation_record()) &&
               db->put_metadata("chainstate_utxo_count", utils::encode_u64_le(0)), "seed inconsistent derived snapshot");
        fixture.chain.reset();
        fixture.state.reset();
        fixture.state = std::make_shared<state::ChainState>(backend(mode, scratch.path));
        expect(fixture.state->get_foundation_address() != consensus::Params().foundation_address(), "snapshot loads non-default foundation");
        fixture.state->set_block_storage(fixture.blocks);
        fixture.chain = std::make_shared<core::Blockchain>(fixture.state, fixture.blocks);
        expect(fixture.chain->initialize(), "recovery replays history from the initial foundation configuration");
        expect(fixture.state->get_foundation_address() == consensus::Params().foundation_address() &&
               fixture.state->get_height() == height && fixture.state->get_tip_hash() == tip &&
               fixture.state->get_balance(test::address_from_seed(42)) == balance,
               "recovery reproduces foundation, position and balances from active history");
        expect(!fixture.state->get_storage()->has_metadata("chainstate_rebuild_pending"),
               "successful recovery clears its pending marker");
    }
}
}

int main(int argc, char** argv) {
    try {
        consensus::SelectParams(consensus::Network::LOADNET);
        const std::map<std::string, std::function<void()>> cases{
            {"replace", [] { replace_snapshot(false); }}, {"detach", [] { replace_snapshot(true); }},
            {"read-failure", interrupted_load}, {"current-batch", [] { batch_replacement(false); }},
            {"incoming-batch", [] { batch_replacement(true); }}, {"clear-undo", clear_undo},
            {"clear-angle", clear_angle}, {"clear-witness", clear_witness}, {"foundation-rebuild", foundation_rebuild}
        };
        if (argc == 2) { cases.at(argv[1])(); }
        else { expect(argc == 1, "optional argument selects one reload scenario"); for (const auto& [name, run] : cases) run(); }
        std::cout << "State reload and reset regressions passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
