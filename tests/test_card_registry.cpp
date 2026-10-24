#include "card_registry_fixture.hpp"
#include "geocoin/consensus/chainparams.hpp"
#include <iostream>

namespace geocoin::state {
struct ChainStateTestAccess {
    static bool valid(const ChainState& state) { return state.derived_state_storage_valid_; }
    static size_t cards(const ChainState& state) { return state.card_instances_.size(); }
    static size_t pubkeys(const ChainState& state) { return state.card_pubkey_to_uid_.size(); }
    static size_t active_cards(const ChainState& state, const Hash256& program) {
        const auto it = state.card_active_counts_.find(utils::hash256_to_hex(program));
        return it == state.card_active_counts_.end() ? 0 : it->second;
    }
    static bytes program(const ChainState& state, const Hash256& id) {
        return records::serialize_card_program(state.card_programs_.at(utils::hash256_to_hex(id)));
    }
    static bytes card(const ChainState& state, const Hash256& id) {
        return records::serialize_card_instance(state.card_instances_.at(utils::hash256_to_hex(id)));
    }
    static TxUndo apply(ChainState& state, const core::Transaction& tx) {
        TxUndo undo;
        state.prepare_card_undo_(tx, undo);
        test::expect(state.apply_card_transaction_(tx, state.get_height()), "apply card projection");
        return undo;
    }
    static void revert(ChainState& state, const TxUndo& undo) {
        test::expect(state.revert_card_undo_(undo), "restore card projection from undo");
    }
};
}

namespace {
using namespace geocoin;
using test::expect;
using Access = state::ChainStateTestAccess;
struct Scratch {
    std::filesystem::path path = test::make_temp_dir("geocoin-card-registry");
    ~Scratch() { std::error_code ec; std::filesystem::remove_all(path, ec); }
};

std::unique_ptr<state::Storage> backend(int mode, const std::filesystem::path& root) {
    if (mode == 0) return std::make_unique<state::MemoryStorage>();
    auto result = mode == 1 ? std::make_unique<state::RocksDBStorage>((root / "state").string()) :
        std::make_unique<state::RocksDBStorage>((root / "state").string(), (root / "addresses").string());
    expect(result->open(), "open card registry RocksDB");
    return result;
}

void seed_projection(state::Storage& db) {
    expect(db.put_metadata("chainstate_height", utils::encode_u64_le(3)), "set projection height");
    expect(db.put_metadata("chainstate_utxo_count", utils::encode_u64_le(0)), "set empty UTXO count");
    expect(db.put_metadata("idx_epoch", bytes(32, '1')), "set projection index epoch");
}

void check_case(const std::string& damage, int mode) {
    Scratch scratch;
    test::CardRegistryFixture fixture;
    const bool valid = damage.empty() || damage == "version-zero";
    if (damage == "version-zero") {
        fixture.program.corporate_policy = {};
        fixture.card.valid_to = 0;
        fixture.card.status = 1;
    }
    const auto records = fixture.records(valid ? "" : damage);
    auto db = backend(mode, scratch.path);
    seed_projection(*db);
    for (const auto& [key, value] : records) expect(db->put_metadata(key, value), "seed card record");
    if (mode != 0) { db.reset(); db = backend(mode, scratch.path); }
    state::ChainState loaded(std::move(db));
    expect(Access::valid(loaded) == valid, "card snapshot validity: " + damage);
    for (const auto& [key, value] : records) {
        bytes preserved;
        expect(loaded.get_storage()->get_metadata(key, preserved) && preserved == value,
               "loading preserves original database records");
    }
    if (valid) {
        expect(Access::cards(loaded) == 1 && Access::pubkeys(loaded) == 1, "rebuild unique card lookup");
        expect(Access::active_cards(loaded, fixture.program_id) == 1,
               "rebuild occupied capacity for active and suspended records");
        expect(Access::program(loaded, fixture.program_id) == records.at("card:prog:" + utils::hash256_to_hex(fixture.program_id)),
               "all program fields survive restart");
        expect(Access::card(loaded, fixture.card.uid_hash) == records.at("card:uid:" + utils::hash256_to_hex(fixture.card.uid_hash)),
               "all card fields survive restart");
    }
}

void lifecycle(int mode) {
    Scratch scratch;
    test::CardRegistryFixture fixture;
    auto db = backend(mode, scratch.path);
    seed_projection(*db);
    for (const auto& [key, value] : fixture.records()) expect(db->put_metadata(key, value), "seed lifecycle projection");
    auto state = std::make_unique<state::ChainState>(std::move(db));
    auto reopen = [&] {
        std::unique_ptr<state::Storage> next;
        if (mode == 0) next = std::make_unique<state::MemoryStorage>(
            *dynamic_cast<state::MemoryStorage*>(state->get_storage()));
        state.reset();
        if (mode != 0) next = backend(mode, scratch.path);
        state = std::make_unique<state::ChainState>(std::move(next));
        expect(Access::valid(*state), "card lifecycle remains valid after restart");
    };
    auto compare = [&] {
        expect(Access::program(*state, fixture.program_id) == state::records::serialize_card_program(fixture.program),
               "complete program projection matches after restart");
        expect(Access::card(*state, fixture.card.uid_hash) == state::records::serialize_card_instance(fixture.card),
               "complete card projection matches after restart");
        expect(Access::active_cards(*state, fixture.program_id) == (fixture.card.status == 2 ? 0 : 1),
               "capacity matches status before and after restart or undo");
    };
    compare();
    core::Transaction issue;
    issue.type = core::TransactionType::CARD_ISSUE;
    issue.card_program_id = fixture.program_id;
    issue.card_issuer = fixture.program.issuer;
    issue.card_uid_hash = test::hash_from_seed(96);
    issue.card_pubkey_hash = test::hash_from_seed(97);
    issue.card_metadata_hash = fixture.card.metadata_hash;
    issue.card_valid_from = fixture.card.valid_from;
    issue.card_valid_to = fixture.card.valid_to;
    state::BlockUndo issue_undo;
    issue_undo.height = 3; issue_undo.block_hash = test::hash_from_seed(95);
    issue_undo.tx_undos.push_back(Access::apply(*state, issue));
    const auto issue_undo_key = "undo:" + utils::hash256_to_hex(issue_undo.block_hash);
    expect(state->get_storage()->put_metadata(issue_undo_key, issue_undo.serialize()), "persist issuance undo");
    expect(Access::active_cards(*state, fixture.program_id) == 2, "issuance occupies one more slot");
    reopen();
    expect(Access::active_cards(*state, fixture.program_id) == 2, "issued capacity survives restart");
    bytes issue_raw;
    expect(state->get_storage()->get_metadata(issue_undo_key, issue_raw), "read durable issuance undo");
    Access::revert(*state, state::BlockUndo::deserialize(issue_raw).tx_undos.front());
    expect(Access::cards(*state) == 1 && Access::pubkeys(*state) == 1, "issuance undo removes UID and pubkey");
    compare();
    reopen(); compare();
    state::BlockUndo undo;
    undo.height = 3; undo.block_hash = test::hash_from_seed(90);
    core::Transaction status;
    status.type = core::TransactionType::CARD_UPDATE_STATUS;
    status.card_uid_hash = fixture.card.uid_hash;
    status.card_status = 2; status.card_metadata_hash = test::hash_from_seed(91);
    undo.tx_undos.push_back(Access::apply(*state, status));
    core::Transaction policy;
    policy.type = core::TransactionType::CARD_UPDATE_POLICY;
    policy.card_program_id = fixture.program_id;
    policy.card_policy_id = 18;
    policy.card_corporate_policy = fixture.program.corporate_policy;
    policy.card_corporate_policy.max_active_cards = 7;
    undo.tx_undos.push_back(Access::apply(*state, policy));
    const auto undo_key = "undo:" + utils::hash256_to_hex(undo.block_hash);
    expect(state->get_storage()->put_metadata(undo_key, undo.serialize()), "persist card undo across restart");
    const auto original = fixture;
    fixture.card.status = status.card_status; fixture.card.metadata_hash = status.card_metadata_hash;
    fixture.program.policy_id = policy.card_policy_id; fixture.program.corporate_policy = policy.card_corporate_policy;
    compare();
    reopen(); compare();
    bytes raw;
    expect(state->get_storage()->get_metadata(undo_key, raw), "read durable card undo");
    const auto restored = state::BlockUndo::deserialize(raw);
    for (auto it = restored.tx_undos.rbegin(); it != restored.tx_undos.rend(); ++it) Access::revert(*state, *it);
    fixture = original;
    compare();
    reopen(); compare();
    state->clear();
    expect(Access::cards(*state) == 0 && Access::pubkeys(*state) == 0 &&
           Access::active_cards(*state, fixture.program_id) == 0, "rebuild clears derived card capacity");
}

class PagedStorage : public state::MemoryStorage {
public:
    mutable size_t pages = 0;
    std::vector<std::string> get_keys_with_prefix(const std::string& prefix) const override {
        expect(!prefix.starts_with("card:"), "card reload must not materialize all keys");
        return MemoryStorage::get_keys_with_prefix(prefix);
    }
    std::vector<std::string> get_keys_page(const std::string& prefix, size_t offset, size_t limit,
                                          size_t* total, const std::string& after) const override {
        if (prefix.starts_with("card:")) {
            expect(offset == 0 && limit <= 1024 && !total, "bounded card cursor scan");
            ++pages;
        }
        return MemoryStorage::get_keys_page(prefix, offset, limit, total, after);
    }
};

void pagination() {
    auto db = std::make_unique<PagedStorage>();
    auto* observed = db.get();
    test::CardRegistryFixture fixture;
    seed_projection(*db);
    for (size_t i = 0; i < 1100; ++i) {
        fixture.card.uid_hash[0] = fixture.card.pubkey_hash[0] = i & 255;
        fixture.card.uid_hash[1] = fixture.card.pubkey_hash[1] = (i >> 8) & 255;
        for (const auto& [key, value] : fixture.records()) expect(db->put_metadata(key, value), "seed paged registry");
    }
    state::ChainState loaded(std::move(db));
    expect(Access::valid(loaded) && Access::cards(loaded) == 1100 && Access::pubkeys(loaded) == 1100,
           "every card and unique pubkey loads across the cursor boundary");
    expect(Access::active_cards(loaded, fixture.program_id) == 1100,
           "capacity includes every page of the card registry");
    expect(observed->pages >= 3, "programs and cards use bounded pages");
}
}

int main(int argc, char** argv) {
    geocoin::consensus::SelectParams(geocoin::consensus::Network::LOADNET);
    const std::string selected = argc > 1 ? argv[1] : "";
    auto cases = geocoin::test::card_damage_cases;
    cases.insert(cases.begin(), {"valid", "version-zero", "lifecycle"}); cases.push_back("pagination");
    if (!selected.empty() && std::find(cases.begin(), cases.end(), selected) == cases.end()) return 2;
    size_t failures = 0;
    for (const auto& name : cases) {
        if (!selected.empty() && selected != name) continue;
        for (int mode = 0; mode < (name == "pagination" ? 1 : 3); ++mode) {
            try {
                if (name == "pagination") pagination();
                else if (name == "lifecycle") lifecycle(mode);
                else check_case(name == "valid" ? "" : name, mode);
                std::cout << name << " backend=" << mode << " passed\n";
            } catch (const std::exception& e) {
                ++failures;
                std::cerr << name << " backend=" << mode << " FAILED: " << e.what() << '\n';
            }
        }
    }
    return failures ? 1 : 0;
}
