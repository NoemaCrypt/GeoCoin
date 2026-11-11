#include "test_utils.hpp"
#include "geocoin/consensus/chainparams.hpp"
#include "geocoin/consensus/consensus.hpp"
#include "geocoin/consensus/proof_of_work.hpp"
#include "geocoin/network/mempool.hpp"
#include "geocoin/state/records.hpp"
#include "geocoin/state/state.hpp"
#include "geocoin/utils/bytes.hpp"
#include "nlohmann_json.hpp"

#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <utility>

namespace geocoin::network {
struct MempoolTestAccess {
    static void seed_coinbase(Mempool& pool, const core::Transaction& tx) {
        MempoolEntry entry;
        entry.tx = tx;
        entry.size = tx.serialize().size();
        pool.transactions_.emplace(tx.get_hash(), entry);
        pool.fee_index_.emplace(0, tx.get_hash());
        pool.current_size_ += entry.size;
    }
};
}

namespace {
using namespace geocoin;
using test::expect;
using core::Transaction;
using core::TransactionType;
nlohmann::json checks = nlohmann::json::array();
bool use_rocksdb = false;

void check(bool result, const std::string& name) {
    checks.push_back({{"name", name}, {"passed", result}});
}

const Address& miner() {
    static const Address address = test::address_from_seed(71);
    return address;
}

Transaction coinbase(Height height, Amount fees = 0) {
    auto tx = Transaction::create_coinbase(height, miner(),
        consensus::ProofOfWork::calculate_block_reward(height), fees,
        consensus::Params().foundation_address());
    tx.timestamp = 1700000000;
    return tx;
}

struct Fixture {
    std::filesystem::path path = test::make_temp_dir("geocoin-coinbase-contract");
    std::shared_ptr<state::ChainState> state;
    explicit Fixture(Height next_height = 200) {
        auto storage = backend();
        const auto& params = consensus::Params();
        const state::FoundationConfig foundation{params.foundation_address(), params.foundation_m(),
            params.foundation_n(), params.foundation_participants()};
        expect(storage->put_metadata("idx_epoch", bytes(32, '2')), "seed index epoch");
        expect(storage->put_metadata("chainstate_utxo_count", utils::encode_u64_le(0)), "seed UTXO count");
        expect(storage->put_metadata("foundation_cfg", state::records::serialize_foundation_config(foundation)),
               "seed foundation");
        expect(storage->put_metadata("interest_pool_balance", utils::encode_u64_le(0)), "seed pool");
        state = std::make_shared<state::ChainState>(std::move(storage));
        expect(state->set_height(next_height - 1), "seed preceding height");
        expect(state->set_tip_hash(test::hash_from_seed(72)), "seed preceding tip");
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
    }
    std::map<std::string, bytes> metadata() const {
        std::map<std::string, bytes> result;
        for (const auto& key : state->get_storage()->get_keys_with_prefix("")) {
            expect(state->get_storage()->get_metadata(key, result[key]), "read snapshot metadata");
        }
        return result;
    }
    core::Block block(const Transaction& tx) const {
        core::Block block;
        block.header.prev_block_hash = state->get_tip_hash();
        block.header.timestamp = tx.timestamp;
        block.header.bits = 0x207fffff;
        block.transactions = {tx};
        block.header.merkle_root = block.calculate_merkle_root();
        block.header.witness_commitment = block.calculate_witness_commitment();
        return block;
    }
};

void test_reward_boundaries() {
    // Independently tabulated subsidy milestones, including the first reward
    // below the normal output threshold and the final nonzero subsidy.
    const std::vector<std::pair<Height, Amount>> milestones{
        {0, 5000000000LL}, {1, 5000000000LL}, {209999, 5000000000LL},
        {210000, 2500000000LL}, {4620000, 1192}, {4830000, 596},
        {6510000, 2}, {6720000, 1}, {6930000, 0}, {13440000, 0}};
    for (const auto& [height, subsidy] : milestones) {
        check(consensus::ProofOfWork::calculate_block_reward(height) == subsidy,
              "tabulated subsidy at height " + std::to_string(height));
        for (Amount fee : {Amount{0}, Amount{1}, Amount{999}, Amount{1000}, Amount{10001}}) {
            const auto tx = coinbase(height, fee);
            consensus::ValidationState basic, reward;
            const auto label = "height " + std::to_string(height) + ", fee " + std::to_string(fee);
            check(consensus::check_transaction(tx, basic), label + ": generated coinbase passes common checks");
            check(consensus::check_coinbase_outputs(tx, height, subsidy, fee,
                      consensus::Params().foundation_address(), reward), label + ": exact reward split is valid");
        }
    }

    const auto tx = coinbase(200);
    consensus::ValidationState reward;
    check(!consensus::check_coinbase_outputs(tx, 200, -1, 50 * COIN + 1,
              consensus::Params().foundation_address(), reward), "negative subsidy is rejected even when total matches");
    reward.reset();
    check(!consensus::check_coinbase_outputs(tx, 200, 50 * COIN + 1, -1,
              consensus::Params().foundation_address(), reward), "negative fee is rejected even when total matches");
    for (Amount small_reward : {Amount{0}, Amount{1}, Amount{9}, Amount{10}}) {
        const auto small = Transaction::create_coinbase(1, miner(), small_reward, 0,
            consensus::Params().foundation_address());
        reward.reset();
        check(consensus::check_coinbase_outputs(small, 1, small_reward, 0,
                  consensus::Params().foundation_address(), reward),
              "constructor preserves pre-pool output layout for reward " + std::to_string(small_reward));
    }

    Transaction ordinary;
    ordinary.inputs = {{test::hash_from_seed(73), 0}};
    ordinary.outputs = {{consensus::params::DUST_LIMIT - 1, miner(), {}}};
    consensus::ValidationState dust;
    check(!consensus::check_transaction(ordinary, dust) && dust.get_reject_reason() == "bad-txns-dust",
          "ordinary transfers still reject positive dust outputs");
}

void test_reward_mutation_matrix() {
    const Address wrong_foundation = test::address_from_seed(79);
    const Address foundation = consensus::Params().foundation_address();
    const std::vector<Height> heights{
        1, 209999, 210000, 210001, 6720000, 6930000, 13440000};
    const std::vector<Amount> fees{
        0, 1, 2, 8, 9, 10, 11, 18, 19, 20, 21, 98, 99, 100, 101,
        998, 999, 1000, 1001, 10001};

    for (const Height height : heights) {
        const Amount subsidy = consensus::ProofOfWork::calculate_block_reward(height);
        const bool pool = consensus::is_foundation_reward_redirected_to_pool(height);
        for (const Amount fee : fees) {
            const std::string label = "reward mutation at height " + std::to_string(height) +
                ", fee " + std::to_string(fee);
            const Amount total = subsidy + fee;
            const Amount foundation_cut = total / 10;
            auto valid = coinbase(height, fee);
            consensus::ValidationState validation;
            check(valid.outputs.size() == (pool ? 1U : 2U) &&
                valid.outputs.front().value == total - foundation_cut &&
                (pool || (valid.outputs.back().address == foundation &&
                    valid.outputs.back().value == foundation_cut)),
                label + ": constructor conserves independently calculated 90/10 split");
            check(consensus::check_coinbase_outputs(valid, height, subsidy, fee,
                      foundation, validation),
                label + ": exact coinbase is accepted");

            const auto rejects = [&](core::Transaction candidate, const char* variant) {
                consensus::ValidationState state;
                check(!consensus::check_coinbase_outputs(candidate, height,
                          subsidy, fee, foundation, state) &&
                          state.is_invalid(),
                      label + ": rejects " + variant);
            };
            const auto checks_accounting = [&](Amount expected_subsidy, Amount expected_fee,
                                               const char* variant) {
                const Amount expected_total = expected_subsidy + expected_fee;
                // In the pool era only the miner share appears in coinbase.
                // At a 10-angle rounding boundary, adjacent total rewards may
                // have the same miner share; block application computes the
                // pool credit from the actual subsidy and transaction fees.
                const bool same_miner_payout = pool &&
                    expected_total - expected_total / 10 == valid.outputs.front().value;
                consensus::ValidationState state;
                const bool accepted = consensus::check_coinbase_outputs(valid, height,
                    expected_subsidy, expected_fee, foundation, state);
                check(accepted == same_miner_payout && (accepted || state.is_invalid()),
                    label + (same_miner_payout ? ": accepts " : ": rejects ") + variant);
            };
            auto miner_over = valid;
            ++miner_over.outputs.front().value;
            rejects(miner_over, "miner overclaim of one angle");
            if (valid.outputs.front().value > 0) {
                auto miner_under = valid;
                --miner_under.outputs.front().value;
                rejects(miner_under, "miner underclaim of one angle");
            }
            checks_accounting(subsidy, fee + 1, "fee accounting one angle too high");
            if (fee > 0) {
                checks_accounting(subsidy, fee - 1, "fee accounting one angle too low");
            }
            checks_accounting(subsidy + 1, fee, "subsidy accounting one angle too high");
            if (subsidy > 0) {
                checks_accounting(subsidy - 1, fee, "subsidy accounting one angle too low");
            }
            auto wrong_height = valid;
            wrong_height.inputs.front().script_sig.front() ^= 1;
            rejects(wrong_height, "incorrect committed height");

            if (pool) {
                auto phantom_foundation = valid;
                phantom_foundation.outputs.push_back({foundation_cut, foundation, {}});
                rejects(phantom_foundation, "extra direct foundation payout after pool activation");
            } else {
                auto wrong_address = valid;
                wrong_address.outputs.back().address = wrong_foundation;
                rejects(wrong_address, "foundation payment to another address");
                auto missing_foundation = valid;
                missing_foundation.outputs.pop_back();
                rejects(missing_foundation, "missing foundation payout");
                auto extra_output = valid;
                extra_output.outputs.push_back({0, wrong_foundation, {}});
                rejects(extra_output, "third coinbase output");
                auto wrong_split = valid;
                --wrong_split.outputs.front().value;
                ++wrong_split.outputs.back().value;
                rejects(wrong_split, "wrong 90/10 split with unchanged total");
            }
        }
    }

    const auto canonical = coinbase(1);
    consensus::ValidationState overflow;
    check(!consensus::check_coinbase_outputs(canonical, 1,
              std::numeric_limits<Amount>::max(), 1, foundation, overflow) &&
              overflow.get_reject_reason() == "bad-cb-amount",
          "reward plus fees overflow is rejected before payout arithmetic");
    bool constructor_rejected_overflow = false;
    try {
        (void)Transaction::create_coinbase(1, miner(),
            std::numeric_limits<Amount>::max(), 1, foundation);
    } catch (const std::invalid_argument&) {
        constructor_rejected_overflow = true;
    }
    check(constructor_rejected_overflow, "coinbase constructor rejects reward plus fee overflow");
}

void rejection_case(const std::string& name, const std::function<void(Transaction&)>& mutate) {
    const std::string label = std::string(use_rocksdb ? "RocksDB: " : "MemoryStorage: ") + name;
    auto tx = coinbase(200);
    mutate(tx);
    consensus::ValidationState basic;
    check(!consensus::check_transaction(tx, basic), label + ": common validation rejects envelope");
    {
        Fixture f;
        const auto before = f.metadata();
        check(!f.state->validate_transaction(tx), label + ": standalone state validation rejects envelope");
        consensus::ValidationState applied;
        check(!f.state->apply_transaction(tx, 200, true, &applied) && applied.is_invalid(),
              label + ": direct coinbase application rejects before mutation");
        check(f.metadata() == before && f.state->get_utxo_count() == 0 && f.state->get_balance(miner()) == 0,
              label + ": direct rejection leaves state unchanged");
        f.restart();
        check(f.metadata() == before && f.state->get_utxo_count() == 0 && f.state->get_balance(miner()) == 0,
              label + ": direct rejection remains unchanged after restart");
    }
    {
        Fixture f;
        const auto block = f.block(tx);
        const auto before = f.metadata();
        consensus::ValidationState applied;
        check(!f.state->apply_block(block, applied, true) && applied.is_invalid(),
              label + ": block application rejects invalid coinbase");
        check(f.metadata() == before && f.state->get_utxo_count() == 0 && f.state->get_height() == 199,
              label + ": block rejection leaves metadata and height unchanged");
        f.restart();
        check(f.metadata() == before && f.state->get_balance(miner()) == 0,
              label + ": block rejection remains unchanged after restart");
    }
}

void test_classification_and_height() {
    const std::string label = use_rocksdb ? "RocksDB: " : "MemoryStorage: ";
    Fixture f;
    const auto before = f.metadata();
    Transaction payment;
    payment.inputs = {{test::hash_from_seed(74), 0}};
    payment.outputs = {{COIN, miner(), {}}};
    consensus::ValidationState validation;
    check(!f.state->apply_transaction(payment, 200, true, &validation) && validation.is_invalid(),
          label + "ordinary payment cannot be applied with the coinbase flag");
    check(f.metadata() == before && f.state->get_utxo_count() == 0,
          label + "incorrect coinbase flag does not create outputs");

    Fixture height_fixture;
    auto wrong_height = coinbase(201);
    validation.reset();
    check(!height_fixture.state->apply_transaction(wrong_height, 200, true, &validation) && validation.is_invalid(),
          label + "direct coinbase application checks the committed height");
    check(height_fixture.state->get_utxo_count() == 0,
          label + "height mismatch leaves no outputs");
}

void test_lifecycle(Height height) {
    const auto label = std::string(use_rocksdb ? "RocksDB: " : "MemoryStorage: ") +
        "coinbase at height " + std::to_string(height);
    Fixture f(height);
    const auto block = f.block(coinbase(height));
    const auto previous_tip = f.state->get_tip_hash();
    consensus::ValidationState basic;
    check(consensus::check_block(block, basic), label + ": full block body checks accept reward");
    const bool applied = f.state->apply_block(block, true);
    check(applied, label + ": state applies reward");
    if (!applied) return;
    const auto expected_pool = consensus::is_foundation_reward_redirected_to_pool(height)
        ? consensus::calculate_foundation_reward_cut(consensus::ProofOfWork::calculate_block_reward(height)) : 0;
    f.restart();
    check(f.state->get_balance(miner()) == block.transactions.front().outputs.front().value &&
              f.state->get_interest_pool_balance() == expected_pool && f.state->get_tip_hash() == block.get_hash(),
          label + ": reward, pool and tip survive restart");
    check(f.state->revert_block(block), label + ": persisted undo reverts reward");
    f.restart();
    check(f.state->get_balance(miner()) == 0 && f.state->get_interest_pool_balance() == 0 &&
              f.state->get_tip_hash() == previous_tip && f.state->get_height() == height - 1 &&
              f.state->get_utxo_count() == 0,
          label + ": restart after undo restores balances and position");
}

void test_fee_only_block(Amount fee) {
    constexpr Height height = 6930000; // The subsidy is already zero.
    const auto label = std::string(use_rocksdb ? "RocksDB: " : "MemoryStorage: ") +
        "fee-only reward " + std::to_string(fee);
    static const Address payee = test::address_from_seed(76);
    Fixture f(height);
    const auto funding = coinbase(1);
    expect(f.state->apply_transaction(funding, 1, true), "seed matured funding");
    Transaction payment;
    payment.timestamp = 1700000001;
    payment.inputs = {{funding.get_hash(), 0}};
    payment.outputs = {{funding.outputs.front().value - fee, payee, {}}};
    payment.sign(test::make_key(71));
    check(f.state->calculate_transaction_fee(payment) == fee, label + ": signed payment has expected fee");
    auto block = f.block(coinbase(height, fee));
    block.transactions.push_back(payment);
    block.header.merkle_root = block.calculate_merkle_root();
    block.header.witness_commitment = block.calculate_witness_commitment();
    consensus::ValidationState basic;
    check(consensus::check_block(block, basic), label + ": full block body validation succeeds");
    const auto before = f.metadata();
    auto overpaid = block;
    ++overpaid.transactions.front().outputs.front().value;
    overpaid.header.merkle_root = overpaid.calculate_merkle_root();
    overpaid.header.witness_commitment = overpaid.calculate_witness_commitment();
    check(!f.state->apply_block(overpaid, true) && f.metadata() == before &&
              f.state->get_balance(miner()) == funding.outputs.front().value,
          label + ": one excess unit is rejected without debiting payment");
    const bool applied = f.state->apply_block(block, true);
    check(applied, label + ": exact fee-only reward and signed payment apply");
    if (!applied) return;
    f.restart();
    check(f.state->get_balance(miner()) == fee - fee / 10 &&
              f.state->get_interest_pool_balance() == fee / 10 &&
              f.state->get_balance(payee) == payment.outputs.front().value,
          label + ": fees are conserved across miner, pool and payment after restart");
    check(f.state->revert_block(block), label + ": undo restores spent funding");
    f.restart();
    check(f.state->get_balance(miner()) == funding.outputs.front().value &&
              f.state->get_balance(payee) == 0 && f.state->get_interest_pool_balance() == 0 &&
              f.state->get_height() == height - 1,
          label + ": restart after undo restores all balances");
}

void test_fee_reward_matrix(Height height, Amount fee) {
    const auto label = std::string(use_rocksdb ? "RocksDB: " : "MemoryStorage: ") +
        "height " + std::to_string(height) + ", fee " + std::to_string(fee);
    const Address recipient = test::address_from_seed(77);
    Fixture f(height);
    const auto funding = coinbase(1);
    expect(f.state->apply_transaction(funding, 1, true), "seed mature funding for fee matrix");
    const Amount funded_miner = funding.outputs.front().value;
    const Amount funded_foundation = funding.outputs.back().value;
    const Amount initial_supply = f.state->get_total_supply();

    Transaction payment;
    payment.timestamp = 1700000001;
    payment.inputs = {{funding.get_hash(), 0}};
    payment.outputs = {{funded_miner - fee, recipient, {}}};
    payment.sign(test::make_key(71));
    check(f.state->calculate_transaction_fee(payment) == fee,
          label + ": signed transfer computes exact fee");

    auto valid = f.block(coinbase(height, fee));
    valid.transactions.push_back(payment);
    valid.header.merkle_root = valid.calculate_merkle_root();
    valid.header.witness_commitment = valid.calculate_witness_commitment();
    const auto before = f.metadata();
    for (Amount delta : {Amount{-1}, Amount{1}}) {
        auto wrong = valid;
        wrong.transactions.front().outputs.front().value += delta;
        wrong.header.merkle_root = wrong.calculate_merkle_root();
        wrong.header.witness_commitment = wrong.calculate_witness_commitment();
        consensus::ValidationState validation;
        check(!f.state->apply_block(wrong, validation, true) && validation.is_invalid() &&
                  f.metadata() == before && f.state->get_height() == height - 1 &&
                  f.state->get_balance(recipient) == 0,
              label + (delta < 0 ? ": underclaimed fee rejected atomically" :
                                   ": overclaimed fee rejected atomically"));
    }

    const bool accepted = f.state->apply_block(valid, true);
    check(accepted, label + ": exact fee-bearing block applies");
    if (!accepted) return;
    const Amount subsidy = consensus::ProofOfWork::calculate_block_reward(height);
    const Amount cut = consensus::calculate_foundation_reward_cut(subsidy + fee);
    const Amount expected_miner = subsidy + fee - cut;
    const bool pool = consensus::is_foundation_reward_redirected_to_pool(height);
    f.restart();
    check(f.state->get_balance(miner()) == expected_miner &&
              f.state->get_balance(recipient) == funded_miner - fee &&
              f.state->get_balance(consensus::Params().foundation_address()) ==
                  funded_foundation + (pool ? 0 : cut) &&
              f.state->get_interest_pool_balance() == (pool ? cut : 0) &&
              f.state->get_total_supply() + f.state->get_interest_pool_balance() ==
                  initial_supply + subsidy,
          label + ": UTXO plus interest pool conserve subsidy after restart");
    check(f.state->revert_block(valid), label + ": fee-bearing block reverts");
    f.restart();
    check(f.state->get_balance(miner()) == funded_miner &&
              f.state->get_balance(recipient) == 0 &&
              f.state->get_balance(consensus::Params().foundation_address()) == funded_foundation &&
              f.state->get_interest_pool_balance() == 0 &&
              f.state->get_total_supply() == initial_supply &&
              f.state->get_height() == height - 1,
          label + ": undo restores funding and supply after restart");
}

void test_dependent_payment_reward(Height height, Amount parent_fee, Amount child_fee) {
    const auto label = std::string(use_rocksdb ? "RocksDB: " : "MemoryStorage: ") +
        "dependent payments at height " + std::to_string(height) +
        ", fees " + std::to_string(parent_fee) + "+" + std::to_string(child_fee);
    const Address recipient = test::address_from_seed(80);
    const Address competitor = test::address_from_seed(81);
    Fixture f(height);
    const auto funding = coinbase(1);
    expect(f.state->apply_transaction(funding, 1, true), label + ": seed mature funding");
    const Amount funded_miner = funding.outputs.front().value;
    const Amount funded_foundation = funding.outputs.back().value;
    const Amount initial_supply = f.state->get_total_supply();
    const Amount total_fees = parent_fee + child_fee;
    const Amount subsidy = consensus::ProofOfWork::calculate_block_reward(height);
    const Amount cut = (subsidy + total_fees) / 10;
    const bool pool = consensus::is_foundation_reward_redirected_to_pool(height);

    Transaction parent;
    parent.timestamp = 1700000000;
    parent.inputs = {{funding.get_hash(), 0}};
    parent.outputs = {{funded_miner - parent_fee, miner(), {}}};
    parent.sign(test::make_key(71));
    Transaction child;
    child.timestamp = 1700000000;
    child.inputs = {{parent.get_hash(), 0}};
    child.outputs = {{funded_miner - total_fees, recipient, {}}};
    child.sign(test::make_key(71));
    check(f.state->calculate_transaction_fee(parent) == parent_fee &&
              f.state->calculate_transaction_fee(child) == 0,
          label + ": parent fee is known and child input is only in the prospective block");

    const auto make_block = [&](std::vector<Transaction> payments, Amount claimed_fees) {
        auto block = f.block(coinbase(height, claimed_fees));
        block.transactions.insert(block.transactions.end(), payments.begin(), payments.end());
        block.header.merkle_root = block.calculate_merkle_root();
        block.header.witness_commitment = block.calculate_witness_commitment();
        return block;
    };
    const auto before = f.metadata();
    const auto reject = [&](const char* variant, std::vector<Transaction> payments,
                            Amount claimed_fees) {
        auto block = make_block(std::move(payments), claimed_fees);
        consensus::ValidationState validation;
        check(!f.state->apply_block(block, validation, true) && validation.is_invalid() &&
                  f.metadata() == before && f.state->get_height() == height - 1 &&
                  f.state->get_balance(miner()) == funded_miner &&
                  f.state->get_balance(recipient) == 0 &&
                  f.state->has_utxo(funding.get_hash(), 0) &&
                  !f.state->has_utxo(parent.get_hash(), 0),
              label + ": " + variant + " rejects atomically");
    };

    reject("child before parent", {child, parent}, total_fees);
    reject("orphan child", {child}, total_fees);
    reject("duplicate child", {parent, child, child}, total_fees + child_fee);
    reject("coinbase overclaims ten fee angles", {parent, child}, total_fees + 10);
    if (total_fees >= 10) {
        reject("coinbase underclaims ten fee angles", {parent, child}, total_fees - 10);
    }
    if (child_fee > 0) {
        reject("coinbase claims omitted child fee", {parent}, total_fees);
    }
    auto bad_signature = child;
    bad_signature.witnesses.front().signature.front() ^= 1;
    reject("invalid child signature", {parent, bad_signature}, total_fees);
    Transaction conflicting = child;
    conflicting.outputs.front().address = competitor;
    conflicting.outputs.front().value = parent.outputs.front().value - 1;
    conflicting.sign(test::make_key(71));
    reject("two children spend one same-block output", {parent, child, conflicting}, total_fees + 1);

    const auto valid = make_block({parent, child}, total_fees);
    consensus::ValidationState body;
    check(consensus::check_block(valid, body), label + ": dependent block passes body validation");
    const bool applied = f.state->apply_block(valid, true);
    check(applied, label + ": ordered parent and child apply with both fees");
    if (!applied) return;
    state::UTXO recipient_utxo;
    check(!f.state->has_utxo(funding.get_hash(), 0) &&
              !f.state->has_utxo(parent.get_hash(), 0) &&
              f.state->get_utxo(child.get_hash(), 0, recipient_utxo) &&
              recipient_utxo.value == funded_miner - total_fees,
          label + ": only the final dependent output remains spendable");
    f.restart();
    check(f.state->get_balance(miner()) == subsidy + total_fees - cut &&
              f.state->get_balance(recipient) == funded_miner - total_fees &&
              f.state->get_balance(consensus::Params().foundation_address()) ==
                  funded_foundation + (pool ? 0 : cut) &&
              f.state->get_interest_pool_balance() == (pool ? cut : 0) &&
              f.state->get_total_supply() + f.state->get_interest_pool_balance() ==
                  initial_supply + subsidy,
          label + ": dependent fees, reward and pool persist without inflation");
    check(f.state->revert_block(valid), label + ": dependent block reverts");
    f.restart();
    check(f.state->get_balance(miner()) == funded_miner &&
              f.state->get_balance(recipient) == 0 &&
              f.state->get_balance(consensus::Params().foundation_address()) == funded_foundation &&
              f.state->get_interest_pool_balance() == 0 &&
              f.state->get_total_supply() == initial_supply &&
              f.state->has_utxo(funding.get_hash(), 0) &&
              !f.state->has_utxo(parent.get_hash(), 0) &&
              !f.state->has_utxo(child.get_hash(), 0),
          label + ": undo and restart restore the funding state exactly");
}

void test_maturity_boundary_policy_gap() {
    constexpr Height next_height = consensus::params::COINBASE_MATURITY;
    constexpr Amount fee = 100000;
    const auto label = std::string(use_rocksdb ? "RocksDB: " : "MemoryStorage: ") +
        "first consensus-mature reward spend";
    const Address recipient = test::address_from_seed(78);
    Fixture f(next_height);
    const auto funding = coinbase(1);
    expect(f.state->apply_transaction(funding, 1, true), "seed height-1 reward for maturity boundary");
    Transaction payment;
    payment.timestamp = 1700000001;
    payment.inputs = {{funding.get_hash(), 0}};
    payment.outputs = {{funding.outputs.front().value - fee, recipient, {}}};
    payment.sign(test::make_key(71));
    check(f.state->calculate_transaction_fee(payment) == fee,
          label + ": reward spend has a valid fee");

    std::string policy_reason;
    const network::MempoolPolicyConfig policy;
    check(!network::MempoolPolicy::check(payment, *f.state, fee, policy, &policy_reason) &&
              policy_reason == "coinbase-not-mature",
          label + ": mempool policy rejects at tip 99");

    auto block = f.block(coinbase(next_height, fee));
    block.transactions.push_back(payment);
    block.header.merkle_root = block.calculate_merkle_root();
    block.header.witness_commitment = block.calculate_witness_commitment();
    const bool accepted = f.state->apply_block(block, true);
    check(accepted, label + ": consensus accepts inclusion at height 100");
    if (accepted) {
        check(f.state->get_balance(recipient) == funding.outputs.front().value - fee,
              label + ": first-maturity payment is credited");
        check(f.state->revert_block(block), label + ": maturity-boundary block reverts");
    }
}

void test_coinbase_maturity_grid(Height funding_height, Height block_height) {
    const auto label = std::string(use_rocksdb ? "RocksDB: " : "MemoryStorage: ") +
        "coinbase maturity funding=" + std::to_string(funding_height) +
        ", block=" + std::to_string(block_height);
    const Address recipient = test::address_from_seed(82);
    constexpr Amount fee = 100000;
    Fixture f(block_height);
    const auto funding = coinbase(funding_height);
    expect(f.state->apply_transaction(funding, funding_height, true), label + ": seed reward");
    const Amount initial_supply = f.state->get_total_supply();
    const auto before = f.metadata();

    Transaction payment;
    payment.timestamp = 1700000001;
    payment.inputs = {{funding.get_hash(), 0}};
    payment.outputs = {{funding.outputs.front().value - fee, recipient, {}}};
    payment.sign(test::make_key(71));
    check(f.state->calculate_transaction_fee(payment) == fee,
          label + ": funding and payment fee resolve");
    std::string policy_reason;
    const bool mempool_admits = network::MempoolPolicy::check(payment, *f.state, fee,
        network::MempoolPolicyConfig{}, &policy_reason);
    const bool policy_mature = block_height - funding_height >= consensus::params::COINBASE_MATURITY;
    check(mempool_admits == policy_mature &&
              (mempool_admits || policy_reason == "coinbase-not-mature"),
          label + ": mempool maturity follows the current tip");

    auto block = f.block(coinbase(block_height, fee));
    block.transactions.push_back(payment);
    block.header.merkle_root = block.calculate_merkle_root();
    block.header.witness_commitment = block.calculate_witness_commitment();
    consensus::ValidationState validation;
    const bool applied = f.state->apply_block(block, validation, true);
    const bool consensus_mature = block_height - funding_height + 1 >=
        consensus::params::COINBASE_MATURITY;
    check(applied == consensus_mature &&
              (applied || validation.get_reject_reason() == "bad-txns-premature-spend-of-coinbase"),
          label + ": block inclusion uses the candidate height");
    if (!applied) {
        check(f.metadata() == before && f.state->get_height() == block_height - 1 &&
                  f.state->has_utxo(funding.get_hash(), 0) &&
                  f.state->get_balance(recipient) == 0 &&
                  f.state->get_total_supply() == initial_supply,
              label + ": immature-spend rejection is atomic");
        f.restart();
        check(f.state->has_utxo(funding.get_hash(), 0) &&
                  f.state->get_balance(recipient) == 0 &&
                  f.state->get_total_supply() == initial_supply,
              label + ": rejected maturity spend remains absent after restart");
        return;
    }

    const Amount subsidy = consensus::ProofOfWork::calculate_block_reward(block_height);
    const Amount cut = (subsidy + fee) / 10;
    f.restart();
    check(!f.state->has_utxo(funding.get_hash(), 0) &&
              f.state->get_balance(miner()) == subsidy + fee - cut &&
              f.state->get_balance(recipient) == funding.outputs.front().value - fee &&
              f.state->get_total_supply() == initial_supply + subsidy,
          label + ": first mature spend and fee reward survive restart");
    check(f.state->revert_block(block), label + ": mature-spend block reverts");
    f.restart();
    check(f.state->has_utxo(funding.get_hash(), 0) &&
              f.state->get_balance(miner()) == funding.outputs.front().value &&
              f.state->get_balance(recipient) == 0 &&
              f.state->get_total_supply() == initial_supply,
          label + ": undo restores maturity funding after restart");
}

void test_same_block_coinbase_spend(Height height) {
    const auto label = std::string(use_rocksdb ? "RocksDB: " : "MemoryStorage: ") +
        "same-block coinbase spend at height " + std::to_string(height);
    Fixture f(height);
    const auto before = f.metadata();
    auto block = f.block(coinbase(height));
    Transaction payment;
    payment.timestamp = block.header.timestamp;
    payment.inputs = {{block.transactions.front().get_hash(), 0}};
    payment.outputs = {{block.transactions.front().outputs.front().value,
                        test::address_from_seed(83), {}}};
    payment.sign(test::make_key(71));
    block.transactions.push_back(payment);
    block.header.merkle_root = block.calculate_merkle_root();
    block.header.witness_commitment = block.calculate_witness_commitment();
    consensus::ValidationState validation;
    check(!f.state->apply_block(block, validation, true) &&
              validation.get_reject_reason() == "bad-txns-premature-spend-of-coinbase" &&
              f.metadata() == before && f.state->get_utxo_count() == 0 &&
              f.state->get_height() == height - 1,
          label + ": new reward cannot fund a payment in its own block");
    f.restart();
    check(f.metadata() == before && f.state->get_utxo_count() == 0,
          label + ": rejected self-spend does not persist after restart");
}

void test_mempool_exclusion() {
    const std::string label = use_rocksdb ? "RocksDB: " : "MemoryStorage: ";
    Fixture f;
    const auto tx = coinbase(200);
    network::MempoolPolicyConfig policy;
    policy.min_relay_fee = 0;
    policy.min_relay_fee_rate_per_kb = 0;
    policy.enforce_coinbase_maturity = false;
    std::string reason;
    check(!network::MempoolPolicy::check(tx, *f.state, 0, policy, &reason) && reason == "system-transaction",
          label + "coinbase is not a relay transaction even under permissive fee policy");
    network::Mempool pool(f.state);
    pool.set_policy_config(policy);
    check(!pool.add_transaction(tx) && pool.empty(), label + "coinbase cannot enter mempool");
    check(pool.get_transactions_for_mining(1000000).empty(), label + "mempool cannot add a second coinbase to a template");

    network::Mempool raw_pool(nullptr);
    network::MempoolTestAccess::seed_coinbase(raw_pool, tx);
    check(raw_pool.get_transactions_for_mining(1000000).empty(),
          label + "selection excludes a raw coinbase entry even without ChainState");
}
}

int main(int argc, char** argv) {
    try {
        consensus::SelectParams(consensus::Network::MAIN);
        test_reward_boundaries();
        test_reward_mutation_matrix();
        for (bool rocksdb : {false, true}) {
            use_rocksdb = rocksdb;
            test_mempool_exclusion();
            rejection_case("transaction version", [](auto& tx) { tx.version = 3; });
            rejection_case("witness version", [](auto& tx) { ++tx.witness_version; });
            rejection_case("missing outputs", [](auto& tx) { tx.outputs.clear(); });
            rejection_case("negative amount", [](auto& tx) { tx.outputs.front().value = -1; });
            rejection_case("oversized transaction", [](auto& tx) {
                tx.outputs.front().script_pubkey.assign(consensus::params::MAX_TX_SIZE, 1);
            });
            rejection_case("cadastral coinbase", [](auto& tx) {
                tx.type = TransactionType::CADASTRAL_DOC;
                tx.cadastral_doc_hash = test::hash_from_seed(75);
            });
            test_classification_and_height();
            test_maturity_boundary_policy_gap();
            for (Height funding_height : {Height{1}, Height{2}}) {
                for (Height block_height : {Height{99}, Height{100}, Height{101}}) {
                    test_coinbase_maturity_grid(funding_height, block_height);
                }
            }
            for (Height height : {Height{1}, Height{210000}}) {
                test_same_block_coinbase_spend(height);
            }
            for (Height height : {Height{1}, Height{209999}, Height{210000}, Height{4830000}, Height{6720000}, Height{6930000}})
                test_lifecycle(height);
            for (Amount fee : {Amount{1}, Amount{999}, Amount{10001}}) test_fee_only_block(fee);
            for (Height height : {Height{209999}, Height{210000}, Height{210001},
                                  Height{4830000}, Height{6720000}, Height{6930000}}) {
                for (Amount fee : {Amount{1}, Amount{9}, Amount{10}, Amount{99}, Amount{101}, Amount{10001}})
                    test_fee_reward_matrix(height, fee);
            }
            for (Height height : {Height{209999}, Height{210000}, Height{6930000}}) {
                for (const auto& [parent_fee, child_fee] :
                     std::vector<std::pair<Amount, Amount>>{{0, 0}, {1, 9}, {9, 2}, {100001, 100009}}) {
                    test_dependent_payment_reward(height, parent_fee, child_fee);
                }
            }
        }
        size_t passed = 0;
        for (const auto& item : checks) if (item.at("passed").get<bool>()) ++passed;
        const nlohmann::json result{{"passed", passed}, {"failed", checks.size() - passed}, {"checks", checks}};
        if (argc > 1) { std::ofstream output(argv[1]); output << result.dump(2) << '\n'; }
        std::cout << result.dump(2) << '\n';
        return passed == checks.size() ? 0 : 1;
    } catch (const std::exception& e) {
        std::cerr << "test_coinbase_contract: " << e.what() << '\n';
        return 2;
    }
}
