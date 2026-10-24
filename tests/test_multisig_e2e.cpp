#include "chain_test_fixture.hpp"
#include "geocoin/network/mempool.hpp"
#include "geocoin/utils/activation_manifest.hpp"
#include "geocoin/wallet/multisig.hpp"

#include <algorithm>
#include <iostream>

using namespace geocoin;
using test::expect;

constexpr Amount kRelayFee = 100000;

namespace {

bytes key_from_seed(uint8_t seed) {
    bytes key(32, seed);
    key[0] = static_cast<byte>(seed + 1);
    return key;
}

Address address_from_key(const bytes& key) {
    Address address;
    expect(crypto::compute_address_from_private_key(key, address), "derive address");
    return address;
}

struct Wallet {
    uint8_t m = 2;
    uint8_t n = 3;
    std::vector<bytes> keys;
    std::vector<Address> addresses;
    Address address;

    static Wallet make(uint8_t m, uint8_t n, uint8_t first_seed) {
        Wallet wallet;
        wallet.m = m;
        wallet.n = n;
        for (uint8_t i = 0; i < n; ++i) {
            wallet.keys.push_back(key_from_seed(static_cast<uint8_t>(first_seed + i)));
            wallet.addresses.push_back(address_from_key(wallet.keys.back()));
        }
        wallet.address = Address(wallet::create_multisig_address(m, n, wallet.addresses).address);
        return wallet;
    }
};

struct Session {
    std::filesystem::path path = test::make_temp_dir("geocoin-multisig-e2e");
    std::shared_ptr<storage::BlockStorage> blocks;
    std::shared_ptr<state::ChainState> state;
    std::shared_ptr<core::Blockchain> chain;

    Session() { open(); }
    ~Session() {
        chain.reset();
        state.reset();
        blocks.reset();
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }

    void open() {
        blocks = std::make_shared<storage::BlockStorage>(path.string());
        expect(blocks->is_open(), "block storage opens");
        auto db = std::make_unique<state::RocksDBStorage>((path / "state").string());
        expect(db->open(), "state storage opens");
        state = std::make_shared<state::ChainState>(std::move(db));
        state->set_block_storage(blocks);
        state->set_activation_manifest(
            utils::ActivationManifest::fully_enabled_for_tests(consensus::Params().network_id()));
        chain = std::make_shared<core::Blockchain>(state, blocks);
        expect(chain->initialize(), "blockchain initializes");
    }

    void restart() {
        const Height height = chain->get_height();
        const Hash256 tip = state->get_tip_hash();
        chain.reset();
        state.reset();
        blocks.reset();
        open();
        expect(chain->get_height() == height && state->get_tip_hash() == tip, "restart restores the active tip");
    }
};

const chain::BlockIndex* tip_of(const Session& session) {
    return session.chain->get_tip();
}

core::Block coinbase_on(
    const chain::BlockIndex* previous,
    const Address& miner,
    const Address& foundation,
    Amount fees = 0
) {
    expect(previous != nullptr, "coinbase parent exists");
    core::Block block;
    block.header.version = 1;
    block.header.prev_block_hash = previous->get_block_hash();
    block.header.timestamp = previous->get_block_time() + 1;
    block.header.bits = consensus::calculate_next_difficulty(previous, block.header.timestamp);
    block.transactions.push_back(core::Transaction::create_coinbase(
        previous->height + 1, miner,
        consensus::ProofOfWork::calculate_block_reward(previous->height + 1),
        fees, foundation));
    block.transactions.front().timestamp = block.header.timestamp;
    return block;
}

bool submit(Session& session, core::Block& block, std::string* reason = nullptr) {
    const auto* parent = session.chain->find_block_index(block.header.prev_block_hash);
    expect(parent != nullptr, "submitted block has a parent");
    test::TestChain::seal(block, parent);
    consensus::ValidationState state;
    const bool accepted = session.chain->process_new_block(block, state);
    if (reason) *reason = state.get_reject_reason();
    return accepted;
}

void accept(Session& session, core::Block block, const char* label) {
    std::string reason;
    expect(submit(session, block, &reason), std::string(label) + ": " + reason);
}

core::Transaction commitment_for(const Address& multisig, const Hash256& signed_hash, const bytes& key) {
    return core::Transaction::create_angle_commitment(
        multisig, signed_hash, key, Address(""), consensus::Params().network_id());
}

struct SignedSpend {
    core::Transaction spend;
    std::vector<core::Transaction> commitments;
};

SignedSpend sign_plain(
    const state::UTXO& utxo,
    const Wallet& wallet,
    const Address& recipient,
    Amount payment,
    const std::vector<size_t>& signers,
    uint8_t claimed_m,
    Timestamp timestamp
) {
    core::TxInput input;
    input.prev_tx_hash = utxo.tx_hash;
    input.vout = utxo.vout;
    const Amount fee = kRelayFee;
    expect(utxo.value > payment + fee + consensus::params::DUST_LIMIT, "funded output covers payment and fee");
    auto spend = core::Transaction::create_multisig_spend(
        {input},
        {{payment, recipient, {}}, {utxo.value - payment - fee, wallet.address, {}}},
        {}, claimed_m, wallet.n);
    spend.spend_participants = wallet.addresses;
    spend.timestamp = timestamp;
    const auto signed_hash = spend.get_hash_without_commitments();
    SignedSpend package;
    package.spend = std::move(spend);
    for (size_t signer : signers) {
        package.commitments.push_back(commitment_for(wallet.address, signed_hash, wallet.keys.at(signer)));
        package.spend.angle_commitment_hashes.push_back(package.commitments.back().get_hash());
    }
    return package;
}

state::UTXO output_of(Session& session, const Address& address, const Hash256& tx_hash, uint32_t vout) {
    for (const auto& utxo : session.state->get_utxos_for_address(address)) {
        if (utxo.tx_hash == tx_hash && utxo.vout == vout) return utxo;
    }
    expect(false, "created output is visible to its owner");
    return {};
}

state::UTXO output_at(Session& session, const Address& address, size_t index) {
    auto utxos = session.state->get_utxos_for_address(address);
    std::sort(utxos.begin(), utxos.end(), [](const state::UTXO& lhs, const state::UTXO& rhs) {
        return lhs.height < rhs.height || (lhs.height == rhs.height && lhs.vout < rhs.vout);
    });
    expect(utxos.size() > index, "multisig output exists");
    return utxos[index];
}

state::UTXO newest_output(Session& session, const Address& address) {
    auto utxos = session.state->get_utxos_for_address(address);
    std::sort(utxos.begin(), utxos.end(), [](const state::UTXO& lhs, const state::UTXO& rhs) {
        return lhs.height < rhs.height || (lhs.height == rhs.height && lhs.vout < rhs.vout);
    });
    expect(!utxos.empty(), "address has an output");
    return utxos.back();
}

struct ProofPackage {
    core::Transaction proof;
    std::vector<core::Transaction> commitments;
};

ProofPackage make_delegation(
    const state::UTXO& utxo,
    const Wallet& primary,
    const Wallet& delegated,
    uint32_t nonce,
    Amount max_per,
    Amount max_total,
    uint8_t percent,
    Timestamp when,
    const Address& change_address
) {
    ProofPackage package;
    auto& proof = package.proof;
    proof.type = core::TransactionType::DELEGATION_PROOF;
    proof.timestamp = when;
    proof.inputs = {{utxo.tx_hash, utxo.vout}};
    proof.outputs = {{utxo.value - consensus::params::MIN_TX_FEE, change_address, {}}};
    proof.primary_multisig_address = primary.address;
    proof.primary_m = primary.m;
    proof.primary_n = primary.n;
    proof.primary_participants = primary.addresses;
    proof.delegated_multisig_address = delegated.address;
    proof.delegated_m = delegated.m;
    proof.delegated_n = delegated.n;
    proof.delegated_participants = delegated.addresses;
    proof.nonce = nonce;
    proof.max_per_transaction = max_per;
    proof.max_total_delegated = max_total;
    proof.max_percent_of_balance = percent;
    const auto signed_hash = proof.get_delegation_proof_hash();
    for (size_t signer = 0; signer < primary.m; ++signer) {
        package.commitments.push_back(commitment_for(primary.address, signed_hash, primary.keys[signer]));
        proof.angle_commitment_hashes.push_back(package.commitments.back().get_hash());
    }
    return package;
}

SignedSpend make_hierarchical(
    const state::UTXO& utxo,
    const Wallet& primary,
    const Wallet& delegated,
    const Hash256& proof_txid,
    const Address& recipient,
    Amount payment,
    Timestamp when,
    const bytes& signer_key
) {
    SignedSpend package;
    package.spend = core::Transaction::create_multisig_spend(
        {{utxo.tx_hash, utxo.vout}},
        {{payment, recipient, {}},
         {utxo.value - payment - consensus::params::MIN_TX_FEE, primary.address, {}}},
        {}, delegated.m, delegated.n);
    package.spend.delegation_proof_txid = proof_txid;
    package.spend.timestamp = when;
    package.commitments.push_back(commitment_for(
        delegated.address, package.spend.get_hierarchical_spend_hash(), signer_key));
    package.spend.angle_commitment_hashes = {package.commitments.back().get_hash()};
    return package;
}

void mine_until(Session& session, const Address& miner, Height height) {
    while (session.chain->get_height() < height) {
        accept(session, coinbase_on(tip_of(session), miner, session.state->get_foundation_address()), "advance chain");
    }
}

core::Block with_package(
    const chain::BlockIndex* parent,
    const Address& miner,
    const Address& foundation,
    const std::vector<core::Transaction>& transactions,
    Amount fees
) {
    auto block = coinbase_on(parent, miner, foundation, fees);
    block.transactions.insert(block.transactions.end(), transactions.begin(), transactions.end());
    return block;
}

bool reject_package(
    Session& session,
    const std::vector<core::Transaction>& transactions,
    const Address& miner,
    Amount fees,
    const char* label
) {
    const Hash256 tip = session.state->get_tip_hash();
    std::string reason;
    auto block = with_package(tip_of(session), miner, session.state->get_foundation_address(), transactions, fees);
    const bool rejected = !submit(session, block, &reason);
    expect(rejected && session.state->get_tip_hash() == tip, std::string(label) + ": " + reason);
    return rejected;
}

void test_multisig_end_to_end() {
    expect(consensus::Params().fixed_difficulty(), "end-to-end chain uses loadnet");
    Session session;
    const auto primary = Wallet::make(2, 3, 11);
    const auto delegated = Wallet::make(1, 1, 70);
    const auto solo = Wallet::make(1, 1, 50);
    const auto triple = Wallet::make(3, 3, 80);
    const Address recipient = address_from_key(key_from_seed(40));
    const Address sink = address_from_key(key_from_seed(42));
    const Address bystander = address_from_key(key_from_seed(90));
    const bytes outsider = key_from_seed(41);
    const Address foundation = session.state->get_foundation_address();

    for (int i = 0; i < 5; ++i) {
        accept(session, coinbase_on(tip_of(session), primary.address, foundation), "fund multisig");
    }
    accept(session, coinbase_on(tip_of(session), solo.address, foundation), "fund 1-of-1");
    accept(session, coinbase_on(tip_of(session), triple.address, foundation), "fund 3-of-3");
    mine_until(session, bystander, 110);

    const auto first = output_at(session, primary.address, 0);
    const auto second = output_at(session, primary.address, 1);
    const auto third = output_at(session, primary.address, 2);
    const auto extra = output_at(session, primary.address, 3);
    const auto budget_utxo = output_at(session, primary.address, 4);
    expect(session.chain->get_height() - extra.height + 1 >= consensus::Params().coinbase_maturity(),
           "funding coinbase is mature");

    const auto solo_utxo = output_at(session, solo.address, 0);
    const auto solo_spend = sign_plain(solo_utxo, solo, sink, COIN, {0}, solo.m, tip_of(session)->get_block_time() + 1);
    accept(session, with_package(tip_of(session), bystander, foundation,
                                 {solo_spend.commitments[0], solo_spend.spend}, kRelayFee),
           "1-of-1 spend activates");
    expect(session.state->get_balance(sink) == COIN && !session.state->has_utxo(solo_utxo.tx_hash, solo_utxo.vout),
           "1-of-1 payment arrives and its input is spent");

    const auto triple_utxo = output_at(session, triple.address, 0);
    const auto triple_partial = sign_plain(triple_utxo, triple, sink, COIN, {0, 1}, triple.m,
                                           tip_of(session)->get_block_time() + 1);
    expect(reject_package(session, {triple_partial.commitments[0], triple_partial.commitments[1], triple_partial.spend},
                          bystander, kRelayFee, "3-of-3 missing signer"),
           "3-of-3 rejects a two-signature package");
    const auto triple_full = sign_plain(triple_utxo, triple, sink, COIN, {0, 1, 2}, triple.m,
                                        tip_of(session)->get_block_time() + 1);
    accept(session, with_package(tip_of(session), bystander, foundation,
                                 {triple_full.commitments[0], triple_full.commitments[1], triple_full.commitments[2],
                                  triple_full.spend},
                                 kRelayFee),
           "3-of-3 spend activates only with every signer");
    expect(session.state->get_balance(sink) == 2 * COIN, "3-of-3 payment is added to the earlier 1-of-1 payment");

    auto tampered = sign_plain(extra, primary, sink, COIN, {0, 1}, primary.m, tip_of(session)->get_block_time() + 1);
    tampered.spend.outputs[0].value += 1;
    tampered.spend.outputs[1].value -= 1;
    expect(reject_package(session, {tampered.commitments[0], tampered.commitments[1], tampered.spend},
                          bystander, kRelayFee, "tampered output"),
           "changing an output after signing invalidates the package");

    auto foreign_network = sign_plain(extra, primary, sink, COIN, {0, 1}, primary.m, tip_of(session)->get_block_time() + 1);
    foreign_network.commitments[0] = core::Transaction::create_angle_commitment(
        primary.address, foreign_network.spend.get_hash_without_commitments(), primary.keys[0], Address(""), "degree");
    foreign_network.spend.angle_commitment_hashes[0] = foreign_network.commitments[0].get_hash();
    expect(reject_package(session, {foreign_network.commitments[0], foreign_network.commitments[1], foreign_network.spend},
                          bystander, kRelayFee, "foreign network commitment"),
           "a commitment from another network is rejected");

    auto duplicated_input = sign_plain(extra, primary, sink, COIN, {}, primary.m, tip_of(session)->get_block_time() + 1);
    duplicated_input.spend.inputs.push_back(duplicated_input.spend.inputs.front());
    duplicated_input.spend.angle_commitment_hashes.clear();
    const auto duplicated_hash = duplicated_input.spend.get_hash_without_commitments();
    duplicated_input.commitments = {
        commitment_for(primary.address, duplicated_hash, primary.keys[0]),
        commitment_for(primary.address, duplicated_hash, primary.keys[1])
    };
    duplicated_input.spend.angle_commitment_hashes = {
        duplicated_input.commitments[0].get_hash(), duplicated_input.commitments[1].get_hash()};
    std::string duplicate_reason;
    const Hash256 tip_before_duplicate = session.state->get_tip_hash();
    auto duplicate_block = with_package(
        tip_of(session), bystander, foundation,
        {duplicated_input.commitments[0], duplicated_input.commitments[1], duplicated_input.spend},
        extra.value + kRelayFee);
    expect(!submit(session, duplicate_block, &duplicate_reason) &&
               duplicate_reason == "bad-txns-inputs-duplicate" &&
               session.state->get_tip_hash() == tip_before_duplicate,
           "a repeated input is rejected: " + duplicate_reason);
    expect(session.state->has_utxo(extra.tx_hash, extra.vout), "rejected experiments leave the extra output");

    const auto mismatched_change = make_delegation(
        extra, primary, delegated, 9, 20000, 50000, 1, tip_of(session)->get_block_time() + 1, sink);
    expect(reject_package(session, {mismatched_change.commitments[0], mismatched_change.commitments[1],
                                    mismatched_change.proof},
                          bystander, consensus::params::MIN_TX_FEE, "delegation change address"),
           "delegation change must return to the primary address");

    const auto limited = make_delegation(
        extra, primary, delegated, 9, extra.value, extra.value, 1, tip_of(session)->get_block_time() + 1, primary.address);
    accept(session, with_package(tip_of(session), bystander, foundation,
                                 {limited.commitments[0], limited.commitments[1], limited.proof},
                                 consensus::params::MIN_TX_FEE),
           "limited delegation is published");
    const auto limited_change = output_of(session, primary.address, limited.proof.get_hash(), 0);
    const Amount percent_ceiling = session.state->get_balance(primary.address) / 100;
    expect(limited_change.value > percent_ceiling + COIN + consensus::params::MIN_TX_FEE, "change can exceed one percent");
    const auto over_percent = make_hierarchical(
        limited_change, primary, delegated, limited.proof.get_hash(), sink,
        percent_ceiling + COIN, tip_of(session)->get_block_time() + 1, delegated.keys[0]);
    expect(reject_package(session, {over_percent.commitments[0], over_percent.spend},
                          bystander, consensus::params::MIN_TX_FEE, "percent ceiling"),
           "a delegated spend cannot exceed its percentage of the primary balance");
    const auto within_limit = make_hierarchical(
        limited_change, primary, delegated, limited.proof.get_hash(), sink,
        10000, tip_of(session)->get_block_time() + 1, delegated.keys[0]);
    accept(session, with_package(tip_of(session), bystander, foundation,
                                 {within_limit.commitments[0], within_limit.spend}, consensus::params::MIN_TX_FEE),
           "a delegated spend inside every ceiling activates");
    const auto tight = make_delegation(
        budget_utxo, primary, delegated, 8, 20000, 25000, 0, tip_of(session)->get_block_time() + 1, primary.address);
    accept(session, with_package(tip_of(session), bystander, foundation,
                                 {tight.commitments[0], tight.commitments[1], tight.proof},
                                 consensus::params::MIN_TX_FEE),
           "a delegation with a tight total ceiling is published");
    const auto tight_change = output_of(session, primary.address, tight.proof.get_hash(), 0);
    const auto over_per_tx = make_hierarchical(
        tight_change, primary, delegated, tight.proof.get_hash(), sink,
        20000, tip_of(session)->get_block_time() + 1, delegated.keys[0]);
    expect(reject_package(session, {over_per_tx.commitments[0], over_per_tx.spend},
                          bystander, consensus::params::MIN_TX_FEE, "per-transaction ceiling"),
           "a delegated spend cannot exceed its per-transaction ceiling");
    const auto tight_spend = make_hierarchical(
        tight_change, primary, delegated, tight.proof.get_hash(), sink,
        10000, tip_of(session)->get_block_time() + 1, delegated.keys[0]);
    accept(session, with_package(tip_of(session), bystander, foundation,
                                 {tight_spend.commitments[0], tight_spend.spend}, consensus::params::MIN_TX_FEE),
           "a spend inside the tight per-transaction ceiling activates");

    const auto split_input = output_of(session, primary.address, tight_spend.spend.get_hash(), 1);
    const auto* split_parent = tip_of(session);
    auto split = core::Transaction::create_multisig_spend(
        {{split_input.tx_hash, split_input.vout}},
        {{2000, sink, {}},
         {(split_input.value - 2000 - consensus::params::MIN_TX_FEE) / 2, primary.address, {}},
         {split_input.value - 2000 - consensus::params::MIN_TX_FEE -
              (split_input.value - 2000 - consensus::params::MIN_TX_FEE) / 2,
          primary.address, {}}},
        {}, delegated.m, delegated.n);
    split.delegation_proof_txid = tight.proof.get_hash();
    split.timestamp = split_parent->get_block_time() + 1;
    const auto split_commitment = commitment_for(
        delegated.address, split.get_hierarchical_spend_hash(), delegated.keys[0]);
    split.angle_commitment_hashes = {split_commitment.get_hash()};
    accept(session, with_package(split_parent, bystander, foundation, {split_commitment, split},
                                 consensus::params::MIN_TX_FEE),
           "delegated spend can split the remainder into two primary outputs");
    const auto left = output_of(session, primary.address, split.get_hash(), 1);
    const auto right = output_of(session, primary.address, split.get_hash(), 2);
    const auto* pair_parent = tip_of(session);
    const auto left_spend = make_hierarchical(
        left, primary, delegated, tight.proof.get_hash(), sink, 10000,
        pair_parent->get_block_time() + 1, delegated.keys[0]);
    const auto right_spend = make_hierarchical(
        right, primary, delegated, tight.proof.get_hash(), sink, 10000,
        pair_parent->get_block_time() + 1, delegated.keys[0]);
    expect(reject_package(session,
                          {left_spend.commitments[0], right_spend.commitments[0], left_spend.spend, right_spend.spend},
                          bystander, 2 * consensus::params::MIN_TX_FEE, "same-block delegation total"),
           "two delegated spends in one block cannot pass the total ceiling together");
    expect(session.state->has_utxo(left.tx_hash, left.vout) && session.state->has_utxo(right.tx_hash, right.vout),
           "the rejected pair leaves both outputs");

    const Amount balance_before = session.state->get_balance(primary.address);

    const Timestamp now = tip_of(session)->get_block_time() + 1;
    const auto good = sign_plain(first, primary, recipient, 2 * COIN, {0, 1}, primary.m, now);
    {
        network::Mempool pool(session.state);
        std::string mempool_reason;
        expect(pool.add_transaction(good.commitments[0]) && pool.add_transaction(good.commitments[1]) &&
                   pool.test_accept_transaction(good.spend, &mempool_reason) && pool.add_transaction(good.spend),
               "mempool accepts a complete 2-of-3 package: " + mempool_reason);
        const auto lone = sign_plain(second, primary, recipient, COIN, {0}, primary.m, now);
        expect(pool.add_transaction(lone.commitments[0]) && !pool.add_transaction(lone.spend),
               "mempool rejects a 2-of-3 spend with one signature");
        auto claimed = sign_plain(second, primary, recipient, COIN, {0, 1}, primary.m, now);
        claimed.spend.angle_commitment_hashes = good.spend.angle_commitment_hashes;
        expect(!pool.add_transaction(claimed.spend), "mempool rejects a second spend that reuses a claimed commitment");
    }

    auto outsider_spend = sign_plain(second, primary, recipient, COIN, {0}, primary.m, now);
    const Hash256 outsider_hash = [&] {
        auto draft = outsider_spend.spend;
        draft.angle_commitment_hashes.clear();
        return draft.get_hash_without_commitments();
    }();
    outsider_spend.commitments.push_back(commitment_for(primary.address, outsider_hash, outsider));
    outsider_spend.spend.angle_commitment_hashes.push_back(outsider_spend.commitments.back().get_hash());
    expect(reject_package(session, {outsider_spend.commitments[0], outsider_spend.commitments[1], outsider_spend.spend},
                          bystander, kRelayFee, "outsider signature"),
           "block rejects an outsider signature");

    const auto lowered = sign_plain(second, primary, recipient, COIN, {0}, 1, now);
    expect(reject_package(session, {lowered.commitments[0], lowered.spend}, bystander, kRelayFee, "lowered threshold"),
           "block rejects a spend that claims a lower threshold than the address");

    auto reuse = sign_plain(second, primary, recipient, COIN, {1}, primary.m, now);
    reuse.spend.angle_commitment_hashes.insert(reuse.spend.angle_commitment_hashes.begin(), good.commitments[0].get_hash());
    expect(reject_package(session,
                          {good.commitments[0], good.commitments[1], reuse.commitments[0], good.spend, reuse.spend},
                          bystander, 2 * kRelayFee, "shared commitment"),
           "one commitment cannot authorize two spends");

    expect(reject_package(session, {good.spend, good.commitments[0], good.commitments[1]}, bystander, kRelayFee, "reversed package"),
           "a commitment after its spend is rejected");
    expect(reject_package(session, {good.commitments[0]}, bystander, 0, "orphan commitment"),
           "a commitment without its spend is rejected");

    const auto foreign = output_at(session, bystander, 0);
    const auto stolen = sign_plain(foreign, primary, recipient, COIN, {0, 1}, primary.m, now);
    expect(reject_package(session, {stolen.commitments[0], stolen.commitments[1], stolen.spend}, bystander, kRelayFee, "foreign output"),
           "signatures for this multisig cannot spend another address");
    expect(session.state->get_balance(primary.address) == balance_before, "rejected packages do not move the balance");

    accept(session, coinbase_on(tip_of(session), primary.address, foundation), "mine an immature coinbase");
    const auto immature = newest_output(session, primary.address);
    expect(session.chain->get_height() - immature.height + 1 < consensus::Params().coinbase_maturity(),
           "latest multisig coinbase is still immature");
    const auto too_soon = sign_plain(immature, primary, recipient, COIN, {0, 1}, primary.m,
                                     tip_of(session)->get_block_time() + 1);
    std::string immature_reason;
    auto immature_block = with_package(tip_of(session), bystander, foundation,
                                       {too_soon.commitments[0], too_soon.commitments[1], too_soon.spend}, kRelayFee);
    expect(!submit(session, immature_block, &immature_reason) &&
               immature_reason == "bad-txns-premature-spend-of-coinbase",
           "multisig cannot spend an immature coinbase: " + immature_reason);

    const auto* reorg_parent = tip_of(session);
    const Amount immature_value = immature.value;
    accept(session, with_package(reorg_parent, bystander, foundation,
                                 {good.commitments[0], good.commitments[1], good.spend}, kRelayFee),
           "2-of-3 spend activates");
    expect(session.state->get_balance(recipient) == 2 * COIN, "recipient receives the signed amount");
    expect(!session.state->has_utxo(first.tx_hash, first.vout), "spent multisig output is consumed");
    expect(session.state->get_balance(primary.address) ==
               balance_before + immature_value - 2 * COIN - kRelayFee,
           "change and fee are deducted from the spent output");
    expect(reject_package(session, {good.commitments[0], good.commitments[1], good.spend}, bystander, kRelayFee, "replay"),
           "the same package cannot be replayed");

    auto sibling = coinbase_on(reorg_parent, bystander, foundation);
    std::string sibling_reason;
    expect(submit(session, sibling, &sibling_reason), "equal-work sibling is retained: " + sibling_reason);
    auto extension = coinbase_on(session.chain->find_block_index(sibling.get_hash()), bystander, foundation);
    accept(session, std::move(extension), "longer sibling fork wins");
    expect(session.state->has_utxo(first.tx_hash, first.vout) && session.state->get_balance(recipient) == 0,
           "reorg restores the spent multisig output and removes its payment");

    const auto* proof_parent = tip_of(session);
    core::Transaction proof;
    proof.type = core::TransactionType::DELEGATION_PROOF;
    proof.timestamp = proof_parent->get_block_time() + 1;
    proof.inputs = {{third.tx_hash, third.vout}};
    proof.outputs = {{third.value - consensus::params::MIN_TX_FEE, primary.address, {}}};
    proof.primary_multisig_address = primary.address;
    proof.primary_m = primary.m;
    proof.primary_n = primary.n;
    proof.primary_participants = primary.addresses;
    proof.delegated_multisig_address = delegated.address;
    proof.delegated_m = delegated.m;
    proof.delegated_n = delegated.n;
    proof.delegated_participants = delegated.addresses;
    proof.nonce = 1;
    proof.max_per_transaction = 50000;
    proof.max_total_delegated = 50000;
    const auto proof_hash = proof.get_delegation_proof_hash();
    const auto proof_a = commitment_for(primary.address, proof_hash, primary.keys[0]);
    const auto proof_b = commitment_for(primary.address, proof_hash, primary.keys[1]);
    proof.angle_commitment_hashes = {proof_a.get_hash(), proof_b.get_hash()};
    accept(session, with_package(proof_parent, bystander, foundation, {proof_a, proof_b, proof},
                                 consensus::params::MIN_TX_FEE),
           "primary 2-of-3 publishes a delegation");

    const auto reused_nonce = make_delegation(
        second, primary, delegated, 1, 50000, 50000, 0, tip_of(session)->get_block_time() + 1, primary.address);
    expect(reject_package(session, {reused_nonce.commitments[0], reused_nonce.commitments[1], reused_nonce.proof},
                          bystander, consensus::params::MIN_TX_FEE, "reused nonce"),
           "a primary address cannot publish a second proof with a used nonce");
    const auto proof_change = output_of(session, primary.address, proof.get_hash(), 0);
    core::Transaction delegated_revoke;
    delegated_revoke.type = core::TransactionType::REVOKE_DELEGATION;
    delegated_revoke.timestamp = tip_of(session)->get_block_time() + 1;
    delegated_revoke.revoked_delegation_proof_txid = proof.get_hash();
    delegated_revoke.inputs = {{proof_change.tx_hash, proof_change.vout}};
    delegated_revoke.outputs = {{proof_change.value - consensus::params::MIN_TX_FEE, primary.address, {}}};
    const auto delegated_revoke_hash = delegated_revoke.get_revoke_delegation_hash();
    const auto delegated_revoke_commitment = commitment_for(
        primary.address, delegated_revoke_hash, delegated.keys[0]);
    delegated_revoke.angle_commitment_hashes = {delegated_revoke_commitment.get_hash()};
    expect(reject_package(session, {delegated_revoke_commitment, delegated_revoke},
                          bystander, consensus::params::MIN_TX_FEE, "delegated revoke"),
           "the delegated key cannot revoke the primary's delegation");
    const auto primary_signed = make_hierarchical(
        proof_change, primary, delegated, proof.get_hash(), recipient, 10000,
        tip_of(session)->get_block_time() + 1, primary.keys[0]);
    expect(reject_package(session, {primary_signed.commitments[0], primary_signed.spend},
                          bystander, consensus::params::MIN_TX_FEE, "primary key as delegate"),
           "a primary key cannot sign a delegated spend");
    expect(session.state->has_utxo(second.tx_hash, second.vout) &&
               session.state->has_utxo(proof_change.tx_hash, proof_change.vout),
           "rejected delegation experiments leave their inputs");

    const Amount delegated_input = third.value - consensus::params::MIN_TX_FEE;
    const Amount delegated_payment = 20000;
    const auto* spend_parent = tip_of(session);
    auto hierarchical = core::Transaction::create_multisig_spend(
        {{proof.get_hash(), 0}},
        {{delegated_payment, recipient, {}},
         {delegated_input - delegated_payment - consensus::params::MIN_TX_FEE, primary.address, {}}},
        {}, delegated.m, delegated.n);
    hierarchical.delegation_proof_txid = proof.get_hash();
    hierarchical.timestamp = spend_parent->get_block_time() + 1;
    const auto hierarchical_hash = hierarchical.get_hierarchical_spend_hash();
    const auto delegated_commitment = commitment_for(delegated.address, hierarchical_hash, delegated.keys[0]);
    hierarchical.angle_commitment_hashes = {delegated_commitment.get_hash()};
    accept(session, with_package(spend_parent, bystander, foundation, {delegated_commitment, hierarchical},
                                 consensus::params::MIN_TX_FEE),
           "delegated key spends inside its limit");
    expect(session.state->get_balance(recipient) == delegated_payment, "delegated payment arrives");

    const auto change = output_of(session, primary.address, hierarchical.get_hash(), 1);
    const auto* excess_parent = tip_of(session);
    auto excess = core::Transaction::create_multisig_spend(
        {{change.tx_hash, change.vout}},
        {{40000, recipient, {}},
         {change.value - 40000 - consensus::params::MIN_TX_FEE, primary.address, {}}},
        {}, delegated.m, delegated.n);
    excess.delegation_proof_txid = proof.get_hash();
    excess.timestamp = excess_parent->get_block_time() + 1;
    const auto excess_commitment = commitment_for(
        delegated.address, excess.get_hierarchical_spend_hash(), delegated.keys[0]);
    excess.angle_commitment_hashes = {excess_commitment.get_hash()};
    expect(reject_package(session, {excess_commitment, excess}, bystander, consensus::params::MIN_TX_FEE, "delegation ceiling"),
           "a second delegated spend cannot pass the total ceiling");

    const auto* revoke_parent = tip_of(session);
    core::Transaction revoke;
    revoke.type = core::TransactionType::REVOKE_DELEGATION;
    revoke.timestamp = revoke_parent->get_block_time() + 1;
    revoke.revoked_delegation_proof_txid = proof.get_hash();
    revoke.inputs = {{change.tx_hash, change.vout}};
    revoke.outputs = {{change.value - consensus::params::MIN_TX_FEE, primary.address, {}}};
    const auto revoke_hash = revoke.get_revoke_delegation_hash();
    const auto revoke_a = commitment_for(primary.address, revoke_hash, primary.keys[0]);
    const auto revoke_b = commitment_for(primary.address, revoke_hash, primary.keys[1]);
    revoke.angle_commitment_hashes = {revoke_a.get_hash(), revoke_b.get_hash()};
    accept(session, with_package(revoke_parent, bystander, foundation, {revoke_a, revoke_b, revoke},
                                 consensus::params::MIN_TX_FEE),
           "primary revokes the delegation");

    const auto returned = output_of(session, primary.address, revoke.get_hash(), 0);
    const auto* after_revoke = tip_of(session);
    auto forbidden = core::Transaction::create_multisig_spend(
        {{returned.tx_hash, returned.vout}},
        {{10000, recipient, {}},
         {returned.value - 10000 - consensus::params::MIN_TX_FEE, primary.address, {}}},
        {}, delegated.m, delegated.n);
    forbidden.delegation_proof_txid = proof.get_hash();
    forbidden.timestamp = after_revoke->get_block_time() + 1;
    const auto forbidden_commitment = commitment_for(
        delegated.address, forbidden.get_hierarchical_spend_hash(), delegated.keys[0]);
    forbidden.angle_commitment_hashes = {forbidden_commitment.get_hash()};
    expect(reject_package(session, {forbidden_commitment, forbidden}, bystander, consensus::params::MIN_TX_FEE, "revoked delegation"),
           "a revoked delegation cannot authorize another spend");

    const Amount recipient_before_restart = session.state->get_balance(recipient);
    const Height height_before_restart = session.chain->get_height();
    session.restart();
    expect(session.chain->get_height() == height_before_restart, "restart keeps the height");
    expect(session.state->has_utxo(first.tx_hash, first.vout), "restart keeps the reorg-restored output");
    expect(!session.state->has_utxo(third.tx_hash, third.vout), "restart keeps the delegation input spent");
    expect(session.state->get_balance(recipient) == recipient_before_restart, "restart keeps the delegated payment");
    expect(reject_package(session, {forbidden_commitment, forbidden}, bystander, consensus::params::MIN_TX_FEE, "revocation survives restart"),
           "restart still rejects a spend from the revoked delegation");

    const auto fresh_parent = tip_of(session);
    const auto fresh = sign_plain(second, primary, recipient, COIN, {0, 2}, primary.m, fresh_parent->get_block_time() + 1);
    accept(session, with_package(fresh_parent, bystander, foundation,
                                 {fresh.commitments[0], fresh.commitments[1], fresh.spend}, kRelayFee),
           "the other two signers spend a restored output after restart");
    expect(session.state->get_balance(recipient) == recipient_before_restart + COIN,
           "post-restart spend pays the recipient");
    session.restart();
    expect(!session.state->has_utxo(second.tx_hash, second.vout) &&
               session.state->get_balance(recipient) == recipient_before_restart + COIN,
           "a second restart still shows the spent output and its payment");
}

} // namespace

int main() {
    try {
        consensus::SelectParams(consensus::Network::LOADNET);
        test_multisig_end_to_end();
        std::cout << "test_multisig_e2e: ok\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "test_multisig_e2e failed: " << e.what() << "\n";
        return 1;
    }
}
