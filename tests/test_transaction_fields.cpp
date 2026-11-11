#include "test_utils.hpp"
#include "card_registry_fixture.hpp"
#include "geocoin/consensus/chainparams.hpp"
#include "geocoin/consensus/consensus.hpp"
#include "geocoin/core/transaction.hpp"
#include "geocoin/network/mempool.hpp"
#include "geocoin/utils/bytes.hpp"
#include "nlohmann_json.hpp"

#include <fstream>
#include <functional>
#include <iostream>

namespace {
using namespace geocoin;
using core::Transaction;
using Type = core::TransactionType;
nlohmann::json checks = nlohmann::json::array();

void check(bool ok, const std::string& name) {
    checks.push_back({{"name", name}, {"passed", ok}});
}

template<class F> bool throws(F&& action) {
    try { action(); } catch (const std::exception&) { return true; }
    return false;
}

Transaction wire_fixture(Type type) {
    Transaction tx;
    tx.type = type;
    tx.timestamp = 1700000000;
    tx.inputs = {{test::hash_from_seed(12), 3, {0x21}, core::TxInput::SEQUENCE_LOCKTIME}};
    tx.outputs = {{COIN, test::address_from_seed(42), {0x34}}};
    tx.lock_time = 17;
    if (type == Type::ANGLE_COMMITMENT) {
        tx.commitment_network_id = consensus::Params().network_id();
        tx.multisig_address = test::address_from_seed(43);
        tx.signer_address = test::address_from_seed(42);
        tx.spending_tx_hash = test::hash_from_seed(44);
        tx.inputs[0].signature = {1, 2, 3};
    }
    if (type == Type::NFT_CREATE) {
        tx.nft_create.emplace();
        tx.nft_create->metadata.geometry.polygons.front().exterior = {{0LL, 0LL}, {0LL, 100000000LL}, {100000000LL, 0LL}};
    }
    if (type == Type::NFT_TRANSFER) tx.nft_transfer.emplace();
    if (type == Type::NFT_BURN) tx.nft_burn.emplace();
    return tx;
}

void check_wire_fixtures(const char* record_path) {
    nlohmann::json recorded = nlohmann::json::array();
    for (unsigned type = 0; type <= 255; ++type) {
        const auto kind = static_cast<Type>(type);
        if (!core::is_known_transaction_type(kind)) continue;
        auto tx = wire_fixture(kind);
        const auto raw = tx.serialize();
        const auto parsed = Transaction::deserialize_canonical(raw);
        const auto name = "type " + std::to_string(type);
        check(parsed.serialize() == raw && parsed.get_hash() == tx.get_hash(), name + ": canonical roundtrip");
        recorded.push_back({{"type", type}, {"raw", utils::bytes_to_hex(raw)},
                            {"txid", utils::hash256_to_hex(tx.get_hash())}});
    }
    if (record_path) {
        std::ofstream(record_path) << recorded.dump(2) << '\n';
    } else {
        nlohmann::json frozen;
        std::ifstream(std::string(GEOCOIN_TEST_SOURCE_DIR) + "/tests/data/transaction_field_wire.json") >> frozen;
        check(frozen == recorded, "transaction encodings and txids match frozen v1 fixtures");
    }
}

struct Case {
    const char* name;
    Type type;
    std::function<void(Transaction&)> mutate;
};

const std::vector<Case>& inactive_fields() {
    static const std::vector<Case> cases{
        {"standard with card data", Type::STANDARD, [](auto& tx) { tx.card_program_id[0] = 1; }},
        {"standard with commitment data", Type::STANDARD, [](auto& tx) { tx.commitment_network_id = "degree"; }},
        {"standard with delegation data", Type::STANDARD, [](auto& tx) { tx.valid_until = 42; }},
        {"standard with multisig data", Type::STANDARD, [](auto& tx) { tx.m = 1; }},
        {"standard with revoke data", Type::STANDARD, [](auto& tx) { tx.revoked_delegation_proof_txid[0] = 1; }},
        {"standard with cadastral data", Type::STANDARD, [](auto& tx) { tx.cadastral_number = "parcel"; }},
        {"standard with foundation data", Type::STANDARD, [](auto& tx) { tx.foundation_new_m = 1; }},
        {"standard with sidechain data", Type::STANDARD, [](auto& tx) { tx.sidechain_amount = 1; }},
        {"standard with NFT data", Type::STANDARD, [](auto& tx) { tx.nft_burn.emplace(); }},
        {"angle with foreign commitment refs", Type::ANGLE_COMMITMENT, [](auto& tx) { tx.angle_commitment_hashes = {test::hash_from_seed(4)}; }},
        {"hierarchical spend with inline participants", Type::MULTISIG_SPEND, [](auto& tx) {
            tx.delegation_proof_txid[0] = 1; tx.spend_participants = {test::address_from_seed(42)};
        }},
        {"proof with spend threshold", Type::DELEGATION_PROOF, [](auto& tx) { tx.m = 1; }},
        {"revoke with proof participants", Type::REVOKE_DELEGATION, [](auto& tx) { tx.primary_n = 1; }},
        {"card status with unsigned pubkey", Type::CARD_UPDATE_STATUS, [](auto& tx) { tx.card_pubkey_hash[0] = 1; }},
        {"card status with validity", Type::CARD_UPDATE_STATUS, [](auto& tx) { tx.card_valid_to = 100; }},
        {"card status with policy", Type::CARD_UPDATE_STATUS, [](auto& tx) { tx.card_policy_id = 1; }},
        {"card issue with corporate policy", Type::CARD_ISSUE, [](auto& tx) { tx.card_corporate_policy.mint.members = {test::address_from_seed(42)}; }},
        {"card policy with applet", Type::CARD_UPDATE_POLICY, [](auto& tx) { tx.card_applet_aid = "app"; }},
        {"card program with card UID", Type::CARD_PROGRAM_DEFINE, [](auto& tx) { tx.card_uid_hash[0] = 1; }},
        {"mint with owner signature", Type::MINT_SIDECHAIN, [](auto& tx) { tx.sidechain_signature = {1}; }},
        {"checkpoint with committee config", Type::SIDECHAIN_CHECKPOINT, [](auto& tx) { tx.sidechain_committee_m = 1; }},
        {"batch with outer sidechain", Type::BATCH_SIDECHAIN_CHECKPOINT, [](auto& tx) { tx.sidechain_id[0] = 1; }},
        {"batch with outer signature", Type::BATCH_SIDECHAIN_CHECKPOINT, [](auto& tx) { tx.sidechain_committee_signatures = {{1}}; }},
        {"accounting with token", Type::REGISTER_ACCOUNTING_SIDECHAIN, [](auto& tx) { tx.sidechain_token_symbol = "GEO"; }},
        {"financial with namespace", Type::ISSUE_SIDECHAIN, [](auto& tx) { tx.sidechain_namespace = "registry"; }},
        {"deposit with checkpoint version", Type::LOCK_DEPOSIT, [](auto& tx) { tx.sidechain_checkpoint_version = 1; }},
        {"deposit with fraud index", Type::LOCK_DEPOSIT, [](auto& tx) { tx.sidechain_fraud_checkpoint_index_a = 0; }},
        {"deposit with committee signatures", Type::LOCK_DEPOSIT, [](auto& tx) { tx.sidechain_committee_signatures = {{1}}; }},
        {"distribution with sidechain data", Type::INTEREST_DISTRIBUTION, [](auto& tx) { tx.sidechain_amount = 1; }},
        {"NFT transfer with burn payload", Type::NFT_TRANSFER, [](auto& tx) { tx.nft_burn.emplace(); }},
        {"NFT burn with creation payload", Type::NFT_BURN, [](auto& tx) { tx.nft_create.emplace(); }},
        {"foundation with sidechain signature", Type::UPDATE_FOUNDATION_ADDRESS, [](auto& tx) { tx.sidechain_signature = {1}; }}
    };
    return cases;
}

void test_inactive_fields() {
    for (const auto& item : inactive_fields()) {
        auto tx = wire_fixture(item.type);
        item.mutate(tx);
        const std::string name = item.name;
        check(throws([&] { (void)tx.serialize(); }), name + ": serialization rejects discarded data");
        check(throws([&] { (void)tx.get_hash(); }), name + ": txid rejects discarded data");
        consensus::ValidationState validation;
        check(!consensus::check_transaction(tx, validation) &&
              validation.get_reject_reason() == "bad-txns-unused-field", name + ": explicit consensus reject");
        check(!tx.verify(), name + ": verification rejects discarded data");
    }
    auto tx = wire_fixture(Type::STANDARD);
    tx.card_program_id[0] = 1;
    check(throws([&] { tx.sign(test::make_key(42)); }), "signing rejects an unsigned field");
    check(tx.witnesses.empty(), "failed signing leaves witnesses unchanged");
    tx = wire_fixture(Type::MINT_SIDECHAIN);
    tx.sidechain_signature = {1};
    check(throws([&] { (void)core::serialize_protocol_authorization(tx); }),
          "protocol signing rejects an inactive signature before clearing signatures");
    tx = wire_fixture(Type::STANDARD);
    tx.angle_commitment_hashes = {test::hash_from_seed(4)};
    check(throws([&] { (void)tx.get_hash_without_commitments(); }),
          "draft signing rejects inactive references before clearing references");
}

void test_angle_reserved_field() {
    auto tx = wire_fixture(Type::ANGLE_COMMITMENT);
    auto raw = tx.serialize();
    const size_t reserved = tx.serialize_without_witness().size() - 1;
    check(raw[reserved] == 0, "angle has a fixed zero reserved byte");
    raw[reserved] = 1;
    raw.insert(raw.begin() + reserved + 1, 'x');
    check(throws([&] { (void)Transaction::deserialize_canonical(raw); }),
          "angle rejects a nonempty reserved extension");
}

void test_persistence(bool rocksdb) {
    const auto path = test::make_temp_dir("geocoin-field-contract");
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
    } cleanup{path};
    const std::string prefix = rocksdb ? "RocksDB: " : "MemoryStorage: ";
    auto backend = [&]() -> std::unique_ptr<state::Storage> {
        if (!rocksdb) return std::make_unique<state::MemoryStorage>();
        auto db = std::make_unique<state::RocksDBStorage>((path / "state").string());
        test::expect(db->open(), "open isolated field contract database");
        return db;
    };
    auto db = backend();
    test::CardRegistryFixture card;
    test::expect(db->put_metadata("chainstate_height", utils::encode_u64_le(100)), "seed height");
    test::expect(db->put_metadata("chainstate_utxo_count", utils::encode_u64_le(0)), "seed UTXO count");
    test::expect(db->put_metadata("idx_epoch", bytes(32, '1')), "seed index epoch");
    test::expect(db->put_metadata("interest_pool_balance", utils::encode_u64_le(0)), "seed interest pool balance");
    const auto& params = consensus::Params();
    const state::FoundationConfig foundation{params.foundation_address(), params.foundation_m(),
        params.foundation_n(), params.foundation_participants()};
    test::expect(db->put_metadata("foundation_cfg", state::records::serialize_foundation_config(foundation)),
                 "seed current foundation policy");
    for (const auto& [key, value] : card.records()) {
        test::expect(db->put_metadata(key, value), "seed card records");
    }
    auto chain_state = std::make_shared<state::ChainState>(std::move(db));
    auto funding = Transaction::create_coinbase(1, card.program.issuer, 2 * COIN);
    test::expect(chain_state->apply_transaction(funding, 1, true), "seed mature issuer funding");
    auto status = Transaction{};
    status.type = Type::CARD_UPDATE_STATUS;
    status.timestamp = 1700000000;
    status.inputs = {{funding.get_hash(), 0}};
    status.outputs = {{funding.outputs.front().value - 100000, card.program.issuer, {}}};
    status.card_program_id = card.program_id;
    status.card_issuer = card.program.issuer;
    status.card_uid_hash = card.card.uid_hash;
    status.card_status = 1;
    status.card_metadata_hash = test::hash_from_seed(113);
    status.sign(test::make_key(81));
    const auto raw = status.serialize();
    const auto txid = status.get_hash();
    const auto decoded = Transaction::deserialize_canonical(raw);
    check(status.verify() && decoded.verify(), prefix + "card signature survives raw decoding");
    check(chain_state->validate_transaction(status) && chain_state->validate_transaction(decoded),
          prefix + "card object and raw have identical acceptance");

    auto invalid = status;
    invalid.card_pubkey_hash = test::hash_from_seed(114);
    {
        network::Mempool pool(chain_state);
        std::string reason;
        check(!pool.test_accept_transaction(invalid, &reason) && reason == "malformed-serialization",
              prefix + "mempool dry run handles an invalid field without exception");
        check(!pool.add_transaction(invalid), prefix + "mempool admission rejects an invalid field");
        check(pool.add_transaction(status), prefix + "valid signed card operation enters mempool");
        const auto selected = pool.get_transactions_for_mining(1000000);
        check(selected.size() == 1 && selected.front().serialize() == raw,
              prefix + "mining selection keeps the complete signed raw");
    }
    check(!chain_state->validate_transaction(invalid), prefix + "status rejects an inactive card key");
    check(!chain_state->apply_transaction(invalid, 101, false), prefix + "invalid field cannot enter state");
    check(chain_state->has_utxo(funding.get_hash(), 0) && !chain_state->get_transaction(txid),
          prefix + "rejected operation keeps funding and transaction index");
    bytes record;
    const std::string card_key = "card:uid:" + utils::hash256_to_hex(card.card.uid_hash);
    test::expect(chain_state->get_storage()->get_metadata(card_key, record), "read original card record");
    check(record == card.records().at(card_key), prefix + "rejected operation keeps all card fields");

    // A failed direct write must not poison an unrelated pending batch write.
    auto* storage = chain_state->get_storage();
    storage->begin_batch();
    test::expect(storage->put_metadata("field-contract-pending", {7}), "stage unrelated batch write");
    bool rejected = false;
    try { rejected = !storage->put_transaction(txid, invalid); }
    catch (const std::exception&) { rejected = true; }
    check(rejected && !storage->has_transaction(txid), prefix + "storage rejects discarded fields before staging");
    test::expect(storage->commit_batch(), "commit unrelated batch data");
    check(storage->get_metadata("field-contract-pending", record) && record == bytes{7},
          prefix + "rejected transaction preserves pending batch writes");

    check(chain_state->apply_transaction(decoded, 101, false), prefix + "decoded card operation applies");
    test::expect(chain_state->set_height(101), "persist new state height");
    // Standalone apply_transaction is inside the block transition in production;
    // complete the fixture's block-level snapshot metadata before reopening it.
    test::expect(chain_state->set_tip_hash(test::hash_from_seed(115)), "persist fixture tip");
    test::expect(chain_state->get_storage()->put_metadata("chainstate_utxo_count",
        utils::encode_u64_le(chain_state->get_utxo_count())), "persist fixture UTXO count");
    check(chain_state->get_storage()->get_metadata(card_key, record) &&
          record != card.records().at(card_key), prefix + "card transition changes its persisted record");
    const auto saved_card = record;
    std::unique_ptr<state::Storage> reopened;
    if (!rocksdb) reopened = std::make_unique<state::MemoryStorage>(
        *dynamic_cast<state::MemoryStorage*>(chain_state->get_storage()));
    chain_state.reset();
    if (rocksdb) reopened = backend();
    chain_state = std::make_shared<state::ChainState>(std::move(reopened));
    const auto loaded = chain_state->get_transaction(txid);
    check(loaded && loaded->serialize() == raw && loaded->verify(), prefix + "signed card raw survives restart");
    check(chain_state->get_storage()->get_metadata(card_key, record) && record == saved_card,
          prefix + "all resulting card fields survive restart");
    check(!chain_state->has_utxo(funding.get_hash(), 0) && chain_state->has_utxo(txid, 0),
          prefix + "UTXO transition survives restart");
    check(chain_state->has_derived_state_snapshot(), prefix + "reopened snapshot is complete and valid");
    auto resume = status;
    resume.inputs = {{txid, 0}};
    resume.outputs.front().value -= 100000;
    resume.card_status = 0;
    resume.sign(test::make_key(81));
    check(chain_state->validate_transaction(resume), prefix + "reopened state validates the next card operation");
}
}

int main(int argc, char** argv) {
    try {
        consensus::SelectParams(consensus::Network::LOADNET);
        const bool record = argc > 2 && std::string(argv[1]) == "--record";
        check_wire_fixtures(record ? argv[2] : nullptr);
        test_inactive_fields();
        test_angle_reserved_field();
        if (!record) {
            test_persistence(false);
            test_persistence(true);
        }
        size_t passed = 0;
        for (const auto& item : checks) if (item["passed"].get<bool>()) ++passed;
        const nlohmann::json result{{"passed", passed}, {"failed", checks.size() - passed}, {"checks", checks}};
        if (!record && argc > 1) std::ofstream(argv[1]) << result.dump(2) << '\n';
        if (record && argc > 3) std::ofstream(argv[3]) << result.dump(2) << '\n';
        for (const auto& item : checks) if (!item["passed"].get<bool>()) std::cerr << "FAIL: " << item["name"] << '\n';
        std::cout << passed << '/' << checks.size() << " transaction field checks passed\n";
        return passed == checks.size() ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "transaction field test failed: " << error.what() << '\n';
        return 2;
    }
}
