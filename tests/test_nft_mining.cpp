#include "test_utils.hpp"
#include "geocoin/consensus/chainparams.hpp"
#include "geocoin/crypto/signatures.hpp"
#include "geocoin/network/mempool.hpp"
#include "geocoin/nft/geodesy.hpp"
#include "geocoin/utils/bytes.hpp"
#include "nlohmann_json.hpp"

#include <cmath>
#include <fstream>
#include <iostream>

namespace {
using namespace geocoin;
using test::expect;
using core::Transaction;
using core::TransactionType;
nlohmann::json checks = nlohmann::json::array();
bool use_rocksdb = false;

void check(bool result, const std::string& name) {
    checks.push_back({{"name", std::string(use_rocksdb ? "RocksDB: " : "MemoryStorage: ") + name}, {"passed", result}});
}

nft::NFTMetadata metadata(const std::string& name, int32_t offset, const std::string& number = {}) {
    nft::NFTMetadata result{};
    result.name = name;
    result.description = "NFT mining fixture";
    result.image_uri = "ipfs://nft-mining";
    result.content_hash = std::string(64, 'a');
    result.geometry.polygons.front().exterior = {{offset, offset}, {offset, offset + 100000},
                      {offset + 100000, offset + 100000}, {offset + 100000, offset}};
    for (auto& point : result.geometry.polygons.front().exterior) {
        point.lat_ndeg *= 1000; point.lon_ndeg *= 1000;
    }
    result.created_at = 1700000000;
    result.block_height = 1;
    result.block_hash = test::hash_from_seed(81);
    result.creator = test::address_from_seed(81).str();
    result.tags = {"mining", "test"};
    if (!number.empty()) {
        nft::NFTMetadata::CadastralData data{};
        data.cadastral_number = number;
        data.owner_name = "Fixture owner";
        data.land_category = "Residential";
        data.permitted_use = "Testing";
        data.area_sqcm_official = std::llround(nft::coordinates::measure_polygon(result.geometry.polygons.front().exterior)->area_sqm() * 10000.0);
        data.ownership_document_hash = test::hash_from_seed(82);
        data.technical_plan_hash = test::hash_from_seed(83);
        result.cadastral = std::move(data);
    }
    return result;
}

Transaction create(const nft::NFTMetadata& data) {
    Transaction tx;
    tx.type = TransactionType::NFT_CREATE;
    nft::NFTCreationTransaction payload{};
    payload.metadata = data;
    payload.nft_id = data.calculate_hash();
    payload.creator_address = test::address_from_seed(81);
    tx.nft_create = std::move(payload);
    return tx;
}

Transaction transfer(const nft::NFTMetadata& data, uint8_t recipient = 82) {
    Transaction tx;
    tx.type = TransactionType::NFT_TRANSFER;
    nft::NFTTransferTransaction payload{};
    payload.nft_id = data.calculate_hash();
    payload.from = test::address_from_seed(81);
    payload.to = test::address_from_seed(recipient);
    tx.nft_transfer = std::move(payload);
    return tx;
}

Transaction burn(const nft::NFTMetadata& data) {
    Transaction tx;
    tx.type = TransactionType::NFT_BURN;
    nft::NFTBurnTransaction payload{};
    payload.nft_id = data.calculate_hash();
    payload.owner = test::address_from_seed(81);
    tx.nft_burn = std::move(payload);
    return tx;
}

Transaction document(const nft::NFTMetadata& data) {
    Transaction tx;
    tx.type = TransactionType::CADASTRAL_DOC;
    tx.cadastral_doc_hash = test::hash_from_seed(84);
    tx.cadastral_nft_id = data.calculate_hash();
    tx.cadastral_owner = test::address_from_seed(81);
    if (data.cadastral) tx.cadastral_number = data.cadastral->cadastral_number;
    return tx;
}

void sign(Transaction& tx, Amount fee) {
    const auto key = test::make_key(81);
    const auto sign_payload = [&key](auto& payload) {
        payload.signature = crypto::sign_recoverable_message(crypto::sha256(payload.serialize_for_signing()), key);
    };
    if (tx.nft_create) sign_payload(*tx.nft_create);
    if (tx.nft_transfer) { tx.nft_transfer->fee = fee; sign_payload(*tx.nft_transfer); }
    if (tx.nft_burn) { tx.nft_burn->fee = fee; sign_payload(*tx.nft_burn); }
    tx.sign(key);
}

void selection_case(const std::string& name, const std::vector<nft::NFTMetadata>& registered,
                    std::vector<Transaction> candidates, const std::vector<size_t>& expected,
                    std::optional<size_t> size_limit_candidate = std::nullopt) {
    struct Scratch {
        std::filesystem::path path = test::make_temp_dir("geocoin-nft-mining");
        ~Scratch() { std::error_code error; std::filesystem::remove_all(path, error); }
    } scratch;
    const auto backend = [&]() -> std::unique_ptr<state::Storage> {
        if (!use_rocksdb) return std::make_unique<state::MemoryStorage>();
        auto db = std::make_unique<state::RocksDBStorage>((scratch.path / "state").string());
        expect(db->open(), "open isolated RocksDB");
        return db;
    };
    auto storage = backend();
    expect(storage->put_metadata("chainstate_height", utils::encode_u64_le(200)), "seed height");
    expect(storage->put_metadata("chainstate_utxo_count", utils::encode_u64_le(0)), "seed UTXO count");
    expect(storage->put_metadata("idx_epoch", bytes(32, '1')), "seed index epoch");
    std::map<std::string, bytes> records;
    const auto owner = test::address_from_seed(81);
    for (const auto& data : registered) {
        const auto id = utils::hash256_to_hex(data.calculate_hash());
        records["nft:meta:" + id] = data.serialize();
        records["nft:owner:" + id] = bytes(owner.str().begin(), owner.str().end());
        records["idx:" + std::string(32, '1') + ":nft:" + id] = {1};
    }
    for (const auto& [key, value] : records) expect(storage->put_metadata(key, value), "seed NFT registry");
    auto state = std::make_shared<state::ChainState>(std::move(storage));
    state->set_activation_manifest(utils::ActivationManifest::fully_enabled_for_tests("degree"));
    auto pool = std::make_unique<network::Mempool>(state);
    std::vector<Transaction> funding;
    Amount fee = COIN / 2;
    for (size_t i = 0; i < candidates.size(); ++i) {
        funding.push_back(Transaction::create_coinbase(i + 1, owner, 2 * COIN));
        expect(state->apply_transaction(funding.back(), i + 1, true), "seed distinct mature input");
        auto& tx = candidates[i];
        tx.timestamp = 1700000100 + i;
        tx.inputs = {{funding.back().get_hash(), 0}};
        tx.outputs = {{funding.back().outputs.front().value - fee, owner, {}}};
        sign(tx, fee);
        fee /= 5;
        if (!pool->add_transaction(tx)) {
            const auto rejects = pool->get_reject_stats();
            expect(false, name + ": individually valid transaction admitted: " +
                nlohmann::json{{"consensus", rejects.consensus}, {"policy", rejects.policy}}.dump());
        }
    }
    const size_t max_size = size_limit_candidate ? candidates.at(*size_limit_candidate).serialize().size() : 1000000;
    if (size_limit_candidate) expect(candidates.front().serialize().size() > max_size, "preferred candidate exceeds budget");
    const auto selected = pool->get_transactions_for_mining(max_size);
    bool matches = selected.size() == expected.size();
    for (size_t i = 0; matches && i < expected.size(); ++i) {
        matches = selected[i].get_hash() == candidates.at(expected[i]).get_hash();
    }
    check(matches, name + ": select the compatible sequence in fee order");
    const auto repeated = pool->get_transactions_for_mining(max_size);
    bool repeatable = repeated.size() == selected.size() && pool->get_transactions_for_mining(0).empty();
    for (size_t i = 0; repeatable && i < selected.size(); ++i) repeatable = repeated[i].get_hash() == selected[i].get_hash();
    check(repeatable, name + ": repeated and empty templates do not leak effects");
    bool preserved = pool->size() == candidates.size() && state->get_height() == 200 &&
        state->get_storage()->get_keys_with_prefix("nft:").size() == 2 * registered.size();
    for (const auto& [key, value] : records) {
        bytes current;
        preserved = preserved && state->get_storage()->get_metadata(key, current) && current == value;
    }
    for (const auto& tx : funding) preserved = preserved && state->has_utxo(tx.get_hash(), 0);
    check(preserved, name + ": selection preserves confirmed records, inputs and mempool");
    bool applies = true;
    for (const auto& tx : selected) {
        if (!state->apply_transaction(tx, 201, false)) { applies = false; break; }
    }
    check(applies, name + ": selected transactions apply through ChainState");
    if (!applies) return;
    expect(state->set_height(201), "persist applied height");
    std::map<Hash256, std::pair<Address, bytes>> snapshot;
    const auto remember = [&](const Hash256& id) {
        snapshot[id] = {state->get_nft_owner(id), state->nft_exists(id) ? state->get_nft_metadata(id).serialize() : bytes{}};
    };
    for (const auto& data : registered) remember(data.calculate_hash());
    for (const auto& tx : selected) if (tx.nft_create) remember(tx.nft_create->nft_id);
    std::unique_ptr<state::Storage> reopened;
    if (!use_rocksdb) reopened = std::make_unique<state::MemoryStorage>(
        *dynamic_cast<state::MemoryStorage*>(state->get_storage()));
    pool.reset();
    state.reset();
    if (use_rocksdb) reopened = backend();
    state = std::make_shared<state::ChainState>(std::move(reopened));
    state->set_activation_manifest(utils::ActivationManifest::fully_enabled_for_tests("degree"));
    bool restored = state->get_height() == 201;
    for (const auto& [id, entry] : snapshot) {
        restored = restored && state->get_nft_owner(id) == entry.first &&
            state->nft_exists(id) == !entry.second.empty() && state->has_nft_id_key(id);
        if (!entry.second.empty()) restored = restored && state->get_nft_metadata(id).serialize() == entry.second;
    }
    check(restored, name + ": owners, metadata, burns and reserved IDs survive restart");
    bool checked_cadastral = false;
    bool restored_cadastral = true;
    Transaction probe_funding;
    for (const auto& [id, entry] : snapshot) {
        (void)id;
        if (entry.second.empty()) continue;
        auto data = nft::NFTMetadata::deserialize(entry.second);
        if (!data.cadastral) continue;
        if (!checked_cadastral) {
            probe_funding = Transaction::create_coinbase(50, owner, 2 * COIN);
            expect(state->apply_transaction(probe_funding, 50, true), "seed restart validation input");
            checked_cadastral = true;
        }
        data.name += "-restart-probe";
        auto blocked = create(data);
        auto free = create(metadata("Free restart probe", 2000000, "restart-free"));
        for (auto* probe : {&blocked, &free}) {
            probe->timestamp = 1700000200;
            probe->inputs = {{probe_funding.get_hash(), 0}};
            probe->outputs = {{probe_funding.outputs.front().value - consensus::params::MIN_TX_FEE, owner, {}}};
            sign(*probe, consensus::params::MIN_TX_FEE);
        }
        restored_cadastral = restored_cadastral && !state->validate_transaction(blocked) && state->validate_transaction(free);
    }
    if (checked_cadastral) check(restored_cadastral, name + ": restart restores cadastral reservations without blocking free land");
}

void test_replayed_nft_authorization() {
    struct Scratch {
        std::filesystem::path path = test::make_temp_dir("geocoin-nft-replay");
        ~Scratch() { std::error_code error; std::filesystem::remove_all(path, error); }
    } scratch;
    const auto backend = [&]() -> std::unique_ptr<state::Storage> {
        if (!use_rocksdb) return std::make_unique<state::MemoryStorage>();
        auto db = std::make_unique<state::RocksDBStorage>((scratch.path / "state").string());
        expect(db->open(), "open replay RocksDB");
        return db;
    };
    auto storage = backend();
    const auto alice = test::address_from_seed(81);
    const auto mallory = test::address_from_seed(82);
    const auto bob = test::address_from_seed(83);
    const auto data = metadata("Replay", 300000);
    const auto nft_id = data.calculate_hash();
    const auto id_hex = utils::hash256_to_hex(nft_id);
    expect(storage->put_metadata("chainstate_height", utils::encode_u64_le(200)), "seed replay height");
    expect(storage->put_metadata("chainstate_utxo_count", utils::encode_u64_le(0)), "seed replay UTXO count");
    expect(storage->put_metadata("idx_epoch", bytes(32, '1')), "seed replay index epoch");
    expect(storage->put_metadata("nft:meta:" + id_hex, data.serialize()), "seed replay metadata");
    expect(storage->put_metadata("nft:owner:" + id_hex, bytes(alice.str().begin(), alice.str().end())),
           "seed replay owner");
    expect(storage->put_metadata("idx:" + std::string(32, '1') + ":nft:" + id_hex, {1}), "seed replay nft index");

    auto state = std::make_shared<state::ChainState>(std::move(storage));
    state->set_activation_manifest(utils::ActivationManifest::fully_enabled_for_tests("degree"));
    const Amount fee = COIN / 2;
    const auto fund = [&](uint8_t seed, Height height) {
        auto coinbase = Transaction::create_coinbase(height, test::address_from_seed(seed), 2 * COIN);
        expect(state->apply_transaction(coinbase, height, true), "seed replay funding");
        return coinbase;
    };
    const auto alice_funding = fund(81, 1);
    const auto mallory_funding = fund(82, 2);
    const auto bob_funding = fund(83, 3);

    const auto sign_owner_payload = [](Transaction& tx, uint8_t owner_seed) {
        const auto key = test::make_key(owner_seed);
        const auto signature = crypto::sign_recoverable_message(
            crypto::sha256(tx.nft_transfer
                ? tx.nft_transfer->serialize_for_signing()
                : tx.nft_burn->serialize_for_signing()),
            key
        );
        if (tx.nft_transfer) tx.nft_transfer->signature = signature;
        if (tx.nft_burn) tx.nft_burn->signature = signature;
        tx.sign(key);
    };
    const auto funded = [&](Transaction tx, const Transaction& funding, uint8_t witness_seed, Timestamp timestamp) {
        tx.timestamp = timestamp;
        tx.inputs = {{funding.get_hash(), 0}};
        tx.outputs = {{funding.outputs.front().value - fee, test::address_from_seed(witness_seed), {}}};
        if (tx.nft_transfer) tx.nft_transfer->fee = fee;
        if (tx.nft_burn) tx.nft_burn->fee = fee;
        return tx;
    };

    auto authorized = funded(transfer(data, 83), alice_funding, 81, 1700001000);
    sign_owner_payload(authorized, 81);
    auto stolen = funded(transfer(data, 83), mallory_funding, 82, 1700001001);
    stolen.nft_transfer->signature = authorized.nft_transfer->signature;
    stolen.sign(test::make_key(82));
    check(!state->validate_transaction(stolen) && !state->apply_transaction(stolen, 201, false) &&
              state->get_nft_owner(nft_id) == alice && state->has_utxo(mallory_funding.get_hash(), 0),
          "copied transfer signature cannot spend another wallet's input");
    check(state->validate_transaction(authorized) && state->apply_transaction(authorized, 201, false) &&
              state->get_nft_owner(nft_id) == bob,
          "owner-funded transfer still applies");

    auto returned = funded(transfer(data, 81), bob_funding, 83, 1700001002);
    returned.nft_transfer->from = bob;
    returned.nft_transfer->to = alice;
    sign_owner_payload(returned, 83);
    check(state->validate_transaction(returned) && state->apply_transaction(returned, 201, false) &&
              state->get_nft_owner(nft_id) == alice,
          "recipient can transfer the NFT back to the original owner");

    auto replay = funded(transfer(data, 83), mallory_funding, 82, 1700001003);
    replay.nft_transfer->signature = authorized.nft_transfer->signature;
    replay.sign(test::make_key(82));
    check(!state->validate_transaction(replay) && !state->apply_transaction(replay, 201, false) &&
              state->get_nft_owner(nft_id) == alice && state->has_utxo(mallory_funding.get_hash(), 0),
          "historical transfer signature cannot be replayed after the NFT returns");

    auto burn_tx = funded(burn(data), mallory_funding, 82, 1700001004);
    burn_tx.nft_burn->signature = crypto::sign_recoverable_message(
        crypto::sha256(burn_tx.nft_burn->serialize_for_signing()), test::make_key(81));
    burn_tx.sign(test::make_key(82));
    check(!state->validate_transaction(burn_tx) && !state->apply_transaction(burn_tx, 201, false) &&
              state->nft_exists(nft_id) && state->get_nft_owner(nft_id) == alice &&
              state->has_utxo(mallory_funding.get_hash(), 0),
          "copied burn signature cannot destroy an NFT from another wallet's input");
}

void test_owner_order() {
    const auto ordinary = metadata("Ordinary", 0);
    selection_case("two-transfers", {ordinary}, {transfer(ordinary), transfer(ordinary, 83)}, {0});
    selection_case("transfer-before-burn", {ordinary}, {transfer(ordinary), burn(ordinary)}, {0});
    selection_case("burn-before-transfer", {ordinary}, {burn(ordinary), transfer(ordinary)}, {0});
    selection_case("two-burns", {ordinary}, {burn(ordinary), burn(ordinary)}, {0});
    selection_case("self-transfer-before-burn", {ordinary}, {transfer(ordinary, 81), burn(ordinary)}, {0, 1});
    const auto other = metadata("Independent", 200000);
    selection_case("independent-transfers", {ordinary, other}, {transfer(ordinary), transfer(other)}, {0, 1});
    const auto cad = metadata("Cadastral", 0, "test-001");
    selection_case("cadastral-transfer-before-burn", {cad}, {transfer(cad), burn(cad)}, {0});
    selection_case("cadastral-transfer-before-transfer", {cad}, {transfer(cad), transfer(cad, 83)}, {0});
    selection_case("cadastral-burn-before-document", {cad}, {burn(cad), document(cad)}, {0});
    selection_case("cadastral-transfer-before-document", {cad}, {transfer(cad), document(cad)}, {0});
    selection_case("cadastral-document-before-burn", {cad}, {document(cad), burn(cad)}, {0, 1});
}

void test_cadastral_documents() {
    struct Scratch {
        std::filesystem::path path = test::make_temp_dir("geocoin-cadastral-document");
        ~Scratch() { std::error_code error; std::filesystem::remove_all(path, error); }
    } scratch;
    std::unique_ptr<state::Storage> backend;
    if (use_rocksdb) {
        auto db = std::make_unique<state::RocksDBStorage>(scratch.path.string());
        expect(db->open(), "open cadastral RocksDB"); backend = std::move(db);
    } else backend = std::make_unique<state::MemoryStorage>();
    expect(backend->put_metadata("chainstate_height", utils::encode_u64_le(200)), "seed document height");
    expect(backend->put_metadata("chainstate_utxo_count", utils::encode_u64_le(0)), "seed document UTXO count");
    expect(backend->put_metadata("idx_epoch", bytes(32, '1')), "seed document epoch");
    const auto parcel = metadata("Linked parcel", 300000, "doc-parcel");
    const auto ordinary = metadata("Non-cadastral NFT", 600000);
    const auto owner = test::address_from_seed(81);
    for (const auto& data : {parcel, ordinary}) {
        const auto hex = utils::hash256_to_hex(data.calculate_hash());
        expect(backend->put_metadata("nft:meta:" + hex, data.serialize()), "seed document parcel");
        expect(backend->put_metadata("nft:owner:" + hex, bytes(owner.str().begin(), owner.str().end())), "seed document owner");
        expect(backend->put_metadata("idx:" + std::string(32, '1') + ":nft:" + hex, {1}), "seed document NFT identity");
    }
    auto state = std::make_shared<state::ChainState>(std::move(backend));
    state->set_activation_manifest(utils::ActivationManifest::fully_enabled_for_tests("degree"));
    const auto funding = Transaction::create_coinbase(1, owner, 2 * COIN);
    const auto outsider_funding = Transaction::create_coinbase(2, test::address_from_seed(82), 2 * COIN);
    expect(state->apply_transaction(funding, 1, true) && state->apply_transaction(outsider_funding, 2, true), "fund document authors");
    auto valid = document(parcel);
    valid.timestamp = 1700002000;
    valid.inputs = {{funding.get_hash(), 0}};
    valid.outputs = {{funding.outputs.front().value - consensus::params::MIN_TX_FEE, owner, {}}};
    sign(valid, consensus::params::MIN_TX_FEE);
    check(state->validate_transaction(valid), "owner authorizes a document linked to an active cadastral NFT");
    const auto restored = Transaction::deserialize_canonical(valid.serialize());
    check(restored.cadastral_nft_id == parcel.calculate_hash() && restored.verify(), "document wire encoding commits the full NFT identifier");
    const auto reject = [&](Transaction tx, const char* reason, uint8_t signer = 81) {
        tx.sign(test::make_key(signer));
        check(!state->validate_transaction(tx) && !state->apply_transaction(tx, 201, false) &&
            state->has_utxo(funding.get_hash(), 0) && state->has_utxo(outsider_funding.get_hash(), 0), reason);
    };
    auto bad = valid; bad.cadastral_nft_id = {}; reject(bad, "document without a parcel link is rejected");
    bad = valid; bad.cadastral_nft_id = test::hash_from_seed(99); reject(bad, "unknown parcel link is rejected");
    bad = valid; bad.cadastral_nft_id = ordinary.calculate_hash(); reject(bad, "ordinary NFTs cannot receive cadastral documents");
    bad = valid; bad.cadastral_number = "other-number"; reject(bad, "document registry number must agree with its NFT");
    bad = valid; bad.cadastral_owner = test::address_from_seed(82); reject(bad, "document cannot name a different owner");
    bad = valid; bad.inputs = {{outsider_funding.get_hash(), 0}};
    reject(bad, "an outsider cannot attach a document by copying the parcel owner address", 82);
    bad = valid; bad.cadastral_doc_hash = {}; reject(bad, "null document digest is rejected");
    auto missing_number = valid; missing_number.cadastral_number.clear(); sign(missing_number, consensus::params::MIN_TX_FEE);
    check(state->validate_transaction(missing_number), "NFT link uniquely identifies parcel when registry number is omitted");
    check(state->apply_transaction(valid, 201, false), "authorized document applies through ChainState");
    auto burn_tx = burn(parcel);
    burn_tx.timestamp = valid.timestamp + 1;
    burn_tx.inputs = {{valid.get_hash(), 0}};
    burn_tx.outputs = {{valid.outputs.front().value - consensus::params::MIN_TX_FEE, owner, {}}};
    sign(burn_tx, consensus::params::MIN_TX_FEE);
    expect(state->apply_transaction(burn_tx, 201, false), "burn linked parcel");
    auto stale = valid; stale.timestamp += 2; stale.inputs = {{burn_tx.get_hash(), 0}};
    stale.outputs = {{burn_tx.outputs.front().value - consensus::params::MIN_TX_FEE, owner, {}}};
    sign(stale, consensus::params::MIN_TX_FEE);
    check(!state->validate_transaction(stale) && !state->apply_transaction(stale, 201, false), "burned parcel cannot receive new documents");
}

void test_cadastral_creation() {
    const auto cad = metadata("First", 0, "test-001");
    selection_case("duplicate-cadastral-number", {}, {create(cad), create(metadata("Second", 200000, "test-001"))}, {0});
    selection_case("overlapping-cadastral-polygons", {}, {create(cad), create(metadata("Second", 50000, "test-002"))}, {0});
    selection_case("touching-cadastral-polygons", {}, {create(cad), create(metadata("Second", 100000, "test-002"))}, {0, 1});
    selection_case("independent-cadastral-polygons", {}, {create(cad), create(metadata("Second", 200000, "test-002"))}, {0, 1});
    const auto ordinary = metadata("Ordinary", 0);
    selection_case("ordinary-polygons-can-overlap", {}, {create(ordinary), create(metadata("Second", 50000))}, {0, 1});
    selection_case("ordinary-does-not-reserve-cadastral-land", {}, {create(ordinary), create(cad)}, {0, 1});
}

void test_rejection_isolation() {
    const auto a = metadata("First", 0, "test-001");
    const auto rejected = metadata("Rejected", 400000, "test-001");
    const auto fallback = metadata("Fallback", 400000, "test-002");
    selection_case("rejected-number-does-not-reserve-geometry", {}, {create(a), create(rejected), create(fallback)}, {0, 2});
    const auto overlap = metadata("Rejected", 50000, "test-002");
    selection_case("rejected-geometry-does-not-reserve-number", {}, {create(a), create(overlap), create(fallback)}, {0, 2});
    const auto ordinary = metadata("Ordinary", 0);
    const auto other = metadata("Other", 400000);
    selection_case("rejected-burn-does-not-affect-other-owner", {ordinary, other},
                   {transfer(ordinary), burn(ordinary), transfer(other)}, {0, 2});
    selection_case("self-transfer-before-transfer", {ordinary}, {transfer(ordinary, 81), transfer(ordinary)}, {0, 1});
    selection_case("independent-burns", {ordinary, other}, {burn(ordinary), burn(other)}, {0, 1});
    selection_case("cadastral-self-transfer-still-burns", {a}, {transfer(a, 81), burn(a)}, {0});
    selection_case("cadastral-does-not-block-ordinary", {}, {create(a), create(ordinary)}, {0, 1});
    auto large = a;
    large.description.assign(1000, 'x');
    selection_case("oversized-cadastral-create-does-not-reserve-land", {}, {create(large), create(a)}, {1}, 1);
}
}

int main(int argc, char** argv) {
    try {
        consensus::SelectParams(consensus::Network::MAIN);
        for (bool backend : {false, true}) {
            use_rocksdb = backend;
            test_replayed_nft_authorization();
            test_owner_order();
            test_cadastral_documents();
            test_cadastral_creation();
            test_rejection_isolation();
        }
        size_t failed = 0;
        for (const auto& result : checks) {
            if (!result["passed"].get<bool>()) { ++failed; std::cerr << "FAILED: " << result["name"] << '\n'; }
        }
        if (argc > 1) std::ofstream(argv[1]) << nlohmann::json{
            {"checks", checks}, {"passed", checks.size() - failed}, {"failed", failed}}.dump(2) << '\n';
        std::cout << "NFT mining checks: " << checks.size() - failed << '/' << checks.size() << '\n';
        return failed ? 1 : 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 2;
    }
}
