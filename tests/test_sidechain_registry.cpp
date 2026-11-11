#include "sidechain_registry_fixture.hpp"
#include "geocoin/consensus/chainparams.hpp"
#include "geocoin/consensus/sidechain_checkpoint.hpp"
#include "geocoin/core/merkle.hpp"
#include "geocoin/crypto/signatures.hpp"
#include <iostream>

namespace geocoin::state {
struct ChainStateTestAccess {
    static bool valid(const ChainState& state) { return state.derived_state_storage_valid_; }
    static void drop_caches(ChainState& state) { state.sidechain_registry_.clear(); state.sidechain_deposits_.clear(); }
    static size_t registries(const ChainState& state) { return state.sidechain_registry_.size(); }
    static size_t deposits(const ChainState& state) { return state.sidechain_deposits_.size(); }
    static TxUndo prepare(const ChainState& state, const core::Transaction& tx) {
        TxUndo undo;
        state.prepare_sidechain_undo_(tx, undo);
        return undo;
    }
    static void revert(ChainState& state, const TxUndo& undo) {
        test::expect(state.revert_sidechain_undo_(undo), "revert sidechain projection");
    }
};
}

namespace {
using namespace geocoin;
using test::expect;
using Access = state::ChainStateTestAccess;
struct Scratch {
    std::filesystem::path path = test::make_temp_dir("geocoin-sidechain-registry");
    ~Scratch() { std::error_code ec; std::filesystem::remove_all(path, ec); }
};

std::unique_ptr<state::Storage> backend(int mode, const std::filesystem::path& root) {
    if (mode == 0) return std::make_unique<state::MemoryStorage>();
    auto db = mode == 1 ? std::make_unique<state::RocksDBStorage>((root / "state").string()) :
        std::make_unique<state::RocksDBStorage>((root / "state").string(), (root / "addresses").string());
    expect(db->open(), "open sidechain registry RocksDB");
    return db;
}
void seed_projection(state::Storage& db) {
    expect(db.put_metadata("chainstate_height", utils::encode_u64_le(3)), "set projection height");
    expect(db.put_metadata("chainstate_utxo_count", utils::encode_u64_le(0)), "set empty UTXO count");
    expect(db.put_metadata("idx_epoch", bytes(32, '1')), "set index epoch");
}

void check_case(const std::string& mode_name, int mode) {
    Scratch scratch;
    test::SidechainRegistryFixture fixture;
    const bool valid = std::find(test::sidechain_valid_cases.begin(), test::sidechain_valid_cases.end(), mode_name) != test::sidechain_valid_cases.end();
    const auto records = fixture.records(mode_name);
    auto db = backend(mode, scratch.path);
    seed_projection(*db);
    for (const auto& [key, value] : records) expect(db->put_metadata(key, value), "seed sidechain projection");
    if (mode != 0) { db.reset(); db = backend(mode, scratch.path); }
    state::ChainState loaded(std::move(db));
    expect(Access::valid(loaded) == valid, "sidechain snapshot validity: " + mode_name);
    for (const auto& [key, value] : records) {
        bytes preserved;
        expect(loaded.get_storage()->get_metadata(key, preserved) && preserved == value, "original bytes preserved");
    }
    if (valid) {
        for (int read = 0; read < 2; ++read) {
            if (read) Access::drop_caches(loaded);
            state::SidechainDeposit deposit;
            expect(loaded.get_sidechain_deposit(fixture.id, deposit) &&
                   state::records::serialize_sidechain_deposit(deposit) == records.at("scd:" + utils::hash256_to_hex(fixture.id)),
                   "every deposit field survives restart and lazy read");
            state::SidechainRegistryEntry registry;
            const bool registered = loaded.get_sidechain_registry(fixture.id, registry);
            expect(registered == (mode_name != "unregistered"), "unregistered deposits remain valid");
            if (registered) expect(state::records::serialize_sidechain_registry(registry) == records.at("sc:reg:" + utils::hash256_to_hex(fixture.id)),
                                   "every registry field survives restart and lazy read");
        }
    }
}

void lazy_corruption(int mode) {
    Scratch scratch;
    auto db = backend(mode, scratch.path);
    seed_projection(*db);
    state::ChainState loaded(std::move(db));
    test::SidechainRegistryFixture fixture;
    for (const auto& name : {"committee-id", "owner-mismatch", "future-checkpoint"}) {
        for (const auto& [key, value] : fixture.records(name)) expect(loaded.get_storage()->put_metadata(key, value), "seed lazy damage");
        auto result = fixture.registry;
        bool rejected = false;
        try { loaded.get_sidechain_registry(fixture.id, result); }
        catch (const storage::ReadError& e) { rejected = e.status() == storage::ReadStatus::CORRUPT; }
        expect(rejected && state::records::serialize_sidechain_registry(result) == state::records::serialize_sidechain_registry(fixture.registry),
               "lazy read reports corrupt records without replacing caller output");
    }
}

void slash_lifecycle(int mode) {
    Scratch scratch;
    auto db = backend(mode, scratch.path);
    seed_projection(*db);
    test::SidechainRegistryFixture fixture;
    for (const auto& [key, value] : fixture.records("valid")) expect(db->put_metadata(key, value), "seed slash projection");
    auto state = std::make_unique<state::ChainState>(std::move(db));
    expect(state->set_height(8), "advance beyond checkpoint interval");
    auto evidence = [&](uint8_t seed) {
        core::BatchCheckpointItem item;
        item.sidechain_id = fixture.id; item.checkpoint_height = 121; item.version = 2;
        item.committee_id = fixture.registry.committee_id;
        item.fee = consensus::calculate_sidechain_checkpoint_min_fee(9, fixture.deposit.last_checkpoint_height);
        const auto supply = static_cast<uint64_t>(fixture.registry.total_issued);
        const auto leaf = consensus::sidechain_checkpoint::build_financial_supply_leaf_v2(fixture.id, item.checkpoint_height, supply);
        const std::vector<Hash256> leaves = {leaf, test::hash_from_seed(seed)};
        const auto root = core::compute_merkle_root(leaves);
        item.checkpoint = consensus::sidechain_checkpoint::build_financial_commitment_v2(fixture.id, root, item.checkpoint_height, supply);
        item.proof = consensus::sidechain_checkpoint::build_financial_proof_v2(root, item.checkpoint_height, supply, 0, {leaves[1]});
        item.committee_signatures.push_back(crypto::sign_recoverable_message(
            crypto::sha256(core::build_sidechain_batch_checkpoint_payload_for_signing(item)), test::make_key(111)));
        core::Transaction tx; tx.type = core::TransactionType::BATCH_SIDECHAIN_CHECKPOINT;
        tx.sidechain_batch_checkpoints = {item};
        // Each conflicting checkpoint is valid on its own branch.
        expect(state->validate_transaction(tx), "checkpoint evidence passes proof and committee validation");
        expect(state->get_storage()->put_transaction(tx.get_hash(), tx), "archive checkpoint evidence");
        return tx.get_hash();
    };
    core::Transaction slash;
    slash.version = 2; slash.type = core::TransactionType::SLASH_DEPOSIT;
    slash.sidechain_id = fixture.id; slash.sidechain_owner = fixture.deposit.owner;
    slash.sidechain_amount = fixture.deposit.amount;
    slash.sidechain_committee_id = fixture.registry.committee_id;
    slash.sidechain_fraud_checkpoint_a = evidence(116);
    slash.sidechain_fraud_checkpoint_b = evidence(117);
    slash.sidechain_fraud_checkpoint_index_a = slash.sidechain_fraud_checkpoint_index_b = 0;
    slash.sidechain_committee_signatures.push_back(crypto::sign_recoverable_message(
        crypto::sha256(core::build_sidechain_payload_for_signing(slash)), test::make_key(111)));
    state::BlockUndo undo; undo.height = 9; undo.block_hash = test::hash_from_seed(118);
    undo.tx_undos = {Access::prepare(*state, slash)};
    expect(state->validate_transaction(slash), "signed slash passes transaction validation");
    expect(state->apply_transaction(slash, 9, false) && state->set_height(9), "apply signed slash through public transaction path");
    const auto undo_key = "undo:" + utils::hash256_to_hex(undo.block_hash);
    expect(state->get_storage()->put_metadata(undo_key, undo.serialize()), "persist complete sidechain undo");
    auto reopen = [&] {
        std::unique_ptr<state::Storage> next;
        if (mode == 0) next = std::make_unique<state::MemoryStorage>(*dynamic_cast<state::MemoryStorage*>(state->get_storage()));
        state.reset();
        if (mode != 0) next = backend(mode, scratch.path);
        state = std::make_unique<state::ChainState>(std::move(next));
        expect(Access::valid(*state), "sidechain projection survives restart");
    };
    reopen();
    state::SidechainDeposit deposit; state::SidechainRegistryEntry registry;
    expect(state->get_sidechain_deposit(fixture.id, deposit) && deposit.amount == 0, "slashing exhausts deposit");
    expect(state->get_sidechain_registry(fixture.id, registry) && registry.issuance_limit == 0 &&
           registry.total_issued == fixture.registry.total_issued, "slashing preserves issuance above reduced limit");
    bytes raw; expect(state->get_storage()->get_metadata(undo_key, raw), "read durable sidechain undo");
    const auto restored = state::BlockUndo::deserialize(raw);
    Access::revert(*state, restored.tx_undos.front());
    reopen();
    expect(state->get_sidechain_deposit(fixture.id, deposit) && state->get_sidechain_registry(fixture.id, registry), "read restored sidechain");
    expect(state::records::serialize_sidechain_deposit(deposit) == state::records::serialize_sidechain_deposit(fixture.deposit) &&
           state::records::serialize_sidechain_registry(registry) == state::records::serialize_sidechain_registry(fixture.registry),
           "undo and second restart restore every original sidechain field");
}

class PagedStorage : public state::MemoryStorage {
public:
    mutable size_t pages = 0;
    std::vector<std::string> get_keys_with_prefix(const std::string& prefix) const override {
        expect(prefix != "scd:" && prefix != "sc:reg:", "sidechain reload must not retain all keys");
        return MemoryStorage::get_keys_with_prefix(prefix);
    }
    std::vector<std::string> get_keys_page(const std::string& prefix, size_t offset, size_t limit,
                                         size_t* total, const std::string& after) const override {
        if (prefix == "scd:" || prefix == "sc:reg:") {
            expect(offset == 0 && limit <= 1024 && !total, "bounded sidechain cursor scan");
            ++pages;
        }
        return MemoryStorage::get_keys_page(prefix, offset, limit, total, after);
    }
};
void pagination() {
    auto db = std::make_unique<PagedStorage>(); auto* observed = db.get();
    seed_projection(*db);
    test::SidechainRegistryFixture fixture;
    for (size_t i = 0; i < 1100; ++i) {
        fixture.id[0] = i & 255; fixture.id[1] = (i >> 8) & 255;
        for (const auto& [key, value] : fixture.records("valid")) expect(db->put_metadata(key, value), "seed paged sidechain registry");
    }
    state::ChainState loaded(std::move(db));
    expect(Access::valid(loaded) && Access::registries(loaded) == 1100 && Access::deposits(loaded) == 1100 && observed->pages >= 4,
           "every registry and deposit loads across page boundaries");
}
}

int main(int argc, char** argv) {
    consensus::SelectParams(consensus::Network::LOADNET);
    auto cases = test::sidechain_valid_cases;
    cases.insert(cases.end(), test::sidechain_damage_cases.begin(), test::sidechain_damage_cases.end());
    cases.insert(cases.end(), {"lazy-corruption", "pagination", "slash-lifecycle"});
    const std::string selected = argc > 1 ? argv[1] : "";
    if (!selected.empty() && std::find(cases.begin(), cases.end(), selected) == cases.end()) return 2;
    size_t failures = 0;
    for (const auto& name : cases) {
        if (!selected.empty() && selected != name) continue;
        for (int mode = 0; mode < (name == "pagination" ? 1 : 3); ++mode) {
            try {
                if (name == "pagination") pagination();
                else if (name == "lazy-corruption") lazy_corruption(mode);
                else if (name == "slash-lifecycle") slash_lifecycle(mode);
                else check_case(name, mode);
                std::cout << name << " backend=" << mode << " passed\n";
            } catch (const std::exception& e) {
                ++failures; std::cerr << name << " backend=" << mode << " FAILED: " << e.what() << '\n';
            }
        }
    }
    return failures ? 1 : 0;
}
