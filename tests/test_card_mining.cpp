#include "card_registry_fixture.hpp"
#include "geocoin/consensus/chainparams.hpp"
#include "geocoin/network/mempool.hpp"
#include "nlohmann_json.hpp"

#include <fstream>
#include <iostream>

namespace {
using namespace geocoin;
using test::expect;
using core::Transaction;
using core::TransactionType;
nlohmann::json checks = nlohmann::json::array();
bool use_rocksdb = false;

void check(bool result, const std::string& name) {
    checks.push_back({{"name", std::string(use_rocksdb ? "RocksDB: " : "MemoryStorage: ") + name}, {"passed", result}});
}

Transaction operation(const test::CardRegistryFixture& fixture, TransactionType type, uint8_t seed) {
    Transaction tx;
    tx.type = type;
    tx.timestamp = 1700000000 + seed;
    tx.card_program_id = fixture.program_id;
    tx.card_issuer = fixture.program.issuer;
    tx.card_policy_id = fixture.program.policy_id;
    tx.card_scope_id = fixture.program.scope_id;
    if (type == TransactionType::CARD_PROGRAM_DEFINE) {
        tx.card_program_id = test::hash_from_seed(90);
        tx.card_chip_vendor = fixture.program.chip_vendor;
        tx.card_chip_os = fixture.program.chip_os;
        tx.card_applet_aid = fixture.program.applet_aid;
        tx.card_applet_version = fixture.program.applet_version;
        tx.card_applet_package_hash = fixture.program.applet_package_hash;
        tx.card_corporate_policy = fixture.program.corporate_policy;
    } else if (type == TransactionType::CARD_ISSUE) {
        tx.card_uid_hash = test::hash_from_seed(seed);
        tx.card_pubkey_hash = test::hash_from_seed(seed + 10);
        tx.card_valid_from = 100;
        tx.card_valid_to = 1000;
        tx.card_metadata_hash = fixture.card.metadata_hash;
    } else if (type == TransactionType::CARD_UPDATE_STATUS) {
        tx.card_policy_id = 0;
        tx.card_scope_id = {};
        tx.card_uid_hash = fixture.card.uid_hash;
        tx.card_status = 1;
    } else if (type == TransactionType::CARD_UPDATE_POLICY) {
        tx.card_corporate_policy = fixture.program.corporate_policy;
    }
    return tx;
}

void selection_case(const std::string& name, const test::CardRegistryFixture& fixture,
                    std::vector<Transaction> candidates, const std::vector<size_t>& expected,
                    const std::vector<test::CardRegistryFixture>& additional = {},
                    std::optional<size_t> size_limit_candidate = std::nullopt) {
    struct Scratch {
        std::filesystem::path path = test::make_temp_dir("geocoin-card-mining");
        ~Scratch() { std::error_code error; std::filesystem::remove_all(path, error); }
    } scratch;
    std::unique_ptr<state::Storage> storage;
    if (use_rocksdb) {
        auto db = std::make_unique<state::RocksDBStorage>((scratch.path / "state").string());
        expect(db->open(), "open isolated RocksDB");
        storage = std::move(db);
    } else storage = std::make_unique<state::MemoryStorage>();
    expect(storage->put_metadata("chainstate_height", utils::encode_u64_le(200)), "seed height");
    expect(storage->put_metadata("chainstate_utxo_count", utils::encode_u64_le(0)), "seed UTXO count");
    expect(storage->put_metadata("idx_epoch", bytes(32, '1')), "seed index epoch");
    auto records = fixture.records();
    for (const auto& other : additional) {
        const auto entries = other.records();
        records.insert(entries.begin(), entries.end());
    }
    for (const auto& [key, value] : records) expect(storage->put_metadata(key, value), "seed card registry");
    auto state = std::make_shared<state::ChainState>(std::move(storage));
    network::Mempool pool(state);
    std::vector<Transaction> funding;
    Amount fee = COIN / 2;
    for (size_t i = 0; i < candidates.size(); ++i) {
        funding.push_back(Transaction::create_coinbase(i + 1, fixture.program.issuer, 2 * COIN));
        expect(state->apply_transaction(funding.back(), i + 1, true), "seed distinct mature input");
        auto& tx = candidates[i];
        tx.inputs = {{funding.back().get_hash(), 0}};
        tx.outputs = {{funding.back().outputs.front().value - fee, fixture.program.issuer, {}}};
        fee /= 5;
        tx.sign(test::make_key(81));
        if (!pool.add_transaction(tx)) {
            const auto rejects = pool.get_reject_stats();
            expect(false, name + ": individually valid transaction admitted: " +
                nlohmann::json{{"consensus", rejects.consensus}, {"policy", rejects.policy}}.dump());
        }
    }
    const size_t max_size = size_limit_candidate ? candidates.at(*size_limit_candidate).serialize().size() : 1000000;
    if (size_limit_candidate) expect(candidates.front().serialize().size() > max_size, "preferred candidate exceeds template budget");
    const auto selected = pool.get_transactions_for_mining(max_size);
    bool matches = selected.size() == expected.size();
    for (size_t i = 0; matches && i < expected.size(); ++i) {
        matches = selected[i].get_hash() == candidates.at(expected[i]).get_hash();
    }
    check(matches, name + ": select the compatible sequence in fee order");
    const auto repeated = pool.get_transactions_for_mining(max_size);
    bool repeatable = repeated.size() == selected.size() && pool.get_transactions_for_mining(0).empty();
    for (size_t i = 0; repeatable && i < selected.size(); ++i) {
        repeatable = repeated[i].get_hash() == selected[i].get_hash();
    }
    check(repeatable, name + ": repeated and empty templates do not leak projected effects");
    bool preserved = pool.size() == candidates.size() && state->get_height() == 200 &&
        state->get_storage()->get_keys_with_prefix("card:").size() == records.size();
    for (const auto& [key, value] : records) {
        bytes current;
        preserved = preserved && state->get_storage()->get_metadata(key, current) && current == value;
    }
    for (const auto& tx : funding) preserved = preserved && state->has_utxo(tx.get_hash(), 0);
    check(preserved, name + ": selection preserves confirmed records, inputs and mempool");
    bool applies = true;
    for (const auto& tx : selected) {
        if (!state->apply_transaction(tx, 201, false)) { applies = false; break; }
    }
    check(applies, name + ": selected transactions apply through ChainState");
}

void test_issuance() {
    for (const std::string mode : {"remaining-card-capacity", "duplicate-uid", "duplicate-pubkey", "independent-cards"}) {
        test::CardRegistryFixture f;
        if (mode == "remaining-card-capacity") f.program.corporate_policy.max_active_cards = 2;
        auto a = operation(f, TransactionType::CARD_ISSUE, 100);
        auto b = operation(f, TransactionType::CARD_ISSUE, 101);
        if (mode == "duplicate-uid") b.card_uid_hash = a.card_uid_hash;
        if (mode == "duplicate-pubkey") b.card_pubkey_hash = a.card_pubkey_hash;
        selection_case(mode, f, {a, b}, mode == "independent-cards" ? std::vector<size_t>{0, 1} : std::vector<size_t>{0});
    }
    test::CardRegistryFixture f;
    selection_case("duplicate-program", f, {operation(f, TransactionType::CARD_PROGRAM_DEFINE, 100),
        operation(f, TransactionType::CARD_PROGRAM_DEFINE, 101)}, {0});
}

void test_status_order() {
    test::CardRegistryFixture f;
    const auto suspend = operation(f, TransactionType::CARD_UPDATE_STATUS, 100);
    auto revoke = operation(f, TransactionType::CARD_UPDATE_STATUS, 101);
    revoke.card_status = 2;
    selection_case("duplicate-suspension", f, {suspend, operation(f, TransactionType::CARD_UPDATE_STATUS, 102)}, {0});
    selection_case("revoke-before-suspension", f, {revoke, suspend}, {0});
    selection_case("suspension-before-revoke", f, {suspend, revoke}, {0, 1});
}

void test_policy_order() {
    for (const std::string mode : {"policy-id-change", "mint-authority-change", "freeze-authority-change",
                                  "recovery-authority-change", "operations-authority-change", "lower-card-capacity", "issue-before-policy-change"}) {
        test::CardRegistryFixture f;
        if (mode == "operations-authority-change") f.card.status = 1;
        auto policy = operation(f, TransactionType::CARD_UPDATE_POLICY, 100);
        auto next = operation(f, TransactionType::CARD_ISSUE, 101);
        core::CardAuthority replacement; replacement.quorum = 1; replacement.members = {test::address_from_seed(88)};
        if (mode == "policy-id-change" || mode == "issue-before-policy-change") ++policy.card_policy_id;
        if (mode == "mint-authority-change") policy.card_corporate_policy.mint = replacement;
        if (mode == "freeze-authority-change") {
            policy.card_corporate_policy.freeze = replacement;
            next = operation(f, TransactionType::CARD_UPDATE_STATUS, 101);
        }
        if (mode == "operations-authority-change") {
            policy.card_corporate_policy.operations = replacement;
            next = operation(f, TransactionType::CARD_UPDATE_STATUS, 101);
            next.card_status = 0;
        }
        if (mode == "recovery-authority-change") {
            policy.card_corporate_policy.recovery = replacement;
            next = operation(f, TransactionType::CARD_UPDATE_POLICY, 101);
            ++next.card_policy_id;
        }
        if (mode == "lower-card-capacity") policy.card_corporate_policy.max_active_cards = 1;
        if (mode == "issue-before-policy-change") selection_case(mode, f, {next, policy}, {0, 1});
        else selection_case(mode, f, {policy, next}, {0});
    }
}

void test_capacity_and_rejection() {
    test::CardRegistryFixture f;
    f.program.corporate_policy.max_active_cards = 2;
    auto revoke = operation(f, TransactionType::CARD_UPDATE_STATUS, 100);
    revoke.card_status = 2;
    auto a = operation(f, TransactionType::CARD_ISSUE, 101);
    auto b = operation(f, TransactionType::CARD_ISSUE, 102);
    selection_case("revocation-frees-capacity", f, {revoke, a, b}, {0, 1, 2});
    revoke.card_status = 1;
    selection_case("suspension-retains-capacity", f, {revoke, a, b}, {0, 1});
    f.program.corporate_policy.max_active_cards = 1;
    f.card.status = 2;
    selection_case("revoked-record-does-not-use-capacity", f, {a, b}, {0});
    f.card.status = 1;
    f.program.corporate_policy.max_active_cards = 2;
    selection_case("suspended-record-uses-capacity", f, {a, b}, {0});
    f.card.status = 0;
    f.program.corporate_policy.max_active_cards = 0;
    selection_case("unlimited-card-capacity", f, {a, b, operation(f, TransactionType::CARD_ISSUE, 103)}, {0, 1, 2});

    f.program.corporate_policy.max_active_cards = 3;
    auto duplicate = b;
    duplicate.card_uid_hash = a.card_uid_hash;
    auto fallback = operation(f, TransactionType::CARD_ISSUE, 103);
    fallback.card_pubkey_hash = duplicate.card_pubkey_hash;
    selection_case("rejected-issue-preserves-pubkey-and-capacity", f, {a, duplicate, fallback}, {0, 2});

    auto policy = operation(f, TransactionType::CARD_UPDATE_POLICY, 100);
    ++policy.card_policy_id;
    // Make the preferred policy larger than the available template. It must
    // not change the policy against which the smaller issue is selected.
    for (auto* role : {&policy.card_corporate_policy.mint, &policy.card_corporate_policy.freeze,
                       &policy.card_corporate_policy.operations, &policy.card_corporate_policy.recovery}) {
        role->members.push_back(test::address_from_seed(88));
        std::sort(role->members.begin(), role->members.end(), [](const Address& x, const Address& y) {
            return x.str() < y.str();
        });
    }
    selection_case("oversized-policy-does-not-change-template", f, {policy, a}, {1}, {}, 1);
}

void test_program_isolation() {
    test::CardRegistryFixture a, b;
    a.program.corporate_policy.max_active_cards = b.program.corporate_policy.max_active_cards = 2;
    b.program_id = b.card.program_id = test::hash_from_seed(91);
    b.card.uid_hash = test::hash_from_seed(92);
    b.card.pubkey_hash = test::hash_from_seed(93);
    auto issue_a = operation(a, TransactionType::CARD_ISSUE, 100);
    auto issue_b = operation(b, TransactionType::CARD_ISSUE, 101);
    selection_case("independent-program-capacity", a, {issue_a, issue_b}, {0, 1}, {b});
    auto duplicate = issue_b;
    duplicate.card_uid_hash = issue_a.card_uid_hash;
    selection_case("uid-unique-across-programs", a, {issue_a, duplicate}, {0}, {b});
    duplicate = issue_b;
    duplicate.card_pubkey_hash = issue_a.card_pubkey_hash;
    selection_case("pubkey-unique-across-programs", a, {issue_a, duplicate}, {0}, {b});
    auto policy = operation(a, TransactionType::CARD_UPDATE_POLICY, 100);
    ++policy.card_policy_id;
    selection_case("policy-only-affects-own-program", a, {policy, issue_b}, {0, 1}, {b});
}

void test_resume_order() {
    test::CardRegistryFixture f;
    f.card.status = 1;
    auto resume = operation(f, TransactionType::CARD_UPDATE_STATUS, 100);
    resume.card_status = 0;
    auto revoke = operation(f, TransactionType::CARD_UPDATE_STATUS, 101);
    revoke.card_status = 2;
    auto duplicate = operation(f, TransactionType::CARD_UPDATE_STATUS, 102);
    duplicate.card_status = 0;
    selection_case("duplicate-resume", f, {resume, duplicate}, {0});
    selection_case("revoke-before-resume", f, {revoke, resume}, {0});
    selection_case("resume-before-revoke", f, {resume, revoke}, {0, 1});
}
}

int main(int argc, char** argv) {
    try {
        consensus::SelectParams(consensus::Network::MAIN);
        for (const bool backend : {false, true}) {
            use_rocksdb = backend;
            test_issuance();
            test_status_order();
            test_policy_order();
            test_capacity_and_rejection();
            test_program_isolation();
            test_resume_order();
        }
        size_t failed = 0;
        for (const auto& result : checks) {
            if (!result["passed"].get<bool>()) { ++failed; std::cerr << "FAILED: " << result["name"] << '\n'; }
        }
        if (argc > 1) std::ofstream(argv[1]) << nlohmann::json{
            {"checks", checks}, {"passed", checks.size() - failed}, {"failed", failed}}.dump(2) << '\n';
        std::cout << "card mining checks: " << checks.size() - failed << '/' << checks.size() << '\n';
        return failed ? 1 : 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 2;
    }
}
