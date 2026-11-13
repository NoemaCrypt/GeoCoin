#include "test_utils.hpp"
#include "geocoin/consensus/chainparams.hpp"
#include "geocoin/consensus/consensus.hpp"
#include "geocoin/consensus/proof_of_work.hpp"
#include "geocoin/network/mempool.hpp"
#include "geocoin/state/records.hpp"
#include "geocoin/utils/bytes.hpp"
#include "nlohmann_json.hpp"

#include <fstream>
#include <functional>
#include <iostream>

namespace geocoin::state {
struct ChainStateTestAccess {
    static void add_holder(ChainState& state, const UTXO& utxo) {
        state.add_utxo(utxo);
        test::expect(state.persist_utxo_count_to_storage(), "persist holder count");
    }
    static bool build(const ChainState& state, Height height, std::vector<core::TxOutput>& outputs, Amount& total) {
        return state.build_interest_distribution_outputs_(height, outputs, total);
    }
    static bool apply(ChainState& state, const core::Transaction& tx, Height height) {
        return state.apply_interest_distribution_(tx, height);
    }
};
}

namespace {
using namespace geocoin;
using test::expect;
using core::Transaction;
using core::TransactionType;
constexpr Height due_height = consensus::params::INTEREST_POOL_ACTIVATION_HEIGHT +
    consensus::params::INTEREST_DISTRIBUTION_INTERVAL;
bool use_rocksdb = false;
nlohmann::json checks = nlohmann::json::array();

void check(bool result, const std::string& name) {
    checks.push_back({{"name", std::string(use_rocksdb ? "RocksDB: " : "MemoryStorage: ") + name}, {"passed", result}});
}

const Address& holder(uint8_t seed) {
    static std::map<uint8_t, Address> addresses;
    auto it = addresses.find(seed);
    if (it == addresses.end()) it = addresses.emplace(seed, test::address_from_seed(seed)).first;
    return it->second;
}

struct Fixture {
    std::filesystem::path path = test::make_temp_dir("geocoin-interest");
    std::shared_ptr<state::ChainState> state;
    Amount initial_pool;
    explicit Fixture(Amount pool = 17 * COIN + 1, bool holders = true) : initial_pool(pool) {
        auto storage = backend();
        const auto& params = consensus::Params();
        const state::FoundationConfig foundation{params.foundation_address(), params.foundation_m(),
            params.foundation_n(), params.foundation_participants()};
        expect(storage->put_metadata("idx_epoch", bytes(32, '2')), "seed index epoch");
        expect(storage->put_metadata("chainstate_utxo_count", utils::encode_u64_le(0)), "seed UTXO count");
        expect(storage->put_metadata("foundation_cfg", state::records::serialize_foundation_config(foundation)), "seed foundation");
        expect(storage->put_metadata("interest_pool_balance", utils::encode_u64_le(pool)), "seed pool");
        state = std::make_shared<state::ChainState>(std::move(storage));
        state->set_activation_manifest(utils::ActivationManifest::fully_enabled_for_tests("degree"));
        expect(state->set_height(due_height - 1), "seed preceding height");
        expect(state->set_tip_hash(test::hash_from_seed(92)), "seed tip");
        if (holders) {
            add_holder(1, holder(81), 6 * COIN);
            add_holder(2, holder(82), 4 * COIN);
        }
    }
    ~Fixture() {
        state.reset();
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
    std::unique_ptr<state::Storage> backend() const {
        if (!use_rocksdb) return std::make_unique<state::MemoryStorage>();
        auto result = std::make_unique<state::RocksDBStorage>((path / "state").string());
        expect(result->open(), "open isolated RocksDB");
        return result;
    }
    void restart() {
        std::unique_ptr<state::Storage> storage;
        if (!use_rocksdb) storage = std::make_unique<state::MemoryStorage>(
            *dynamic_cast<state::MemoryStorage*>(state->get_storage()));
        state.reset();
        if (use_rocksdb) storage = backend();
        state = std::make_shared<state::ChainState>(std::move(storage));
        state->set_activation_manifest(utils::ActivationManifest::fully_enabled_for_tests("degree"));
    }
    void add_holder(uint32_t index, const Address& address, Amount value, Height height = 1) {
        state::UTXO utxo;
        utxo.tx_hash = test::hash_from_seed(93);
        utxo.vout = index;
        utxo.address = address;
        utxo.value = value;
        utxo.height = height;
        state::ChainStateTestAccess::add_holder(*state, utxo);
    }
    Transaction distribution() const {
        Transaction tx;
        expect(state->create_interest_distribution_transaction(due_height, tx), "build distribution");
        tx.timestamp = 1700000000;
        return tx;
    }
    core::Block block(const std::vector<Transaction>& transactions, Amount fees = 0) const {
        const auto height = state->get_height() + 1;
        core::Block block;
        block.header.prev_block_hash = state->get_tip_hash();
        block.header.timestamp = 1700000000 + height;
        block.header.bits = 0x207fffff;
        block.transactions.push_back(Transaction::create_coinbase(height, holder(83),
            consensus::ProofOfWork::calculate_block_reward(height), fees, state->get_foundation_address()));
        block.transactions.insert(block.transactions.end(), transactions.begin(), transactions.end());
        block.header.merkle_root = block.calculate_merkle_root();
        block.header.witness_commitment = block.calculate_witness_commitment();
        return block;
    }
};

void test_mempool() {
    Fixture f;
    network::Mempool pool(f.state);
    auto tx = f.distribution();
    expect(f.state->validate_transaction(tx), "canonical distribution is consensus-valid");
    check(!pool.test_accept_transaction(tx), "system distribution is rejected by mempool preflight");
    check(!pool.add_transaction(tx) && pool.empty() && pool.get_transactions_for_mining(1000000).empty(),
          "system distribution never occupies mempool or user transaction budget");
}

void validation_case(const std::string& name, const std::function<void(Transaction&)>& mutate) {
    Fixture f;
    auto tx = f.distribution();
    mutate(tx);
    check(!f.state->validate_transaction(tx), name + ": state validation rejects malformed distribution");
    check(!f.state->apply_transaction(tx, due_height, false) && f.state->get_interest_pool_balance() == f.initial_pool,
          name + ": application rejects before debiting pool");
}

void test_apply_checks_exact_snapshot() {
    {
        Fixture f;
        auto tx = f.distribution();
        tx.outputs.front().address = holder(99);
        check(!state::ChainStateTestAccess::apply(*f.state, tx, due_height) &&
                  f.state->get_interest_pool_balance() == f.initial_pool &&
                  !f.state->has_utxo(tx.get_hash(), 0),
              "apply rejects a payout that does not match the scheduled snapshot");
    }
    {
        Fixture f;
        const auto tx = f.distribution();
        check(state::ChainStateTestAccess::apply(*f.state, tx, due_height) &&
                  f.state->get_interest_pool_balance() < f.initial_pool &&
                  f.state->has_utxo(tx.get_hash(), 0),
              "apply accepts the exact scheduled snapshot");
    }
}

void test_lifecycle() {
    Fixture f;
    const auto tx = f.distribution();
    std::map<std::string, Amount> payouts;
    for (const auto& out : tx.outputs) payouts[out.address.str()] = out.value;
    check(payouts[holder(81).str()] == 1020000000 && payouts[holder(82).str()] == 680000000,
          "integer shares use complete address balances and leave rounding remainder");
    check(tx.outputs[0].address.str() < tx.outputs[1].address.str(), "payout order is deterministic");
    const auto block = f.block({tx});
    const auto current_cut = consensus::calculate_foundation_reward_cut(consensus::ProofOfWork::calculate_block_reward(due_height));
    expect(f.state->apply_block(block, true), "apply actual due block");
    check(f.state->get_interest_pool_balance() == current_cut + 1 &&
              f.state->get_balance(holder(83)) == block.transactions.front().outputs.front().value,
          "new block reward does not enter its own distribution");
    f.restart();
    check(f.state->get_interest_pool_balance() == current_cut + 1 &&
              f.state->has_utxo(tx.get_hash(), 0) && f.state->has_utxo(tx.get_hash(), 1),
          "restart preserves pool remainder and every payout");
    check(f.state->revert_block(block) && f.state->get_interest_pool_balance() == f.initial_pool &&
              !f.state->has_utxo(tx.get_hash(), 0) && !f.state->has_utxo(tx.get_hash(), 1),
          "durable undo restores pool and removes payouts");
    f.restart();
    check(f.state->validate_transaction(tx) && f.state->apply_block(block, true),
          "reverted distribution can be reapplied after restart");
}

void test_output_limit() {
    Fixture f(2 * COIN, false);
    // Distinct fixed-length address strings model consensus-permitted burn
    // outputs; no wallet signing or mempool admission is used for this fixture.
    f.state->get_storage()->begin_batch();
    for (uint32_t i = 0; i <= consensus::params::MAX_TX_OUTPUTS; ++i) {
        f.add_holder(i, Address("GEOrecipient-" + std::to_string(10000 + i)), COIN);
    }
    expect(f.state->get_storage()->commit_batch(), "commit holder fixture");
    std::vector<core::TxOutput> outputs;
    Amount total = 1;
    check(!state::ChainStateTestAccess::build(*f.state, due_height, outputs, total),
          "unsupported recipient count does not produce a partial distribution");
    check(outputs.empty() && total == 0, "failed output calculation clears both result values");
}

void test_envelope_limits() {
    for (bool oversized : {true, false}) {
        Fixture f(oversized ? 2 * COIN : 21000000LL * COIN + 1, false);
        f.state->get_storage()->begin_batch();
        const uint32_t count = oversized ? consensus::params::MAX_TX_OUTPUTS : 1;
        for (uint32_t i = 0; i < count; ++i) {
            const Address address = oversized
                ? Address(std::string(120, 'a') + std::to_string(10000 + i)) : holder(81);
            f.add_holder(i, address, COIN);
        }
        expect(f.state->get_storage()->commit_batch(), "commit envelope fixture");
        Transaction tx;
        const auto label = oversized ? "serialized size limit" : "transaction monetary limit";
        check(!f.state->create_interest_distribution_transaction(due_height, tx),
              std::string(label) + ": builder does not return a consensus-invalid transaction");
        check(f.state->apply_block(f.block({}), true),
              std::string(label) + ": unrepresentable distribution does not make every block invalid");
    }
}

void test_block_rejection(const std::string& name, unsigned mode) {
    Fixture f(mode == 4 ? 0 : 17 * COIN + 1);
    Transaction tx;
    if (mode == 4) { tx.type = TransactionType::INTEREST_DISTRIBUTION; tx.outputs = {{COIN, holder(81), {}}}; }
    else tx = f.distribution();
    if (mode == 5) expect(f.state->set_height(due_height), "move past due height");
    std::vector<Transaction> transactions{tx};
    if (mode == 0) transactions.clear();
    if (mode == 1) { ++tx.timestamp; transactions.push_back(tx); }
    if (mode == 2) transactions.insert(transactions.begin(), Transaction{});
    if (mode == 3) ++transactions.front().outputs.front().value;
    const auto block = f.block(transactions);
    const auto snapshot = [&]() {
        std::map<std::string, bytes> result;
        for (const auto& key : f.state->get_storage()->get_keys_with_prefix("")) {
            expect(f.state->get_storage()->get_metadata(key, result[key]), "snapshot metadata");
        }
        return result;
    };
    const auto before = snapshot();
    const auto height = f.state->get_height();
    const auto tip = f.state->get_tip_hash();
    check(!f.state->apply_block(block, true), name + ": reject invalid block");
    check(snapshot() == before && f.state->get_height() == height && f.state->get_tip_hash() == tip &&
              f.state->get_interest_pool_balance() == f.initial_pool &&
              f.state->get_balance(holder(81)) == 6 * COIN && f.state->get_balance(holder(83)) == 0,
          name + ": rejection restores balances, pool, tip and every metadata record");
    f.restart();
    check(f.state->get_height() == height && f.state->get_interest_pool_balance() == f.initial_pool &&
              f.state->get_balance(holder(83)) == 0, name + ": rejected block leaves no changes after restart");
}

void test_payout_spent_in_same_block() {
    Fixture f;
    const auto distribution = f.distribution();
    size_t index = 0;
    while (distribution.outputs.at(index).address != holder(81)) ++index;
    const Amount fee = 10000;
    Transaction payment;
    payment.timestamp = distribution.timestamp + 1;
    payment.inputs = {{distribution.get_hash(), static_cast<uint32_t>(index)}};
    payment.outputs = {{distribution.outputs[index].value - fee, holder(84), {}}};
    payment.sign(test::make_key(81));
    const auto block = f.block({distribution, payment}, fee);
    const bool applied = f.state->apply_block(block, true);
    check(applied, "payout can fund a correctly signed payment later in the same block");
    if (!applied) return;
    const auto cut = consensus::calculate_foundation_reward_cut(
        consensus::ProofOfWork::calculate_block_reward(due_height) + fee);
    f.restart();
    check(f.state->get_balance(holder(84)) == payment.outputs.front().value &&
              f.state->get_interest_pool_balance() == cut + 1,
          "same-block payment and new fee contribution survive restart");
    check(f.state->revert_block(block) && f.state->get_interest_pool_balance() == f.initial_pool &&
              f.state->get_balance(holder(84)) == 0 && !f.state->has_utxo(distribution.get_hash(), index),
          "undo restores a consumed payout before removing the distribution");
}

void test_integer_shares() {
    const Amount maximum = (std::numeric_limits<Amount>::max)();
    // Golden payouts were computed with arbitrary-precision integer arithmetic:
    // floor(pool * balance / (a + b)). Avoid requiring __divti3 from ClangCL's
    // runtime just to evaluate the test oracle; production arithmetic is unchanged.
    const std::vector<std::array<Amount, 5>> vectors{
        {1000, 1, 1, 500, 500}, {2000, 1, 1, 1000, 1000},
        {3001, 1, 2, 1000, 2000}, {1000001, 499, 501, 499000, 501000},
        {21000000LL * COIN, maximum - 1, 1, 2099999999999999, 0},
        {21000000LL * COIN, maximum / 2, maximum / 2, 1050000000000000, 1050000000000000},
        {1234567890123, 123456789012345, 987654321098765, 137174208902, 1097393681220},
        {987654321, 1, maximum - 1, 0, 987654320}};
    for (size_t i = 0; i < vectors.size(); ++i) {
        const auto [pool, a, b, payout_a, payout_b] = vectors[i];
        Fixture f(pool, false);
        f.add_holder(1, holder(81), a);
        f.add_holder(2, holder(82), b);
        std::map<std::string, Amount> expected;
        Amount expected_total = 0;
        for (const auto& [address, amount] : std::vector<std::pair<Address, Amount>>{{holder(81), payout_a}, {holder(82), payout_b}}) {
            if (amount >= consensus::params::DUST_LIMIT) { expected[address.str()] = amount; expected_total += amount; }
        }
        std::vector<core::TxOutput> outputs;
        Amount total = 0;
        const bool built = state::ChainStateTestAccess::build(*f.state, due_height, outputs, total);
        std::map<std::string, Amount> actual;
        for (const auto& out : outputs) actual[out.address.str()] = out.value;
        check(built == (expected_total > 0) && total == expected_total && actual == expected,
              "exact integer payout oracle vector " + std::to_string(i));
    }
}

void run() {
    test_mempool();
    validation_case("extra input", [](auto& tx) { tx.inputs.push_back({test::hash_from_seed(94), 0}); });
    validation_case("wrong transaction version", [](auto& tx) { tx.version = 3; });
    validation_case("wrong witness version", [](auto& tx) { ++tx.witness_version; });
    validation_case("wrong amount", [](auto& tx) { ++tx.outputs[0].value; });
    validation_case("wrong ordering", [](auto& tx) { std::reverse(tx.outputs.begin(), tx.outputs.end()); });
    validation_case("output script", [](auto& tx) { tx.outputs[0].script_pubkey = {1}; });
    test_apply_checks_exact_snapshot();
    test_lifecycle();
    test_output_limit();
    test_envelope_limits();
    test_block_rejection("missing mandatory distribution", 0);
    test_block_rejection("duplicate distribution", 1);
    test_block_rejection("wrong transaction position", 2);
    test_block_rejection("incorrect payout", 3);
    test_block_rejection("unexpected payout from empty pool", 4);
    test_block_rejection("distribution outside schedule", 5);
    test_payout_spent_in_same_block();
    test_integer_shares();
}
}

int main(int argc, char** argv) {
    try {
        consensus::SelectParams(consensus::Network::MAIN);
        for (bool rocksdb : {false, true}) { use_rocksdb = rocksdb; run(); }
        size_t passed = 0;
        for (const auto& item : checks) if (item.at("passed").get<bool>()) ++passed;
        const nlohmann::json result{{"passed", passed}, {"failed", checks.size() - passed}, {"checks", checks}};
        if (argc > 1) { std::ofstream output(argv[1]); output << result.dump(2) << '\n'; }
        std::cout << result.dump(2) << '\n';
        return passed == checks.size() ? 0 : 1;
    } catch (const std::exception& e) {
        std::cerr << "test_interest_distribution: " << e.what() << '\n';
        return 2;
    }
}
