#include "test_utils.hpp"
#include "geocoin/consensus/chainparams.hpp"
#include "geocoin/consensus/consensus.hpp"
#include "geocoin/consensus/proof_of_work.hpp"
#include "geocoin/core/blockchain.hpp"
#include "geocoin/network/mempool.hpp"
#include "geocoin/node/node.hpp"
#include "geocoin/rpc/rpc_mining_helpers.hpp"
#include "geocoin/state/records.hpp"
#include "geocoin/utils/bytes.hpp"
#include "nlohmann_json.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <limits>

namespace geocoin::core {
struct BlockchainTestAccess {
    static void set_tip(Blockchain& chain, const Block& block, Height height, Timestamp time) {
        auto header = block.header;
        header.timestamp = static_cast<uint32_t>(time);
        auto index = std::make_unique<chain::BlockIndex>(header);
        index->hash = block.get_hash();
        index->height = height;
        index->status = chain::BlockStatus::VALID_SCRIPTS;
        auto* ptr = index.get();
        chain.block_index_[index->hash] = std::move(index);
        chain.chain_active_.set_tip(ptr);
    }
};
}

namespace geocoin::node {
struct NodeTestAccess {
    static core::Block select(Node& node, std::shared_ptr<state::ChainState> state,
                              std::shared_ptr<network::Mempool> pool, const core::ActiveTipSnapshot& tip) {
        struct Restore {
            Node& node;
            std::shared_ptr<state::ChainState>& state;
            std::shared_ptr<network::Mempool>& pool;
            ~Restore() { node.chain_state_.swap(state); node.mempool_.swap(pool); }
        } restore{node, state, pool};
        node.chain_state_.swap(state);
        node.mempool_.swap(pool);
        return node.create_mining_block_for_tip(tip);
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
Timestamp now = 0;

void check(bool ok, const std::string& name) {
    checks.push_back({{"name", std::string(use_rocksdb ? "RocksDB: " : "MemoryStorage: ") + name},
                      {"passed", ok}});
}

struct Fixture {
    std::filesystem::path path = test::make_temp_dir("geocoin-delegation-time");
    bytes primary_key = test::make_key(11), delegated_key = test::make_key(29);
    Address primary_member = test::address_from_seed(11), delegated_member = test::address_from_seed(29);
    Address primary = Address(multisig::create_multisig_address(1, 1, {primary_member}).address);
    Address delegated = Address(multisig::create_multisig_address(1, 1, {delegated_member}).address);
    Address recipient = test::address_from_seed(71);
    std::shared_ptr<state::ChainState> state;
    std::shared_ptr<core::Blockchain> chain;
    std::shared_ptr<network::Mempool> pool;
    Transaction proof, spend, commitment;
    core::Block proof_block;
    static constexpr Amount fee = 20000;

    Fixture(Timestamp from, Timestamp until, Timestamp spend_time) {
        auto storage = backend();
        state::FoundationConfig foundation;
        foundation.address = consensus::Params().foundation_address();
        foundation.m = consensus::Params().foundation_m();
        foundation.n = consensus::Params().foundation_n();
        foundation.participants = consensus::Params().foundation_participants();
        expect(storage->put_metadata("idx_epoch", bytes(32, '3')), "seed epoch");
        expect(storage->put_metadata("chainstate_utxo_count", utils::encode_u64_le(0)), "seed UTXO count");
        expect(storage->put_metadata("foundation_cfg", state::records::serialize_foundation_config(foundation)),
               "seed foundation");
        expect(storage->put_metadata("interest_pool_balance", utils::encode_u64_le(0)), "seed pool");
        state = std::make_shared<state::ChainState>(std::move(storage));
        state->set_activation_manifest(utils::ActivationManifest::fully_enabled_for_tests("degree"));
        auto funding = Transaction::create_coinbase(1, primary, 50 * COIN, 0, foundation.address);
        expect(state->apply_transaction(funding, 1, true), "create isolated funding");
        expect(state->set_height(100) && state->set_tip_hash(test::hash_from_seed(72)), "set mature state");

        proof.type = TransactionType::DELEGATION_PROOF;
        proof.timestamp = now - 1800;
        proof.inputs = {{funding.get_hash(), 0}};
        proof.outputs = {{funding.outputs.front().value - fee, primary, {}}};
        proof.primary_multisig_address = primary;
        proof.primary_m = proof.primary_n = 1;
        proof.primary_participants = {primary_member};
        proof.delegated_multisig_address = delegated;
        proof.delegated_m = proof.delegated_n = 1;
        proof.delegated_participants = {delegated_member};
        proof.valid_from = from;
        proof.valid_until = until;
        proof.nonce = 7;
        proof.max_total_delegated = proof.outputs.front().value;
        const auto authorization = Transaction::create_angle_commitment(
            primary, proof.get_delegation_proof_hash(), primary_key, {}, "degree");
        proof.angle_commitment_hashes = {authorization.get_hash()};
        proof_block = block({authorization, proof}, now - 1800);
        consensus::ValidationState validation;
        if (!consensus::check_block(proof_block, validation)) {
            throw std::runtime_error("signed proof block: " + validation.get_reject_reason() + " " +
                validation.get_debug_message());
        }
        expect(state->apply_block(proof_block, validation, true), "apply signed proof block");

        spend = Transaction::create_multisig_spend({{proof.get_hash(), 0}},
            {{COIN, recipient, {}}, {proof.outputs.front().value - COIN - fee, primary, {}}}, {}, 1, 1);
        spend.timestamp = spend_time;
        spend.delegation_proof_txid = proof.get_hash();
        commitment = Transaction::create_angle_commitment(
            delegated, spend.get_hierarchical_spend_hash(), delegated_key, {}, "degree");
        spend.angle_commitment_hashes = {commitment.get_hash()};
        connect();
    }
    ~Fixture() {
        pool.reset(); chain.reset(); state.reset();
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
    std::unique_ptr<state::Storage> backend() const {
        if (!use_rocksdb) return std::make_unique<state::MemoryStorage>();
        auto storage = std::make_unique<state::RocksDBStorage>((path / "state").string());
        expect(storage->open(), "open isolated RocksDB");
        return storage;
    }
    void connect() {
        chain = std::make_shared<core::Blockchain>(state, nullptr);
        core::BlockchainTestAccess::set_tip(*chain, proof_block, 101, now - 1800);
        pool = std::make_shared<network::Mempool>(state);
        expect(pool->add_transaction(commitment) && pool->add_transaction(spend), "admit signed package");
    }
    void restart() {
        pool.reset(); chain.reset();
        std::unique_ptr<state::Storage> storage;
        if (!use_rocksdb) storage = std::make_unique<state::MemoryStorage>(
            *dynamic_cast<state::MemoryStorage*>(state->get_storage()));
        state.reset();
        if (use_rocksdb) storage = backend();
        state = std::make_shared<state::ChainState>(std::move(storage));
        state->set_activation_manifest(utils::ActivationManifest::fully_enabled_for_tests("degree"));
        connect();
    }
    core::Block block(const std::vector<Transaction>& txs, Timestamp time) const {
        core::Block result;
        result.header.prev_block_hash = state->get_tip_hash();
        result.header.timestamp = static_cast<uint32_t>(time);
        result.header.bits = 0x207fffff;
        result.transactions = {Transaction::create_coinbase(state->get_height() + 1, recipient,
            consensus::ProofOfWork::calculate_block_reward(state->get_height() + 1), fee,
            state->get_foundation_address())};
        result.transactions.insert(result.transactions.end(), txs.begin(), txs.end());
        result.header.merkle_root = result.calculate_merkle_root();
        result.header.witness_commitment = result.calculate_witness_commitment();
        return result;
    }
    core::Block selected(Timestamp time) const {
        return rpc::build_block_template(chain.get(), state.get(), pool.get(), recipient, time);
    }
};

void test_window(const std::string& label, Timestamp from, Timestamp until, Timestamp spend_time,
                 const std::vector<std::pair<Timestamp, bool>>& cases) {
    Fixture f(from, until, spend_time);
    for (unsigned restart = 0; restart < 2; ++restart) {
        for (const auto& [time, include] : cases) {
            const std::string name = label + " time=" + std::to_string(time) +
                (restart ? " after restart" : "");
            const auto block = f.selected(time);
            check(block.header.timestamp == time, name + ": requested template time is retained");
            check(block.transactions.size() == (include ? 3 : 1), name + ": selects complete eligible package");
            check(f.pool->size() == 2, name + ": selection leaves pending package intact");
            consensus::ValidationState validation;
            check(consensus::check_block(block, validation), name + ": selected block passes body validation");
            const bool applied = f.state->apply_block(block, validation, true);
            check(applied, name + ": selected block applies to chain state");
            if (applied) expect(f.state->revert_block(block), "undo selected block");
            check(f.state->get_height() == 101 && f.state->get_tip_hash() == f.proof_block.get_hash() &&
                      f.state->get_delegation_total_spent(f.proof.get_hash()) == 0,
                  name + ": rejected or reverted block preserves delegation state");
            const auto repeated = f.selected(time);
            check(repeated.transactions.size() == block.transactions.size(), name + ": repeatable selection");
        }
        if (!restart) f.restart();
    }
}

void test_time_clamping() {
    Fixture f(0, now + 300, now);
    core::BlockchainTestAccess::set_tip(*f.chain, f.proof_block, 101, f.proof.valid_until);
    auto rpc_block = f.selected(now);
    check(rpc_block.header.timestamp == f.proof.valid_until + 1 && rpc_block.transactions.size() == 1,
          "RPC selection uses time after the median-time clamp");

    node::NodeConfig config;
    config.data_dir = (f.path / "node").string();
    config.network = "degree";
    config.enable_auto_connect = false;
    config.enable_p2p_listen = false;
    config.enable_mining = false;
    config.mining_address = f.recipient;
    node::Node node(config);
    const auto block = node::NodeTestAccess::select(node, f.state, f.pool, f.chain->get_active_tip_snapshot());
    check(block.header.timestamp == f.proof.valid_until + 1 && block.transactions.size() == 1,
          "node selection uses time after the median-time clamp");
    consensus::ValidationState validation;
    check(consensus::check_block(block, validation) && f.state->apply_block(block, validation, true),
          "node's clamped template applies without the expired package");
    if (f.state->get_height() == 102) expect(f.state->revert_block(block), "undo clamped node block");

    core::BlockchainTestAccess::set_tip(*f.chain, f.proof_block, 101, now - 1800);
    const auto eligible = node::NodeTestAccess::select(node, f.state, f.pool, f.chain->get_active_tip_snapshot());
    check(eligible.transactions.size() == 3 && eligible.transactions.back().get_hash() == f.spend.get_hash(),
          "node includes the complete package at an eligible timestamp");
    check(f.pool->size() == 2 && f.selected(now).transactions.size() == 3,
          "earlier candidate after tip change can include a previously skipped package");
}

void test_skipped_package() {
    Fixture f(0, 0, now);
    auto funding = Transaction::create_coinbase(2, f.primary, 50 * COIN, 0, f.state->get_foundation_address());
    expect(f.state->apply_transaction(funding, 2, true), "seed independent mature funding");
    auto stale = Transaction::create_multisig_spend({{funding.get_hash(), 0}},
        {{COIN, f.recipient, {}}, {funding.outputs.front().value - COIN - 2 * Fixture::fee, f.primary, {}}}, {}, 1, 1);
    stale.timestamp = now - consensus::params::MAX_FUTURE_BLOCK_TIME - 1;
    stale.delegation_proof_txid = f.proof.get_hash();
    const auto authorization = Transaction::create_angle_commitment(
        f.delegated, stale.get_hierarchical_spend_hash(), f.delegated_key, {}, "degree");
    stale.angle_commitment_hashes = {authorization.get_hash()};
    expect(f.pool->add_transaction(authorization) && f.pool->add_transaction(stale), "admit higher-fee stale package");
    const size_t limit = std::max(f.commitment.serialize().size() + f.spend.serialize().size(),
                                  authorization.serialize().size() + stale.serialize().size());
    const auto selected = f.pool->get_transactions_for_mining(limit, now);
    check(selected.size() == 2 && selected.front().get_hash() == f.commitment.get_hash() &&
              selected.back().get_hash() == f.spend.get_hash(),
          "skipped higher-fee package leaves room for the eligible package sharing its proof");
    check(f.pool->get_transactions_for_mining(0, now).empty() && f.pool->size() == 4,
          "empty template does not remove either package");
    const auto default_time = f.pool->get_transactions_for_mining(1000000);
    check(default_time.size() == 2 && default_time.back().get_hash() == f.spend.get_hash(),
          "default selection captures current time and omits a stale spend");
    const auto earlier = f.pool->get_transactions_for_mining(limit, stale.timestamp);
    check(earlier.size() == 2 && earlier.back().get_hash() == stale.get_hash(),
          "a fresh selection can choose the higher-fee package for an earlier block time");
}

void test_time_arithmetic() {
    Transaction proof, spend;
    proof.type = TransactionType::DELEGATION_PROOF;
    const auto max = (std::numeric_limits<Timestamp>::max)();
    spend.timestamp = max;
    check(!consensus::is_delegated_spend_valid_at(spend, proof, 0), "timestamp subtraction cannot wrap at zero");
    spend.timestamp = 0;
    check(!consensus::is_delegated_spend_valid_at(spend, proof, max), "timestamp subtraction cannot wrap at maximum");
    check(consensus::is_delegated_spend_valid_at(spend, proof, 0), "explicit zero timestamp is evaluated as a value");
    proof.valid_from = proof.valid_until = max;
    spend.timestamp = max;
    check(consensus::is_delegated_spend_valid_at(spend, proof, max) &&
              !consensus::is_delegated_spend_valid_at(spend, proof, max - 1),
          "closed one-second validity interval works at the integer boundary");
}

void test_zero_block_context() {
    Fixture f(0, 0, now);
    const auto block = f.block({f.commitment, f.spend}, 0);
    consensus::ValidationState validation;
    check(!f.state->apply_block(block, validation, true),
          "zero block timestamp still applies the delegated spend timestamp bound");
    check(f.state->get_height() == 101 && f.state->has_utxo(f.proof.get_hash(), 0),
          "rejected zero-time block preserves the delegation input and height");
    check(f.state->validate_transaction(f.spend),
          "delegation validation outside a block uses the restored admission time context");
}
}

int main(int argc, char** argv) {
    try {
        consensus::SelectParams(consensus::Network::MAIN);
        now = static_cast<Timestamp>(std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count()) - 120;
        const Timestamp skew = consensus::params::MAX_FUTURE_BLOCK_TIME;
        test_time_arithmetic();
        for (bool rocks : {false, true}) {
            use_rocksdb = rocks;
            test_window("start", now - 300, 0, now,
                {{now - 301, false}, {now - 300, true}, {now - 299, true}});
            test_window("expiry", 0, now + 300, now,
                {{now + 301, false}, {now + 300, true}, {now + 299, true}});
            test_window("spend timestamp", 0, 0, now,
                {{now - skew - 1, false}, {now - skew, true}, {now - skew + 1, true},
                 {now + skew + 1, false}, {now + skew, true}, {now + skew - 1, true}});
            test_time_clamping();
            test_skipped_package();
            test_zero_block_context();
        }
        size_t failed = 0;
        for (const auto& c : checks) if (!c["passed"].get<bool>()) {
            ++failed; std::cerr << "FAILED: " << c["name"] << '\n';
        }
        const nlohmann::json report{{"checks", checks}, {"passed", checks.size() - failed}, {"failed", failed}};
        if (argc > 1) std::ofstream(argv[1]) << report.dump(2) << '\n';
        std::cout << "delegation timing checks: " << checks.size() - failed << '/' << checks.size() << '\n';
        return failed ? 1 : 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 2;
    }
}
