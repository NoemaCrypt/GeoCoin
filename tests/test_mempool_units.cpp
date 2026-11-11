#include "geocoin/consensus/consensus.hpp"
#include "geocoin/consensus/proof_of_work.hpp"
#include "geocoin/consensus/sidechain_fee.hpp"
#include "geocoin/core/block.hpp"
#include "geocoin/network/mempool.hpp"
#include "geocoin/network/mempool_policy.hpp"
#include "geocoin/state/state.hpp"
#include "geocoin/crypto/geo_area.hpp"
#include "geocoin/wallet/multisig.hpp"
#include "geocoin/wallet/wallet.hpp"

#include <algorithm>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>

namespace geocoin::network {
struct MempoolTestAccess {
    static std::string hash256_to_hex(const Hash256& hash)
    {
        return Mempool::hash256_to_hex(hash);
    }

    static geocoin::Amount hierarchical_amount_spent(
        const Mempool& mempool,
        const core::Transaction& tx,
        const core::Transaction& delegation_proof_tx,
        uint64_t fee = 0
    )
    {
        return mempool.hierarchical_amount_spent_locked(tx, delegation_proof_tx, fee);
    }

    static uint64_t calculate_fee(const Mempool& mempool, const core::Transaction& tx)
    {
        return mempool.calculate_fee(tx);
    }

    static bool has_conflicts(const Mempool& mempool, const core::Transaction& tx)
    {
        return mempool.has_conflicts(tx);
    }

    static void record_reject(Mempool& mempool, bool policy, const std::string& reason)
    {
        mempool.record_reject(policy, reason);
    }

    static void seed_entry(
        Mempool& mempool,
        const core::Transaction& tx,
        size_t size,
        uint64_t fee_per_byte
    ) {
        std::lock_guard<std::mutex> lock(mempool.mutex_);
        MempoolEntry entry;
        entry.tx = tx;
        entry.fee = fee_per_byte * size;
        entry.fee_per_byte = fee_per_byte;
        entry.added_at = std::chrono::steady_clock::now();
        entry.size = size;
        if (tx.type == core::TransactionType::MULTISIG_SPEND && mempool.chain_state_) {
            if (const auto proof = mempool.chain_state_->get_transaction(tx.delegation_proof_txid)) {
                entry.delegation_debit = mempool.hierarchical_amount_spent_locked(tx, *proof, entry.fee);
            }
        }
        const Hash256 tx_hash = tx.get_hash();
        mempool.transactions_[tx_hash] = entry;
        mempool.fee_index_.insert({fee_per_byte, tx_hash});
        mempool.current_size_ += size;
    }

    static void seed_stale_fee_index(Mempool& mempool, const Hash256& tx_hash, uint64_t fee_per_byte)
    {
        std::lock_guard<std::mutex> lock(mempool.mutex_);
        mempool.fee_index_.insert({fee_per_byte, tx_hash});
    }

    static void seed_spent_output(Mempool& mempool, const Hash256& tx_hash, uint32_t vout)
    {
        std::lock_guard<std::mutex> lock(mempool.mutex_);
        mempool.spent_outputs_[tx_hash].insert(vout);
    }

    static void set_current_size(Mempool& mempool, size_t size)
    {
        std::lock_guard<std::mutex> lock(mempool.mutex_);
        mempool.current_size_ = size;
    }

    static void set_added_at(
        Mempool& mempool,
        const Hash256& tx_hash,
        std::chrono::steady_clock::time_point added_at
    ) {
        std::lock_guard<std::mutex> lock(mempool.mutex_);
        auto it = mempool.transactions_.find(tx_hash);
        if (it != mempool.transactions_.end()) {
            it->second.added_at = added_at;
        }
    }

    static void seed_delegation_pending(Mempool& mempool, const Hash256& proof_txid, Amount amount)
    {
        std::lock_guard<std::mutex> lock(mempool.mutex_);
        mempool.mempool_delegation_spent_[Mempool::hash256_to_hex(proof_txid)] = amount;
    }

    static bool has_delegation_pending(Mempool& mempool, const Hash256& proof_txid)
    {
        std::lock_guard<std::mutex> lock(mempool.mutex_);
        return mempool.mempool_delegation_spent_.count(Mempool::hash256_to_hex(proof_txid)) > 0;
    }

    // Unsigned stand-ins for a flood of valid zero-fee commitments. Admission
    // is bypassed because only the pool bookkeeping is under test.
    static std::vector<Hash256> seed_unclaimed_angle_commitments(
        Mempool& mempool,
        size_t count,
        std::chrono::steady_clock::time_point added_at
    ) {
        std::lock_guard<std::mutex> lock(mempool.mutex_);
        std::vector<Hash256> hashes;
        hashes.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            core::Transaction tx;
            tx.type = core::TransactionType::ANGLE_COMMITMENT;
            core::TxInput input;
            input.vout = 0xFFFFFFFE;
            for (size_t b = 0; b < 8; ++b) {
                input.prev_tx_hash[b] = static_cast<byte>((i >> (8 * b)) & 0xFF);
                tx.spending_tx_hash[b] = static_cast<byte>((i >> (8 * b)) & 0xFF);
            }
            input.prev_tx_hash[31] = 0xA5;
            tx.spending_tx_hash[31] = 0x5A;
            tx.inputs.push_back(input);
            MempoolEntry entry;
            entry.tx = tx;
            entry.added_at = added_at + std::chrono::milliseconds(static_cast<int64_t>(i));
            entry.size = 64;
            const Hash256 hash = tx.get_hash();
            mempool.transactions_[hash] = entry;
            mempool.fee_index_.insert({0, hash});
            mempool.angle_commitment_keys_.insert(input.prev_tx_hash);
            mempool.pending_angle_commitments_by_spend_[tx.spending_tx_hash]++;
            mempool.current_size_ += entry.size;
            hashes.push_back(hash);
        }
        return hashes;
    }

    static size_t pending_angle_commitments(Mempool& mempool)
    {
        std::lock_guard<std::mutex> lock(mempool.mutex_);
        return mempool.angle_commitment_keys_.size();
    }

    static void evict_low_fee_transactions(Mempool& mempool, size_t bytes_needed)
    {
        std::lock_guard<std::mutex> lock(mempool.mutex_);
        mempool.evict_low_fee_transactions(bytes_needed);
    }

    static size_t fee_index_size(Mempool& mempool)
    {
        std::lock_guard<std::mutex> lock(mempool.mutex_);
        return mempool.fee_index_.size();
    }

    // Mirrors the nonce bookkeeping add_transaction() performs for an
    // admitted DELEGATION_PROOF; seed_entry() alone bypasses it.
    static void seed_delegation_nonce(Mempool& mempool, const core::Transaction& proof)
    {
        std::lock_guard<std::mutex> lock(mempool.mutex_);
        mempool.delegation_nonces_[proof.primary_multisig_address.str()].insert(proof.nonce);
    }

    static bool has_delegation_nonce(Mempool& mempool, const core::Transaction& proof)
    {
        std::lock_guard<std::mutex> lock(mempool.mutex_);
        const auto it = mempool.delegation_nonces_.find(proof.primary_multisig_address.str());
        return it != mempool.delegation_nonces_.end() && it->second.count(proof.nonce) > 0;
    }
};
}

namespace geocoin::state {
struct ChainStateTestAccess {
    static void put_sidechain_registry(
        ChainState& state,
        const Hash256& sidechain_id,
        const SidechainRegistryEntry& entry
    )
    {
        state.sidechain_registry_[ChainState::hash256_to_hex(sidechain_id)] = entry;
    }

    static void put_utxo(ChainState& state, const UTXO& utxo)
    {
        state.add_utxo(utxo);
    }
};
}

namespace {

void expect(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

geocoin::bytes make_key(uint8_t seed)
{
    geocoin::bytes key(32);
    for (size_t i = 0; i < key.size(); ++i) {
        key[i] = static_cast<geocoin::byte>(seed + i);
    }
    return key;
}

geocoin::Hash256 hash_from_seed(uint8_t seed)
{
    geocoin::Hash256 hash{};
    for (size_t i = 0; i < hash.size(); ++i) {
        hash[i] = static_cast<geocoin::byte>(seed + i * 17);
    }
    return hash;
}

geocoin::Address address_from_key(uint8_t seed)
{
    geocoin::Address address;
    expect(geocoin::crypto::compute_address_from_private_key(make_key(seed), address),
           "address derives from private key");
    return address;
}

geocoin::wallet::MultisigConfig make_multisig_config(
    uint8_t m,
    uint8_t n,
    const std::vector<geocoin::Address>& addresses
)
{
    const auto multisig = geocoin::wallet::create_multisig_address(m, n, addresses);
    geocoin::wallet::MultisigConfig config = multisig.config;
    config.address = geocoin::Address(multisig.address);
    config.addresses = addresses;
    config.participants.clear();
    for (const auto& address : addresses) {
        config.participants.emplace_back(address);
    }
    return config;
}

std::shared_ptr<geocoin::state::ChainState> make_chain_state()
{
    return std::make_shared<geocoin::state::ChainState>(
        std::make_unique<geocoin::state::MemoryStorage>()
    );
}

void fund_wallet(
    const std::shared_ptr<geocoin::state::ChainState>& state,
    const geocoin::wallet::Wallet& wallet,
    geocoin::Height coinbase_height,
    geocoin::Amount reward
)
{
    const auto coinbase = geocoin::core::Transaction::create_coinbase(
        coinbase_height,
        wallet.get_address(),
        reward
    );
    expect(state->apply_transaction(coinbase, coinbase_height, true), "coinbase applies");
}

void fund_address(
    const std::shared_ptr<geocoin::state::ChainState>& state,
    const geocoin::Address& address,
    geocoin::Height coinbase_height,
    geocoin::Amount reward
)
{
    const auto coinbase = geocoin::core::Transaction::create_coinbase(
        coinbase_height,
        address,
        reward
    );
    expect(state->apply_transaction(coinbase, coinbase_height, true), "coinbase to address applies");
}

void mature_chain(const std::shared_ptr<geocoin::state::ChainState>& state, geocoin::Height highest_coinbase)
{
    state->set_height(highest_coinbase + geocoin::consensus::params::COINBASE_MATURITY + 2);
}

geocoin::core::Transaction create_payment(
    geocoin::wallet::Wallet& wallet,
    const geocoin::Address& recipient,
    const std::shared_ptr<geocoin::state::ChainState>& state,
    geocoin::Amount amount,
    geocoin::Amount fee
)
{
    geocoin::core::Transaction tx;
    expect(wallet.create_transaction(recipient, amount, fee, state, tx), "wallet creates payment");
    expect(state->validate_transaction(tx), "payment validates against chainstate");
    return tx;
}

geocoin::Amount relay_fee_for_payment(
    geocoin::wallet::Wallet& wallet,
    geocoin::Amount amount,
    const std::shared_ptr<geocoin::state::ChainState>& state
)
{
    const geocoin::Amount fee = wallet.estimate_required_fee(amount, state);
    expect(fee >= geocoin::consensus::params::MIN_TX_FEE,
           "wallet estimates a relayable payment fee");
    return fee;
}

geocoin::core::Transaction create_manual_payment_from_first_utxo(
    const geocoin::wallet::Wallet& wallet,
    const geocoin::Address& recipient,
    const std::shared_ptr<geocoin::state::ChainState>& state,
    geocoin::Amount amount,
    geocoin::Amount fee
)
{
    const auto utxos = state->get_utxos_for_address(wallet.get_address());
    expect(!utxos.empty(), "wallet has a UTXO");
    const auto& utxo = utxos.front();
    expect(utxo.value > amount + fee, "UTXO can fund manual payment");

    geocoin::core::Transaction tx;
    tx.version = 2;
    tx.type = geocoin::core::TransactionType::STANDARD;
    tx.timestamp = 1000;

    geocoin::core::TxInput input;
    input.prev_tx_hash = utxo.tx_hash;
    input.vout = utxo.vout;
    tx.inputs.push_back(input);

    geocoin::core::TxOutput output;
    output.value = amount;
    output.address = recipient;
    tx.outputs.push_back(output);

    geocoin::core::TxOutput change;
    change.value = utxo.value - amount - fee;
    change.address = wallet.get_address();
    tx.outputs.push_back(change);

    geocoin::bytes private_key;
    expect(wallet.get_private_key(wallet.get_address(), private_key), "wallet exports private key for manual tx");
    tx.sign(private_key);
    return tx;
}

void test_mempool_basic_lifecycle()
{
    auto state = make_chain_state();
    geocoin::wallet::Wallet miner;
    geocoin::wallet::Wallet recipient;
    geocoin::wallet::Wallet other;
    expect(miner.initialize(make_key(1)), "miner wallet initializes");
    expect(recipient.initialize(make_key(41)), "recipient wallet initializes");
    expect(other.initialize(make_key(81)), "other wallet initializes");

    fund_wallet(state, miner, 1, 80 * geocoin::COIN);
    mature_chain(state, 1);

    geocoin::network::Mempool mempool(state);
    expect(mempool.empty(), "new mempool is empty");
    expect(mempool.get_transactions_updated() == 0, "new mempool update counter is zero");
    expect(mempool.max_size_limit() == 300 * 1024 * 1024, "default mempool size is 300 MiB");
    const auto empty_stats = mempool.get_stats();
    expect(empty_stats.tx_count == 0, "empty stats tx_count");
    expect(empty_stats.total_size == 0, "empty stats total_size");
    expect(empty_stats.total_fees == 0, "empty stats total_fees");
    expect(empty_stats.min_fee_per_byte == 0, "empty stats min fee");
    expect(empty_stats.max_fee_per_byte == 0, "empty stats max fee");
    expect(empty_stats.avg_fee_per_byte == 0, "empty stats avg fee");
    expect(mempool.get_transactions_for_relay().empty(), "empty relay queue is empty");
    expect(!mempool.remove_transaction(hash_from_seed(0xee)), "removing missing tx returns false");
    expect(mempool.get_transactions_updated() == 0, "missing remove does not update counter");

    mempool.set_max_size(1024 * 1024);
    expect(mempool.max_size_limit() == 1024 * 1024, "set_max_size updates limit");

    const auto tx = create_payment(
        miner,
        recipient.get_address(),
        state,
        5 * geocoin::COIN,
        relay_fee_for_payment(miner, 5 * geocoin::COIN, state)
    );
    const auto tx_hash = tx.get_hash();
    expect(mempool.add_transaction(tx), "valid transaction enters mempool");
    expect(mempool.size() == 1 && !mempool.empty(), "mempool size tracks add");
    expect(mempool.has_transaction(tx_hash), "mempool has tx by hash");
    expect(mempool.get_transactions_updated() == 1, "add increments update counter");

    geocoin::core::Transaction fetched;
    expect(mempool.get_transaction(tx_hash, fetched), "get_transaction returns tx");
    expect(fetched.get_hash() == tx_hash, "fetched tx hash matches");
    expect(!mempool.add_transaction(tx), "duplicate transaction is rejected");

    const auto conflict = create_payment(
        miner,
        other.get_address(),
        state,
        4 * geocoin::COIN,
        relay_fee_for_payment(miner, 4 * geocoin::COIN, state)
    );
    expect(!mempool.add_transaction(conflict), "double spend conflict is rejected");

    const auto entries = mempool.get_all_entries();
    expect(entries.size() == 1, "entries expose one tx");
    expect(entries[0].fee >= geocoin::consensus::params::MIN_TX_FEE, "entry fee is recorded");
    expect(entries[0].size == tx.serialize().size(), "entry size is recorded");
    expect(mempool.get_all_transactions().size() == 1, "get_all_transactions returns one tx");
    size_t page_total = 0;
    const auto page = mempool.get_entries_page(0, 1, &page_total);
    expect(page_total == 1 && page.size() == 1 && page[0].tx.get_hash() == tx_hash,
           "bounded mempool page reports total and returns the requested entry");
    expect(mempool.get_entries_page(1, 1).empty(),
           "bounded mempool page returns empty beyond the end");
    geocoin::network::MempoolEntry fetched_entry;
    expect(mempool.get_entry(tx_hash, fetched_entry) && fetched_entry.tx.get_hash() == tx_hash,
           "direct mempool entry lookup avoids a full snapshot");
    expect(!mempool.get_entry(hash_from_seed(0xef), fetched_entry),
           "direct mempool entry lookup reports a missing transaction");

    const auto stats = mempool.get_stats();
    expect(stats.tx_count == 1, "stats tx_count");
    expect(stats.total_size == entries[0].size, "stats total_size");
    expect(stats.total_fees == entries[0].fee, "stats total_fees");
    expect(stats.min_fee_per_byte <= stats.max_fee_per_byte, "stats fee range");

    expect(mempool.get_transactions_for_mining(1).empty(), "too-small block template skips tx");
    const auto mining = mempool.get_transactions_for_mining(1024 * 1024);
    expect(mining.size() == 1 && mining[0].get_hash() == tx_hash, "mining selection returns tx");

    auto relay = mempool.get_transactions_for_relay();
    expect(relay.size() == 1 && relay[0] == tx_hash, "relay queue returns new tx once");
    expect(mempool.get_transactions_for_relay().empty(), "relay queue drains");

    const geocoin::Hash256 queued_hash = hash_from_seed(0xab);
    mempool.queue_inventory_for_relay({queued_hash});
    auto queued = mempool.get_transactions_for_relay();
    expect(queued.size() == 1 && queued[0] == queued_hash, "queue_inventory_for_relay enqueues hashes");

    std::vector<geocoin::Hash256> many_hashes;
    many_hashes.push_back(hash_from_seed(0xac));
    many_hashes.push_back(hash_from_seed(0xad));
    many_hashes.push_back(hash_from_seed(0xae));
    mempool.queue_inventory_for_relay(many_hashes);
    auto first_batch = mempool.get_transactions_for_relay(2);
    expect(first_batch.size() == 2, "relay drain honors max_items");
    auto remainder = mempool.get_transactions_for_relay();
    expect(remainder.size() == 1, "remaining relay hashes stay queued");

    geocoin::core::Block block;
    block.transactions.push_back(tx);
    mempool.remove_block_transactions(block);
    expect(!mempool.has_transaction(tx_hash), "remove_block_transactions removes confirmed tx");
    expect(mempool.empty(), "mempool empty after block removal");
    expect(mempool.get_transactions_updated() == 2, "block removal increments update counter");

    expect(mempool.add_transaction(tx), "tx can be re-added after removal in unit fixture");
    expect(mempool.remove_transaction(tx_hash), "remove_transaction removes existing tx");
    expect(!mempool.remove_transaction(tx_hash), "remove_transaction reports missing tx");
    expect(mempool.empty(), "mempool empty after explicit remove");

    expect(mempool.add_transaction(tx), "tx can be re-added before clear");
    mempool.clear();
    expect(mempool.empty(), "clear empties mempool");
}

void test_mempool_reject_reason_accounting()
{
    auto state = make_chain_state();
    geocoin::wallet::Wallet recipient;
    expect(recipient.initialize(make_key(91)), "reject recipient initializes");

    geocoin::network::Mempool mempool(state);

    geocoin::core::Transaction bad_version;
    bad_version.version = 1;
    bad_version.type = geocoin::core::TransactionType::STANDARD;
    expect(!mempool.add_transaction(bad_version), "bad version is rejected");

    geocoin::core::Transaction bad_witness_version;
    bad_witness_version.type = geocoin::core::TransactionType::STANDARD;
    bad_witness_version.witness_version = 0;
    expect(!mempool.add_transaction(bad_witness_version), "bad witness version is rejected");

    geocoin::core::Transaction empty_standard;
    empty_standard.type = geocoin::core::TransactionType::STANDARD;
    expect(!mempool.add_transaction(empty_standard), "standard tx without inputs is rejected");

    geocoin::core::Transaction missing_utxo;
    missing_utxo.type = geocoin::core::TransactionType::STANDARD;
    geocoin::core::TxInput missing_input;
    missing_input.prev_tx_hash = hash_from_seed(0xb1);
    missing_input.vout = 0;
    missing_utxo.inputs.push_back(missing_input);
    geocoin::core::TxOutput missing_output;
    missing_output.value = 1 * geocoin::COIN;
    missing_output.address = recipient.get_address();
    missing_utxo.outputs.push_back(missing_output);
    expect(!mempool.add_transaction(missing_utxo), "missing UTXO is rejected by cheap policy");

    geocoin::core::Transaction unsigned_cadastral;
    unsigned_cadastral.type = geocoin::core::TransactionType::CADASTRAL_DOC;
    unsigned_cadastral.inputs.push_back(missing_input);
    unsigned_cadastral.outputs.push_back(missing_output);
    unsigned_cadastral.cadastral_doc_hash = hash_from_seed(0xc1);
    unsigned_cadastral.cadastral_nft_id = hash_from_seed(0xc2);
    unsigned_cadastral.cadastral_owner = recipient.get_address();
    expect(!mempool.add_transaction(unsigned_cadastral),
           "missing-input cadastral tx is rejected before signature verification");

    auto rejects = mempool.get_reject_stats();
    expect(rejects.consensus["bad-tx-version"] == 1, "bad version reject is counted");
    expect(rejects.consensus["bad-witness-version"] == 1, "bad witness version reject is counted");
    expect(rejects.consensus["bad-txns-vin-empty"] == 1, "empty vin reject is counted");
    expect(rejects.policy["utxo-not-found"] == 2,
           "missing inputs are counted by pre-signature policy");
}

void test_remove_block_transactions_conflicts_and_descendants()
{
    using geocoin::core::Transaction;
    using geocoin::core::TransactionType;
    using geocoin::network::MempoolTestAccess;

    // A null ChainState keeps this test focused on graph cleanup; entries are
    // seeded because ordinary admission correctly rejects children whose
    // parents are not yet part of ChainState.
    geocoin::network::Mempool mempool(nullptr);

    auto make_tx = [](uint8_t timestamp_seed, const geocoin::Hash256& prev, uint32_t vout) {
        Transaction tx;
        tx.version = 2;
        tx.type = TransactionType::STANDARD;
        tx.timestamp = timestamp_seed;
        tx.inputs.push_back({prev, vout, {}, 0xffffffff, {}, {}});
        tx.outputs.push_back({1, address_from_key(static_cast<uint8_t>(50 + timestamp_seed)), {}});
        return tx;
    };

    const auto shared_outpoint = hash_from_seed(0xa0);
    const auto exact = make_tx(1, shared_outpoint, 0);
    const auto conflict = make_tx(2, shared_outpoint, 0);
    const auto conflict_child = make_tx(3, conflict.get_hash(), 0);
    const auto confirmed_child = make_tx(4, exact.get_hash(), 0);
    const auto unrelated = make_tx(5, hash_from_seed(0xa1), 0);

    for (const auto* tx : {&exact, &conflict, &conflict_child, &confirmed_child, &unrelated}) {
        MempoolTestAccess::seed_entry(mempool, *tx, tx->serialize().size(), 1);
    }

    geocoin::core::Block block;
    block.transactions.push_back(Transaction::create_coinbase(1, address_from_key(60), 1));
    block.transactions.push_back(exact);
    mempool.remove_block_transactions(block);

    expect(!mempool.has_transaction(exact.get_hash()), "confirmed transaction is removed");
    expect(!mempool.has_transaction(conflict.get_hash()), "block input conflict is removed");
    expect(!mempool.has_transaction(conflict_child.get_hash()), "conflict descendant is removed");
    expect(mempool.has_transaction(confirmed_child.get_hash()),
           "descendant of confirmed transaction remains valid");
    expect(mempool.has_transaction(unrelated.get_hash()), "unrelated transaction remains");
}

void test_remove_block_transactions_requires_active_tip()
{
    auto state = make_chain_state();
    geocoin::wallet::Wallet sender;
    geocoin::wallet::Wallet recipient;
    expect(sender.initialize(make_key(81)), "active-tip sender initializes");
    expect(recipient.initialize(make_key(82)), "active-tip recipient initializes");
    fund_wallet(state, sender, 1, 20 * geocoin::COIN);
    state->set_height(geocoin::consensus::params::COINBASE_MATURITY + 1);

    geocoin::network::Mempool mempool(state);
    const auto tx = create_payment(
        sender,
        recipient.get_address(),
        state,
        2 * geocoin::COIN,
        relay_fee_for_payment(sender, 2 * geocoin::COIN, state)
    );
    expect(mempool.add_transaction(tx), "active-tip fixture enters mempool");

    geocoin::core::Block block;
    block.header.version = 1;
    block.header.prev_block_hash = hash_from_seed(0xd0);
    block.header.timestamp = 12345;
    block.header.bits = geocoin::consensus::ProofOfWork::get_initial_bits();
    block.transactions.push_back(tx);
    block.header.merkle_root = block.calculate_merkle_root();
    block.header.witness_commitment = block.calculate_witness_commitment();

    state->set_tip_hash(hash_from_seed(0xd1));
    mempool.remove_block_transactions(block);
    expect(mempool.has_transaction(tx.get_hash()),
           "side-branch block does not remove an active-chain mempool transaction");

    state->set_tip_hash(block.get_hash());
    mempool.remove_block_transactions(block);
    expect(!mempool.has_transaction(tx.get_hash()),
           "active-tip block removes its confirmed mempool transaction");
}

void test_mempool_internal_bookkeeping_guards()
{
    auto state = make_chain_state();
    geocoin::network::Mempool mempool(state);

    geocoin::core::Transaction underflow_tx;
    underflow_tx.version = 2;
    underflow_tx.type = geocoin::core::TransactionType::STANDARD;
    underflow_tx.timestamp = 1;
    underflow_tx.outputs.push_back({1, address_from_key(10), {}});
    geocoin::network::MempoolTestAccess::seed_entry(mempool, underflow_tx, 100, 1);
    geocoin::network::MempoolTestAccess::set_current_size(mempool, 1);
    expect(mempool.remove_transaction(underflow_tx.get_hash()),
           "remove_transaction tolerates current_size underflow");
    expect(mempool.get_stats().total_size == 0,
           "underflow guard resets mempool byte size to zero");

    geocoin::core::Transaction expired_tx = underflow_tx;
    expired_tx.timestamp = 9;
    geocoin::network::MempoolTestAccess::seed_entry(
        mempool,
        expired_tx,
        expired_tx.serialize().size(),
        1
    );
    geocoin::network::MempoolTestAccess::set_added_at(
        mempool,
        expired_tx.get_hash(),
        std::chrono::steady_clock::now() - std::chrono::hours(73)
    );
    expect(mempool.expire_old_transactions() == 1,
           "stale mempool transactions expire");
    expect(!mempool.has_transaction(expired_tx.get_hash()),
           "expired transaction is removed from mempool indexes");

    geocoin::core::Transaction angle;
    angle.version = 2;
    angle.type = geocoin::core::TransactionType::ANGLE_COMMITMENT;
    angle.timestamp = 2;
    angle.inputs.push_back({hash_from_seed(0x40), 0, {}, 0xffffffff, {}, {}});
    geocoin::network::MempoolTestAccess::seed_entry(mempool, angle, angle.serialize().size(), 100);

    geocoin::core::Transaction multisig;
    multisig.version = 2;
    multisig.type = geocoin::core::TransactionType::MULTISIG_SPEND;
    multisig.timestamp = 3;
    multisig.m = 1;
    multisig.n = 1;
    multisig.spend_participants = {address_from_key(11)};
    multisig.angle_commitment_hashes.push_back(angle.get_hash());
    multisig.angle_commitment_hashes.push_back(hash_from_seed(0x41));
    multisig.outputs.push_back({2, address_from_key(12), {}});
    geocoin::network::MempoolTestAccess::seed_entry(mempool, multisig, multisig.serialize().size(), 10);

    auto mining = mempool.get_transactions_for_mining(1024 * 1024);
    expect(std::none_of(mining.begin(), mining.end(), [&](const auto& tx) {
               return tx.get_hash() == angle.get_hash();
           }),
           "mining selection never includes an orphan commitment");
    expect(std::none_of(mining.begin(), mining.end(), [&](const auto& tx) {
               return tx.get_hash() == multisig.get_hash();
           }),
           "mining selection rejects a multisig package with a missing commitment");

    expect(mempool.remove_transaction(multisig.get_hash()),
           "non-hierarchical multisig bookkeeping removes cleanly");
    expect(mempool.remove_transaction(angle.get_hash()),
           "angle commitment bookkeeping removes cleanly");

    geocoin::network::MempoolTestAccess::seed_stale_fee_index(mempool, hash_from_seed(0x42), 50);
    geocoin::network::MempoolTestAccess::evict_low_fee_transactions(mempool, 0);
    geocoin::network::MempoolTestAccess::evict_low_fee_transactions(mempool, 1);
    expect(geocoin::network::MempoolTestAccess::fee_index_size(mempool) == 0,
           "eviction tolerates and clears stale fee index entries");

    geocoin::core::Transaction delegation;
    delegation.version = 2;
    delegation.type = geocoin::core::TransactionType::DELEGATION_PROOF;
    delegation.timestamp = 4;
    delegation.primary_multisig_address = address_from_key(20);
    delegation.delegated_multisig_address = address_from_key(21);
    delegation.nonce = 1;
    delegation.max_total_delegated = 100;
    const auto delegation_txid = delegation.get_hash();
    expect(state->get_storage()->put_transaction(delegation_txid, delegation),
           "delegation proof stores for mempool bookkeeping");

    auto make_hierarchical = [&](uint64_t timestamp, geocoin::Amount amount) {
        geocoin::core::Transaction tx;
        tx.version = 2;
        tx.type = geocoin::core::TransactionType::MULTISIG_SPEND;
        tx.timestamp = timestamp;
        tx.m = 1;
        tx.n = 1;
        tx.delegation_proof_txid = delegation_txid;
        tx.outputs.push_back({amount, address_from_key(static_cast<uint8_t>(30 + timestamp)), {}});
        return tx;
    };

    auto hierarchical_subtract = make_hierarchical(5, 10);
    geocoin::network::MempoolTestAccess::seed_entry(
        mempool,
        hierarchical_subtract,
        hierarchical_subtract.serialize().size(),
        20
    );
    geocoin::network::MempoolTestAccess::seed_delegation_pending(
        mempool, delegation_txid, 30 + 20 * hierarchical_subtract.serialize().size());
    expect(mempool.remove_transaction(hierarchical_subtract.get_hash()),
           "hierarchical multisig removal subtracts pending delegation amount");
    expect(geocoin::network::MempoolTestAccess::has_delegation_pending(mempool, delegation_txid),
           "pending delegation bucket remains when amount is partially removed");

    auto hierarchical_erase = make_hierarchical(6, 10);
    geocoin::network::MempoolTestAccess::seed_entry(
        mempool,
        hierarchical_erase,
        hierarchical_erase.serialize().size(),
        20
    );
    geocoin::network::MempoolTestAccess::seed_delegation_pending(
        mempool, delegation_txid, 10 + 20 * hierarchical_erase.serialize().size());
    expect(mempool.remove_transaction(hierarchical_erase.get_hash()),
           "hierarchical multisig removal erases empty pending delegation bucket");
    expect(!geocoin::network::MempoolTestAccess::has_delegation_pending(mempool, delegation_txid),
           "pending delegation bucket is removed at zero");

    geocoin::network::MempoolTestAccess::seed_entry(mempool, delegation, delegation.serialize().size(), 5);
    expect(mempool.remove_transaction(delegation.get_hash()),
           "delegation proof anti-replay bookkeeping removes cleanly");

    geocoin::core::Transaction revoke;
    revoke.version = 2;
    revoke.type = geocoin::core::TransactionType::REVOKE_DELEGATION;
    revoke.timestamp = 7;
    revoke.revoked_delegation_proof_txid = delegation_txid;
    geocoin::network::MempoolTestAccess::seed_entry(mempool, revoke, revoke.serialize().size(), 5);
    expect(mempool.remove_transaction(revoke.get_hash()),
           "revoke delegation anti-replay bookkeeping removes cleanly");

    geocoin::core::Transaction nft_create;
    nft_create.version = 2;
    nft_create.type = geocoin::core::TransactionType::NFT_CREATE;
    nft_create.timestamp = 8;
    geocoin::nft::NFTCreationTransaction nft_payload;
    nft_payload.nft_id = hash_from_seed(0x43);
    nft_payload.creator_address = address_from_key(40);
    nft_create.nft_create = nft_payload;
    geocoin::network::MempoolTestAccess::seed_entry(mempool, nft_create, nft_create.serialize().size(), 5);
    expect(mempool.remove_transaction(nft_create.get_hash()),
           "NFT_CREATE anti-replay bookkeeping removes cleanly");
}

void test_mempool_eviction_and_reject_stats()
{
    geocoin::network::Mempool null_mempool(nullptr);
    geocoin::core::Transaction dummy;
    dummy.type = geocoin::core::TransactionType::STANDARD;
    expect(!null_mempool.add_transaction(dummy), "null chainstate rejects add");
    auto rejects = null_mempool.get_reject_stats();
    expect(rejects.consensus["no-chainstate"] == 1, "no-chainstate reject is counted");

    auto state = make_chain_state();
    geocoin::wallet::Wallet low_wallet;
    geocoin::wallet::Wallet high_wallet;
    geocoin::wallet::Wallet recipient;
    expect(low_wallet.initialize(make_key(11)), "low wallet initializes");
    expect(high_wallet.initialize(make_key(21)), "high wallet initializes");
    expect(recipient.initialize(make_key(31)), "recipient wallet initializes");

    fund_wallet(state, low_wallet, 1, 80 * geocoin::COIN);
    fund_wallet(state, high_wallet, 2, 80 * geocoin::COIN);
    mature_chain(state, 2);

    const geocoin::Amount low_relay_fee =
        relay_fee_for_payment(low_wallet, 2 * geocoin::COIN, state);
    const auto low_fee_tx = create_payment(
        low_wallet,
        recipient.get_address(),
        state,
        2 * geocoin::COIN,
        low_relay_fee
    );
    const auto high_fee_tx = create_payment(
        high_wallet,
        recipient.get_address(),
        state,
        2 * geocoin::COIN,
        low_relay_fee * 10
    );

    geocoin::network::Mempool mempool(state);
    mempool.set_max_size(high_fee_tx.serialize().size() + 16);
    expect(mempool.add_transaction(low_fee_tx), "low-fee tx initially fits");
    expect(mempool.add_transaction(high_fee_tx), "high-fee tx evicts low-fee tx");
    expect(!mempool.has_transaction(low_fee_tx.get_hash()), "low-fee tx evicted");
    expect(mempool.has_transaction(high_fee_tx.get_hash()), "high-fee tx retained");
    expect(mempool.size() == 1, "eviction leaves one tx");
    expect(!mempool.add_transaction(low_fee_tx),
           "lower-fee transaction cannot evict a higher-fee mempool entry");
    expect(mempool.has_transaction(high_fee_tx.get_hash()),
           "failed low-fee replacement keeps higher-fee transaction");

    auto low_fee_reject = create_payment(
        low_wallet,
        recipient.get_address(),
        state,
        1 * geocoin::COIN,
        relay_fee_for_payment(low_wallet, 1 * geocoin::COIN, state)
    );
    geocoin::network::MempoolPolicyConfig policy;
    policy.min_relay_fee = static_cast<uint64_t>(geocoin::consensus::params::MIN_TX_FEE * 100);
    mempool.set_policy_config(policy);
    expect(!mempool.add_transaction(low_fee_reject), "policy rejects fee below configured relay floor");
    rejects = mempool.get_reject_stats();
    expect(rejects.policy["fee-too-low"] == 1, "fee-too-low reject is counted");
}

void test_mempool_size_limit_rejects_oversized_single_tx()
{
    auto state = make_chain_state();
    geocoin::wallet::Wallet wallet;
    geocoin::wallet::Wallet recipient;
    expect(wallet.initialize(make_key(32)), "oversized wallet initializes");
    expect(recipient.initialize(make_key(33)), "oversized recipient initializes");

    fund_wallet(state, wallet, 1, 80 * geocoin::COIN);
    mature_chain(state, 1);

    const auto tx = create_payment(
        wallet,
        recipient.get_address(),
        state,
        2 * geocoin::COIN,
        relay_fee_for_payment(wallet, 2 * geocoin::COIN, state)
    );

    geocoin::network::Mempool mempool(state);
    mempool.set_max_size(tx.serialize().size() - 1);
    expect(!mempool.add_transaction(tx), "single transaction larger than mempool limit is rejected");
    expect(mempool.empty(), "oversized transaction does not enter mempool");
    expect(mempool.get_transactions_for_relay().empty(), "oversized transaction is not queued for relay");
}

void test_mempool_fee_conflict_and_accounting_helpers()
{
    using Access = geocoin::network::MempoolTestAccess;

    geocoin::network::Mempool null_mempool(nullptr);
    geocoin::core::Transaction any_tx;
    expect(Access::calculate_fee(null_mempool, any_tx) == 0,
           "fee helper returns zero without chainstate");

    auto state = make_chain_state();
    geocoin::wallet::Wallet wallet;
    geocoin::wallet::Wallet recipient;
    geocoin::wallet::Wallet other;
    expect(wallet.initialize(make_key(34)), "helper wallet initializes");
    expect(recipient.initialize(make_key(35)), "helper recipient initializes");
    expect(other.initialize(make_key(36)), "helper other wallet initializes");

    fund_wallet(state, wallet, 1, 80 * geocoin::COIN);
    mature_chain(state, 1);

    const auto standard = create_payment(
        wallet,
        recipient.get_address(),
        state,
        2 * geocoin::COIN,
        relay_fee_for_payment(wallet, 2 * geocoin::COIN, state)
    );

    geocoin::network::Mempool mempool(state);
    expect(Access::calculate_fee(mempool, standard) >= geocoin::consensus::params::MIN_TX_FEE,
           "standard fee helper uses input minus output");
    expect(!Access::has_conflicts(mempool, standard), "empty mempool has no conflicts");
    expect(mempool.add_transaction(standard), "helper standard tx enters mempool");

    const auto conflict = create_payment(
        wallet,
        other.get_address(),
        state,
        3 * geocoin::COIN,
        relay_fee_for_payment(wallet, 3 * geocoin::COIN, state)
    );
    expect(Access::has_conflicts(mempool, conflict), "conflict helper detects spent output");

    auto delegation_proof = conflict;
    delegation_proof.type = geocoin::core::TransactionType::DELEGATION_PROOF;
    expect(Access::has_conflicts(mempool, delegation_proof),
           "delegation proof participates in real UTXO input conflicts");

    auto angle_commitment = conflict;
    angle_commitment.type = geocoin::core::TransactionType::ANGLE_COMMITMENT;
    expect(!Access::has_conflicts(mempool, angle_commitment),
           "angle commitment is exempt from mempool input conflicts");

    geocoin::core::Transaction revoke_a;
    revoke_a.type = geocoin::core::TransactionType::REVOKE_DELEGATION;
    revoke_a.inputs.push_back({{}, 0xFFFFFFFCu, {}, 0xffffffff, {}, {}});
    revoke_a.revoked_delegation_proof_txid = hash_from_seed(0xd1);
    auto revoke_b = revoke_a;
    revoke_b.revoked_delegation_proof_txid = hash_from_seed(0xd2);
    Access::seed_spent_output(mempool, geocoin::Hash256{}, 0xFFFFFFFCu);
    expect(Access::has_conflicts(mempool, revoke_a) && Access::has_conflicts(mempool, revoke_b),
           "revocations participate in real UTXO input conflicts");

    const auto utxos = state->get_utxos_for_address(wallet.get_address());
    expect(!utxos.empty(), "helper wallet has a funding UTXO");
    const auto& utxo = utxos.front();

    geocoin::core::Transaction lock_deposit;
    lock_deposit.type = geocoin::core::TransactionType::LOCK_DEPOSIT;
    lock_deposit.inputs.push_back({utxo.tx_hash, utxo.vout, {}, 0xffffffff, {}, {}});
    lock_deposit.outputs.push_back({utxo.value / 2, wallet.get_address(), {}});
    lock_deposit.sidechain_amount = utxo.value / 4;
    expect(Access::calculate_fee(mempool, lock_deposit) ==
               utxo.value - lock_deposit.outputs[0].value - lock_deposit.sidechain_amount,
           "LOCK_DEPOSIT fee excludes locked sidechain amount");
    lock_deposit.sidechain_amount = utxo.value;
    expect(Access::calculate_fee(mempool, lock_deposit) == 0,
           "LOCK_DEPOSIT fee is zero when lock plus outputs consume the input");

    geocoin::core::Transaction issue;
    issue.type = geocoin::core::TransactionType::ISSUE_SIDECHAIN;
    issue.sidechain_fee_bps = 500;
    expect(Access::calculate_fee(mempool, issue) ==
               geocoin::consensus::calc_sidechain_fee(0, issue.sidechain_fee_bps),
           "ISSUE_SIDECHAIN fee uses configured bps with minimum fee");

    geocoin::core::Transaction mint_missing_registry;
    mint_missing_registry.type = geocoin::core::TransactionType::MINT_SIDECHAIN;
    mint_missing_registry.sidechain_id = hash_from_seed(0xa4);
    mint_missing_registry.sidechain_amount = 100 * geocoin::COIN;
    expect(Access::calculate_fee(mempool, mint_missing_registry) == 0,
           "MINT_SIDECHAIN fee is zero when registry is missing");

    geocoin::state::SidechainRegistryEntry registry;
    registry.fee_bps = 250;
    geocoin::state::ChainStateTestAccess::put_sidechain_registry(
        *state,
        mint_missing_registry.sidechain_id,
        registry
    );
    expect(Access::calculate_fee(mempool, mint_missing_registry) ==
               geocoin::consensus::calc_sidechain_fee(mint_missing_registry.sidechain_amount, registry.fee_bps),
           "MINT_SIDECHAIN fee uses registry fee bps");

    geocoin::core::Transaction checkpoint;
    checkpoint.type = geocoin::core::TransactionType::SIDECHAIN_CHECKPOINT;
    checkpoint.sidechain_amount = 77;
    expect(Access::calculate_fee(mempool, checkpoint) == 77,
           "SIDECHAIN_CHECKPOINT fee uses sidechain_amount");

    geocoin::core::Transaction batch;
    batch.type = geocoin::core::TransactionType::BATCH_SIDECHAIN_CHECKPOINT;
    geocoin::core::BatchCheckpointItem item_a;
    item_a.fee = 3;
    geocoin::core::BatchCheckpointItem item_b;
    item_b.fee = 4;
    batch.sidechain_batch_checkpoints = {item_a, item_b};
    expect(Access::calculate_fee(mempool, batch) == 7,
           "BATCH_SIDECHAIN_CHECKPOINT fee sums item fees");

    geocoin::core::BatchCheckpointItem overflow_item_a;
    overflow_item_a.fee = (std::numeric_limits<geocoin::Amount>::max)();
    geocoin::core::BatchCheckpointItem overflow_item_b;
    overflow_item_b.fee = 1;
    batch.sidechain_batch_checkpoints = {overflow_item_a, overflow_item_b};
    expect(Access::calculate_fee(mempool, batch) == 0,
           "BATCH_SIDECHAIN_CHECKPOINT fee overflow fails closed");
    expect(state->calculate_transaction_fee(batch) == 0,
           "ChainState batch checkpoint fee overflow fails closed");
    overflow_item_a.fee = -1;
    batch.sidechain_batch_checkpoints = {overflow_item_a};
    expect(Access::calculate_fee(mempool, batch) == 0,
           "negative batch checkpoint fee cannot wrap to uint64");
    expect(state->calculate_transaction_fee(batch) == 0,
           "ChainState rejects negative batch checkpoint fee");

    checkpoint.sidechain_amount = -1;
    expect(Access::calculate_fee(mempool, checkpoint) == 0,
           "negative checkpoint fee cannot wrap to uint64");
    expect(state->calculate_transaction_fee(checkpoint) == 0,
           "ChainState rejects negative checkpoint fee");

    geocoin::core::Transaction committee_update;
    committee_update.type = geocoin::core::TransactionType::UPDATE_SIDECHAIN_COMMITTEE;
    expect(Access::calculate_fee(mempool, committee_update) == 0,
           "sidechain control transactions without explicit fee return zero");

    geocoin::core::Transaction proof_tx;
    proof_tx.primary_multisig_address = wallet.get_address();
    geocoin::core::Transaction hierarchical_spend;
    hierarchical_spend.outputs.push_back({2 * geocoin::COIN, recipient.get_address(), {}});
    hierarchical_spend.outputs.push_back({5 * geocoin::COIN, wallet.get_address(), {}});
    hierarchical_spend.outputs.push_back({3 * geocoin::COIN, other.get_address(), {}});
    expect(Access::hierarchical_amount_spent(mempool, hierarchical_spend, proof_tx, 500) ==
               5 * geocoin::COIN + 500,
           "hierarchical spent helper includes fee and excludes change back to primary multisig");

    geocoin::core::Transaction overflowing_hierarchical;
    overflowing_hierarchical.outputs.push_back({
        (std::numeric_limits<geocoin::Amount>::max)() - 5,
        recipient.get_address(),
        {}
    });
    overflowing_hierarchical.outputs.push_back({10, other.get_address(), {}});
    expect(Access::hierarchical_amount_spent(mempool, overflowing_hierarchical, proof_tx) < 0,
           "hierarchical amount overflow returns a rejecting sentinel");

    auto overflow_state = make_chain_state();
    const auto overflow_address = wallet.get_address();
    geocoin::state::UTXO overflow_utxo_a;
    overflow_utxo_a.tx_hash = hash_from_seed(0xe1);
    overflow_utxo_a.vout = 0;
    overflow_utxo_a.value = (std::numeric_limits<geocoin::Amount>::max)() - 5;
    overflow_utxo_a.address = overflow_address;
    geocoin::state::UTXO overflow_utxo_b = overflow_utxo_a;
    overflow_utxo_b.tx_hash = hash_from_seed(0xe2);
    overflow_utxo_b.value = 10;
    geocoin::state::ChainStateTestAccess::put_utxo(*overflow_state, overflow_utxo_a);
    geocoin::state::ChainStateTestAccess::put_utxo(*overflow_state, overflow_utxo_b);

    expect(overflow_state->get_balance(overflow_address) == 0,
           "ChainState balance overflow fails closed");
    expect(overflow_state->get_total_supply() == 0,
           "ChainState total supply overflow fails closed");

    geocoin::network::Mempool overflow_mempool(overflow_state);
    geocoin::core::Transaction input_overflow;
    input_overflow.type = geocoin::core::TransactionType::STANDARD;
    input_overflow.inputs.push_back({overflow_utxo_a.tx_hash, 0, {}, 0xffffffff, {}, {}});
    input_overflow.inputs.push_back({overflow_utxo_b.tx_hash, 0, {}, 0xffffffff, {}, {}});
    input_overflow.outputs.push_back({1, recipient.get_address(), {}});
    expect(Access::calculate_fee(overflow_mempool, input_overflow) == 0,
           "mempool input sum overflow fails closed");
    expect(overflow_state->calculate_transaction_fee(input_overflow) == 0,
           "ChainState input sum overflow fails closed");

    geocoin::core::Transaction output_overflow;
    output_overflow.type = geocoin::core::TransactionType::STANDARD;
    output_overflow.inputs.push_back({overflow_utxo_a.tx_hash, 0, {}, 0xffffffff, {}, {}});
    output_overflow.outputs.push_back({
        (std::numeric_limits<geocoin::Amount>::max)() - 5,
        recipient.get_address(),
        {}
    });
    output_overflow.outputs.push_back({10, other.get_address(), {}});
    expect(Access::calculate_fee(overflow_mempool, output_overflow) == 0,
           "mempool output sum overflow fails closed");
    expect(overflow_state->calculate_transaction_fee(output_overflow) == 0,
           "ChainState output sum overflow fails closed");

    const auto hex = Access::hash256_to_hex(hash_from_seed(0x44));
    expect(hex.size() == 64 && hex.substr(0, 2) == "44", "hash256 hex helper formats 32 bytes");

    Access::record_reject(mempool, true, "");
    Access::record_reject(mempool, false, "");
    const auto rejects = mempool.get_reject_stats();
    expect(rejects.policy.at("unknown") == 1, "empty policy reject reason is stored as unknown");
    expect(rejects.consensus.at("unknown") == 1, "empty consensus reject reason is stored as unknown");
}

void test_mempool_multisig_package_bookkeeping()
{
    auto state = make_chain_state();
    const auto signer_a_key = make_key(101);
    const auto signer_b_key = make_key(111);
    const auto signer_c_key = make_key(121);
    const auto signer_a = address_from_key(101);
    const auto signer_b = address_from_key(111);
    const auto signer_c = address_from_key(121);
    const auto recipient = address_from_key(131);
    const auto config = make_multisig_config(2, 3, {signer_a, signer_b, signer_c});

    fund_address(state, config.address, 1, 20 * geocoin::COIN);
    mature_chain(state, 1);

    const auto utxos = state->get_utxos_for_address(config.address);
    expect(!utxos.empty(), "multisig address has spendable UTXO");

    geocoin::core::TxInput input;
    input.prev_tx_hash = utxos.front().tx_hash;
    input.vout = utxos.front().vout;

    geocoin::core::TxOutput payment;
    payment.value = 3 * geocoin::COIN;
    payment.address = recipient;

    constexpr geocoin::Amount package_fee = 20'000;
    geocoin::core::TxOutput change;
    change.value = utxos.front().value - payment.value - package_fee;
    change.address = config.address;

    geocoin::core::Transaction spend = geocoin::core::Transaction::create_multisig_spend(
        {input},
        {payment, change},
        {},
        config.m,
        config.n
    );
    spend.spend_participants = config.addresses;

    const geocoin::Hash256 spend_hash = spend.get_hash_without_commitments();
    geocoin::core::Transaction commitment_a = geocoin::core::Transaction::create_angle_commitment(
        config.address,
        spend_hash,
        signer_a_key,
        geocoin::Address(""),
        "degree"
    );
    geocoin::core::Transaction commitment_b = geocoin::core::Transaction::create_angle_commitment(
        config.address,
        spend_hash,
        signer_b_key,
        geocoin::Address(""),
        "degree"
    );
    spend.angle_commitment_hashes = {commitment_a.get_hash(), commitment_b.get_hash()};

    geocoin::network::Mempool mempool(state);
    expect(mempool.add_transaction(commitment_a), "first angle commitment enters mempool");
    expect(!mempool.add_transaction(commitment_a), "duplicate angle commitment is rejected");
    expect(mempool.add_transaction(commitment_b), "second angle commitment enters mempool");
    expect(mempool.add_transaction(spend), "multisig spend enters mempool after commitments");
    expect(!mempool.add_transaction(spend), "duplicate multisig spend is rejected");

    const auto small_template = mempool.get_transactions_for_mining(spend.serialize().size());
    expect(std::none_of(small_template.begin(), small_template.end(), [](const auto& tx) {
        return tx.type == geocoin::core::TransactionType::MULTISIG_SPEND;
    }), "multisig spend is skipped when its package does not fit");

    const auto mining = mempool.get_transactions_for_mining(1024 * 1024);
    expect(mining.size() == 3, "mining package contains commitments and spend");
    expect(mining[0].get_hash() == commitment_a.get_hash() ||
               mining[0].get_hash() == commitment_b.get_hash(),
           "mining package starts with an angle commitment");
    expect(mining[1].get_hash() == commitment_a.get_hash() ||
               mining[1].get_hash() == commitment_b.get_hash(),
           "mining package includes the second angle commitment");
    expect(mining[0].get_hash() != mining[1].get_hash(), "commitments are not duplicated in package");
    expect(mining[2].get_hash() == spend.get_hash(), "mining package appends multisig spend after commitments");

    const auto relay = mempool.get_transactions_for_relay();
    expect(relay.size() == 3, "relay queue contains package transactions");
    expect(mempool.get_transactions_for_relay().empty(), "relay queue clears after read");

    expect(mempool.remove_transaction(commitment_a.get_hash()), "first commitment can be removed");
    expect(!mempool.has_transaction(spend.get_hash()),
           "removing a commitment also removes its dependent multisig spend");
    expect(!state->get_angle_store()->has_commitment(commitment_a.get_hash()),
           "removing a mempool commitment clears the temporary angle store entry");
    expect(mempool.add_transaction(commitment_a), "angle commitment can be re-added after removal clears indexes");
    expect(mempool.add_transaction(spend), "dependent spend can be re-added after restoring its commitment");
    mempool.clear();
    expect(mempool.empty(), "multisig package mempool clears");
    expect(!state->get_angle_store()->has_commitment(commitment_a.get_hash()) &&
               !state->get_angle_store()->has_commitment(commitment_b.get_hash()),
           "mempool clear removes temporary commitments from the shared angle store");
}

// ANGLE_COMMITMENT pays no fee. A pool filled with commitments that no root
// ever claims must not reject an honest multisig package.
void test_angle_commitment_flood_cannot_lock_out_multisig()
{
    using Access = geocoin::network::MempoolTestAccess;
    auto state = make_chain_state();
    const auto signer_a_key = make_key(141);
    const auto signer_b_key = make_key(151);
    const auto config = make_multisig_config(
        2, 3, {address_from_key(141), address_from_key(151), address_from_key(161)});
    fund_address(state, config.address, 1, 20 * geocoin::COIN);
    mature_chain(state, 1);
    const auto utxos = state->get_utxos_for_address(config.address);
    expect(!utxos.empty(), "flood test multisig address has a spendable UTXO");

    geocoin::core::TxInput input;
    input.prev_tx_hash = utxos.front().tx_hash;
    input.vout = utxos.front().vout;
    geocoin::core::TxOutput payment;
    payment.value = 3 * geocoin::COIN;
    payment.address = address_from_key(171);
    geocoin::core::TxOutput change;
    change.value = utxos.front().value - payment.value - 20'000;
    change.address = config.address;
    auto spend = geocoin::core::Transaction::create_multisig_spend(
        {input}, {payment, change}, {}, config.m, config.n);
    spend.spend_participants = config.addresses;
    const auto spend_hash = spend.get_hash_without_commitments();
    const auto commitment_a = geocoin::core::Transaction::create_angle_commitment(
        config.address, spend_hash, signer_a_key, geocoin::Address(""), "degree");
    const auto commitment_b = geocoin::core::Transaction::create_angle_commitment(
        config.address, spend_hash, signer_b_key, geocoin::Address(""), "degree");
    spend.angle_commitment_hashes = {commitment_a.get_hash(), commitment_b.get_hash()};

    geocoin::network::Mempool mempool(state);
    constexpr size_t capacity = geocoin::network::Mempool::kMaxPendingAngleCommitments;
    const auto flood = Access::seed_unclaimed_angle_commitments(
        mempool, capacity, std::chrono::steady_clock::now() - std::chrono::minutes(10));
    expect(Access::pending_angle_commitments(mempool) == capacity, "flood fills the commitment pool");

    expect(mempool.test_accept_transaction(commitment_a), "a full pool still accepts a fresh commitment in dry runs");
    expect(mempool.add_transaction(commitment_a), "a fresh commitment displaces an unclaimed one");
    expect(mempool.add_transaction(commitment_b), "the second signer's commitment is admitted");
    expect(mempool.add_transaction(spend), "the multisig spend claims its commitments");
    expect(Access::pending_angle_commitments(mempool) == capacity, "eviction keeps the commitment pool bounded");
    expect(!mempool.has_transaction(flood[0]) && !mempool.has_transaction(flood[1]),
           "the oldest unclaimed commitments are evicted first");

    // A commitment claimed by a fee-paying root is never displaced.
    for (uint8_t seed = 0; seed < 3; ++seed) {
        const auto spam = geocoin::core::Transaction::create_angle_commitment(
            address_from_key(static_cast<uint8_t>(181 + seed)), hash_from_seed(static_cast<uint8_t>(0x90 + seed)),
            make_key(static_cast<uint8_t>(191 + seed)), geocoin::Address(""), "degree");
        expect(mempool.add_transaction(spam), "new spam can only displace older unclaimed commitments");
    }
    expect(mempool.has_transaction(commitment_a.get_hash()) &&
               mempool.has_transaction(commitment_b.get_hash()) &&
               mempool.has_transaction(spend.get_hash()),
           "claimed commitments and their spend survive a continued flood");
}

// Two commitments for the same spend must outrank a later flood of isolated
// zero-fee envelopes. Otherwise an honest package that arrives first is aged
// out before its root can claim the slots.
void test_angle_commitment_package_survives_isolated_flood()
{
    using Access = geocoin::network::MempoolTestAccess;
    auto state = make_chain_state();
    const auto signer_a_key = make_key(201);
    const auto signer_b_key = make_key(211);
    const auto config = make_multisig_config(
        2, 3, {address_from_key(201), address_from_key(211), address_from_key(221)});
    fund_address(state, config.address, 1, 20 * geocoin::COIN);
    mature_chain(state, 1);
    const auto utxos = state->get_utxos_for_address(config.address);
    expect(!utxos.empty(), "isolated-flood test multisig address has a spendable UTXO");

    geocoin::core::TxInput input;
    input.prev_tx_hash = utxos.front().tx_hash;
    input.vout = utxos.front().vout;
    geocoin::core::TxOutput payment;
    payment.value = 3 * geocoin::COIN;
    payment.address = address_from_key(231);
    geocoin::core::TxOutput change;
    change.value = utxos.front().value - payment.value - 20'000;
    change.address = config.address;
    auto spend = geocoin::core::Transaction::create_multisig_spend(
        {input}, {payment, change}, {}, config.m, config.n);
    spend.spend_participants = config.addresses;
    const auto spend_hash = spend.get_hash_without_commitments();
    const auto commitment_a = geocoin::core::Transaction::create_angle_commitment(
        config.address, spend_hash, signer_a_key, geocoin::Address(""), "degree");
    const auto commitment_b = geocoin::core::Transaction::create_angle_commitment(
        config.address, spend_hash, signer_b_key, geocoin::Address(""), "degree");
    spend.angle_commitment_hashes = {commitment_a.get_hash(), commitment_b.get_hash()};

    geocoin::network::Mempool mempool(state);
    expect(mempool.add_transaction(commitment_a), "first honest commitment is admitted");
    expect(mempool.add_transaction(commitment_b), "second honest commitment is admitted");

    constexpr size_t flood_count =
        geocoin::network::Mempool::kMaxPendingAngleCommitments - 2;
    Access::seed_unclaimed_angle_commitments(
        mempool, flood_count, std::chrono::steady_clock::now() + std::chrono::minutes(10));
    expect(Access::pending_angle_commitments(mempool) ==
               geocoin::network::Mempool::kMaxPendingAngleCommitments,
           "isolated flood fills the remaining commitment slots");

    for (uint8_t seed = 0; seed < 4; ++seed) {
        const auto spam = geocoin::core::Transaction::create_angle_commitment(
            address_from_key(static_cast<uint8_t>(241 + seed)),
            hash_from_seed(static_cast<uint8_t>(0xa0 + seed)),
            make_key(static_cast<uint8_t>(251 + seed)),
            geocoin::Address(""),
            "degree");
        expect(mempool.add_transaction(spam), "isolated spam can only displace other isolated commitments");
    }
    expect(mempool.has_transaction(commitment_a.get_hash()) &&
               mempool.has_transaction(commitment_b.get_hash()),
           "a two-signer package is not evicted by isolated commitment spam");
    expect(mempool.add_transaction(spend), "the honest spend still claims its protected commitments");
}

void test_delegation_proof_nonce_is_unique_in_mempool_and_template()
{
    using Access = geocoin::network::MempoolTestAccess;
    // No ChainState: policy and state projections are skipped, so only the
    // per-address nonce rule decides what the template may contain.
    geocoin::network::Mempool mempool(nullptr);

    geocoin::core::Transaction first;
    first.type = geocoin::core::TransactionType::DELEGATION_PROOF;
    first.primary_multisig_address = geocoin::Address("GEOnonce-primary");
    first.delegated_multisig_address = geocoin::Address("GEOnonce-delegate-a");
    first.nonce = 7;
    first.timestamp = 1;

    // Same owner and nonce, different content: a different proof hash, so
    // the proof-key index alone does not catch it.
    auto same_nonce = first;
    same_nonce.delegated_multisig_address = geocoin::Address("GEOnonce-delegate-b");
    auto other_nonce = first;
    other_nonce.nonce = 8;
    other_nonce.timestamp = 2;
    auto other_owner = first;
    other_owner.primary_multisig_address = geocoin::Address("GEOnonce-other-primary");

    Access::seed_entry(mempool, first, 200, 50);
    Access::seed_delegation_nonce(mempool, first);
    expect(Access::has_conflicts(mempool, same_nonce),
           "a second pending proof with the same owner nonce conflicts");
    expect(!Access::has_conflicts(mempool, other_nonce),
           "a different nonce for the same owner does not conflict");
    expect(!Access::has_conflicts(mempool, other_owner),
           "the same nonce for another owner does not conflict");

    // Regression: two such proofs used to enter every block template, and
    // block application rejected the second, so every mined block was invalid.
    Access::seed_entry(mempool, same_nonce, 200, 40);
    Access::seed_entry(mempool, other_nonce, 200, 30);
    const auto selected = mempool.get_transactions_for_mining(1'000'000);
    size_t nonce_seven = 0;
    bool has_other_nonce = false;
    for (const auto& tx : selected) {
        if (tx.type != geocoin::core::TransactionType::DELEGATION_PROOF) continue;
        if (tx.nonce == 7) ++nonce_seven;
        if (tx.nonce == 8) has_other_nonce = true;
    }
    expect(nonce_seven == 1, "the template selects at most one proof per owner nonce");
    expect(has_other_nonce, "the template still selects proofs with other nonces");

    expect(mempool.remove_transaction(first.get_hash()), "pending proof is removed");
    expect(!Access::has_delegation_nonce(mempool, first),
           "removing a proof releases its pending nonce");
    expect(!Access::has_conflicts(mempool, same_nonce),
           "a released nonce no longer conflicts");
}

void test_mempool_policy_reasons()
{
    auto state = make_chain_state();
    geocoin::wallet::Wallet wallet;
    geocoin::wallet::Wallet recipient;
    expect(wallet.initialize(make_key(51)), "policy wallet initializes");
    expect(recipient.initialize(make_key(61)), "policy recipient initializes");

    fund_wallet(state, wallet, 1, 80 * geocoin::COIN);

    geocoin::network::MempoolPolicyConfig config;
    std::string reason;

    geocoin::core::Transaction missing_utxo;
    missing_utxo.type = geocoin::core::TransactionType::STANDARD;
    geocoin::core::TxInput missing_input;
    missing_input.prev_tx_hash = hash_from_seed(0xaa);
    missing_input.vout = 3;
    missing_utxo.inputs.push_back(missing_input);
    expect(!geocoin::network::MempoolPolicy::check(
               missing_utxo,
               *state,
               geocoin::consensus::params::MIN_TX_FEE,
               config,
               &reason),
           "policy rejects missing UTXO");
    expect(reason == "utxo-not-found", "missing UTXO reason");

    mature_chain(state, 1);
    const geocoin::Amount valid_fee =
        relay_fee_for_payment(wallet, 3 * geocoin::COIN, state);
    const auto valid_tx = create_payment(
        wallet,
        recipient.get_address(),
        state,
        3 * geocoin::COIN,
        valid_fee
    );

    reason.clear();
    expect(!geocoin::network::MempoolPolicy::check(valid_tx, *state, 0, config, &reason),
           "policy rejects low fee");
    expect(reason == "fee-too-low", "low fee reason");

    reason.clear();
    expect(!geocoin::network::MempoolPolicy::check(
               valid_tx,
               *state,
               geocoin::consensus::params::MIN_TX_FEE,
               config,
               &reason),
           "policy rejects a large standard payment at the flat minimum");
    expect(reason == "fee-rate-too-low", "size-based relay fee reason");

    const uint64_t generous_policy_fee = static_cast<uint64_t>(
        std::max<geocoin::Amount>(
            valid_fee,
            geocoin::consensus::params::MIN_TX_FEE * 100
        )
    );

    config.max_new_witness_pubkeys_per_tx = 0;
    reason.clear();
    expect(!geocoin::network::MempoolPolicy::check(
               valid_tx,
               *state,
               generous_policy_fee,
               config,
               &reason),
           "policy rejects too many new witness pubkeys");
    expect(reason == "too-many-new-witness-pubkeys", "new witness key limit reason");

    config.max_new_witness_pubkeys_per_tx = 128;

    // Regression: Falcon also verifies a compressed signature zero-padded to
    // its fixed size. A relayer could re-encode any witness signature and
    // change the txid of a transaction it did not sign.
    {
        expect(valid_tx.verify(), "unpadded payment verifies");
        reason.clear();
        expect(geocoin::network::MempoolPolicy::check(
                   valid_tx, *state, generous_policy_fee, config, &reason),
               "minimal signature encoding passes policy");
        auto padded = valid_tx;
        expect(!padded.witnesses.empty() && padded.witnesses[0].signature.size() < 666,
               "payment carries a compressed witness signature");
        padded.witnesses[0].signature.resize(666, 0);
        padded.inputs[0].signature_hash = geocoin::crypto::sha256(padded.witnesses[0].signature);
        expect(padded.verify(), "zero-padded signature still verifies cryptographically");
        expect(padded.get_hash() != valid_tx.get_hash(), "padding changes the txid");
        reason.clear();
        expect(!geocoin::network::MempoolPolicy::check(
                   padded, *state, generous_policy_fee, config, &reason),
               "policy rejects a zero-padded signature");
        expect(reason == "non-canonical-signature", "non-canonical signature reason");
    }

    geocoin::core::Transaction bad_pubkey_size = valid_tx;
    expect(!bad_pubkey_size.witnesses.empty(), "valid tx has witness");
    bad_pubkey_size.witnesses[0].public_key.pop_back();
    reason.clear();
    expect(!geocoin::network::MempoolPolicy::check(
               bad_pubkey_size,
               *state,
               generous_policy_fee,
               config,
               &reason),
           "policy rejects invalid witness public key size");
    expect(reason == "witness-pubkey-size-invalid", "witness size reason");

    auto bad_pubkey_hash = valid_tx;
    bad_pubkey_hash.witnesses[0].public_key[0] ^= 0x01;
    reason.clear();
    expect(!geocoin::network::MempoolPolicy::check(
               bad_pubkey_hash,
               *state,
               generous_policy_fee,
               config,
               &reason),
           "policy rejects witness public key hash mismatch");
    expect(reason == "witness-pubkey-hash-mismatch", "witness hash mismatch reason");

    auto duplicate_witness_key = valid_tx;
    duplicate_witness_key.witnesses.push_back(duplicate_witness_key.witnesses[0]);
    config.max_new_witness_pubkeys_per_tx = 1;
    reason.clear();
    expect(geocoin::network::MempoolPolicy::check(
               duplicate_witness_key,
               *state,
               generous_policy_fee,
               config,
               &reason),
           "policy counts duplicate witness pubkey once");

    config.max_new_witness_pubkeys_per_tx = 128;
    auto immature_state = make_chain_state();
    geocoin::wallet::Wallet immature_wallet;
    geocoin::wallet::Wallet immature_recipient;
    expect(immature_wallet.initialize(make_key(71)), "immature wallet initializes");
    expect(immature_recipient.initialize(make_key(72)), "immature recipient initializes");
    fund_wallet(immature_state, immature_wallet, 1, 80 * geocoin::COIN);
    immature_state->set_height(1);
    const auto immature_tx = create_manual_payment_from_first_utxo(
        immature_wallet,
        immature_recipient.get_address(),
        immature_state,
        1 * geocoin::COIN,
        geocoin::consensus::params::MIN_TX_FEE
    );
    reason.clear();
    expect(!geocoin::network::MempoolPolicy::check(
               immature_tx,
               *immature_state,
               geocoin::consensus::params::MIN_TX_FEE,
               config,
               &reason),
           "policy rejects immature coinbase spend");
    expect(reason == "coinbase-not-mature", "coinbase maturity reason");

    immature_state->set_height(0);
    reason.clear();
    expect(!geocoin::network::MempoolPolicy::check(
               immature_tx,
               *immature_state,
               geocoin::consensus::params::MIN_TX_FEE,
               config,
               &reason),
           "policy rejects a UTXO whose creation height is in the future");
    expect(reason == "utxo-from-future", "future UTXO reason");

    geocoin::core::Transaction invalid_output_address;
    invalid_output_address.version = 2;
    invalid_output_address.type = geocoin::core::TransactionType::STANDARD;
    invalid_output_address.outputs.push_back({
        geocoin::consensus::params::DUST_LIMIT,
        geocoin::Address("GEO-invalid-but-checksummed-looking"),
        {}
    });
    reason.clear();
    expect(!geocoin::network::MempoolPolicy::check(
               invalid_output_address,
               *state,
               geocoin::consensus::params::MIN_TX_FEE,
               config,
               &reason),
           "policy rejects a value-bearing non-canonical output address");
    expect(reason == "invalid-output-address", "invalid output address reason");

    geocoin::core::Transaction exempt;
    exempt.type = geocoin::core::TransactionType::ISSUE_SIDECHAIN;
    reason.clear();
    expect(geocoin::network::MempoolPolicy::check(exempt, *state, 0, config, &reason),
           "policy exempts sidechain control transactions");

    geocoin::core::Transaction decommission;
    decommission.type = geocoin::core::TransactionType::DECOMMISSION_SIDECHAIN;
    reason.clear();
    expect(geocoin::network::MempoolPolicy::check(decommission, *state, 0, config, &reason),
           "policy exempts zero-fee sidechain decommission transactions");
}

} // namespace

int main()
{
    try {
        test_mempool_basic_lifecycle();
        test_mempool_reject_reason_accounting();
        test_remove_block_transactions_conflicts_and_descendants();
        test_remove_block_transactions_requires_active_tip();
        test_mempool_internal_bookkeeping_guards();
        test_mempool_eviction_and_reject_stats();
        test_mempool_size_limit_rejects_oversized_single_tx();
        test_mempool_fee_conflict_and_accounting_helpers();
        test_mempool_multisig_package_bookkeeping();
        test_angle_commitment_flood_cannot_lock_out_multisig();
        test_angle_commitment_package_survives_isolated_flood();
        test_delegation_proof_nonce_is_unique_in_mempool_and_template();
        test_mempool_policy_reasons();
        std::cout << "test_mempool_units: ok\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "test_mempool_units failed: " << e.what() << "\n";
        return 1;
    }
}
