#include "geocoin/consensus/consensus.hpp"
#include "geocoin/consensus/chainparams.hpp"
#include "geocoin/consensus/proof_of_work.hpp"
#include "geocoin/crypto/signatures.hpp"
#include "geocoin/utils/bytes.hpp"
#include "geocoin/core/transaction.hpp"
#include "geocoin/crypto/geo_area.hpp"
#include "geocoin/multisig/verifier.hpp"
#include "geocoin/network/mempool.hpp"
#include "geocoin/state/state.hpp"
#include "geocoin/wallet/multisig_service.hpp"
#include "test_utils.hpp"
#include "nlohmann_json.hpp"

#include <fstream>
#include <iostream>

namespace geocoin::state {
struct ChainStateTestAccess {
    static void seed_funding(ChainState& state, const core::Transaction& tx) {
        for (uint32_t i = 0; i < tx.outputs.size(); ++i) {
            UTXO utxo;
            utxo.tx_hash = tx.get_hash();
            utxo.vout = i;
            utxo.value = tx.outputs[i].value;
            utxo.address = tx.outputs[i].address;
            utxo.height = 1;
            utxo.is_coinbase = true;
            state.add_utxo(utxo);
        }
    }
    static void add_utxo(ChainState& state, const UTXO& utxo) { state.add_utxo(utxo); }
    static bool remove_utxo(ChainState& state, const Hash256& hash) { return state.remove_utxo(hash, 0); }
    static bool revert(ChainState& state, const core::Transaction& tx, const TxUndo& undo) {
        return state.revert_transaction(tx, undo);
    }
    static void seed_sidechain(ChainState& state, const Hash256& id,
                               const SidechainDeposit& deposit, const SidechainRegistryEntry& registry) {
        const auto key = utils::hash256_to_hex(id);
        state.sidechain_deposits_[key] = deposit;
        state.sidechain_registry_[key] = registry;
        state.persist_sidechain_deposit_to_storage(id, deposit);
        state.persist_sidechain_registry_to_storage(id, registry);
    }
    static TxUndo sidechain_undo(const ChainState& state, const core::Transaction& tx) {
        TxUndo undo;
        state.prepare_sidechain_undo_(tx, undo);
        return undo;
    }
    static void foundation(ChainState& state, const FoundationConfig& config) {
        state.foundation_config_ = config;
        state.persist_foundation_config_to_storage();
    }
};
}

namespace {
using namespace geocoin;
using core::Transaction;
using core::TransactionType;
nlohmann::json checks = nlohmann::json::array();

void check(bool result, const std::string& label) {
    checks.push_back({{"name", label}, {"passed", result}});
}

struct Fixture {
    bytes primary_key = bytes(32, 11);
    bytes delegated_key = bytes(32, 29);
    Address recipient = test::address_from_seed(71);
    multisig::MultisigConfig primary, delegated;
    std::filesystem::path directory = test::make_temp_dir("geocoin-authorization");
    std::shared_ptr<state::ChainState> state;
    Transaction funding, proof;

    Fixture(Amount per_transaction = 50000, Amount total = 90000, uint8_t percent = 0) {
        Address a, b;
        test::expect(crypto::compute_address_from_private_key(primary_key, a), "derive primary address");
        test::expect(crypto::compute_address_from_private_key(delegated_key, b), "derive delegated address");
        auto p = multisig::create_multisig_address(1, 1, {a});
        primary = p.config; primary.address = Address(p.address);
        auto d = multisig::create_multisig_address(1, 1, {b});
        delegated = d.config; delegated.address = Address(d.address);
        reopen();
        funding.outputs = {{100000, primary.address, {}}};
        // Isolated funding fixture; every authorization below uses real signatures
        // and the normal context-free/state validation and application paths.
        state::ChainStateTestAccess::seed_funding(*state, funding);
        test::expect(state->set_height(100), "set mature funding height");
        proof.type = TransactionType::DELEGATION_PROOF;
        proof.timestamp = 1700000000;
        proof.inputs = {{funding.get_hash(), 0}};
        proof.outputs = {{99500, primary.address, {}}};
        proof.primary_multisig_address = primary.address;
        proof.primary_m = proof.primary_n = 1;
        proof.primary_participants = {a};
        proof.delegated_multisig_address = delegated.address;
        proof.delegated_m = proof.delegated_n = 1;
        proof.delegated_participants = {b};
        proof.nonce = 7;
        proof.max_per_transaction = per_transaction;
        proof.max_total_delegated = total;
        proof.max_percent_of_balance = percent;
        authorize(proof, primary.address, primary_key, proof.get_delegation_proof_hash());
        consensus::ValidationState validation;
        test::expect(consensus::check_transaction(proof, validation), "proof passes structure checks");
        test::expect(state->apply_transaction(proof, 101, false), "apply signed proof");
        test::expect(state->set_height(101), "persist proof height");
    }
    ~Fixture() {
        state.reset();
        std::error_code error;
        std::filesystem::remove_all(directory, error);
    }
    void reopen() {
        state.reset();
        auto storage = std::make_unique<state::RocksDBStorage>((directory / "state").string());
        test::expect(storage->open(), "open isolated RocksDB");
        state = std::make_shared<state::ChainState>(std::move(storage));
        state->set_activation_manifest(utils::ActivationManifest::fully_enabled_for_tests("degree"));
    }
    void authorize(Transaction& tx, const Address& address, const bytes& key, const Hash256& hash) {
        const auto commitment = Transaction::create_angle_commitment(address, hash, key, {}, "degree");
        consensus::ValidationState validation;
        test::expect(consensus::check_transaction(commitment, validation), "commitment passes structure checks");
        test::expect(state->apply_transaction(commitment, 101, false), "apply signed commitment");
        tx.angle_commitment_hashes = {commitment.get_hash()};
    }
    Transaction spend(Amount payment, Amount change) {
        auto tx = Transaction::create_multisig_spend(
            {{proof.get_hash(), 0}}, {{payment, recipient, {}}, {change, primary.address, {}}}, {}, 1, 1);
        tx.delegation_proof_txid = proof.get_hash();
        authorize(tx, delegated.address, delegated_key, tx.get_hierarchical_spend_hash());
        return tx;
    }
    Transaction revoke() {
        Transaction tx;
        tx.type = TransactionType::REVOKE_DELEGATION;
        tx.timestamp = 1700000001;
        tx.revoked_delegation_proof_txid = proof.get_hash();
        tx.inputs = {{proof.get_hash(), 0}};
        tx.outputs = {{99000, primary.address, {}}};
        authorize(tx, primary.address, primary_key, tx.get_revoke_delegation_hash());
        return tx;
    }
};

void test_complete_signing_intent() {
    Fixture f;
    const auto tx = f.spend(2000, 97000);
    const auto hash = tx.get_hierarchical_spend_hash();
    const auto reject = [&](Transaction changed, const std::string& field) {
        check(changed.get_hierarchical_spend_hash() != hash, "hierarchical hash binds " + field);
        multisig::MultisigVerifier verifier(f.state->get_angle_store());
        check(!verifier.verify_hierarchical_spend(changed, f.proof), "existing signatures reject changed " + field);
    };
    auto changed = tx; ++changed.inputs[0].vout; reject(changed, "input index");
    changed = tx; changed.inputs[0].prev_tx_hash[0] ^= 1; reject(changed, "input transaction");
    changed = tx; --changed.inputs[0].sequence; reject(changed, "input sequence");
    changed = tx; ++changed.lock_time; reject(changed, "lock time");
    changed = tx; ++changed.version; reject(changed, "transaction version");
    changed = tx; ++changed.outputs[0].value; reject(changed, "output amount");
    changed = tx; changed.angle_commitment_hashes.clear();
    check(changed.get_hierarchical_spend_hash() == hash, "signing hash excludes only commitment references");
}

void test_delegation_limits_and_undo() {
    for (const auto mode : {0, 1, 2}) {
        Fixture f(mode == 0 ? 3000 : 0, mode == 1 ? 3000 : 90000, mode == 2 ? 3 : 0);
        const auto tx = f.spend(2000, 94500); // 2000 payout + 3000 fee = 5000 debit.
        check(!f.state->validate_transaction(tx), "delegated fee counts against limit " + std::to_string(mode));
    }
    Fixture f;
    const auto tx = f.spend(2000, 97000); // Debit 2500, including fee 500.
    state::TxUndo undo;
    state::UTXO input;
    test::expect(f.state->get_utxo(f.proof.get_hash(), 0, input), "read spent UTXO for undo");
    undo.spent_utxos.push_back(input);
    test::expect(f.state->apply_transaction(tx, 102, false), "apply valid delegated spend");
    check(f.state->get_delegation_total_spent(f.proof.get_hash()) == 2500,
          "delegation stores the complete debit");
    check(state::ChainStateTestAccess::revert(*f.state, tx, undo), "revert delegated spend");
    check(f.state->get_delegation_total_spent(f.proof.get_hash()) == 0 &&
              f.state->get_balance(f.primary.address) == 99500,
          "revert restores delegated budget and primary funds");
}

void test_restart_and_reference_binding() {
    for (bool revocation : {false, true}) {
        Fixture f;
        const auto tx = revocation ? f.revoke() : f.spend(2000, 97000);
        const auto label = revocation ? "revocation" : "delegated spend";
        test::expect(f.state->validate_transaction(tx), "warm-cache transaction validates");
        f.reopen();
        check(f.state->validate_transaction(tx), std::string(label) + " validates after RocksDB reopen");
    }
    Fixture f;
    auto spend = f.spend(2000, 97000);
    auto revoke = f.revoke();
    auto unrelated = f.proof;
    ++unrelated.nonce;
    f.authorize(unrelated, f.primary.address, f.primary_key, unrelated.get_delegation_proof_hash());
    multisig::MultisigVerifier verifier(f.state->get_angle_store());
    check(!verifier.verify_hierarchical_spend(spend, unrelated), "verifier binds spend to the exact proof id");
    check(!verifier.verify_revoke_delegation(revoke, unrelated), "verifier binds revoke to the exact proof id");
    Transaction empty;
    empty.type = TransactionType::MULTISIG_SPEND;
    empty.m = empty.n = 0;
    multisig::MultisigConfig empty_config;
    empty_config.m = empty_config.n = 0;
    check(!verifier.verify_multisig_spend(empty, empty_config), "explicit multisig config rejects zero quorum");
}

void test_multisig_dust_totals() {
    auto chain = std::make_shared<state::ChainState>(std::make_unique<state::MemoryStorage>());
    const auto address = test::address_from_seed(77);
    const auto policy = multisig::create_multisig_address(1, 1, {address});
    auto config = policy.config;
    config.address = Address(policy.address);
    Transaction funding;
    funding.outputs = {{20000, config.address, {}}};
    state::ChainStateTestAccess::seed_funding(*chain, funding);
    test::expect(chain->set_height(100), "mature draft funding");
    wallet::MultisigDraftResult result;
    std::string error;
    test::expect(wallet::create_multisig_draft(config, test::address_from_seed(78), 10000, 9500,
                                             chain, result, error), "build dust-change draft");
    Amount output = 0;
    for (const auto& value : result.transaction.outputs) output += value.value;
    check(result.transaction.outputs.size() == 1 && result.total_output == output &&
              result.fee_used == result.total_input - output,
          "multisig draft totals include absorbed dust in fee");
}

void test_pending_delegation_budget() {
    Fixture f(10000, 20000);
    Transaction funding;
    funding.timestamp = 1700000002;
    funding.outputs = {{50000, f.primary.address, {}}, {50000, f.primary.address, {}},
                       {50000, f.primary.address, {}}};
    state::ChainStateTestAccess::seed_funding(*f.state, funding);
    network::Mempool pool(f.state);
    std::vector<Transaction> spends, commitments;
    for (uint32_t index = 0; index < 3; ++index) {
        auto tx = Transaction::create_multisig_spend(
            {{funding.get_hash(), index}},
            {{2000, f.recipient, {}}, {42000, f.primary.address, {}}}, {}, 1, 1);
        tx.delegation_proof_txid = f.proof.get_hash();
        const auto commitment = Transaction::create_angle_commitment(
            f.delegated.address, tx.get_hierarchical_spend_hash(), f.delegated_key, {}, "degree");
        test::expect(pool.add_transaction(commitment), "signed commitment enters mempool");
        tx.angle_commitment_hashes = {commitment.get_hash()};
        spends.push_back(tx);
        commitments.push_back(commitment);
    }
    test::expect(pool.add_transaction(spends[0]) && pool.add_transaction(spends[1]),
                 "two spends fit the pending budget including fees");
    const bool excess_accepted = pool.add_transaction(spends[2]);
    check(!excess_accepted, "mempool reserves fees against the combined delegation budget");
    if (excess_accepted) pool.remove_transaction(spends[2].get_hash());

    test::expect(f.state->apply_transaction(commitments[0], 102, false) &&
                     f.state->apply_transaction(spends[0], 102, false),
                 "confirm the first pending spend");
    test::expect(pool.remove_transaction(spends[0].get_hash()), "remove confirmed pending spend");
    check(!pool.add_transaction(spends[2]),
          "confirmed spend removal preserves the remaining reservation including fees");
    pool.remove_transaction(spends[2].get_hash());
    test::expect(pool.remove_transaction(spends[1].get_hash()), "release the remaining pending spend");
    check(pool.add_transaction(spends[2]), "removal releases the complete pending debit");
}

std::vector<std::pair<Transaction, Transaction>> pending_spends(Fixture& f, network::Mempool& pool) {
    Transaction funding;
    funding.timestamp = 1700000020;
    funding.outputs = {{50000, f.primary.address, {}}, {50000, f.primary.address, {}},
                       {50000, f.primary.address, {}}};
    state::ChainStateTestAccess::seed_funding(*f.state, funding);
    std::vector<std::pair<Transaction, Transaction>> result;
    for (uint32_t index = 0; index < 3; ++index) {
        auto spend = Transaction::create_multisig_spend(
            {{funding.get_hash(), index}},
            {{2000, f.recipient, {}}, {42000, f.primary.address, {}}}, {}, 1, 1);
        spend.delegation_proof_txid = f.proof.get_hash();
        auto commitment = Transaction::create_angle_commitment(
            f.delegated.address, spend.get_hierarchical_spend_hash(), f.delegated_key, {}, "degree");
        spend.angle_commitment_hashes = {commitment.get_hash()};
        if (index < 2) {
            test::expect(pool.add_transaction(commitment) && pool.add_transaction(spend),
                         "admit lifecycle package");
        }
        result.emplace_back(spend, commitment);
    }
    return result;
}

void test_pending_budget_after_proof_rollback() {
    Fixture f(10000, 20000);
    network::Mempool pool(f.state);
    const auto packages = pending_spends(f, pool);
    state::TxUndo undo;
    undo.spent_utxos.push_back({f.funding.get_hash(), 0, 100000, f.primary.address, 1, true});
    test::expect(state::ChainStateTestAccess::revert(*f.state, f.proof, undo), "disconnect delegation proof");
    test::expect(!f.state->get_transaction(f.proof.get_hash()), "disconnected proof is absent");
    pool.remove_block_transactions(core::Block{});
    check(!pool.has_transaction(packages[0].first.get_hash()) &&
              !pool.has_transaction(packages[1].first.get_hash()),
          "chain reconciliation removes spends of a disconnected proof");
    test::expect(f.state->apply_transaction(f.proof, 101, false), "reconnect identical delegation proof");
    check(pool.add_transaction(packages[0].first) && pool.add_transaction(packages[1].first),
          "proof rollback releases pending debits before the proof is reconnected");
}

void test_pending_budget_after_external_confirmation() {
    Fixture f(10000, 20000);
    network::Mempool pool(f.state);
    const auto packages = pending_spends(f, pool);
    test::expect(f.state->apply_transaction(packages[2].second, 102, false) &&
                     f.state->apply_transaction(packages[2].first, 102, false),
                 "confirm a package received outside this mempool");
    const auto selected_roots = [&]() {
        size_t roots = 0;
        for (const auto& tx : pool.get_transactions_for_mining(1000000)) {
            if (tx.type == TransactionType::MULTISIG_SPEND) ++roots;
        }
        return roots;
    };
    check(selected_roots() == 1, "mining selection respects the remaining aggregate delegation budget");
    core::Block connected;
    connected.transactions = {packages[2].second, packages[2].first};
    pool.remove_block_transactions(connected);
    const size_t surviving_roots = pool.has_transaction(packages[0].first.get_hash()) +
        pool.has_transaction(packages[1].first.get_hash());
    check(surviving_roots == 1 && selected_roots() == 1,
          "chain reconciliation releases pending spends that exceed the updated total limit");
    bool applied = true;
    for (const auto& tx : pool.get_transactions_for_mining(1000000)) {
        if (!f.state->apply_transaction(tx, 103, false)) { applied = false; break; }
    }
    check(applied && f.state->get_delegation_total_spent(f.proof.get_hash()) <= 20000,
          "selected package applies within the updated delegation budget");
}

void test_multisig_draft_spendable_inputs() {
    Fixture f;
    // Smaller unavailable outputs must not displace the usable proof change.
    Hash256 immature_hash{}; immature_hash[0] = 91;
    Hash256 future_hash{}; future_hash[0] = 92;
    state::ChainStateTestAccess::add_utxo(*f.state, {immature_hash, 0, 20000, f.primary.address, 101, true});
    state::ChainStateTestAccess::add_utxo(*f.state, {future_hash, 0, 15000, f.primary.address, 103, false});
    wallet::MultisigDraftResult direct;
    wallet::HierarchicalSpendDraftResult delegated;
    wallet::HierarchicalSpendRequest request;
    request.delegation_proof_txid = f.proof.get_hash();
    request.delegated_config = f.delegated;
    request.recipient = f.recipient;
    request.amount = 10000;
    request.fee = 6000;
    request.timestamp = 1700000021;
    std::string error;
    const auto uses_usable_input = [&](const Transaction& tx) {
        return tx.inputs.size() == 1 && tx.inputs[0].prev_tx_hash == f.proof.get_hash();
    };
    check(wallet::create_multisig_draft(f.primary, f.recipient, 10000, 6000, f.state, direct, error) &&
              uses_usable_input(direct.transaction),
          "direct multisig draft skips immature rewards and future UTXOs");
    check(wallet::create_hierarchical_spend_draft(request, f.state, delegated, error) &&
              uses_usable_input(delegated.transaction),
          "delegated draft skips immature rewards and future UTXOs");
    test::expect(state::ChainStateTestAccess::remove_utxo(*f.state, f.proof.get_hash()),
                 "remove the only available draft input");
    check(!wallet::create_multisig_draft(f.primary, f.recipient, 10000, 6000, f.state, direct, error) &&
              !wallet::create_hierarchical_spend_draft(request, f.state, delegated, error),
          "both draft builders reject unavailable balance before collecting signatures");
}

void test_mining_delegation_order() {
    for (bool direct_first : {false, true}) {
        Fixture f(0, 1000000, 50);
        network::Mempool pool(f.state);
        Transaction funding;
        funding.timestamp = 1700000022;
        funding.outputs = {{120000, f.primary.address, {}}, {120000, f.primary.address, {}}};
        state::ChainStateTestAccess::seed_funding(*f.state, funding);
        for (uint32_t index = 0; index < 2; ++index) {
            auto tx = Transaction::create_multisig_spend(
                {{funding.get_hash(), index}}, {{index == 0 ? 100000 : 114000, f.recipient, {}}}, {}, 1, 1);
            const bool direct = direct_first && index == 0;
            if (direct) tx.spend_participants = f.primary.addresses;
            else tx.delegation_proof_txid = f.proof.get_hash();
            const auto commitment = Transaction::create_angle_commitment(
                direct ? f.primary.address : f.delegated.address,
                direct ? tx.get_hash_without_commitments() : tx.get_hierarchical_spend_hash(),
                direct ? f.primary_key : f.delegated_key, {}, "degree");
            tx.angle_commitment_hashes = {commitment.get_hash()};
            test::expect(pool.add_transaction(commitment) && pool.add_transaction(tx),
                         "admit individually valid percentage-limit packages");
        }
        const auto selected = pool.get_transactions_for_mining(1000000);
        check(selected.size() == 2 && std::count_if(selected.begin(), selected.end(), [](const auto& tx) {
                  return tx.type == TransactionType::MULTISIG_SPEND;
              }) == 1,
              direct_first ? "mining accounts for a direct spend before applying the percentage limit" :
                             "mining accounts for a delegated spend before applying the percentage limit");
        bool applied = true;
        for (const auto& tx : selected) {
            if (!f.state->apply_transaction(tx, 102, false)) { applied = false; break; }
        }
        check(applied, direct_first ? "direct-first percentage-limited selection applies in order" :
                                    "delegated-first percentage-limited selection applies in order");
    }

    for (bool revoke_first : {false, true}) {
        Fixture f(50000, 90000);
        network::Mempool pool(f.state);
        Transaction funding;
        funding.timestamp = 1700000023;
        funding.outputs = {{50000, f.primary.address, {}}, {50000, f.primary.address, {}}};
        state::ChainStateTestAccess::seed_funding(*f.state, funding);
        auto spend = Transaction::create_multisig_spend(
            {{funding.get_hash(), 0}},
            {{2000, f.recipient, {}}, {revoke_first ? 42000 : 28000, f.primary.address, {}}}, {}, 1, 1);
        spend.delegation_proof_txid = f.proof.get_hash();
        Transaction revoke;
        revoke.type = TransactionType::REVOKE_DELEGATION;
        revoke.timestamp = 1700000024;
        revoke.revoked_delegation_proof_txid = f.proof.get_hash();
        revoke.inputs = {{funding.get_hash(), 1}};
        revoke.outputs = {{revoke_first ? 30000 : 44000, f.primary.address, {}}};
        for (auto* tx : {&spend, &revoke}) {
            const bool is_revoke = tx == &revoke;
            const auto commitment = Transaction::create_angle_commitment(
                is_revoke ? f.primary.address : f.delegated.address,
                is_revoke ? tx->get_revoke_delegation_hash() : tx->get_hierarchical_spend_hash(),
                is_revoke ? f.primary_key : f.delegated_key, {}, "degree");
            tx->angle_commitment_hashes = {commitment.get_hash()};
            test::expect(pool.add_transaction(commitment) && pool.add_transaction(*tx),
                         "admit independently funded spend and revocation");
        }
        const auto selected = pool.get_transactions_for_mining(1000000);
        const bool selected_spend = std::any_of(selected.begin(), selected.end(), [&](const auto& tx) {
            return tx.get_hash() == spend.get_hash();
        });
        const bool selected_revoke = std::any_of(selected.begin(), selected.end(), [&](const auto& tx) {
            return tx.get_hash() == revoke.get_hash();
        });
        check(selected_revoke && selected_spend != revoke_first && selected.size() == (revoke_first ? 2 : 4),
              revoke_first ? "mining excludes spends after an earlier revocation in the same template" :
                             "mining permits a spend followed by revocation in the same template");
        bool applied = true;
        for (const auto& tx : selected) {
            if (!f.state->apply_transaction(tx, 102, false)) { applied = false; break; }
        }
        check(applied, revoke_first ? "revocation-first selection applies without orphan commitments" :
                                    "spend-first selection applies before revocation");
    }
}

void test_protocol_authorization_reuse() {
    Fixture f;
    Hash256 id{}; id[0] = 81;
    state::SidechainDeposit deposit;
    deposit.owner = f.primary.addresses.front();
    deposit.amount = 100 * COIN;
    deposit.unlock_height = 1000;
    state::SidechainRegistryEntry registry;
    registry.owner = deposit.owner;
    registry.issuance_limit = 100 * COIN;
    registry.multiplier = registry.max_multiplier = consensus::params::MIN_SIDECHAIN_MULTIPLIER;
    registry.token_symbol = "AUD";
    registry.committee_m = registry.committee_n = 2;
    registry.committee_members = {f.primary.addresses.front(), f.delegated.addresses.front()};
    std::sort(registry.committee_members.begin(), registry.committee_members.end(),
              [](const auto& a, const auto& b) { return a.str() < b.str(); });
    registry.committee_id = consensus::compute_sidechain_committee_id(2, 2, registry.committee_members);
    state::ChainStateTestAccess::seed_sidechain(*f.state, id, deposit, registry);
    Transaction mint;
    mint.type = TransactionType::MINT_SIDECHAIN;
    mint.timestamp = 1700000003;
    mint.sidechain_id = id;
    mint.sidechain_owner = deposit.owner;
    mint.sidechain_amount = COIN;
    mint.sidechain_committee_id = registry.committee_id;
    const auto hash = crypto::sha256(core::build_sidechain_payload_for_signing(mint));
    for (const auto& key : {f.primary_key, f.delegated_key}) {
        mint.sidechain_committee_signatures.push_back(crypto::sign_recoverable_message(hash, key));
    }
    consensus::ValidationState validation;
    test::expect(consensus::check_transaction(mint, validation) && f.state->validate_transaction(mint),
                 "committee-signed mint validates");
    auto changed = mint; ++changed.timestamp;
    check(!f.state->validate_transaction(changed), "committee signatures bind transaction timestamp");
    changed = mint; ++changed.lock_time;
    check(!f.state->validate_transaction(changed), "committee signatures bind transaction lock time");
    auto alternate = mint;
    std::reverse(alternate.sidechain_committee_signatures.begin(), alternate.sidechain_committee_signatures.end());
    {
        network::Mempool pool(f.state);
        test::expect(pool.add_transaction(mint), "committee mint enters mempool");
        check(!pool.add_transaction(alternate), "mempool rejects duplicate committee authorization with reordered signatures");
    }
    const auto undo = state::ChainStateTestAccess::sidechain_undo(*f.state, mint);
    test::expect(f.state->apply_transaction(mint, 102, false), "committee mint applies");
    check(!f.state->validate_transaction(alternate), "state rejects reuse of committee authorization");
    check(!f.state->apply_transaction(alternate, 103, false), "reordered signatures cannot apply the same mint twice");
    f.reopen();
    test::expect(f.state->get_storage()->delete_transaction(mint.get_hash()), "remove redundant mint archive");
    check(!f.state->validate_transaction(alternate), "committee authorization remains consumed after restart and archive cleanup");
    check(state::ChainStateTestAccess::revert(*f.state, mint, undo), "revert committee authorization");
    check(f.state->validate_transaction(alternate), "reorg releases committee authorization for reconfirmation");
}

void test_angle_package_eviction() {
    for (unsigned mode = 0; mode < 4; ++mode) {
        Fixture f;
        network::Mempool pool(f.state);
        const Amount root_fee = mode == 2 ? 20000 : 6000;
        auto root = Transaction::create_multisig_spend(
            {{f.proof.get_hash(), 0}},
            {{2000, f.recipient, {}}, {99500 - 2000 - root_fee, f.primary.address, {}}}, {}, 1, 1);
        root.delegation_proof_txid = f.proof.get_hash();
        const auto commitment = Transaction::create_angle_commitment(
            f.delegated.address, root.get_hierarchical_spend_hash(), f.delegated_key, {}, "degree");
        root.angle_commitment_hashes = {commitment.get_hash()};
        Hash256 unrelated_hash{};
        size_t unrelated_size = 0;
        if (mode == 1) {
            auto intent = root.get_hierarchical_spend_hash(); intent[0] ^= 1;
            const auto unrelated = Transaction::create_angle_commitment(
                f.delegated.address, intent, f.delegated_key, {}, "degree");
            unrelated_hash = unrelated.get_hash();
            unrelated_size = unrelated.serialize().size();
            test::expect(pool.add_transaction(unrelated), "admit an unrelated orphan commitment");
        }
        test::expect(pool.add_transaction(commitment), "admit required commitment before capacity check");
        const size_t package_size = commitment.serialize().size() + root.serialize().size();
        auto candidate = root;
        if (mode < 2) {
            pool.set_max_size(package_size + unrelated_size - 1);
        } else {
            test::expect(pool.add_transaction(root), "admit the existing complete package");
            Transaction funding;
            funding.timestamp = 1700000030;
            funding.outputs = {{2 * COIN, test::address_from_seed(95), {}}};
            state::ChainStateTestAccess::seed_funding(*f.state, funding);
            candidate = Transaction{};
            candidate.timestamp = 1700000031;
            candidate.inputs = {{funding.get_hash(), 0}};
            const Amount candidate_fee = mode == 2 ? 10000 : COIN;
            candidate.outputs = {{2 * COIN - candidate_fee, f.recipient, {}}};
            candidate.sign(test::make_key(95));
            const size_t needed = mode == 2 ? 1 : root.serialize().size() + 1;
            pool.set_max_size(package_size + candidate.serialize().size() - needed);
        }
        const auto before = pool.get_all_transaction_hashes();
        const bool preview = pool.test_accept_transaction(candidate);
        check(pool.get_all_transaction_hashes() == before, "eviction preview leaves mempool unchanged, case " + std::to_string(mode));
        const bool accepted = pool.add_transaction(candidate);
        if (mode == 0) {
            check(!preview && !accepted && pool.has_transaction(commitment.get_hash()),
                  "an oversized package cannot evict its own required commitment");
        } else if (mode == 1) {
            check(preview && accepted && pool.has_transaction(commitment.get_hash()) &&
                      !pool.has_transaction(unrelated_hash) && pool.get_transactions_for_mining(1000000).size() == 2,
                  "admission evicts an unrelated orphan while preserving the candidate package");
        } else if (mode == 2) {
            check(!preview && !accepted && pool.has_transaction(root.get_hash()) &&
                      pool.has_transaction(commitment.get_hash()),
                  "a cheaper candidate cannot evict a valuable package through its zero-fee commitment");
        } else {
            check(preview && accepted && !pool.has_transaction(root.get_hash()) &&
                      !pool.has_transaction(commitment.get_hash()),
                  "a more valuable candidate can evict a complete cheaper package");
        }
    }
}

void test_chainwork_rounding() {
    consensus::SelectParams(consensus::Network::TESTNET);
    // floor(2^256 / (target + 1)); independently computed integer vectors.
    check(consensus::calculate_chainwork(nullptr, 0x207fffff) == uint256(2),
          "chainwork at testnet limit uses floor, not ceiling");
    check(consensus::calculate_chainwork(nullptr, 0x1d00ffff) == uint256(4295032833ULL),
          "chainwork at mainnet limit has the exact expected weight");
    consensus::SelectParams(consensus::Network::MAIN);
}

void test_foundation_rotation_authorization() {
    Fixture f;
    const std::vector<Address> members{f.primary.addresses.front(), f.delegated.addresses.front()};
    const Address a(multisig::create_multisig_address(2, 2, members).address);
    const Address b(multisig::create_multisig_address(1, 2, members).address);
    state::ChainStateTestAccess::foundation(*f.state, {a, 2, 2, members});
    const auto update = [&](bool forward, Timestamp timestamp) {
        Transaction tx;
        tx.type = TransactionType::UPDATE_FOUNDATION_ADDRESS;
        tx.timestamp = timestamp;
        tx.foundation_old_address = forward ? a : b;
        tx.foundation_new_address = forward ? b : a;
        tx.foundation_m = forward ? 2 : 1;
        tx.foundation_new_m = forward ? 1 : 2;
        tx.foundation_n = tx.foundation_new_n = 2;
        tx.foundation_participants = tx.foundation_new_participants = members;
        const auto hash = crypto::sha256(core::build_foundation_update_payload_for_signing(tx));
        tx.foundation_signatures.push_back(crypto::sign_recoverable_message(hash, f.primary_key));
        if (forward) tx.foundation_signatures.push_back(crypto::sign_recoverable_message(hash, f.delegated_key));
        return tx;
    };
    const auto forward = update(true, 1700000010);
    auto changed = forward; ++changed.timestamp;
    check(!f.state->validate_transaction(changed), "foundation signatures bind the complete transaction");
    test::expect(f.state->apply_transaction(forward, 102, false), "apply authorized foundation rotation");
    test::expect(f.state->apply_transaction(update(false, 1700000011), 103, false), "authorize rotation back");
    auto reordered = forward;
    std::reverse(reordered.foundation_signatures.begin(), reordered.foundation_signatures.end());
    f.reopen();
    check(!f.state->validate_transaction(reordered), "old foundation approval stays consumed after rotation cycle and restart");
    check(f.state->apply_transaction(update(true, 1700000012), 104, false),
          "fresh foundation approval permits a later rotation");
}
}

int main(int argc, char** argv) {
    try {
        consensus::SelectParams(consensus::Network::MAIN);
        test_complete_signing_intent();
        test_delegation_limits_and_undo();
        test_restart_and_reference_binding();
        test_multisig_dust_totals();
        test_pending_delegation_budget();
        test_pending_budget_after_proof_rollback();
        test_pending_budget_after_external_confirmation();
        test_multisig_draft_spendable_inputs();
        test_mining_delegation_order();
        test_protocol_authorization_reuse();
        test_angle_package_eviction();
        test_chainwork_rounding();
        test_foundation_rotation_authorization();
        size_t failures = 0;
        for (const auto& result : checks) {
            if (!result["passed"].get<bool>()) {
                ++failures; std::cerr << "FAILED: " << result["name"] << '\n';
            }
        }
        if (argc > 1) std::ofstream(argv[1]) << nlohmann::json{
            {"checks", checks}, {"passed", checks.size() - failures}, {"failed", failures}}.dump(2) << '\n';
        std::cout << "authorization checks: " << checks.size() - failures << '/' << checks.size() << '\n';
        return failures ? 1 : 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 2;
    }
}
