#include "sidechain_registry_fixture.hpp"
#include "geocoin/consensus/chainparams.hpp"
#include "geocoin/consensus/sidechain_checkpoint.hpp"
#include "geocoin/crypto/signatures.hpp"
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

Transaction operation(const test::SidechainRegistryFixture& fixture, TransactionType type,
                      Amount amount, Timestamp timestamp) {
    Transaction tx;
    tx.type = type;
    tx.timestamp = timestamp;
    tx.sidechain_id = fixture.id;
    tx.sidechain_owner = fixture.deposit.owner;
    tx.sidechain_amount = amount;
    const bool deposit_operation = type == TransactionType::SPEND_DEPOSIT || type == TransactionType::UNLOCK_DEPOSIT;
    const bool owner_only = deposit_operation || type == TransactionType::REGISTER_ACCOUNTING_SIDECHAIN;
    if (!deposit_operation) tx.sidechain_committee_id = fixture.registry.committee_id;
    if (type == TransactionType::REDEEM_SIDECHAIN) {
        tx.outputs = {{amount, test::address_from_seed(115), {}}};
    }
    if (type == TransactionType::SPEND_DEPOSIT || type == TransactionType::UNLOCK_DEPOSIT) {
        tx.outputs = {{type == TransactionType::SPEND_DEPOSIT ? amount : fixture.deposit.amount,
                       test::address_from_seed(115), {}}};
    }
    if (type == TransactionType::UPDATE_SIDECHAIN_COMMITTEE) {
        tx.sidechain_committee_m = tx.sidechain_committee_n = 1;
        tx.sidechain_committee_members = {test::address_from_seed(114)};
        tx.sidechain_committee_id = consensus::compute_sidechain_committee_id(
            1, 1, tx.sidechain_committee_members);
    }
    if (type == TransactionType::REGISTER_ACCOUNTING_SIDECHAIN) {
        tx.sidechain_checkpoint = fixture.id;
        tx.sidechain_namespace = "warehouse";
        tx.sidechain_schema_hash = test::hash_from_seed(116);
        tx.sidechain_checkpoint_policy_id = consensus::params::ACCOUNTING_CHECKPOINT_POLICY_ID;
        tx.sidechain_checkpoint_version = consensus::params::ACCOUNTING_CHECKPOINT_VERSION;
        tx.sidechain_committee_m = fixture.registry.committee_m;
        tx.sidechain_committee_n = fixture.registry.committee_n;
        tx.sidechain_committee_members = fixture.registry.committee_members;
    }
    const auto hash = crypto::sha256(core::build_sidechain_payload_for_signing(tx));
    if (!owner_only) {
        tx.sidechain_committee_signatures = {crypto::sign_recoverable_message(hash, test::make_key(111))};
    }
    if (type == TransactionType::DECOMMISSION_SIDECHAIN || owner_only) {
        tx.sidechain_signature = crypto::sign_recoverable_message(hash, test::make_key(111));
    }
    return tx;
}

void selection_cases(const std::string& name, const std::vector<test::SidechainRegistryFixture>& fixtures,
                     const std::vector<Transaction>& candidates, size_t expected_count,
                     bool registered = true, const std::vector<Transaction>& funding = {}) {
    struct Scratch {
        std::filesystem::path path = test::make_temp_dir("geocoin-sidechain-mining");
        ~Scratch() { std::error_code error; std::filesystem::remove_all(path, error); }
    } scratch;
    std::unique_ptr<state::Storage> storage;
    if (use_rocksdb) {
        auto db = std::make_unique<state::RocksDBStorage>((scratch.path / "state").string());
        expect(db->open(), "open isolated RocksDB");
        storage = std::move(db);
    } else storage = std::make_unique<state::MemoryStorage>();
    expect(storage->put_metadata("chainstate_height", utils::encode_u64_le(100)), "seed height");
    expect(storage->put_metadata("chainstate_utxo_count", utils::encode_u64_le(0)), "seed UTXO count");
    expect(storage->put_metadata("idx_epoch", bytes(32, '1')), "seed index epoch");
    std::map<std::string, bytes> records;
    for (const auto& fixture : fixtures) {
        const auto entries = fixture.records(registered ? "valid" : "unregistered");
        records.insert(entries.begin(), entries.end());
    }
    for (const auto& [key, value] : records) expect(storage->put_metadata(key, value), "seed sidechain");
    auto state = std::make_shared<state::ChainState>(std::move(storage));
    state->set_activation_manifest(utils::ActivationManifest::fully_enabled_for_tests("degree"));
    for (const auto& tx : funding) expect(state->apply_transaction(tx, 1, true), "seed mature funding");
    network::Mempool pool(state);
    for (const auto& tx : candidates) {
        if (!pool.add_transaction(tx)) {
            std::string reason;
            (void)pool.test_accept_transaction(tx, &reason);
            const auto rejected = pool.get_reject_stats();
            expect(false, name + ": individually valid transaction admitted: " + reason + " " +
                nlohmann::json{{"consensus", rejected.consensus}, {"policy", rejected.policy}}.dump());
        }
    }

    const auto selected = pool.get_transactions_for_mining(1000000);
    check(selected.size() == expected_count, name + ": select only a compatible sequence");
    bool preserved = pool.size() == candidates.size() && state->get_height() == 100;
    for (const auto& [key, value] : records) {
        bytes current;
        preserved = preserved && state->get_storage()->get_metadata(key, current) && current == value;
    }
    check(preserved, name + ": selection preserves confirmed records and mempool");
    bool applies = true;
    for (const auto& tx : selected) {
        if (!state->apply_transaction(tx, 101, false)) { applies = false; break; }
    }
    check(applies, name + ": the selected sequence applies through ChainState");
}

void selection_case(const std::string& name, const test::SidechainRegistryFixture& fixture,
                    const std::vector<Transaction>& candidates, size_t expected_count) {
    selection_cases(name, {fixture}, candidates, expected_count);
}

void test_shared_limits() {
    for (const std::string name : {"mint-capacity", "mint-fees", "redeem-supply", "redeem-deposit", "compatible-mints"}) {
        test::SidechainRegistryFixture f;
        f.registry.fee_bps = 0;
        f.registry.issuance_limit = 100 * COIN;
        f.registry.total_issued = 0;
        f.deposit.amount = 100 * COIN;
        auto type = TransactionType::MINT_SIDECHAIN;
        Amount amount = 6 * COIN;
        if (name == "mint-capacity") f.registry.issuance_limit = 10 * COIN;
        if (name == "mint-fees") f.deposit.amount = 3 * consensus::params::MIN_TX_FEE / 2;
        if (name == "redeem-supply" || name == "redeem-deposit") {
            type = TransactionType::REDEEM_SIDECHAIN;
            f.registry.total_issued = name == "redeem-supply" ? 10 * COIN : 20 * COIN;
            if (name == "redeem-deposit") f.deposit.amount = 10 * COIN;
        }
        selection_case(name, f, {operation(f, type, amount, 1700000040), operation(f, type, amount, 1700000041)},
                       name == "compatible-mints" ? 2 : 1);
    }
}

void test_committee_and_status_order() {
    for (const auto type : {TransactionType::UPDATE_SIDECHAIN_COMMITTEE, TransactionType::DECOMMISSION_SIDECHAIN}) {
        test::SidechainRegistryFixture f;
        f.registry.fee_bps = 0;
        f.registry.total_issued = 0;
        auto control = operation(f, type, 0, 1700000042);
        auto mint = operation(f, TransactionType::MINT_SIDECHAIN, COIN, 1700000043);
        const std::string name = type == TransactionType::UPDATE_SIDECHAIN_COMMITTEE ? "committee-rotation" : "decommission";
        selection_case(name, f, {control, mint}, 1);
        if (type == TransactionType::UPDATE_SIDECHAIN_COMMITTEE) {
            selection_case("mint-before-rotation", f, {mint, control}, 2);
        }
    }
}

core::BatchCheckpointItem checkpoint(const test::SidechainRegistryFixture& fixture,
                                   Height height, Amount fee_multiplier = 1) {
    core::BatchCheckpointItem item;
    item.sidechain_id = fixture.id;
    item.checkpoint_height = height;
    item.version = consensus::params::FINANCIAL_CHECKPOINT_VERSION;
    item.committee_id = fixture.registry.committee_id;
    item.fee = fee_multiplier * consensus::calculate_sidechain_checkpoint_min_fee(101, fixture.deposit.last_checkpoint_height);
    const auto supply = static_cast<uint64_t>(fixture.registry.total_issued);
    const auto root = consensus::sidechain_checkpoint::build_financial_supply_leaf_v2(fixture.id, height, supply);
    item.checkpoint = consensus::sidechain_checkpoint::build_financial_commitment_v2(fixture.id, root, height, supply);
    item.proof = consensus::sidechain_checkpoint::build_financial_proof_v2(root, height, supply, 0, {});
    item.committee_signatures = {crypto::sign_recoverable_message(
        crypto::sha256(core::build_sidechain_batch_checkpoint_payload_for_signing(item)), test::make_key(111))};
    return item;
}

Transaction batch(std::vector<core::BatchCheckpointItem> items) {
    Transaction tx;
    tx.type = TransactionType::BATCH_SIDECHAIN_CHECKPOINT;
    tx.sidechain_batch_checkpoints = std::move(items);
    return tx;
}

void test_checkpoints() {
    test::SidechainRegistryFixture a, b;
    a.registry.fee_bps = b.registry.fee_bps = 0;
    b.id = test::hash_from_seed(117);
    selection_case("checkpoint-interval", a, {batch({checkpoint(a, 121)}), batch({checkpoint(a, 122)})}, 1);
    selection_cases("failed-batch-releases-all-items", {a, b}, {
        batch({checkpoint(a, 121, 20)}),
        batch({checkpoint(b, 121, 4), checkpoint(a, 122, 4)}),
        batch({checkpoint(b, 122)})}, 2);
    const auto mint = operation(a, TransactionType::MINT_SIDECHAIN, COIN, 1700000045);
    const auto cp = batch({checkpoint(a, 121)});
    selection_case("mint-changes-checkpoint-supply", a, {mint, cp}, 1);
    selection_case("checkpoint-before-mint", a, {cp, mint}, 2);
}

void test_deposits_and_namespaces() {
    test::SidechainRegistryFixture a, b;
    a.registry.fee_bps = b.registry.fee_bps = 0;
    b.id = test::hash_from_seed(118);
    const auto registration = operation(a, TransactionType::REGISTER_ACCOUNTING_SIDECHAIN, 0, 1700000046);
    auto other_registration = operation(b, TransactionType::REGISTER_ACCOUNTING_SIDECHAIN, 0, 1700000047);
    selection_cases("duplicate-accounting-namespace", {a, b}, {registration, other_registration}, 1, false);
    other_registration.sidechain_namespace = "inventory";
    other_registration.sidechain_signature = crypto::sign_recoverable_message(
        crypto::sha256(core::build_sidechain_payload_for_signing(other_registration)), test::make_key(111));
    selection_cases("independent-accounting-namespaces", {a, b}, {registration, other_registration}, 2, false);
    const auto spend = operation(a, TransactionType::SPEND_DEPOSIT, COIN, 1700000048);
    selection_cases("registration-disables-deposit-spend", {a}, {registration, spend}, 1, false);
    selection_cases("deposit-spend-before-registration", {a}, {spend, registration}, 2, false);
    const auto unlock = operation(a, TransactionType::UNLOCK_DEPOSIT, 0, 1700000049);
    selection_cases("unlock-removes-deposit", {a}, {unlock, spend}, 1, false);

    a.registry.total_issued = 0;
    a.registry.issuance_limit = 10 * COIN;
    const auto funding = Transaction::create_coinbase(1, a.deposit.owner, 3 * COIN);
    Transaction top_up;
    top_up.type = TransactionType::TOP_UP_SIDECHAIN_DEPOSIT;
    top_up.sidechain_id = a.id;
    top_up.sidechain_owner = a.deposit.owner;
    top_up.sidechain_amount = COIN;
    top_up.inputs = {{funding.get_hash(), 0}};
    top_up.outputs = {{funding.outputs.front().value - COIN - 20000, a.deposit.owner, {}}};
    top_up.sidechain_signature = crypto::sign_recoverable_message(
        crypto::sha256(core::build_sidechain_payload_for_signing(top_up)), test::make_key(111));
    top_up.sign(test::make_key(111));
    selection_cases("top-up-expands-selected-issuance-capacity", {a}, {top_up,
        operation(a, TransactionType::MINT_SIDECHAIN, 6 * COIN, 1700000050),
        operation(a, TransactionType::MINT_SIDECHAIN, 6 * COIN, 1700000051)}, 3, true, {funding});
}
}

int main(int argc, char** argv) {
    try {
        consensus::SelectParams(consensus::Network::MAIN);
        for (bool rocksdb : {false, true}) {
            use_rocksdb = rocksdb;
            test_shared_limits();
            test_committee_and_status_order();
            test_checkpoints();
            test_deposits_and_namespaces();
        }
        size_t failed = 0;
        for (const auto& result : checks) {
            if (!result["passed"].get<bool>()) { ++failed; std::cerr << "FAILED: " << result["name"] << '\n'; }
        }
        if (argc > 1) std::ofstream(argv[1]) << nlohmann::json{
            {"checks", checks}, {"passed", checks.size() - failed}, {"failed", failed}}.dump(2) << '\n';
        std::cout << "sidechain mining checks: " << checks.size() - failed << '/' << checks.size() << '\n';
        return failed ? 1 : 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 2;
    }
}
