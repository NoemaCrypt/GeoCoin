#include "chain_test_fixture.hpp"
#include "geocoin/network/mempool.hpp"
#include "geocoin/node/chain_coordinator.hpp"
#include "geocoin/rpc/rpc_blockchain_format.hpp"
#include "geocoin/utils/bytes.hpp"
#include "nlohmann_json.hpp"

#include <chrono>
#include <fstream>
#include <iostream>
#include <limits>

namespace geocoin::state {
struct ChainStateTestAccess {
    static bool revert(ChainState& state, const core::Transaction& tx, const TxUndo& undo) {
        return state.revert_transaction(tx, undo);
    }
};
}

namespace {
using namespace geocoin;
using test::expect;
using core::Transaction;
nlohmann::json checks = nlohmann::json::array();
bool use_rocksdb = false;
constexpr uint32_t final_sequence = 0xFFFFFFFFu;
constexpr uint32_t enabled_sequence = 0xFFFFFFFEu;
constexpr Amount fee = 20000;

void check(bool ok, const std::string& name) {
    checks.push_back({{"name", std::string(use_rocksdb ? "RocksDB: " : "MemoryStorage: ") + name},
                      {"passed", ok}});
}

std::unique_ptr<state::Storage> backend(const std::filesystem::path& path) {
    if (!use_rocksdb) return std::make_unique<state::MemoryStorage>();
    auto storage = std::make_unique<state::RocksDBStorage>((path / "state").string());
    expect(storage->open(), "open isolated RocksDB");
    return storage;
}

struct Fixture {
    std::filesystem::path path = test::make_temp_dir("geocoin-finality");
    std::unique_ptr<test::TestChain> chain = std::make_unique<test::TestChain>(backend(path));
    Transaction funding;
    Fixture() {
        for (Height height = 1; height <= 100; ++height) {
            auto block = chain->make_block();
            if (height == 1) funding = block.transactions.front();
            chain->append(block);
        }
    }
    ~Fixture() {
        chain.reset();
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
    void restart() {
        chain->chain.reset();
        std::unique_ptr<state::Storage> storage;
        if (!use_rocksdb) storage = std::make_unique<state::MemoryStorage>(
            *dynamic_cast<state::MemoryStorage*>(chain->state->get_storage()));
        chain->state.reset();
        if (use_rocksdb) storage = backend(path);
        chain->state = std::make_shared<state::ChainState>(std::move(storage));
        chain->state->set_block_storage(chain->blocks);
        chain->chain = std::make_shared<core::Blockchain>(chain->state, chain->blocks);
        expect(chain->chain->initialize(), "reload active chain and state");
    }
    Transaction payment(uint32_t lock_time, uint32_t sequence = enabled_sequence) const {
        Transaction tx;
        tx.timestamp = chain->chain->get_tip()->get_block_time();
        tx.inputs = {{funding.get_hash(), 0}};
        tx.inputs.front().sequence = sequence;
        tx.outputs = {{funding.outputs.front().value - fee, test::address_from_seed(71), {}}};
        tx.lock_time = lock_time;
        tx.sign(test::make_key(42));
        return tx;
    }
    core::Block block(const Transaction& tx, Timestamp time) const {
        auto block = chain->make_block();
        block.header.timestamp = static_cast<uint32_t>(time);
        block.transactions.front() = Transaction::create_coinbase(chain->state->get_height() + 1,
            test::address_from_seed(42),
            consensus::ProofOfWork::calculate_block_reward(chain->state->get_height() + 1), fee,
            chain->state->get_foundation_address());
        block.transactions.push_back(tx);
        test::TestChain::seal(block, chain->chain->get_tip());
        return block;
    }
};

void test_candidate(Fixture& f, const std::string& label, uint32_t lock_time,
                    uint32_t sequence, Timestamp block_time, bool expected) {
    const auto tx = f.payment(lock_time, sequence);
    const auto block = f.block(tx, block_time);
    auto& state = *f.chain->state;
    consensus::ValidationState validation;
    check(consensus::check_transaction(tx, validation) && tx.verify(), label + ": format and signatures remain valid");
    validation.reset();
    check(consensus::contextual_check_block(block, validation, f.chain->chain->get_tip()) == expected,
          label + ": contextual block check enforces finality");
    const bool applied = state.apply_block(block, validation, true);
    check(applied == expected, label + ": block application enforces finality");
    if (applied) expect(state.revert_block(block), "undo candidate block");
    state::UTXO input;
    check(state.get_height() == 100 && state.get_tip_hash() == f.chain->chain->get_tip()->get_block_hash() &&
              state.get_utxo(f.funding.get_hash(), 0, input), label + ": rejection or undo preserves input and tip");
}

void test_height_and_time() {
    Fixture f;
    const Timestamp block_time = f.chain->make_block().header.timestamp;
    test_candidate(f, "unlocked", 0, enabled_sequence, block_time, true);
    test_candidate(f, "elapsed height", 100, enabled_sequence, block_time, true);
    test_candidate(f, "equal height", 101, enabled_sequence, block_time, false);
    test_candidate(f, "future height", 102, enabled_sequence, block_time, false);
    test_candidate(f, "disabled height lock", 102, final_sequence, block_time, true);
    test_candidate(f, "elapsed time", static_cast<uint32_t>(block_time - 1), enabled_sequence, block_time, true);
    test_candidate(f, "equal time", static_cast<uint32_t>(block_time), enabled_sequence, block_time, false);
    test_candidate(f, "future time", static_cast<uint32_t>(block_time + 1), enabled_sequence, block_time, false);
    test_candidate(f, "disabled time lock", static_cast<uint32_t>(block_time + 1), final_sequence, block_time, true);

    const auto locked = f.payment(101);
    auto changed = locked;
    changed.lock_time = 100;
    check(!changed.verify(), "signed payment rejects a changed lock_time");
    changed = locked;
    changed.inputs.front().sequence = final_sequence;
    check(!changed.verify(), "signed payment rejects disabling its lock after signing");
    check(!f.chain->state->validate_transaction(locked), "standalone validation rejects a future height lock");
    state::UTXO input;
    expect(f.chain->state->get_utxo(f.funding.get_hash(), 0, input), "read direct-application input");
    state::TxUndo undo;
    undo.spent_utxos.push_back(input);
    const auto direct = f.payment(100);
    bool applied = f.chain->state->apply_transaction(direct, 100, false);
    check(!applied, "direct application checks its supplied height, not the next chain height");
    if (applied) expect(state::ChainStateTestAccess::revert(*f.chain->state, direct, undo), "undo premature direct call");
    applied = f.chain->state->apply_transaction(direct, 101, false);
    check(applied, "direct application accepts the first eligible height");
    if (applied) expect(state::ChainStateTestAccess::revert(*f.chain->state, direct, undo), "undo eligible direct call");

    auto reward = Transaction::create_coinbase(101, test::address_from_seed(42), 50 * COIN);
    reward.lock_time = 101;
    reward.inputs.front().sequence = enabled_sequence;
    check(!f.chain->state->validate_transaction(reward) &&
              !f.chain->state->apply_transaction(reward, 101, true),
          "coinbase has no direct-application exemption from an enabled lock");
    const auto timed = f.payment(500000000u);
    const auto zero_time = f.block(timed, 0);
    consensus::ValidationState validation;
    check(!f.chain->state->apply_block(zero_time, validation, true),
          "explicit zero block time cannot fall back to the wall clock");
    check(f.chain->state->validate_transaction(timed) && f.chain->state->has_utxo(f.funding.get_hash(), 0),
          "early rejection restores standalone time context and preserves funding");
    auto empty = f.chain->make_block();
    empty.transactions.clear();
    test::TestChain::seal(empty, f.chain->chain->get_tip());
    validation.reset();
    check(!consensus::contextual_check_block(empty, validation, f.chain->chain->get_tip()) &&
              validation.get_reject_reason() == "bad-cb-missing", "empty contextual block fails cleanly");

    const auto decoded = rpc::transaction_to_verbose_json(locked, locked.get_hash(),
        utils::hash256_to_hex(locked.get_hash()), *f.chain->state);
    check(decoded.value("lock_time", uint32_t{0}) == locked.lock_time,
          "verbose decoding exposes the signed lock_time");
    check(decoded["inputs"][0].value("sequence", uint32_t{0}) == enabled_sequence,
          "verbose decoding exposes each signed sequence");
}

void test_mempool() {
    Fixture f;
    auto& state = *f.chain->state;
    const Timestamp now = static_cast<Timestamp>(std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    for (const uint32_t lock_time : {uint32_t{101}, static_cast<uint32_t>(now + 3600)}) {
        network::Mempool pool(f.chain->state);
        const auto tx = f.payment(lock_time);
        check(!pool.add_transaction(tx) && pool.empty(), "mempool rejects future lock " + std::to_string(lock_time));
        const auto stats = pool.get_reject_stats();
        check(stats.policy.count("non-final") > 0, "mempool reports non-final instead of signature failure");
    }
    f.chain->append(f.chain->make_block());
    network::Mempool pool(f.chain->state);
    const auto eligible = f.payment(101);
    expect(pool.add_transaction(eligible), "admit eligible height-locked transaction");
    check(pool.get_transactions_for_mining(1000000).size() == 1, "eligible height lock enters mining selection");
    expect(state.set_height(100), "set earlier height while funding remains mature");
    check(pool.get_transactions_for_mining(1000000).empty(), "selection rechecks height after a change");
    expect(state.set_height(101), "restore height");

    pool.clear();
    const Timestamp candidate_time = f.chain->make_block().header.timestamp;
    const auto timed = f.payment(static_cast<uint32_t>(candidate_time));
    expect(pool.add_transaction(timed), "admit time lock already elapsed on the wall clock");
    check(pool.get_transactions_for_mining(1000000, candidate_time).empty(),
          "selection uses candidate time rather than admission time");
    check(pool.get_transactions_for_mining(1000000, candidate_time + 1).size() == 1,
          "selection accepts a time lock after its strict boundary");
    check(pool.size() == 1, "selection does not discard a transaction valid for a later candidate");
}

void test_restart_and_reorg() {
    Fixture f;
    const Hash256 height100 = f.chain->chain->get_tip()->get_block_hash();
    const auto tx = f.payment(101);
    auto premature = f.block(tx, f.chain->make_block().header.timestamp);
    consensus::ValidationState validation;
    check(!f.chain->chain->process_new_block(premature, validation) &&
              validation.get_reject_reason() == "bad-txns-nonfinal",
          "full block processing rejects an early signed payment");
    f.chain->append(f.chain->make_block());
    const Hash256 height101 = f.chain->chain->get_tip()->get_block_hash();
    const auto eligible = f.block(tx, f.chain->make_block().header.timestamp);
    f.chain->append(eligible);
    check(!f.chain->state->has_utxo(f.funding.get_hash(), 0), "eligible payment consumes its funding");
    f.restart();
    const auto stored = f.chain->state->get_transaction(tx.get_hash());
    check(stored && stored->lock_time == 101 && stored->inputs.front().sequence == enabled_sequence && stored->verify(),
          "restart retains the signed lock and sequence in the stored transaction");
    check(f.chain->state->get_height() == 102 && f.chain->state->get_tip_hash() == eligible.get_hash(),
          "restart retains the eligible block as the active tip");

    auto pool = std::make_shared<network::Mempool>(f.chain->state);
    node::ChainCoordinator coordinator(f.chain->chain, pool, [](services::ChainMutationEvent) { return true; });
    expect(coordinator.start(), "start isolated coordinator");
    expect(coordinator.activate_tip(height101).is_accepted(), "disconnect eligible payment");
    check(pool->has_transaction(tx.get_hash()) && pool->get_transactions_for_mining(1000000).size() == 1,
          "disconnect re-admits a payment still final for the next block");
    expect(coordinator.activate_tip(height100).is_accepted(), "disconnect below the finality boundary");
    check(!pool->has_transaction(tx.get_hash()) && pool->get_transactions_for_mining(1000000).empty(),
          "a further disconnect removes a payment that became non-final");
    check(f.chain->state->has_utxo(f.funding.get_hash(), 0) && !f.chain->state->validate_transaction(tx),
          "rollback restores mature funds without allowing the payment early");
    coordinator.stop();
}

void test_multisig_package() {
    Fixture f;
    auto& state = *f.chain->state;
    state.set_activation_manifest(utils::ActivationManifest::fully_enabled_for_tests("degree-load"));
    const auto member = test::address_from_seed(43);
    const Address owner(multisig::create_multisig_address(1, 1, {member}).address);
    auto funding = f.payment(0);
    funding.outputs.front().address = owner;
    funding.sign(test::make_key(42));
    f.chain->append(f.block(funding, f.chain->make_block().header.timestamp));
    auto spend = Transaction::create_multisig_spend({{funding.get_hash(), 0}},
        {{funding.outputs.front().value - fee, test::address_from_seed(71), {}}}, {}, 1, 1);
    spend.spend_participants = {member};
    spend.lock_time = 103;
    spend.inputs.front().sequence = enabled_sequence;
    const auto commitment = Transaction::create_angle_commitment(owner, spend.get_hash_without_commitments(),
        test::make_key(43), {}, "degree-load");
    spend.angle_commitment_hashes = {commitment.get_hash()};
    network::Mempool pool(f.chain->state);
    expect(pool.add_transaction(commitment), "admit multisig commitment");
    check(!pool.add_transaction(spend), "multisig root is not admitted before its lock height");
    f.chain->append(f.chain->make_block());
    f.chain->append(f.chain->make_block());
    expect(pool.add_transaction(spend), "admit the same signed root after its lock height");
    auto selected = pool.get_transactions_for_mining(1000000);
    check(selected.size() == 2 && selected.front().get_hash() == commitment.get_hash() &&
              selected.back().get_hash() == spend.get_hash(), "eligible multisig package remains ordered and complete");
    expect(state.set_height(102), "set earlier height with confirmed non-coinbase funding");
    check(pool.get_transactions_for_mining(1000000).empty() && pool.size() == 2,
          "non-final root leaves no orphan commitment in the template");
    expect(state.set_height(103), "restore multisig height");
    check(pool.get_transactions_for_mining(1000000).size() == 2,
          "the complete package becomes selectable again after the boundary");
}

void test_numeric_boundaries() {
    struct Case { const char* name; uint32_t lock; Height height; Timestamp time; std::vector<uint32_t> sequence; bool final; };
    const auto max = (std::numeric_limits<uint64_t>::max)();
    const std::vector<Case> cases{
        {"zero lock", 0, 0, 0, {0}, true},
        {"height at threshold minus one", 499999999u, 499999999u, max, {enabled_sequence}, false},
        {"height passed at threshold", 499999999u, 500000000u, 0, {enabled_sequence}, true},
        {"threshold selects time", 500000000u, max, 499999999u, {enabled_sequence}, false},
        {"time equality", 500000000u, max, 500000000u, {enabled_sequence}, false},
        {"time passed", 500000000u, 0, 500000001u, {enabled_sequence}, true},
        {"max lock equality", 0xFFFFFFFFu, max, 0xFFFFFFFFu, {enabled_sequence}, false},
        {"max lock passed", 0xFFFFFFFFu, 0, uint64_t{1} + 0xFFFFFFFFu, {enabled_sequence}, true},
        {"wide height", 499999999u, max, 0, {enabled_sequence}, true},
        {"wide time", 0xFFFFFFFFu, 0, max, {enabled_sequence}, true},
        {"all final disable lock", 0xFFFFFFFFu, 0, 0, {final_sequence, final_sequence}, true},
        {"first input enables lock", 100, 100, max, {enabled_sequence, final_sequence}, false},
        {"last input enables lock", 100, 100, max, {final_sequence, enabled_sequence}, false},
        {"sequence zero is an absolute flag", 100, 100, 0, {0}, false},
        {"sequence is not a relative delay", 100, 101, 0, {12345}, true},
        {"input-free future lock", 100, 100, 0, {}, false},
        {"input-free elapsed lock", 100, 101, 0, {}, true},
        {"input-free zero lock", 0, 0, 0, {}, true},
    };
    for (const auto& c : cases) {
        Transaction tx;
        tx.lock_time = c.lock;
        for (auto sequence : c.sequence) {
            core::TxInput input{}; input.sequence = sequence; tx.inputs.push_back(input);
        }
        check(consensus::is_final_transaction(tx, c.height, c.time) == c.final, c.name);
    }
}
}

int main(int argc, char** argv) {
    try {
        consensus::SelectParams(consensus::Network::LOADNET);
        test_numeric_boundaries();
        for (bool rocks : {false, true}) {
            use_rocksdb = rocks;
            test_height_and_time();
            test_mempool();
            test_restart_and_reorg();
            test_multisig_package();
        }
        size_t failed = 0;
        for (const auto& c : checks) if (!c["passed"].get<bool>()) {
            ++failed; std::cerr << "FAILED: " << c["name"] << '\n';
        }
        const nlohmann::json report{{"checks", checks}, {"passed", checks.size() - failed}, {"failed", failed}};
        if (argc > 1) std::ofstream(argv[1]) << report.dump(2) << '\n';
        std::cout << "transaction finality checks: " << checks.size() - failed << '/' << checks.size() << '\n';
        return failed ? 1 : 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 2;
    }
}
