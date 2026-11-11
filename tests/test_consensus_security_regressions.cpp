#include "test_utils.hpp"
#include "geocoin/consensus/chainparams.hpp"
#include "geocoin/consensus/consensus.hpp"
#include "geocoin/consensus/proof_of_work.hpp"
#include "geocoin/consensus/sidechain_checkpoint.hpp"
#include "geocoin/consensus/sidechain_fee.hpp"
#include "geocoin/core/merkle.hpp"
#include "geocoin/crypto/signatures.hpp"
#include "geocoin/state/records.hpp"
#include "geocoin/state/state.hpp"
#include "geocoin/utils/bytes.hpp"

#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace {
using namespace geocoin;
using core::Transaction;
using core::TransactionType;
using test::expect;

size_t failures = 0;
size_t passed = 0;
bool use_rocksdb = false;

void check(bool condition, const std::string& name) {
    const std::string label = std::string(use_rocksdb ? "RocksDB: " : "MemoryStorage: ") + name;
    if (condition) {
        ++passed;
        return;
    }
    ++failures;
    std::cerr << "FAILED: " << label << '\n';
}

constexpr uint8_t kOwnerSeed = 151;
constexpr uint8_t kMinerSeed = 152;

const Address& owner() {
    static const Address address = test::address_from_seed(kOwnerSeed);
    return address;
}

const Address& miner() {
    static const Address address = test::address_from_seed(kMinerSeed);
    return address;
}

Transaction coinbase(Height height, Amount fees) {
    auto tx = Transaction::create_coinbase(height, miner(),
        consensus::ProofOfWork::calculate_block_reward(height), fees,
        consensus::Params().foundation_address());
    tx.timestamp = 1700000000;
    return tx;
}

struct Fixture {
    std::filesystem::path path = test::make_temp_dir("geocoin-consensus-security");
    std::shared_ptr<state::ChainState> state;

    explicit Fixture(Height next_height, const std::map<std::string, bytes>& extra = {}) {
        std::unique_ptr<state::Storage> storage;
        if (use_rocksdb) {
            auto db = std::make_unique<state::RocksDBStorage>((path / "state").string());
            expect(db->open(), "open isolated RocksDB");
            storage = std::move(db);
        } else {
            storage = std::make_unique<state::MemoryStorage>();
        }
        const auto& params = consensus::Params();
        const state::FoundationConfig foundation{params.foundation_address(), params.foundation_m(),
            params.foundation_n(), params.foundation_participants()};
        expect(storage->put_metadata("chainstate_height", utils::encode_u64_le(next_height - 1)),
               "seed persisted height");
        expect(storage->put_metadata("idx_epoch", bytes(32, '3')), "seed index epoch");
        expect(storage->put_metadata("chainstate_utxo_count", utils::encode_u64_le(0)), "seed UTXO count");
        expect(storage->put_metadata("foundation_cfg", state::records::serialize_foundation_config(foundation)),
               "seed foundation");
        expect(storage->put_metadata("interest_pool_balance", utils::encode_u64_le(0)), "seed pool");
        for (const auto& [key, value] : extra) expect(storage->put_metadata(key, value), "seed record");
        state = std::make_shared<state::ChainState>(std::move(storage));
        expect(state->set_height(next_height - 1), "seed preceding height");
        expect(state->set_tip_hash(test::hash_from_seed(153)), "seed preceding tip");
    }

    ~Fixture() {
        state.reset();
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }

    core::Block block(std::vector<Transaction> transactions) const {
        core::Block block;
        block.header.prev_block_hash = state->get_tip_hash();
        block.header.timestamp = 1700000000;
        block.header.bits = 0x207fffff;
        block.transactions = std::move(transactions);
        block.header.merkle_root = block.calculate_merkle_root();
        block.header.witness_commitment = block.calculate_witness_commitment();
        return block;
    }

    // Coins in UTXOs, in the interest pool and locked as sidechain collateral.
    Amount money_supply(const std::vector<Hash256>& sidechains) const {
        Amount total = state->get_total_supply() + state->get_interest_pool_balance();
        for (const auto& id : sidechains) {
            state::SidechainDeposit deposit;
            if (state->get_sidechain_deposit(id, deposit)) total += deposit.amount;
        }
        return total;
    }
};

void sign_owner(Transaction& tx) {
    tx.sidechain_signature = crypto::sign_recoverable_message(
        crypto::sha256(core::build_sidechain_payload_for_signing(tx)), test::make_key(kOwnerSeed));
}

void sign_committee(Transaction& tx) {
    tx.sidechain_committee_signatures = {crypto::sign_recoverable_message(
        crypto::sha256(core::build_sidechain_payload_for_signing(tx)), test::make_key(kOwnerSeed))};
}

const std::vector<Address>& committee() {
    static const std::vector<Address> members{owner()};
    return members;
}

const Hash256& committee_id() {
    static const Hash256 id = consensus::compute_sidechain_committee_id(1, 1, committee());
    return id;
}

Transaction lock_deposit(const Hash256& sidechain_id, const Transaction& funding, Amount amount, Amount fee) {
    Transaction tx;
    tx.type = TransactionType::LOCK_DEPOSIT;
    tx.timestamp = 1700000002;
    tx.sidechain_id = sidechain_id;
    tx.sidechain_owner = owner();
    tx.sidechain_amount = amount;
    tx.sidechain_unlock_height = 1000;
    tx.inputs = {{funding.get_hash(), 0}};
    tx.outputs = {{funding.outputs.front().value - amount - fee, owner(), {}}};
    sign_owner(tx);
    tx.sign(test::make_key(kOwnerSeed));
    return tx;
}

Transaction issue_sidechain(const Hash256& sidechain_id, uint32_t multiplier, uint32_t fee_bps) {
    Transaction tx;
    tx.type = TransactionType::ISSUE_SIDECHAIN;
    tx.timestamp = 1700000003;
    tx.sidechain_id = sidechain_id;
    tx.sidechain_owner = owner();
    tx.sidechain_checkpoint = sidechain_id;
    tx.sidechain_token_symbol = "NEW";
    tx.sidechain_multiplier = multiplier;
    tx.sidechain_max_multiplier = multiplier;
    tx.sidechain_fee_bps = fee_bps;
    tx.sidechain_committee_m = tx.sidechain_committee_n = 1;
    tx.sidechain_committee_members = committee();
    tx.sidechain_committee_id = committee_id();
    sign_owner(tx);
    return tx;
}

Transaction mint_sidechain(const Hash256& sidechain_id, Amount amount) {
    Transaction tx;
    tx.type = TransactionType::MINT_SIDECHAIN;
    tx.timestamp = 1700000004;
    tx.sidechain_id = sidechain_id;
    tx.sidechain_owner = owner();
    tx.sidechain_amount = amount;
    tx.sidechain_committee_id = committee_id();
    sign_committee(tx);
    return tx;
}

Transaction funding_coinbase() {
    return Transaction::create_coinbase(1, owner(), 50 * COIN, 0, consensus::Params().foundation_address());
}

// A registry deleted and recreated inside one block must not let the coinbase
// claim a MINT fee computed from the pre-block registry.
void test_recreated_sidechain_fee_cannot_be_claimed() {
    constexpr Height height = 200;
    const Hash256 sidechain_id = test::hash_from_seed(154);

    state::SidechainDeposit deposit;
    deposit.owner = owner();
    deposit.amount = 10 * COIN;
    deposit.unlock_height = 100;
    state::SidechainRegistryEntry registry;
    registry.status = state::SidechainStatus::DECOMMISSIONING;
    registry.decommission_height = 3;
    registry.owner = owner();
    registry.multiplier = consensus::params::MIN_SIDECHAIN_MULTIPLIER;
    registry.max_multiplier = consensus::params::MIN_SIDECHAIN_MULTIPLIER;
    registry.fee_bps = consensus::params::MAX_SIDECHAIN_FEE_BPS;
    registry.issuance_limit = deposit.amount * registry.multiplier;
    registry.token_symbol = "OLD";
    registry.committee_m = registry.committee_n = 1;
    registry.committee_members = committee();
    registry.committee_id = committee_id();
    const auto hex = utils::hash256_to_hex(sidechain_id);
    Fixture f(height, {
        {"scd:" + hex, state::records::serialize_sidechain_deposit(deposit)},
        {"sc:reg:" + hex, state::records::serialize_sidechain_registry(registry)}});
    const auto funding = funding_coinbase();
    expect(f.state->apply_transaction(funding, 1, true), "seed mature funding");

    Transaction unlock;
    unlock.type = TransactionType::UNLOCK_DEPOSIT;
    unlock.timestamp = 1700000001;
    unlock.sidechain_id = sidechain_id;
    unlock.sidechain_owner = owner();
    unlock.outputs = {{deposit.amount, owner(), {}}};
    sign_owner(unlock);

    constexpr Amount lock_fee = 20000;
    constexpr Amount mint_amount = 100000 * COIN;
    const auto lock = lock_deposit(sidechain_id, funding, COIN, lock_fee);
    const auto issue = issue_sidechain(sidechain_id, consensus::params::MAX_SIDECHAIN_MULTIPLIER, 0);
    const auto mint = mint_sidechain(sidechain_id, mint_amount);

    const Amount paid_fees = lock_fee + consensus::calc_sidechain_fee(0, 0) +
        consensus::calc_sidechain_fee(mint_amount, 0);
    const Amount stale_mint_fee = consensus::calc_sidechain_fee(
        mint_amount, consensus::params::MAX_SIDECHAIN_FEE_BPS);
    const Amount inflated_fees = paid_fees - consensus::calc_sidechain_fee(mint_amount, 0) + stale_mint_fee;
    expect(stale_mint_fee > consensus::calc_sidechain_fee(mint_amount, 0),
           "fixture must make the stale fee larger");

    const std::vector<Transaction> payload{unlock, lock, issue, mint};
    check(f.state->calculate_transaction_fee(mint) == stale_mint_fee,
          "standalone estimate still prices the mint with the committed registry");
    const auto projected = f.state->project_block_transaction_fees(payload, 0, height);
    check(projected.size() == payload.size() && projected[0] == 0 && projected[1] == lock_fee &&
              projected[3] == consensus::calc_sidechain_fee(mint_amount, 0),
          "block projection prices the mint with the registry recreated earlier in the block");

    auto make_block = [&](Amount claimed_fees) {
        std::vector<Transaction> txs{coinbase(height, claimed_fees)};
        txs.insert(txs.end(), payload.begin(), payload.end());
        return f.block(std::move(txs));
    };

    const Amount supply_before = f.money_supply({sidechain_id});
    consensus::ValidationState inflated_state;
    const bool inflated_applied = f.state->apply_block(make_block(inflated_fees), inflated_state, true);
    check(!inflated_applied && inflated_state.is_invalid() &&
              inflated_state.get_reject_reason().rfind("bad-cb-", 0) == 0,
          "coinbase cannot claim a fee from a sidechain registry replaced in the same block");
    check(f.state->get_height() == height - 1 && f.money_supply({sidechain_id}) == supply_before,
          "rejected recreated-registry block leaves the money supply unchanged");
    if (inflated_applied) return;

    consensus::ValidationState honest_state;
    const bool honest_applied = f.state->apply_block(make_block(paid_fees), honest_state, true);
    check(honest_applied, "coinbase may claim the fee actually charged by the recreated registry: " +
          honest_state.get_reject_reason());
    if (!honest_applied) return;
    state::SidechainRegistryEntry recreated;
    check(f.state->get_sidechain_registry(sidechain_id, recreated) && recreated.fee_bps == 0 &&
              recreated.total_issued == mint_amount,
          "recreated registry is applied with its own fee policy");
    check(f.money_supply({sidechain_id}) ==
              supply_before + consensus::ProofOfWork::calculate_block_reward(height),
          "recreated-registry block creates exactly the block subsidy");
}

// A MINT of a sidechain issued earlier in the same block pays the new
// registry's fee; the coinbase must claim exactly that amount.
void test_same_block_issue_and_mint_fee() {
    constexpr Height height = 200;
    const Hash256 sidechain_id = test::hash_from_seed(155);
    Fixture f(height);
    const auto funding = funding_coinbase();
    expect(f.state->apply_transaction(funding, 1, true), "seed mature funding");

    constexpr Amount lock_fee = 20000;
    constexpr uint32_t fee_bps = 100;
    constexpr Amount mint_amount = 500 * COIN;
    const auto lock = lock_deposit(sidechain_id, funding, 10 * COIN, lock_fee);
    const auto issue = issue_sidechain(sidechain_id, consensus::params::MIN_SIDECHAIN_MULTIPLIER, fee_bps);
    const auto mint = mint_sidechain(sidechain_id, mint_amount);
    const Amount mint_fee = consensus::calc_sidechain_fee(mint_amount, fee_bps);
    const Amount paid_fees = lock_fee + consensus::calc_sidechain_fee(0, fee_bps) + mint_fee;

    auto make_block = [&](Amount claimed_fees) {
        return f.block({coinbase(height, claimed_fees), lock, issue, mint});
    };
    const Amount supply_before = f.money_supply({sidechain_id});
    consensus::ValidationState underclaim_state;
    check(!f.state->apply_block(make_block(paid_fees - mint_fee), underclaim_state, true) &&
              underclaim_state.is_invalid() && f.money_supply({sidechain_id}) == supply_before,
          "coinbase that ignores the same-block registry fee is rejected");

    consensus::ValidationState honest_state;
    const bool applied = f.state->apply_block(make_block(paid_fees), honest_state, true);
    check(applied, "coinbase claims the fee charged by a registry issued in the same block: " +
          honest_state.get_reject_reason());
    if (!applied) return;
    check(f.money_supply({sidechain_id}) ==
              supply_before + consensus::ProofOfWork::calculate_block_reward(height),
          "same-block issue and mint conserve the money supply");
}

state::SidechainDeposit seeded_deposit() {
    state::SidechainDeposit deposit;
    deposit.owner = owner();
    deposit.amount = 10 * COIN;
    deposit.unlock_height = 100;
    return deposit;
}

state::SidechainRegistryEntry seeded_registry(const state::SidechainDeposit& deposit, uint32_t fee_bps) {
    state::SidechainRegistryEntry registry;
    registry.owner = owner();
    registry.multiplier = consensus::params::MIN_SIDECHAIN_MULTIPLIER;
    registry.max_multiplier = consensus::params::MIN_SIDECHAIN_MULTIPLIER;
    registry.fee_bps = fee_bps;
    registry.issuance_limit = deposit.amount * registry.multiplier;
    registry.token_symbol = "TST";
    registry.committee_m = registry.committee_n = 1;
    registry.committee_members = committee();
    registry.committee_id = committee_id();
    return registry;
}

Transaction register_accounting(const Hash256& sidechain_id) {
    Transaction tx;
    tx.type = TransactionType::REGISTER_ACCOUNTING_SIDECHAIN;
    tx.timestamp = 1700000005;
    tx.sidechain_id = sidechain_id;
    tx.sidechain_owner = owner();
    tx.sidechain_checkpoint = sidechain_id;
    tx.sidechain_namespace = "ledger";
    tx.sidechain_schema_hash = test::hash_from_seed(157);
    tx.sidechain_checkpoint_policy_id = consensus::params::ACCOUNTING_CHECKPOINT_POLICY_ID;
    tx.sidechain_checkpoint_version = consensus::params::ACCOUNTING_CHECKPOINT_VERSION;
    tx.sidechain_committee_m = tx.sidechain_committee_n = 1;
    tx.sidechain_committee_members = committee();
    tx.sidechain_committee_id = committee_id();
    sign_owner(tx);
    return tx;
}

// Disconnecting a block must restore the collateral its sidechain fees took.
// A node that reorganized must otherwise disagree with a node that never saw
// the stale block about every later deposit-bounded operation.
void test_sidechain_fee_revert(const std::string& name, const Transaction& operation,
                               bool registered) {
    constexpr Height height = 200;
    const auto deposit = seeded_deposit();
    const auto hex = utils::hash256_to_hex(operation.sidechain_id);
    std::map<std::string, bytes> records{{"scd:" + hex, state::records::serialize_sidechain_deposit(deposit)}};
    if (registered) {
        records["sc:reg:" + hex] = state::records::serialize_sidechain_registry(seeded_registry(deposit, 100));
    }
    Fixture f(height, records);
    const auto fees = f.state->project_block_transaction_fees({operation}, 0, height);
    expect(fees.size() == 1 && fees.front() > 0, "sidechain operation must charge a fee");
    const auto block = f.block({coinbase(height, fees.front()), operation});

    state::SidechainRegistryEntry registry_before;
    const bool had_registry = f.state->get_sidechain_registry(operation.sidechain_id, registry_before);
    consensus::ValidationState validation;
    const bool applied = f.state->apply_block(block, validation, true);
    check(applied, name + ": block applies: " + validation.get_reject_reason());
    if (!applied) return;
    state::SidechainDeposit charged;
    check(f.state->get_sidechain_deposit(operation.sidechain_id, charged) &&
              charged.amount == deposit.amount - fees.front(),
          name + ": the fee is paid from the deposit");

    check(f.state->revert_block(block), name + ": block reverts");
    state::SidechainDeposit restored;
    check(f.state->get_sidechain_deposit(operation.sidechain_id, restored) &&
              restored.amount == deposit.amount,
          name + ": revert returns the fee to the deposit");
    state::SidechainRegistryEntry registry_after;
    const bool has_registry = f.state->get_sidechain_registry(operation.sidechain_id, registry_after);
    check(has_registry == had_registry &&
              (!had_registry || registry_after.total_issued == registry_before.total_issued),
          name + ": revert restores the registry");

    validation.reset();
    check(f.state->apply_block(block, validation, true) &&
              f.state->get_sidechain_deposit(operation.sidechain_id, charged) &&
              charged.amount == deposit.amount - fees.front(),
          name + ": reconnecting the block charges the fee exactly once");
}

Transaction redeem_sidechain(const Hash256& sidechain_id, Amount amount, const Address& recipient) {
    Transaction tx;
    tx.type = TransactionType::REDEEM_SIDECHAIN;
    tx.timestamp = 1700000006;
    tx.sidechain_id = sidechain_id;
    tx.sidechain_owner = owner();
    tx.sidechain_amount = amount;
    tx.outputs = {{amount, recipient, {}}};
    tx.sidechain_committee_id = committee_id();
    sign_committee(tx);
    return tx;
}

// REDEEM releases collateral into mainchain outputs. Disconnecting it must
// return the whole debit (amount plus fee) and restore the issued supply.
void test_redeem_revert() {
    constexpr Height height = 200;
    const Hash256 sidechain_id = test::hash_from_seed(160);
    const Address recipient = test::address_from_seed(161);
    const auto deposit = seeded_deposit();
    auto registry = seeded_registry(deposit, 100);
    registry.total_issued = 5 * COIN;
    const auto hex = utils::hash256_to_hex(sidechain_id);
    Fixture f(height, {
        {"scd:" + hex, state::records::serialize_sidechain_deposit(deposit)},
        {"sc:reg:" + hex, state::records::serialize_sidechain_registry(registry)}});

    constexpr Amount amount = 2 * COIN;
    const auto redeem = redeem_sidechain(sidechain_id, amount, recipient);
    const Amount fee = consensus::calc_sidechain_fee(amount, registry.fee_bps);
    const auto block = f.block({coinbase(height, fee), redeem});
    const Amount supply_before = f.money_supply({sidechain_id});

    consensus::ValidationState validation;
    const bool applied = f.state->apply_block(block, validation, true);
    check(applied, "redeem: block applies: " + validation.get_reject_reason());
    if (!applied) return;
    state::SidechainDeposit charged;
    state::SidechainRegistryEntry issued;
    check(f.state->get_balance(recipient) == amount &&
              f.state->get_sidechain_deposit(sidechain_id, charged) &&
              charged.amount == deposit.amount - amount - fee &&
              f.state->get_sidechain_registry(sidechain_id, issued) &&
              issued.total_issued == registry.total_issued - amount,
          "redeem: collateral pays the output and the fee");
    check(f.money_supply({sidechain_id}) ==
              supply_before + consensus::ProofOfWork::calculate_block_reward(height),
          "redeem: block conserves the money supply");

    check(f.state->revert_block(block), "redeem: block reverts");
    state::SidechainDeposit restored;
    state::SidechainRegistryEntry restored_registry;
    check(f.state->get_balance(recipient) == 0 &&
              f.state->get_sidechain_deposit(sidechain_id, restored) &&
              restored.amount == deposit.amount &&
              f.state->get_sidechain_registry(sidechain_id, restored_registry) &&
              restored_registry.total_issued == registry.total_issued &&
              f.money_supply({sidechain_id}) == supply_before,
          "redeem: revert restores collateral, supply and outputs");

    validation.reset();
    check(f.state->apply_block(block, validation, true) &&
              f.state->get_sidechain_deposit(sidechain_id, charged) &&
              charged.amount == deposit.amount - amount - fee,
          "redeem: reconnecting debits the collateral exactly once");
}

core::BatchCheckpointItem signed_checkpoint(const Hash256& sidechain_id, Height checkpoint_height,
                                            uint8_t sibling_seed) {
    const Hash256 leaf = consensus::sidechain_checkpoint::build_financial_supply_leaf_v2(
        sidechain_id, checkpoint_height, 0);
    const Hash256 sibling = test::hash_from_seed(sibling_seed);
    const Hash256 root = core::compute_merkle_root({leaf, sibling});
    core::BatchCheckpointItem item;
    item.sidechain_id = sidechain_id;
    item.checkpoint_height = checkpoint_height;
    item.checkpoint = consensus::sidechain_checkpoint::build_financial_commitment_v2(
        sidechain_id, root, checkpoint_height, 0);
    item.proof = consensus::sidechain_checkpoint::build_financial_proof_v2(
        root, checkpoint_height, 0, 0, {sibling});
    item.fee = consensus::params::MIN_TX_FEE;
    item.version = consensus::params::FINANCIAL_CHECKPOINT_VERSION;
    item.committee_id = committee_id();
    item.committee_signatures = {crypto::sign_recoverable_message(
        crypto::sha256(core::build_sidechain_batch_checkpoint_payload_for_signing(item)),
        test::make_key(kOwnerSeed))};
    return item;
}

Transaction checkpoint_batch(core::BatchCheckpointItem item) {
    Transaction tx;
    tx.type = TransactionType::BATCH_SIDECHAIN_CHECKPOINT;
    tx.sidechain_batch_checkpoints = {std::move(item)};
    return tx;
}

void test_sidechain_fee_reverts() {
    test_sidechain_fee_revert("mint", mint_sidechain(test::hash_from_seed(156), 50 * COIN), true);
    test_sidechain_fee_revert("issue", issue_sidechain(test::hash_from_seed(158),
        consensus::params::MIN_SIDECHAIN_MULTIPLIER, 100), false);
    test_sidechain_fee_revert("accounting registration", register_accounting(test::hash_from_seed(159)), false);
}

} // namespace

int main() {
    try {
        consensus::SelectParams(consensus::Network::MAIN);
        for (bool rocksdb : {false, true}) {
            use_rocksdb = rocksdb;
            test_recreated_sidechain_fee_cannot_be_claimed();
            test_same_block_issue_and_mint_fee();
            test_sidechain_fee_reverts();
            test_redeem_revert();
        }
        std::cout << "consensus security regressions: " << passed << '/' << (passed + failures) << '\n';
        return failures == 0 ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "test_consensus_security_regressions: " << error.what() << '\n';
        return 2;
    }
}
