#include "chain_test_fixture.hpp"
#include "geocoin/wallet/wallet.hpp"
#include "geocoin/utils/bytes.hpp"
#include "geocoin/multisig/multisig.hpp"
#include "geocoin/storage/block_records.hpp"
#include "geocoin/state/records.hpp"
#include <fstream>
#include <iostream>

namespace geocoin::state {
// Exercise persistence/rollback independently of transaction signature fixtures.
// The existing test-access switch is enabled only on this test executable.
struct ChainStateTestAccess {
    static bool store_delegation(ChainState& state, const core::Transaction& tx) {
        return state.apply_delegation_proof_(tx, 0);
    }
    static bool nonce_used(const ChainState& state, const Address& owner, uint32_t nonce) {
        const auto it = state.used_delegation_nonces_.find(owner.str());
        return it != state.used_delegation_nonces_.end() && it->second.count(nonce);
    }
    static bool spend(ChainState& state, const core::Transaction& tx, Amount input_total) {
        return state.apply_multisig_spend_effects_(tx, input_total);
    }
    static bool unspend(ChainState& state, const core::Transaction& tx, const TxUndo& undo) {
        return state.revert_multisig_hierarchical_spent_(tx, undo);
    }
    static bool valid(const ChainState& state) { return state.derived_state_storage_valid_; }
    static bool validate_spend(const ChainState& state, const core::Transaction& tx) { return state.validate_multisig_spend_(tx); }
    static void add_utxo(ChainState& state, const UTXO& utxo) { state.add_utxo(utxo); }
    static void deposit(ChainState& state, const Hash256& id, const SidechainDeposit& entry) {
        state.persist_sidechain_deposit_to_storage(id, entry);
    }
    static void registry(ChainState& state, const Hash256& id, const SidechainRegistryEntry& entry) {
        state.persist_sidechain_registry_to_storage(id, entry);
    }
    static void program(ChainState& state, const Hash256& id, const CardProgramEntry& entry) {
        state.persist_card_program_to_storage(id, entry);
    }
    static void index_history(ChainState& state, const core::Transaction& tx, Height height) {
        state.index_query_refs_for_tx_(tx, height, {}, tx.get_hash());
    }
    static void erase_history(ChainState& state, const Hash256& txid) { state.erase_query_refs_for_tx_(txid); }
};
}

namespace {
using namespace geocoin;
using test::expect;
using Access = state::ChainStateTestAccess;
struct Scratch {
    std::filesystem::path path = test::make_temp_dir("geocoin-schema");
    ~Scratch() { std::error_code ec; std::filesystem::remove_all(path, ec); }
};

std::unique_ptr<state::Storage> backend(int mode, const std::filesystem::path& root) {
    if (mode == 0) return std::make_unique<state::MemoryStorage>();
    auto db = mode == 1 ? std::make_unique<state::RocksDBStorage>((root / "state").string())
                        : std::make_unique<state::RocksDBStorage>((root / "state").string(), (root / "addresses").string());
    expect(db->open(), "open test RocksDB");
    return db;
}

std::unique_ptr<state::ChainState> reload(std::unique_ptr<state::ChainState> state,
                                        int mode, const std::filesystem::path& root) {
    std::unique_ptr<state::Storage> next;
    if (mode == 0) next = std::make_unique<state::MemoryStorage>(
        *dynamic_cast<state::MemoryStorage*>(state->get_storage()));
    state.reset();
    if (mode != 0) next = backend(mode, root);
    return std::make_unique<state::ChainState>(std::move(next));
}

void delegation(const std::string& mode_name) {
    for (int mode = 0; mode < 3; ++mode) {
        Scratch scratch;
        auto state = std::make_unique<state::ChainState>(backend(mode, scratch.path));
        core::Transaction proof;
        proof.type = core::TransactionType::DELEGATION_PROOF;
        proof.primary_multisig_address = test::address_from_seed(71);
        proof.delegated_multisig_address = test::address_from_seed(72);
        proof.nonce = 17;
        expect(Access::store_delegation(*state, proof), "apply delegation persistence fixture");
        core::Transaction spend;
        spend.type = core::TransactionType::MULTISIG_SPEND;
        spend.delegation_proof_txid = proof.get_hash();
        spend.outputs = {{3 * COIN, test::address_from_seed(73)}, {COIN, proof.primary_multisig_address}};
        state::UTXO input;
        input.tx_hash = test::hash_from_seed(74);
        input.address = proof.primary_multisig_address;
        input.value = 4 * COIN + 500;
        spend.inputs = {{input.tx_hash, input.vout}};
        state::TxUndo undo;
        undo.spent_utxos = {input};
        expect(Access::spend(*state, spend, input.value), "apply delegated expense projection");
        expect(state->get_delegation_total_spent(proof.get_hash()) == 3 * COIN + 500, "expense includes fee before restart");
        state = reload(std::move(state), mode, scratch.path);
        if (mode_name == "delegation-nonce") {
            expect(Access::nonce_used(*state, proof.primary_multisig_address, 17), "delegation nonce survives restart");
        } else {
            expect(state->get_delegation_total_spent(proof.get_hash()) == 3 * COIN + 500, "delegated expense survives restart");
            expect(Access::unspend(*state, spend, undo), "rollback expense after restart");
            state = reload(std::move(state), mode, scratch.path);
            expect(state->get_delegation_total_spent(proof.get_hash()) == 0, "rolled-back expense survives second restart");
        }
    }
}

void truncated_record(const std::string& kind) {
    for (int mode = 0; mode < 3; ++mode) {
        Scratch scratch;
        auto state = std::make_unique<state::ChainState>(backend(mode, scratch.path));
        const auto id = test::hash_from_seed(41);
        const auto owner = test::address_from_seed(43);
        std::string key;
        size_t tail = 0;
        if (kind == "deposit-tail") {
            state::SidechainDeposit entry;
            entry.owner = owner; entry.amount = COIN; entry.unlock_height = 100;
            entry.last_checkpoint_height = 7; entry.last_sidechain_checkpoint_height = 19;
            Access::deposit(*state, id, entry);
            key = "scd:"; tail = 16;
        } else if (kind == "registry-tail") {
            state::SidechainRegistryEntry entry;
            entry.owner = owner; entry.issuance_limit = 10 * COIN;
            entry.committee_m = entry.committee_n = 1; entry.committee_members = {owner};
            entry.committee_id = test::hash_from_seed(45);
            Access::registry(*state, id, entry);
            key = "sc:reg:"; tail = 48;
        } else {
            state::CardProgramEntry entry;
            entry.issuer = owner; entry.chip_vendor = "vendor"; entry.chip_os = "os";
            entry.applet_aid = "A000000001"; entry.applet_version = 1;
            entry.corporate_policy.version = 1;
            core::CardAuthority authority; authority.quorum = 1; authority.members = {owner};
            entry.corporate_policy.mint = entry.corporate_policy.freeze =
                entry.corporate_policy.operations = entry.corporate_policy.recovery = authority;
            Access::program(*state, id, entry);
            key = "card:prog:"; tail = 1 + 4 * (1 + 4 + 4 + owner.str().size()) + 32 + 4;
        }
        key += utils::hash256_to_hex(id);
        bytes raw;
        expect(state->get_storage()->get_metadata(key, raw) && raw.size() > tail, "read complete domain record");
        raw.resize(raw.size() - tail);
        expect(state->get_storage()->put_metadata(key, raw), "store truncated domain record");
        state = reload(std::move(state), mode, scratch.path);
        expect(!Access::valid(*state), "missing domain fields invalidate snapshot instead of receiving defaults");
        bytes preserved;
        expect(state->get_storage()->get_metadata(key, preserved) && preserved == raw, "invalid record preserved for diagnosis");
    }
}

void wallet_fields(const std::string& kind) {
    Scratch scratch;
    wallet::Wallet wallet;
    expect(wallet.initialize(test::make_key(41)), "initialize wallet");
    auto selected = wallet.get_address();
    if (kind == "wallet-primary") {
        // Two default-labelled keys expose the difference between the selected
        // primary address and a map-order fallback during reload.
        for (uint8_t seed = 42; seed < 60; ++seed) {
            expect(wallet.initialize(test::make_key(seed)), "add selected primary key");
            selected = wallet.get_address();
            if (selected != wallet.get_all_addresses().front()) break;
        }
        expect(selected != wallet.get_all_addresses().front(), "primary differs from lexical fallback");
    }
    auto chain_state = std::make_shared<state::ChainState>();
    const auto funding = core::Transaction::create_coinbase(1, selected, 10 * COIN);
    expect(chain_state->apply_transaction(funding, 1, true) && chain_state->set_height(1000), "fund wallet fixture");
    core::Transaction payment;
    expect(wallet.create_transaction(test::address_from_seed(73), COIN,
        consensus::params::MIN_TX_FEE, chain_state, payment), "creating a payment marks its source address used");
    for (const auto& password : {std::string{}, std::string{"schema-test-password"}}) {
        const auto file = scratch.path / (password.empty() ? "plain.dat" : "encrypted.dat");
        expect(wallet.save_to_file(file.string(), password), "save wallet fields");
        wallet::Wallet loaded;
        expect(loaded.load_from_file(file.string(), password), "reload wallet fields");
        if (kind == "wallet-primary") expect(loaded.get_address() == selected, "selected primary survives restart");
        else {
            wallet::KeyInfo info;
            expect(loaded.get_key_info(selected, info) && info.used, "used-address flag survives restart");
        }
    }
}

void wallet_integrity() {
    Scratch scratch;
    wallet::Wallet wallet;
    expect(wallet.initialize(test::make_key(41)), "initialize duplicate-key fixture");
    const auto file = scratch.path / "wallet.dat";
    expect(wallet.save_to_file(file.string(), ""), "save original wallet");
    auto read = [&] { std::ifstream in(file, std::ios::binary); return bytes(std::istreambuf_iterator<char>(in), {}); };
    const auto original = read();
    // Current plaintext payload starts with password-mode and key count.
    constexpr size_t header = 13, count_offset = header + 1, key_offset = count_offset + 4;
    const auto label_offset = key_offset + 32;
    const auto u32 = [&](size_t offset) { uint32_t n = 0; for (size_t i = 0; i < 4; ++i) n |= uint32_t(original.at(offset + i)) << (8 * i); return n; };
    const auto key_size = 32 + 4 + u32(label_offset) + 8 + 1;
    bytes duplicate = original;
    duplicate.insert(duplicate.begin() + key_offset, original.begin() + key_offset, original.begin() + key_offset + key_size);
    duplicate[count_offset] = 2;
    const auto size = duplicate.size() - header;
    for (size_t i = 0; i < 4; ++i) duplicate[9 + i] = byte(size >> (8 * i));
    { std::ofstream out(file, std::ios::binary | std::ios::trunc); out.write(reinterpret_cast<const char*>(duplicate.data()), duplicate.size()); }
    wallet::Wallet loaded;
    expect(!loaded.load_from_file(file.string(), ""), "duplicate persisted key must not overwrite the first entry");
    expect(read() == duplicate, "failed wallet read preserves source");
    { std::ofstream out(file, std::ios::binary | std::ios::trunc); out.write(reinterpret_cast<const char*>(original.data()), original.size()); }
    wallet::WalletTransaction inconsistent;
    inconsistent.tx = core::Transaction::create_coinbase(1, wallet.get_address(), COIN);
    inconsistent.tx_hash = test::hash_from_seed(99);
    inconsistent.is_coinbase = true;
    wallet.add_transaction(inconsistent);
    expect(!wallet.save_to_file(file.string(), ""), "inconsistent history cannot replace a valid wallet file");
    expect(read() == original, "rejected wallet write preserves committed file");
}

void delegation_integrity() {
    for (int mode = 0; mode < 3; ++mode) for (const auto damage : {"state", "index", "nonce"}) {
        Scratch scratch;
        auto chain = std::make_unique<state::ChainState>(backend(mode, scratch.path));
        core::Transaction proof;
        proof.type = core::TransactionType::DELEGATION_PROOF;
        proof.primary_multisig_address = test::address_from_seed(71);
        proof.delegated_multisig_address = test::address_from_seed(72);
        proof.nonce = 17;
        expect(Access::store_delegation(*chain, proof), "apply delegation integrity fixture");
        auto* db = chain->get_storage();
        const auto key = "delegation:" + utils::hash256_to_hex(proof.get_hash());
        const auto index = db->get_keys_with_prefix("idx:").front();
        if (std::string_view(damage) == "state") expect(db->delete_metadata(key), "remove referenced delegation state");
        else if (std::string_view(damage) == "index") expect(db->delete_metadata(index), "remove delegation reverse index");
        else {
            auto second = proof; second.nonce = 18;
            expect(Access::store_delegation(*chain, second), "apply second delegation");
            const auto second_key = "delegation:" + utils::hash256_to_hex(second.get_hash());
            bytes raw; state::DelegationState entry;
            expect(db->get_metadata(second_key, raw) && state::records::deserialize_delegation(raw, entry), "read second delegation state");
            entry.nonce = proof.nonce;
            expect(db->put_metadata(second_key, state::records::serialize_delegation(entry)), "duplicate persisted owner nonce");
        }
        chain = reload(std::move(chain), mode, scratch.path);
        expect(!Access::valid(*chain), "incomplete or ambiguous delegation state invalidates snapshot");
    }
}

void chainwork_layout() {
    uint256 work;
    work.data[0] = 0x0807060504030201ULL; work.data[1] = 0x1817161514131211ULL;
    work.data[2] = 0x2827262524232221ULL; work.data[3] = 0x3837363534333231ULL;
    const auto raw = storage::encode_chain_work_record(work);
    expect(utils::bytes_to_hex(raw) == "0102030405060708111213141516171821222324252627283132333435363738", "chainwork uses explicit little-endian words");
    uint256 decoded;
    expect(storage::decode_chain_work_record(std::string(raw.begin(), raw.end()), decoded) && decoded == work, "all chainwork words survive serialization");
    const auto anchor = storage::encode_pruned_index_anchor_record(19, test::hash_from_seed(17), work);
    expect(std::equal(raw.begin(), raw.end(), anchor.begin() + 44), "pruning anchor shares chainwork encoding");
}

void angle_reload() {
    for (int mode = 0; mode < 3; ++mode) {
        Scratch scratch;
        auto state = std::make_unique<state::ChainState>(backend(mode, scratch.path));
        const auto signer = test::address_from_seed(71);
        const auto owner = Address(multisig::create_multisig_address(1, 1, {signer}).address);
        state::UTXO utxo;
        utxo.tx_hash = test::hash_from_seed(76); utxo.value = 5 * COIN; utxo.address = owner;
        Access::add_utxo(*state, utxo);
        core::TxInput input; input.prev_tx_hash = utxo.tx_hash; input.vout = 0;
        auto spend = core::Transaction::create_multisig_spend({input}, {{COIN, test::address_from_seed(72)}}, {}, 1, 1);
        spend.spend_participants = {signer};
        const auto commitment = core::Transaction::create_angle_commitment(owner, spend.get_hash_without_commitments(),
            test::make_key(71), signer, consensus::Params().network_id());
        expect(state->apply_transaction(commitment, 0, false), "apply signed angle commitment");
        spend.angle_commitment_hashes = {commitment.get_hash()};
        expect(Access::validate_spend(*state, spend), "multisig reference resolves before restart");
        state = reload(std::move(state), mode, scratch.path);
        expect(Access::validate_spend(*state, spend), "multisig reference resolves after restart");
    }
}

void cadastral_history() {
    // Verify the persisted query contract independently of the signed document
    // admission tests: public history reads must survive reopen and rollback.
    for (int mode = 0; mode < 3; ++mode) {
        Scratch scratch;
        auto state = std::make_unique<state::ChainState>(backend(mode, scratch.path));
        core::Transaction document;
        document.type = core::TransactionType::CADASTRAL_DOC;
        document.cadastral_doc_hash = test::hash_from_seed(91);
        document.cadastral_nft_id = test::hash_from_seed(92);
        document.cadastral_owner = test::address_from_seed(93);
        document.outputs = {{COIN, document.cadastral_owner, {}}};
        auto other = document; other.cadastral_nft_id = test::hash_from_seed(94);
        Access::index_history(*state, document, 7); Access::index_history(*state, other, 7);
        expect(state->set_height(7), "persist cadastral history position");
        state = reload(std::move(state), mode, scratch.path);
        const auto history = state->get_nft_txids(document.cadastral_nft_id);
        expect(history.size() == 1 && history.front().txid == document.get_hash() && history.front().height == 7,
               "parcel history retains its cadastral documents after restart");
        Access::erase_history(*state, document.get_hash());
        state = reload(std::move(state), mode, scratch.path);
        expect(state->get_nft_txids(document.cadastral_nft_id).empty() &&
               state->get_nft_txids(other.cadastral_nft_id).size() == 1,
               "rollback removes only its document references from packed indexes");
        expect(!state->get_storage()->has_metadata("qrefs:" + utils::hash256_to_hex(document.get_hash())),
               "rollback removes the document reverse list");
    }
}
}

int main(int argc, char** argv) {
    consensus::SelectParams(consensus::Network::LOADNET);
    try {
        const std::vector<std::string> modes = {"delegation-nonce", "delegation-spent", "deposit-tail",
            "registry-tail", "card-tail", "wallet-used", "wallet-primary", "angle-reload",
            "delegation-integrity", "wallet-integrity", "chainwork-layout", "cadastral-history"};
        const std::string selected = argc > 1 ? argv[1] : "";
        expect(selected.empty() || std::find(modes.begin(), modes.end(), selected) != modes.end(), "known test mode");
        for (const auto& mode : modes) {
            if (!selected.empty() && selected != mode) continue;
            if (mode == "delegation-integrity") delegation_integrity();
            else if (mode == "wallet-integrity") wallet_integrity();
            else if (mode == "chainwork-layout") chainwork_layout();
            else if (mode.starts_with("delegation-")) delegation(mode);
            else if (mode.starts_with("wallet-")) wallet_fields(mode);
            else if (mode == "angle-reload") angle_reload();
            else if (mode == "cadastral-history") cadastral_history();
            else truncated_record(mode);
            std::cout << mode << " passed\n";
        }
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
