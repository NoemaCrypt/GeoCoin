#include "test_utils.hpp"
#include "geocoin/consensus/consensus.hpp"
#include "geocoin/consensus/optimized_miner.hpp"
#include "geocoin/consensus/proof_of_work.hpp"
#include "geocoin/node/node.hpp"
#include "geocoin/network/mempool_policy.hpp"
#include "geocoin/rpc/rpc_mining_helpers.hpp"
#include "geocoin/wallet/wallet.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace geocoin::node {
struct NodeTestAccess {
    static core::Block create_mining_block(Node& node) { return node.create_mining_block(); }
    static core::Block create_mining_block_for_parent(Node& node, chain::BlockIndex* parent) {
        core::ActiveTipSnapshot snapshot;
        snapshot.index = parent;
        snapshot.hash = parent->get_block_hash();
        snapshot.height = parent->height;
        snapshot.median_time_past = parent->get_median_time_past();
        return node.create_mining_block_for_tip(snapshot);
    }
};
} // namespace geocoin::node

namespace {
using namespace geocoin;
using test::expect;

node::NodeConfig config_for(const std::filesystem::path& path, const Address& miner) {
    node::NodeConfig config;
    config.data_dir = path.string();
    config.network = "degree-load";
    config.enable_auto_connect = false;
    config.enable_p2p_listen = false;
    config.enable_mining = false;
    config.mining_address = miner;
    config.mining_threads = 1;
    return config;
}

struct Fixture {
    std::filesystem::path path = test::make_temp_dir("geocoin-mining-reward-scenarios");
    wallet::Wallet wallet;
    std::unique_ptr<node::Node> node;

    Fixture() {
        expect(wallet.initialize(test::make_key(131)), "initialize payout wallet");
        node = std::make_unique<node::Node>(config_for(path, wallet.get_address()));
        expect(node->start(test::make_key(132)), "start isolated loadnet node");
    }
    ~Fixture() {
        if (node) {
            node->stop();
            node.reset();
        }
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
    void restart() {
        node->stop();
        node.reset();
        node = std::make_unique<node::Node>(config_for(path, wallet.get_address()));
        expect(node->start(test::make_key(132)), "restart isolated loadnet node");
    }
};

void update_commitments(core::Block& block) {
    block.header.merkle_root = block.calculate_merkle_root();
    block.header.witness_commitment = block.calculate_witness_commitment();
}

bool mine(node::Node& node, core::Block& block) {
    const auto parent = node.get_blockchain()->get_active_tip_snapshot();
    if (!parent.index || block.header.prev_block_hash != parent.hash) return false;
    std::atomic<bool> running{true};
    std::atomic<uint64_t> hashes{0};
    consensus::OptimizedMiner miner;
    return miner.mine_block_optimized(block, block.header.bits,
        static_cast<uint32_t>(parent.median_time_past), parent.hash, parent.height + 1,
        65536, &running, -1, &hashes);
}

bool mine_on_parent(core::Block& block, const chain::BlockIndex& parent) {
    std::atomic<bool> running{true};
    std::atomic<uint64_t> hashes{0};
    consensus::OptimizedMiner miner;
    return miner.mine_block_optimized(block, block.header.bits,
        static_cast<uint32_t>(parent.get_median_time_past()), parent.get_block_hash(), parent.height + 1,
        65536, &running, -1, &hashes);
}

void run_scenarios() {
    Fixture f;
    auto state = f.node->get_chain_state();
    const Address miner_address = f.wallet.get_address();
    const Address foundation = state->get_foundation_address();
    const Address other_miner = test::address_from_seed(133);
    const Address stale_miner = test::address_from_seed(134);
    const Address reorg_miner = test::address_from_seed(135);
    const Amount subsidy = consensus::ProofOfWork::calculate_block_reward(1);
    const Amount foundation_cut = consensus::calculate_foundation_reward_cut(subsidy);
    const Amount miner_cut = subsidy - foundation_cut;
    const Amount starting_supply = state->get_total_supply();

    auto initial = node::NodeTestAccess::create_mining_block(*f.node);
    auto rpc = rpc::build_block_template(f.node->get_blockchain().get(), state.get(),
        f.node->get_mempool().get(), miner_address, initial.header.timestamp);
    expect(initial.transactions.size() == 1 && rpc.transactions.size() == 1,
        "empty mempool templates have one coinbase");
    expect(initial.header.prev_block_hash == rpc.header.prev_block_hash &&
        initial.header.bits == rpc.header.bits && initial.header.geo_tag == rpc.header.geo_tag,
        "node and RPC templates agree on parent and PoW parameters");
    expect(initial.transactions[0].outputs.size() == 2 &&
        rpc.transactions[0].outputs.size() == 2,
        "pre-pool templates have miner and foundation outputs");
    expect(initial.transactions[0].outputs[0].value == miner_cut &&
        initial.transactions[0].outputs[1].value == foundation_cut &&
        initial.transactions[0].outputs[0].address == miner_address &&
        initial.transactions[0].outputs[1].address == foundation &&
        rpc.transactions[0].outputs[0].value == miner_cut &&
        rpc.transactions[0].outputs[1].value == foundation_cut &&
        rpc.transactions[0].outputs[0].address == miner_address &&
        rpc.transactions[0].outputs[1].address == foundation,
        "node and RPC templates pay exact miner/foundation split");

    struct Mutation {
        const char* name;
        std::function<void(core::Block&)> change;
        bool repair_commitments = true;
        bool repair_pow = true;
    };
    const std::vector<Mutation> mutations{
        {"miner overpaid by one angle", [](auto& b) { ++b.transactions[0].outputs[0].value; }},
        {"miner underpaid by one angle", [](auto& b) { --b.transactions[0].outputs[0].value; }},
        {"foundation overpaid by one angle", [](auto& b) { ++b.transactions[0].outputs[1].value; }},
        {"foundation underpaid by one angle", [](auto& b) { --b.transactions[0].outputs[1].value; }},
        {"wrong foundation address", [other_miner](auto& b) { b.transactions[0].outputs[1].address = other_miner; }},
        {"missing foundation output", [](auto& b) { b.transactions[0].outputs.pop_back(); }},
        {"extra foundation output", [](auto& b) { b.transactions[0].outputs.push_back(b.transactions[0].outputs.back()); }},
        {"negative miner output", [](auto& b) { b.transactions[0].outputs[0].value = -1; }},
        {"wrong coinbase height", [](auto& b) { b.transactions[0].inputs[0].script_sig[0] ^= 1; }},
        {"second coinbase", [](auto& b) { b.transactions.push_back(b.transactions[0]); }},
        {"empty block", [](auto& b) { b.transactions.clear(); }},
        {"corrupt merkle root", [](auto& b) { b.header.merkle_root[0] ^= 1; }, false},
        {"corrupt witness commitment", [](auto& b) { b.header.witness_commitment[0] ^= 1; }, false},
        {"incorrect difficulty", [](auto& b) { b.header.bits = 0x2070ffff; }},
        {"future timestamp", [](auto& b) { b.header.timestamp += 4 * 60 * 60; }},
        {"corrupt geo tag", [](auto& b) { b.header.geo_tag ^= 1; }, false, false},
        {"invalid proof of work", [](auto& b) { ++b.header.nonce; }, false, false},
    };

    for (const auto& mutation : mutations) {
        auto candidate = node::NodeTestAccess::create_mining_block(*f.node);
        mutation.change(candidate);
        if (mutation.repair_commitments) update_commitments(candidate);
        if (mutation.repair_pow) {
            expect(mine(*f.node, candidate), std::string(mutation.name) + ": mine candidate");
        } else if (std::string(mutation.name) == "invalid proof of work") {
            // Start with a valid header, then advance to the first invalid nonce.
            candidate = node::NodeTestAccess::create_mining_block(*f.node);
            expect(mine(*f.node, candidate), "mine candidate before invalidating PoW");
            const auto parent = f.node->get_blockchain()->get_active_tip_snapshot();
            do {
                ++candidate.header.nonce;
            } while (consensus::ProofOfWork::verify_proof_of_work(candidate.header,
                candidate.header.bits, static_cast<uint32_t>(parent.median_time_past), parent.hash, parent.height + 1));
        } else {
            // Geo-tag is changed after mining because OptimizedMiner restores it.
            candidate = node::NodeTestAccess::create_mining_block(*f.node);
            expect(mine(*f.node, candidate), "mine candidate before invalidating geo tag");
            candidate.header.geo_tag ^= 1;
        }

        const auto before_tip = f.node->get_blockchain()->get_active_tip_snapshot();
        const auto before_supply = state->get_total_supply();
        const auto before_balance = state->get_balance(miner_address);
        const auto before_foundation = state->get_balance(foundation);
        const auto before_utxos = state->get_utxo_count();
        const auto submission = f.node->process_block(candidate);
        const bool rejected_without_effect = !submission.is_accepted() &&
            !submission.active_tip_changed &&
            f.node->get_blockchain()->get_active_tip_snapshot().hash == before_tip.hash &&
            state->get_total_supply() == before_supply &&
            state->get_balance(miner_address) == before_balance &&
            state->get_balance(foundation) == before_foundation &&
            state->get_utxo_count() == before_utxos;
        if (!rejected_without_effect) {
            throw std::runtime_error(std::string("invalid block accepted or mutated state: ") + mutation.name +
                " (reason=" + submission.reject_reason + ")");
        }
        std::cout << "PASS rejected: " << mutation.name << " (" << submission.reject_reason << ")\n";
    }

    auto stale = rpc::build_block_template(f.node->get_blockchain().get(), state.get(),
        f.node->get_mempool().get(), stale_miner);
    expect(mine(*f.node, stale), "mine a competing old-tip block");

    expect(mine(*f.node, initial), "mine normal node template");
    consensus::ValidationState validation;
    expect(consensus::check_block(initial, validation), "mined block passes body validation");
    const auto parent = f.node->get_blockchain()->get_active_tip_snapshot();
    expect(consensus::contextual_check_block(initial, validation, parent.index),
        "mined block passes contextual PoW validation");
    const auto first = f.node->process_block(initial);
    expect(first.is_accepted() && first.active_tip_changed && first.block_is_active &&
        first.active_height == 1, "mined block becomes active through node");
    expect(state->get_balance(miner_address) == miner_cut &&
        state->get_balance(foundation) == starting_supply + foundation_cut &&
        state->get_total_supply() == starting_supply + subsidy,
        "first reward is conserved across miner, foundation and total supply");
    state::UTXO miner_utxo;
    expect(state->get_utxo(initial.transactions[0].get_hash(), 0, miner_utxo) &&
        miner_utxo.value == miner_cut && miner_utxo.height == 1 && miner_utxo.is_coinbase,
        "miner reward is a height-1 coinbase UTXO");
    expect(f.wallet.get_balance(miner_address, state) == miner_cut &&
        f.node->get_balance(miner_address) == miner_cut,
        "wallet and node report the credited reward");

    const auto duplicate = f.node->process_block(initial);
    expect(!duplicate.active_tip_changed && state->get_balance(miner_address) == miner_cut &&
        state->get_total_supply() == starting_supply + subsidy,
        "duplicate submission cannot credit the reward twice");

    auto second = rpc::build_block_template(f.node->get_blockchain().get(), state.get(),
        f.node->get_mempool().get(), other_miner);
    expect(second.transactions[0].outputs[0].address == other_miner &&
        second.transactions[0].outputs[0].value == miner_cut,
        "RPC template directs next reward to requested address");
    expect(mine(*f.node, second), "mine second block for alternate payout");
    const auto second_submission = f.node->process_block(second);
    expect(second_submission.is_accepted() && second_submission.block_is_active &&
        second_submission.active_height == 2,
        "second mined block becomes active");
    expect(state->get_balance(miner_address) == miner_cut &&
        state->get_balance(other_miner) == miner_cut &&
        state->get_total_supply() == starting_supply + 2 * subsidy,
        "rewards reach distinct miners without duplication");

    const auto tip_before_stale = f.node->get_blockchain()->get_active_tip_snapshot();
    const auto stale_submission = f.node->process_block(stale);
    expect(!stale_submission.active_tip_changed &&
        f.node->get_blockchain()->get_active_tip_snapshot().hash == tip_before_stale.hash &&
        state->get_balance(stale_miner) == 0 &&
        state->get_total_supply() == starting_supply + 2 * subsidy,
        "stale competing block cannot credit a non-active reward");

    core::Transaction premature_spend;
    premature_spend.timestamp = second.header.timestamp;
    premature_spend.inputs = {{initial.transactions[0].get_hash(), 0}};
    premature_spend.outputs = {{miner_cut - 1000, other_miner, {}}};
    premature_spend.sign(test::make_key(131));
    std::string policy_reason;
    const auto immature_fee = state->calculate_transaction_fee(premature_spend);
    expect(!network::MempoolPolicy::check(premature_spend, *state, immature_fee,
        network::MempoolPolicyConfig{}, &policy_reason) &&
        policy_reason == "coinbase-not-mature" &&
        !f.node->get_mempool()->add_transaction(premature_spend),
        "mempool rejects immature mined reward with a maturity reason");
    auto premature_block = node::NodeTestAccess::create_mining_block(*f.node);
    premature_block.transactions[0] = core::Transaction::create_coinbase(
        3, miner_address, consensus::ProofOfWork::calculate_block_reward(3), immature_fee, foundation);
    premature_block.transactions.push_back(premature_spend);
    update_commitments(premature_block);
    expect(mine(*f.node, premature_block), "mine block containing immature spend");
    const auto supply_before_spend = state->get_total_supply();
    const auto premature_submission = f.node->process_block(premature_block);
    expect(!premature_submission.is_accepted() &&
        premature_submission.reject_reason == "bad-txns-premature-spend-of-coinbase" &&
        f.node->get_blockchain()->get_active_tip_snapshot().height == 2 &&
        state->get_balance(miner_address) == miner_cut &&
        state->get_balance(other_miner) == miner_cut &&
        state->get_total_supply() == supply_before_spend,
        "consensus rejects immature coinbase spend and rolls back block reward");

    state.reset();
    f.restart();
    state = f.node->get_chain_state();
    expect(f.node->get_blockchain()->get_active_tip_snapshot().height == 2 &&
        state->get_balance(miner_address) == miner_cut &&
        state->get_balance(other_miner) == miner_cut &&
        state->get_balance(stale_miner) == 0 &&
        state->get_total_supply() == starting_supply + 2 * subsidy,
        "active rewards and rejected stale payout survive restart");

    // Advance a real chain past the 100-confirmation maturity boundary.
    // The isolated loadnet has fixed, very easy PoW, so this remains a fast
    // end-to-end test rather than a simulated state-height change.
    for (Height height = 3; height <= 101; ++height) {
        auto block = node::NodeTestAccess::create_mining_block(*f.node);
        expect(block.transactions.size() == 1, "empty mempool creates reward-only block");
        expect(mine(*f.node, block), "mine reward-only maturity block");
        const auto submission = f.node->process_block(block);
        expect(submission.is_accepted() && submission.block_is_active &&
            submission.active_height == height, "maturity chain advances by one active block");
    }
    expect(state->get_balance(miner_address) == 100 * miner_cut &&
        state->get_balance(other_miner) == miner_cut &&
        state->get_total_supply() == starting_supply + 101 * subsidy,
        "101 mined blocks conserve every block subsidy");

    constexpr Amount fee1 = 100000;
    constexpr Amount fee2 = 200000;
    core::Transaction spend1;
    spend1.timestamp = initial.header.timestamp;
    spend1.inputs = {{initial.transactions[0].get_hash(), 0}};
    spend1.outputs = {{miner_cut - fee1, stale_miner, {}}};
    spend1.sign(test::make_key(131));
    core::Transaction spend2;
    spend2.timestamp = second.header.timestamp;
    spend2.inputs = {{second.transactions[0].get_hash(), 0}};
    spend2.outputs = {{miner_cut - fee2, stale_miner, {}}};
    spend2.sign(test::make_key(133));
    expect(state->calculate_transaction_fee(spend1) == fee1 &&
        state->calculate_transaction_fee(spend2) == fee2,
        "two mature reward spends have independently calculated fees");
    expect(f.node->get_mempool()->add_transaction(spend1) &&
        f.node->get_mempool()->add_transaction(spend2),
        "mempool admits spends after coinbase maturity");

    const Height fee_height = 102;
    const Amount total_fees = fee1 + fee2;
    const Amount fee_subsidy = consensus::ProofOfWork::calculate_block_reward(fee_height);
    const Amount fee_foundation_cut = consensus::calculate_foundation_reward_cut(fee_subsidy + total_fees);
    const Amount fee_miner_cut = fee_subsidy + total_fees - fee_foundation_cut;
    auto fee_block = node::NodeTestAccess::create_mining_block(*f.node);
    auto fee_rpc_block = rpc::build_block_template(f.node->get_blockchain().get(), state.get(),
        f.node->get_mempool().get(), other_miner, fee_block.header.timestamp);
    const auto contains_both_spends = [&](const core::Block& block) {
        if (block.transactions.size() != 3) return false;
        const auto first_hash = block.transactions[1].get_hash();
        const auto second_hash = block.transactions[2].get_hash();
        return (first_hash == spend1.get_hash() && second_hash == spend2.get_hash()) ||
               (first_hash == spend2.get_hash() && second_hash == spend1.get_hash());
    };
    expect(contains_both_spends(fee_block) && contains_both_spends(fee_rpc_block),
        "node and RPC templates include both fee-paying transactions");
    expect(fee_block.transactions[0].outputs[0].value == fee_miner_cut &&
        fee_block.transactions[0].outputs[1].value == fee_foundation_cut &&
        fee_rpc_block.transactions[0].outputs[0].value == fee_miner_cut &&
        fee_rpc_block.transactions[0].outputs[1].value == fee_foundation_cut,
        "node and RPC templates both split subsidy plus exact fees");

    const auto reject_fee_candidate = [&](const char* name, core::Block candidate,
                                          const char* expected_reason = nullptr) {
        update_commitments(candidate);
        expect(mine(*f.node, candidate), std::string(name) + ": mine candidate");
        const auto before_supply = state->get_total_supply();
        const auto before_miner = state->get_balance(miner_address);
        const auto before_foundation = state->get_balance(foundation);
        const auto before_recipient = state->get_balance(stale_miner);
        const auto before_utxos = state->get_utxo_count();
        const auto submission = f.node->process_block(candidate);
        expect(!submission.is_accepted() && !submission.active_tip_changed &&
            (!expected_reason || submission.reject_reason == expected_reason) &&
            f.node->get_blockchain()->get_active_tip_snapshot().height == 101 &&
            state->get_total_supply() == before_supply &&
            state->get_balance(miner_address) == before_miner &&
            state->get_balance(foundation) == before_foundation &&
            state->get_balance(stale_miner) == before_recipient &&
            state->get_utxo_count() == before_utxos &&
            f.node->get_mempool()->has_transaction(spend1.get_hash()) &&
            f.node->get_mempool()->has_transaction(spend2.get_hash()),
            std::string(name) + ": rejection is atomic and leaves mempool intact");
        std::cout << "PASS rejected: " << name << " (" << submission.reject_reason << ")\n";
    };
    auto overclaimed = fee_block;
    overclaimed.transactions[0] = core::Transaction::create_coinbase(
        fee_height, miner_address, fee_subsidy, total_fees + 1, foundation);
    reject_fee_candidate("one extra fee angle", overclaimed);
    auto underclaimed = fee_block;
    underclaimed.transactions[0] = core::Transaction::create_coinbase(
        fee_height, miner_address, fee_subsidy, total_fees - 1, foundation);
    reject_fee_candidate("one missing fee angle", underclaimed);
    auto omitted_payment = fee_block;
    omitted_payment.transactions.pop_back();
    reject_fee_candidate("claimed fee from omitted transaction", omitted_payment);

    auto conflicting_payment = spend2;
    conflicting_payment.outputs[0].value -= 100000;
    conflicting_payment.outputs[0].address = miner_address;
    conflicting_payment.sign(test::make_key(133));
    auto double_spend = fee_block;
    double_spend.transactions[0] = core::Transaction::create_coinbase(
        fee_height, miner_address, fee_subsidy, total_fees + fee2 + 100000, foundation);
    double_spend.transactions.push_back(conflicting_payment);
    reject_fee_candidate("two payments spend the same reward", double_spend,
        "bad-txns-inputs-missing-or-spent");

    auto bad_signature = fee_block;
    // Flip a nonce byte: the encoding stays canonical, so the signature check
    // itself rejects the block rather than the hardening encoding rule.
    bad_signature.transactions[1].witnesses[0].signature[1] ^= 1;
    reject_fee_candidate("tampered payment signature", bad_signature, "bad-txns-state-validation");

    auto nonfinal = fee_block;
    nonfinal.transactions[1].lock_time = fee_height + 1;
    nonfinal.transactions[1].inputs[0].sequence = core::TxInput::SEQUENCE_LOCKTIME;
    nonfinal.transactions[1].sign(test::make_key(
        nonfinal.transactions[1].inputs[0].prev_tx_hash == initial.transactions[0].get_hash() ? 131 : 133));
    reject_fee_candidate("nonfinal fee transaction", nonfinal, "bad-txns-nonfinal");

    auto missing_input = fee_block;
    const Amount removed_fee = state->calculate_transaction_fee(missing_input.transactions[1]);
    missing_input.transactions[1].inputs[0].vout = 999;
    missing_input.transactions[1].sign(test::make_key(
        missing_input.transactions[1].inputs[0].prev_tx_hash == initial.transactions[0].get_hash() ? 131 : 133));
    missing_input.transactions[0] = core::Transaction::create_coinbase(
        fee_height, miner_address, fee_subsidy, total_fees - removed_fee, foundation);
    reject_fee_candidate("fee transaction references missing UTXO", missing_input,
        "bad-txns-inputs-missing-or-spent");

    const auto supply_before_fees = state->get_total_supply();
    expect(mine(*f.node, fee_block), "mine block with two fee transactions");
    const auto fee_submission = f.node->process_block(fee_block);
    expect(fee_submission.is_accepted() && fee_submission.block_is_active &&
        fee_submission.active_height == fee_height,
        "fee-paying block becomes active");
    expect(state->get_total_supply() == supply_before_fees + fee_subsidy &&
        state->get_balance(miner_address) == 99 * miner_cut + fee_miner_cut &&
        state->get_balance(other_miner) == 0 &&
        state->get_balance(stale_miner) == 2 * miner_cut - total_fees &&
        state->get_balance(foundation) == starting_supply + 101 * foundation_cut + fee_foundation_cut,
        "fees redistribute existing coins without inflating total supply");
    state::UTXO spent_utxo;
    expect(!state->get_utxo(initial.transactions[0].get_hash(), 0, spent_utxo) &&
        !state->get_utxo(second.transactions[0].get_hash(), 0, spent_utxo) &&
        !f.node->get_mempool()->has_transaction(spend1.get_hash()) &&
        !f.node->get_mempool()->has_transaction(spend2.get_hash()),
        "confirmed payments consume reward UTXOs and leave mempool");

    state.reset();
    f.restart();
    state = f.node->get_chain_state();
    expect(f.node->get_blockchain()->get_active_tip_snapshot().height == fee_height &&
        state->get_total_supply() == supply_before_fees + fee_subsidy &&
        state->get_balance(miner_address) == 99 * miner_cut + fee_miner_cut &&
        state->get_balance(stale_miner) == 2 * miner_cut - total_fees &&
        state->get_balance(foundation) == starting_supply + 101 * foundation_cut + fee_foundation_cut,
        "fee-bearing reward and all balances survive restart");

    // Replace the fee-bearing block with a longer branch. The disconnected
    // payments must return to the mempool, while both their effects and the
    // old coinbase payout disappear from active UTXO state.
    auto* fork_parent = f.node->get_blockchain()->find_block_index(fee_block.header.prev_block_hash);
    expect(fork_parent != nullptr && fork_parent->height == fee_height - 1,
        "locate the parent of the fee-bearing block");
    auto replacement = node::NodeTestAccess::create_mining_block_for_parent(*f.node, fork_parent);
    replacement.transactions = {core::Transaction::create_coinbase(
        fee_height, reorg_miner, fee_subsidy, 0, foundation)};
    update_commitments(replacement);
    expect(mine_on_parent(replacement, *fork_parent), "mine competing fee-free block");
    // Stage the equal-work sibling without asking the node to activate another
    // candidate. The isolated regression test covers invalid candidates that
    // lose their failure mark after restart.
    consensus::ValidationState replacement_state;
    expect(consensus::check_block(replacement, replacement_state) &&
        consensus::contextual_check_block(replacement, replacement_state, fork_parent),
        "competing block passes body and contextual consensus validation");
    expect(f.node->get_blockchain()->accept_block(replacement, replacement_state, nullptr),
        "blockchain stores competing block on old parent");
    auto* replacement_index = f.node->get_blockchain()->find_block_index(replacement.get_hash());
    expect(replacement_index != nullptr && replacement_index->height == fee_height,
        "competing block is indexed as a side-branch candidate");

    auto extension = node::NodeTestAccess::create_mining_block_for_parent(*f.node, replacement_index);
    extension.transactions = {core::Transaction::create_coinbase(
        fee_height + 1, reorg_miner, consensus::ProofOfWork::calculate_block_reward(fee_height + 1),
        0, foundation)};
    update_commitments(extension);
    expect(mine_on_parent(extension, *replacement_index), "mine the heavier branch extension");
    const auto reorg_submission = f.node->process_block(extension);
    expect(reorg_submission.is_accepted() && reorg_submission.active_tip_changed &&
        reorg_submission.block_is_active && reorg_submission.active_height == fee_height + 1,
        "longer branch replaces fee-bearing active block");
    const auto verify_replacement_state = [&] {
        state::UTXO restored_first, restored_second, removed_coinbase;
        return state->get_balance(miner_address) == 100 * miner_cut &&
            state->get_balance(other_miner) == miner_cut &&
            state->get_balance(stale_miner) == 0 &&
            state->get_balance(reorg_miner) == 2 * miner_cut &&
            state->get_balance(foundation) == starting_supply + 103 * foundation_cut &&
            state->get_total_supply() == starting_supply + 103 * subsidy &&
            state->get_utxo(initial.transactions[0].get_hash(), 0, restored_first) &&
            state->get_utxo(second.transactions[0].get_hash(), 0, restored_second) &&
            !state->get_utxo(fee_block.transactions[0].get_hash(), 0, removed_coinbase) &&
            f.node->get_mempool()->has_transaction(spend1.get_hash()) &&
            f.node->get_mempool()->has_transaction(spend2.get_hash());
    };
    expect(verify_replacement_state(),
        "reorg removes old reward and payments, restores funding, and requeues both fees");

    const auto restore_old = f.node->activate_tip(fee_block.get_hash());
    expect(restore_old.is_accepted() && restore_old.active_tip_changed &&
        restore_old.active_height == fee_height,
        "explicit tip switch restores original fee-bearing branch");
    expect(state->get_balance(miner_address) == 99 * miner_cut + fee_miner_cut &&
        state->get_balance(stale_miner) == 2 * miner_cut - total_fees &&
        state->get_balance(reorg_miner) == 0 &&
        state->get_total_supply() == starting_supply + 102 * subsidy &&
        !f.node->get_mempool()->has_transaction(spend1.get_hash()) &&
        !f.node->get_mempool()->has_transaction(spend2.get_hash()),
        "switching back reapplies fee payout once and removes confirmed payments from mempool");

    const auto restore_new = f.node->activate_tip(extension.get_hash());
    expect(restore_new.is_accepted() && restore_new.active_tip_changed &&
        restore_new.active_height == fee_height + 1,
        "explicit tip switch restores longer branch");
    expect(verify_replacement_state(), "second reorg preserves rewards and mempool reconciliation");
    state.reset();
    f.restart();
    state = f.node->get_chain_state();
    expect(f.node->get_blockchain()->get_active_tip_snapshot().hash == extension.get_hash() &&
        state->get_balance(miner_address) == 100 * miner_cut &&
        state->get_balance(reorg_miner) == 2 * miner_cut &&
        state->get_balance(stale_miner) == 0 &&
        state->get_total_supply() == starting_supply + 103 * subsidy,
        "final reorg reward state survives restart");
}

void run_background_worker_scenario() {
    Fixture f;
    auto state = f.node->get_chain_state();
    const Amount initial_supply = state->get_total_supply();
    const Address miner = f.wallet.get_address();
    const Address foundation = state->get_foundation_address();
    const Amount subsidy = consensus::ProofOfWork::calculate_block_reward(1);
    const Amount cut = consensus::calculate_foundation_reward_cut(subsidy);
    expect(f.node->start_mining(), "start a real background mining worker");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (f.node->get_blockchain()->get_active_tip_snapshot().height == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    f.node->stop_mining();
    const auto height = f.node->get_blockchain()->get_active_tip_snapshot().height;
    const auto status = f.node->get_status();
    expect(height >= 1 && !status.mining && status.mined_blocks_accepted >= 1,
        "background worker mines and commits at least one active block");
    expect(state->get_balance(miner) == static_cast<Amount>(height) * (subsidy - cut) &&
        state->get_balance(foundation) == initial_supply + static_cast<Amount>(height) * cut &&
        state->get_total_supply() == initial_supply + static_cast<Amount>(height) * subsidy,
        "background worker credits each mined reward exactly once");
    state.reset();
    f.restart();
    state = f.node->get_chain_state();
    expect(f.node->get_blockchain()->get_active_tip_snapshot().height == height &&
        state->get_balance(miner) == static_cast<Amount>(height) * (subsidy - cut) &&
        state->get_total_supply() == initial_supply + static_cast<Amount>(height) * subsidy,
        "background-mined rewards survive a node restart");
}

void run_clean_fork_scenario() {
    Fixture f;
    auto state = f.node->get_chain_state();
    const Address first_miner = f.wallet.get_address();
    const Address second_miner = test::address_from_seed(136);
    const Address foundation = state->get_foundation_address();
    const Amount initial_foundation = state->get_balance(foundation);
    const Amount initial_supply = state->get_total_supply();
    const Amount subsidy = consensus::ProofOfWork::calculate_block_reward(1);
    const Amount cut = consensus::calculate_foundation_reward_cut(subsidy);
    const Amount miner_cut = subsidy - cut;
    auto* genesis = f.node->get_blockchain()->get_active_tip_snapshot().index;
    expect(genesis != nullptr && genesis->height == 0, "clean fork starts at genesis");

    auto first = node::NodeTestAccess::create_mining_block(*f.node);
    expect(mine(*f.node, first) && f.node->process_block(first).block_is_active,
        "first reward branch becomes active");
    auto sibling = node::NodeTestAccess::create_mining_block_for_parent(*f.node, genesis);
    sibling.transactions = {core::Transaction::create_coinbase(1, second_miner, subsidy, 0, foundation)};
    update_commitments(sibling);
    expect(mine_on_parent(sibling, *genesis), "mine equal-work reward sibling");
    const auto equal_work = f.node->process_block(sibling);
    expect(equal_work.is_accepted() && !equal_work.active_tip_changed &&
        f.node->get_blockchain()->get_active_tip_snapshot().hash == first.get_hash() &&
        state->get_balance(first_miner) == miner_cut && state->get_balance(second_miner) == 0 &&
        state->get_balance(foundation) == initial_foundation + cut &&
        state->get_total_supply() == initial_supply + subsidy,
        "equal-work sibling is indexed but does not credit an inactive reward");

    auto* sibling_index = f.node->get_blockchain()->find_block_index(sibling.get_hash());
    expect(sibling_index != nullptr && sibling_index->height == 1,
        "equal-work sibling has a height-one block index");
    auto extension = node::NodeTestAccess::create_mining_block_for_parent(*f.node, sibling_index);
    extension.transactions = {core::Transaction::create_coinbase(2, second_miner,
        consensus::ProofOfWork::calculate_block_reward(2), 0, foundation)};
    update_commitments(extension);
    expect(mine_on_parent(extension, *sibling_index), "mine second branch's extension");
    const auto heavier = f.node->process_block(extension);
    expect(heavier.is_accepted() && heavier.active_tip_changed && heavier.block_is_active &&
        heavier.active_height == 2 && state->get_balance(first_miner) == 0 &&
        state->get_balance(second_miner) == 2 * miner_cut &&
        state->get_balance(foundation) == initial_foundation + 2 * cut &&
        state->get_total_supply() == initial_supply + 2 * subsidy,
        "heavier sibling pays only its own miner and removes the stale reward");

    const auto old_branch = f.node->activate_tip(first.get_hash());
    expect(old_branch.is_accepted() && old_branch.active_tip_changed &&
        state->get_balance(first_miner) == miner_cut && state->get_balance(second_miner) == 0 &&
        state->get_total_supply() == initial_supply + subsidy,
        "forcing old tip restores exactly its one active reward");
    const auto new_branch = f.node->activate_tip(extension.get_hash());
    expect(new_branch.is_accepted() && new_branch.active_tip_changed &&
        state->get_balance(first_miner) == 0 && state->get_balance(second_miner) == 2 * miner_cut &&
        state->get_total_supply() == initial_supply + 2 * subsidy,
        "switching to heavier branch twice never duplicates coinbase credit");

    state.reset();
    f.restart();
    state = f.node->get_chain_state();
    expect(f.node->get_blockchain()->get_active_tip_snapshot().hash == extension.get_hash() &&
        state->get_balance(first_miner) == 0 && state->get_balance(second_miner) == 2 * miner_cut &&
        state->get_total_supply() == initial_supply + 2 * subsidy,
        "clean-fork reward choice survives restart");
}

void run_same_coinbase_fork_scenario() {
    Fixture f;
    auto state = f.node->get_chain_state();
    const Address miner = f.wallet.get_address();
    const Amount initial_supply = state->get_total_supply();
    const auto initial_utxos = state->get_utxo_count();
    const Amount subsidy = consensus::ProofOfWork::calculate_block_reward(1);
    const Amount miner_cut = subsidy - consensus::calculate_foundation_reward_cut(subsidy);
    auto* genesis = f.node->get_blockchain()->get_active_tip_snapshot().index;
    expect(genesis != nullptr && genesis->height == 0, "duplicate-coinbase fork starts at genesis");

    auto first = node::NodeTestAccess::create_mining_block(*f.node);
    expect(mine(*f.node, first) && f.node->process_block(first).block_is_active,
        "first branch with shared coinbase becomes active");
    auto sibling = node::NodeTestAccess::create_mining_block_for_parent(*f.node, genesis);
    sibling.transactions = first.transactions;
    sibling.header.timestamp = first.header.timestamp + 1;
    update_commitments(sibling);
    expect(mine_on_parent(sibling, *genesis) &&
        sibling.get_hash() != first.get_hash() &&
        sibling.transactions.front().get_hash() == first.transactions.front().get_hash(),
        "competing blocks differ but carry the same coinbase transaction ID");
    const auto inactive = f.node->process_block(sibling);
    expect(inactive.is_accepted() && !inactive.active_tip_changed &&
        state->get_balance(miner) == miner_cut &&
        state->get_total_supply() == initial_supply + subsidy &&
        state->get_utxo_count() == initial_utxos + 2,
        "inactive same-coinbase sibling does not duplicate its reward");

    auto* sibling_index = f.node->get_blockchain()->find_block_index(sibling.get_hash());
    expect(sibling_index != nullptr, "same-coinbase sibling is indexed");
    auto extension = node::NodeTestAccess::create_mining_block_for_parent(*f.node, sibling_index);
    expect(mine_on_parent(extension, *sibling_index), "mine longer same-coinbase branch");
    const auto heavier = f.node->process_block(extension);
    state::UTXO shared_reward;
    expect(heavier.is_accepted() && heavier.block_is_active && heavier.active_height == 2 &&
        state->get_balance(miner) == 2 * miner_cut &&
        state->get_total_supply() == initial_supply + 2 * subsidy &&
        state->get_utxo_count() == initial_utxos + 4 &&
        state->get_utxo(first.transactions.front().get_hash(), 0, shared_reward) &&
        shared_reward.height == 1 && shared_reward.value == miner_cut,
        "reorg with identical coinbase TXID keeps exactly one height-one reward");

    const auto restore_old = f.node->activate_tip(first.get_hash());
    expect(restore_old.is_accepted() && restore_old.active_tip_changed &&
        state->get_balance(miner) == miner_cut &&
        state->get_total_supply() == initial_supply + subsidy &&
        state->get_utxo_count() == initial_utxos + 2,
        "switching back removes only the extension's reward");
    const auto restore_new = f.node->activate_tip(extension.get_hash());
    expect(restore_new.is_accepted() && restore_new.active_tip_changed &&
        state->get_balance(miner) == 2 * miner_cut &&
        state->get_total_supply() == initial_supply + 2 * subsidy &&
        state->get_utxo_count() == initial_utxos + 4,
        "repeated same-TXID reorg does not multiply UTXOs");

    state.reset();
    f.restart();
    state = f.node->get_chain_state();
    expect(f.node->get_blockchain()->get_active_tip_snapshot().hash == extension.get_hash() &&
        state->get_balance(miner) == 2 * miner_cut &&
        state->get_total_supply() == initial_supply + 2 * subsidy &&
        state->get_utxo_count() == initial_utxos + 4,
        "same-TXID fork reward remains exact after restart");
}

void run_invalid_reward_restart_regression() {
    Fixture f;
    auto state = f.node->get_chain_state();
    auto invalid = node::NodeTestAccess::create_mining_block(*f.node);
    ++invalid.transactions.front().outputs.front().value;
    update_commitments(invalid);
    expect(mine(*f.node, invalid), "mine a block with an overpaid reward");
    const auto rejected = f.node->process_block(invalid);
    expect(!rejected.is_accepted() && rejected.reject_reason == "bad-cb-miner-amount" &&
        f.node->get_blockchain()->get_active_tip_snapshot().height == 0 &&
        state->get_balance(f.wallet.get_address()) == 0,
        "overpaid reward is rejected without changing active state");
    auto* before = f.node->get_blockchain()->find_block_index(invalid.get_hash());
    expect(before != nullptr && before->is_failed(),
        "rejected reward is marked invalid in the live block index");

    auto valid = node::NodeTestAccess::create_mining_block(*f.node);
    expect(mine(*f.node, valid) && f.node->process_block(valid).block_is_active &&
        f.node->get_blockchain()->get_active_tip_snapshot().height == 1,
        "valid competing reward becomes active at the same height");

    state.reset();
    f.restart();
    auto* after = f.node->get_blockchain()->find_block_index(invalid.get_hash());
    expect(after != nullptr && after->is_failed(),
        "rejected reward remains invalid in the block index after restart");
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "invalid-reward-restart") {
            run_invalid_reward_restart_regression();
            std::cout << "invalid reward restart regression passed\n";
            return 0;
        }
        run_scenarios();
        run_clean_fork_scenario();
        run_same_coinbase_fork_scenario();
        run_background_worker_scenario();
        std::cout << "mining reward scenarios passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "test_mining_reward_scenarios: " << error.what() << '\n';
        return 1;
    }
}
