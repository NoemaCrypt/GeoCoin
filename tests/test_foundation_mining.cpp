#include "test_utils.hpp"
#include "geocoin/consensus/chainparams.hpp"
#include "geocoin/consensus/consensus.hpp"
#include "geocoin/consensus/proof_of_work.hpp"
#include "geocoin/crypto/signatures.hpp"
#include "geocoin/network/mempool.hpp"
#include "geocoin/state/records.hpp"
#include "geocoin/utils/bytes.hpp"
#include "nlohmann_json.hpp"

#include <algorithm>
#include <fstream>
#include <functional>
#include <iostream>

namespace {
using namespace geocoin;
using test::expect;
using core::Transaction;
using core::TransactionType;
using state::FoundationConfig;
nlohmann::json checks = nlohmann::json::array();
bool use_rocksdb = false;

void check(bool result, const std::string& name) {
    checks.push_back({{"name", std::string(use_rocksdb ? "RocksDB: " : "MemoryStorage: ") + name},
                      {"passed", result}});
}

const Address& member(uint8_t seed) {
    static std::map<uint8_t, Address> addresses;
    auto it = addresses.find(seed);
    if (it == addresses.end()) it = addresses.emplace(seed, test::address_from_seed(seed)).first;
    return it->second;
}

FoundationConfig config(uint8_t quorum, std::initializer_list<uint8_t> seeds) {
    FoundationConfig result;
    result.m = quorum;
    for (auto seed : seeds) result.participants.push_back(member(seed));
    result.n = static_cast<uint8_t>(result.participants.size());
    result.address = Address(multisig::create_multisig_address(result.m, result.n, result.participants).address);
    return result;
}

void sign(Transaction& tx, std::initializer_list<uint8_t> signers = {81, 82}) {
    tx.foundation_signatures.clear();
    const auto hash = crypto::sha256(core::build_foundation_update_payload_for_signing(tx));
    for (auto seed : signers) tx.foundation_signatures.push_back(crypto::sign_recoverable_message(hash, test::make_key(seed)));
}

Transaction update(const FoundationConfig& from, const FoundationConfig& to, Timestamp timestamp) {
    Transaction tx;
    tx.type = TransactionType::UPDATE_FOUNDATION_ADDRESS;
    tx.timestamp = timestamp;
    tx.foundation_old_address = from.address;
    tx.foundation_m = from.m;
    tx.foundation_n = from.n;
    tx.foundation_participants = from.participants;
    tx.foundation_new_address = to.address;
    tx.foundation_new_m = to.m;
    tx.foundation_new_n = to.n;
    tx.foundation_new_participants = to.participants;
    sign(tx);
    return tx;
}

struct Fixture {
    std::filesystem::path path = test::make_temp_dir("geocoin-foundation-mining");
    FoundationConfig initial = config(2, {81, 82});
    std::shared_ptr<state::ChainState> state;

    Fixture() {
        auto storage = backend();
        expect(storage->put_metadata("idx_epoch", bytes(32, '1')), "seed index epoch");
        expect(storage->put_metadata("chainstate_utxo_count", utils::encode_u64_le(0)), "seed UTXO count");
        expect(storage->put_metadata("foundation_cfg", state::records::serialize_foundation_config(initial)), "seed foundation");
        expect(storage->put_metadata("interest_pool_balance", utils::encode_u64_le(0)), "seed interest pool");
        state = std::make_shared<state::ChainState>(std::move(storage));
        state->set_activation_manifest(utils::ActivationManifest::fully_enabled_for_tests("degree"));
        expect(state->set_height(200), "seed height");
        expect(state->set_tip_hash(test::hash_from_seed(90)), "seed tip");
    }
    ~Fixture() {
        state.reset();
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
    std::unique_ptr<state::Storage> backend() const {
        if (!use_rocksdb) return std::make_unique<state::MemoryStorage>();
        auto storage = std::make_unique<state::RocksDBStorage>((path / "state").string());
        expect(storage->open(), "open isolated RocksDB");
        return storage;
    }
    void restart() {
        std::unique_ptr<state::Storage> storage;
        if (!use_rocksdb) {
            storage = std::make_unique<state::MemoryStorage>(
                *dynamic_cast<state::MemoryStorage*>(state->get_storage()));
        }
        state.reset();
        if (use_rocksdb) storage = backend();
        state = std::make_shared<state::ChainState>(std::move(storage));
        state->set_activation_manifest(utils::ActivationManifest::fully_enabled_for_tests("degree"));
    }
    core::Block block(std::vector<Transaction> transactions, std::optional<Address> reward_address = std::nullopt) const {
        const Height height = state->get_height() + 1;
        core::Block block;
        block.header.prev_block_hash = state->get_tip_hash();
        block.header.timestamp = 1700000000 + height;
        block.header.bits = 0x207fffff;
        block.transactions.push_back(Transaction::create_coinbase(height, member(85),
            consensus::ProofOfWork::calculate_block_reward(height), 0,
            reward_address.value_or(state->get_foundation_address())));
        block.transactions.insert(block.transactions.end(), transactions.begin(), transactions.end());
        block.header.merkle_root = block.calculate_merkle_root();
        block.header.witness_commitment = block.calculate_witness_commitment();
        return block;
    }
};

void selection_case(const std::string& name, const std::vector<FoundationConfig>& destinations,
                    const std::vector<size_t>& expected, bool skip_oversized = false) {
    Fixture f;
    auto pool = std::make_unique<network::Mempool>(f.state);
    std::vector<Transaction> candidates;
    for (size_t i = 0; i < destinations.size(); ++i) {
        candidates.push_back(update(f.initial, destinations[i], 1700000100 + i));
        expect(pool->add_transaction(candidates.back()), name + ": admit independently valid approval");
    }
    const auto hashes = [](const auto& txs) {
        std::vector<Hash256> result;
        for (const auto& tx : txs) result.push_back(tx.get_hash());
        return result;
    };
    std::vector<Hash256> expected_hashes;
    for (auto index : expected) expected_hashes.push_back(candidates.at(index).get_hash());
    const size_t max_size = skip_oversized ? candidates.at(1).serialize().size() : 1000000;
    if (skip_oversized) expect(candidates.front().serialize().size() > max_size, "first candidate exceeds budget");
    const auto selected = pool->get_transactions_for_mining(max_size);
    check(hashes(selected) == expected_hashes, name + ": select compatible rotations in admission order at equal fees");
    check(hashes(pool->get_transactions_for_mining(max_size)) == hashes(selected) &&
              pool->get_transactions_for_mining(0).empty(), name + ": repeated and empty templates are independent");
    check(pool->size() == candidates.size() && f.state->get_foundation_address() == f.initial.address &&
              f.state->get_height() == 200, name + ": selection preserves confirmed state and mempool");
    auto block = f.block(selected);
    const bool applied = f.state->apply_block(block, true);
    check(applied, name + ": selected template applies with normal transaction and reward checks");
    if (!applied) return;
    pool.reset();
    f.restart();
    const auto last = expected.empty() ? f.initial : destinations[expected.back()];
    check(f.state->get_foundation_address() == last.address && f.state->get_height() == 201,
          name + ": configuration survives reconstruction from stored records");
    check(f.state->revert_block(block) && f.state->get_foundation_address() == f.initial.address &&
              f.state->get_height() == 200, name + ": stored undo restores the complete rotation sequence");
    f.restart();
    network::Mempool restored_pool(f.state);
    bool can_resubmit = true;
    for (const auto& tx : candidates) can_resubmit = restored_pool.add_transaction(tx) && can_resubmit;
    check(can_resubmit, name + ": undo releases approvals durably for resubmission");
}

void validation_case(const std::string& name, Transaction tx, bool expected) {
    Fixture f;
    network::Mempool pool(f.state);
    check(f.state->validate_transaction(tx) == expected, name + ": ChainState validation agrees");
    check(pool.test_accept_transaction(tx) == expected, name + ": mempool admission agrees");
    check(f.state->apply_transaction(tx, 201, false) == expected, name + ": application agrees");
    if (!expected) check(f.state->get_foundation_address() == f.initial.address &&
                            !f.state->has_protocol_authorization_key(*tx.get_protocol_authorization_hash()),
                        name + ": rejection preserves authority and unconsumed approval");
}

// Relay always enforces strict signature lists. Blocks keep the original rule
// below the activation height, so established history stays valid.
void hardening_case(const std::string& name, const Transaction& tx) {
    Fixture f;
    network::Mempool pool(f.state);
    check(!f.state->validate_transaction(tx), name + ": relay validation rejects the list");
    check(!pool.test_accept_transaction(tx), name + ": mempool admission rejects the list");
    check(f.state->apply_transaction(tx, 201, false),
          name + ": a block below the activation height keeps the original rule");

    Fixture g;
    const Height activation = consensus::Params().transaction_hardening_activation_height();
    expect(g.state->set_height(activation - 1), "advance to the hardening boundary");
    check(!g.state->apply_transaction(tx, activation, false) &&
              g.state->get_foundation_address() == g.initial.address,
          name + ": a block at the activation height rejects the list");
}

void undo_case(const std::string& name, const std::function<void(state::BlockUndo&)>& corrupt) {
    Fixture f;
    const auto b = config(1, {81, 82});
    const auto c = config(2, {83, 84});
    auto second = update(b, c, 1700000301);
    sign(second, {81});
    const auto block = f.block({update(f.initial, b, 1700000300), second});
    expect(f.state->apply_block(block, true), "apply two consecutive authority changes");
    state::BlockUndo original;
    expect(f.state->load_block_undo(block.get_hash(), original), "load actual durable undo");
    auto changed = original;
    corrupt(changed);
    expect(f.state->save_block_undo(changed), "write malformed undo fixture");
    f.restart();
    const auto snapshot = [&]() {
        std::map<std::string, bytes> result;
        for (const auto& key : f.state->get_storage()->get_keys_with_prefix("")) {
            expect(f.state->get_storage()->get_metadata(key, result[key]), "snapshot stored record");
        }
        return result;
    };
    const auto before = snapshot();
    const bool rejected = !f.state->revert_block(block);
    check(rejected, name + ": malformed undo is rejected");
    bool intact = rejected && snapshot() == before && f.state->get_height() == 201 &&
        f.state->get_tip_hash() == block.get_hash() && f.state->get_foundation_address() == c.address;
    for (const auto& tx : block.transactions) intact = intact && f.state->get_transaction(tx.get_hash()) != nullptr;
    check(intact, name + ": failure precedes every state and storage mutation");
    if (!intact) return;
    expect(f.state->save_block_undo(original), "restore original undo");
    check(f.state->revert_block(block) && f.state->get_foundation_address() == f.initial.address,
          name + ": intact undo remains usable after rejection");
}

void lifecycle_case() {
    Fixture f;
    const auto b = config(1, {81, 82});
    auto back = update(b, f.initial, 1700000401);
    sign(back, {81});
    const auto forward = update(f.initial, b, 1700000400);
    const auto first = f.block({forward});
    check(first.transactions.front().outputs.back().address == f.initial.address,
          "rotation block rewards the preceding foundation");
    auto wrong_reward = f.block({forward}, b.address);
    check(!f.state->apply_block(wrong_reward, true) && f.state->get_foundation_address() == f.initial.address,
          "rotation block rejects premature reward address without mutation");
    expect(f.state->apply_block(first, true), "apply rotation block");
    auto second = f.block({back});
    check(second.transactions.front().outputs.back().address == b.address,
          "next block rewards the new foundation");
    expect(f.state->apply_block(second, true), "apply authorized return to original address");
    f.restart();
    auto replay = forward;
    std::reverse(replay.foundation_signatures.begin(), replay.foundation_signatures.end());
    check(!f.state->validate_transaction(replay), "return and restart preserve consumed canonical approval");
    check(f.state->revert_block(second) && f.state->get_foundation_address() == b.address,
          "undo of return restores intermediate authority");
    f.restart();
    check(f.state->revert_block(first) && f.state->get_foundation_address() == f.initial.address &&
              f.state->validate_transaction(forward), "undo of first rotation restores its original approval");
}

void run_cases() {
    const auto a = config(2, {81, 82});
    const auto b = config(1, {81, 82});
    const auto c = config(2, {83, 84});
    selection_case("two conflicting destinations", {b, c}, {0});
    selection_case("two distinct approvals for one destination", {b, b}, {0});
    selection_case("noop after authority change", {b, a}, {0});
    selection_case("noop before authority change", {a, b}, {0, 1});
    selection_case("two independent noops", {a, a}, {0, 1});
    selection_case("noop then conflicting destinations", {a, b, c}, {0, 1});
    // A larger destination policy makes the first candidate exceed the budget.
    selection_case("oversized rotation does not reserve authority", {config(2, {83, 84, 85}), c}, {1}, true);

    const auto valid = update(a, b, 1700000200);
    validation_case("valid quorum", valid, true);
    auto changed = valid;
    changed.foundation_signatures.push_back({1, 2, 3});
    validation_case("malformed extra signature", changed, false);
    changed = valid;
    changed.inputs = {{test::hash_from_seed(99), 0}};
    sign(changed);
    validation_case("unexpected UTXO input", changed, false);
    changed = valid;
    changed.outputs = {{COIN, member(85), {}}};
    sign(changed);
    validation_case("unexpected UTXO output", changed, false);
    changed = valid;
    changed.version = 3;
    sign(changed);
    validation_case("unsupported envelope version", changed, false);

    auto invalid = config(2, {83, 84});
    invalid.participants[0] = Address("GEOinvalid-checksum");
    invalid.address = Address(multisig::create_multisig_address(2, 2, invalid.participants).address);
    validation_case("invalid new participant address", update(a, invalid, 1700000201), false);
    invalid.participants = {member(83), member(83)};
    invalid.address = Address(multisig::create_multisig_address(2, 2, invalid.participants).address);
    validation_case("duplicate new participants", update(a, invalid, 1700000202), false);

    changed = valid;
    sign(changed, {81});
    validation_case("insufficient quorum", changed, false);
    sign(changed, {81, 81});
    validation_case("duplicate signer cannot satisfy quorum", changed, false);
    sign(changed, {83, 84});
    validation_case("unrelated signers cannot satisfy quorum", changed, false);
    // Hardening soft fork: a third party could append its own valid signature
    // or repeat a participant's one to change the txid of a signed approval.
    sign(changed, {81, 82, 83});
    hardening_case("non-participant extra signature", changed);
    sign(changed, {81, 82, 81});
    hardening_case("repeated participant signature", changed);
    changed = valid;
    ++changed.timestamp;
    validation_case("changed signed payload", changed, false);
    changed = valid;
    std::reverse(changed.foundation_participants.begin(), changed.foundation_participants.end());
    std::reverse(changed.foundation_new_participants.begin(), changed.foundation_new_participants.end());
    sign(changed);
    validation_case("participant order does not change multisig authority", changed, true);

    state::FoundationConfig decoded;
    check(!state::records::deserialize_foundation_config(state::records::serialize_foundation_config(invalid), decoded),
          "stored configuration rejects duplicate participants");
    invalid.participants = {Address("GEOinvalid-checksum"), member(84)};
    invalid.address = Address(multisig::create_multisig_address(2, 2, invalid.participants).address);
    check(!state::records::deserialize_foundation_config(state::records::serialize_foundation_config(invalid), decoded),
          "stored configuration rejects invalid participant address");
    check(state::records::deserialize_foundation_config(state::records::serialize_foundation_config(a), decoded) &&
              decoded.address == a.address && decoded.m == a.m && decoded.participants == a.participants,
          "valid configuration round trips through the current record codec");

    lifecycle_case();
    undo_case("missing foundation undo", [](auto& undo) { undo.tx_undos[1].foundation_undo = {}; });
    undo_case("unrelated previous policy", [](auto& undo) {
        const auto other = config(1, {83, 84});
        auto& item = undo.tx_undos[1].foundation_undo;
        item.prev_address = other.address.str(); item.prev_m = other.m; item.prev_n = other.n;
        item.prev_participants.clear();
        for (const auto& p : other.participants) item.prev_participants.push_back(p.str());
    });
    undo_case("broken previous quorum", [](auto& undo) { undo.tx_undos[1].foundation_undo.prev_m = 0; });
    undo_case("foundation undo on coinbase", [](auto& undo) {
        undo.tx_undos[0].foundation_undo = undo.tx_undos[1].foundation_undo;
    });
}
}

int main(int argc, char** argv) {
    try {
        consensus::SelectParams(consensus::Network::MAIN);
        for (bool rocksdb : {false, true}) { use_rocksdb = rocksdb; run_cases(); }
        size_t passed = 0;
        for (const auto& item : checks) if (item.at("passed").get<bool>()) ++passed;
        const nlohmann::json result{{"passed", passed}, {"failed", checks.size() - passed}, {"checks", checks}};
        if (argc > 1) { std::ofstream output(argv[1]); output << result.dump(2) << '\n'; }
        std::cout << result.dump(2) << '\n';
        return passed == checks.size() ? 0 : 1;
    } catch (const std::exception& e) {
        std::cerr << "test_foundation_mining: " << e.what() << '\n';
        return 2;
    }
}
