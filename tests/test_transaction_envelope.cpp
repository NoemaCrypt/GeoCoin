#include "chain_test_fixture.hpp"
#include "geocoin/network/mempool.hpp"
#include "geocoin/rpc/rpc_blockchain_format.hpp"
#include "geocoin/rpc/rpc_encoding.hpp"
#include "geocoin/rpc/rpc_server.hpp"
#include "geocoin/state/records.hpp"
#include "geocoin/utils/bytes.hpp"
#include "nlohmann_json.hpp"

#include <fstream>
#include <functional>
#include <iostream>

namespace geocoin::rpc {
struct RPCServerTestAccess {
    static json call(RPCServer& server, const json& request) {
        return server.handle_rpc_call(request, RPCAuthRole::ADMIN);
    }
};
}

namespace {
using namespace geocoin;
using test::expect;
using core::Transaction;
nlohmann::json checks = nlohmann::json::array();
bool use_rocksdb = false;
constexpr Amount fee = 100000;

void check(bool ok, const std::string& name) {
    checks.push_back({{"name", name}, {"passed", ok}});
}

Transaction reward(Height height = 101) {
    auto tx = Transaction::create_coinbase(height, test::address_from_seed(42),
        consensus::ProofOfWork::calculate_block_reward(height), 0,
        consensus::Params().foundation_address());
    tx.timestamp = 1700000000;
    return tx;
}

bool readable(const Transaction& tx) {
    try {
        const auto raw = tx.serialize();
        return Transaction::deserialize_canonical(raw).serialize() == raw;
    } catch (const std::exception&) {
        return false;
    }
}

void test_wire_limits() {
    struct Case {
        const char* name;
        std::function<void(Transaction&, size_t)> set;
        size_t limit;
    };
    const std::vector<Case> cases{
        {"input script", [](auto& tx, size_t size) { tx.inputs[0].script_sig.assign(size, 1); },
            consensus::params::MAX_SCRIPT_SIG_SIZE},
        {"output script", [](auto& tx, size_t size) { tx.outputs[0].script_pubkey.assign(size, 2); },
            consensus::params::MAX_SCRIPT_PUBKEY_SIZE},
        {"output address", [](auto& tx, size_t size) { tx.outputs[0].address = Address(std::string(size, 'a')); },
            consensus::params::MAX_ADDRESS_SIZE},
        {"witness signature", [](auto& tx, size_t size) {
            tx.witnesses.resize(1); tx.witnesses[0].signature.assign(size, 3);
        }, consensus::params::MAX_WITNESS_SIG_SIZE}
    };
    for (const auto& item : cases) {
        for (const size_t size : {item.limit - 1, item.limit, item.limit + 1}) {
            auto tx = reward();
            item.set(tx, size);
            const auto label = std::string(item.name) + " length " + std::to_string(size);
            consensus::ValidationState validation;
            check(consensus::check_transaction(tx, validation) == (size <= item.limit), label + ": common limit");
            check(readable(tx) == (size <= item.limit), label + ": wire limit");
        }
    }
    for (const size_t count : {size_t{0}, size_t{1}, size_t{2}, consensus::params::MAX_TX_INPUTS + 1}) {
        auto tx = reward();
        tx.witnesses.resize(count);
        consensus::ValidationState validation;
        const auto label = "coinbase witness count " + std::to_string(count);
        check(consensus::check_transaction(tx, validation) == (count <= tx.inputs.size()), label + ": common limit");
        check(readable(tx) == (count <= tx.inputs.size()), label + ": wire limit");
    }
    // Input-free typed transactions take early exits from normal UTXO checks.
    Transaction distribution;
    distribution.type = core::TransactionType::INTEREST_DISTRIBUTION;
    distribution.outputs = {{COIN, test::address_from_seed(71), {}}};
    distribution.witnesses.resize(1);
    consensus::ValidationState validation;
    check(!consensus::check_transaction(distribution, validation), "input-free transaction rejects an extra witness");
    check(!readable(distribution), "input-free extra witness is not readable");

    auto malformed_key = reward();
    malformed_key.witnesses.resize(1);
    malformed_key.witnesses[0].public_key.assign(consensus::params::WITNESS_PUBKEY_SIZE - 1, 1);
    validation.reset();
    check(!consensus::check_transaction(malformed_key, validation), "malformed witness key fails without exception");

    auto implicit = reward();
    auto explicit_empty = implicit;
    explicit_empty.witnesses.resize(implicit.inputs.size());
    check(implicit.serialize() == explicit_empty.serialize(), "implicit empty witnesses have canonical bytes");
    check(implicit.witnesses.empty(), "serialization does not mutate an unsigned object");

    for (const uint8_t version : {uint8_t{0}, uint8_t{2}, uint8_t{255}}) {
        for (const bool has_inputs : {false, true}) {
            auto tx = has_inputs ? reward() : Transaction{};
            tx.witness_version = version;
            check(!readable(tx), "reader rejects witness version " + std::to_string(version) +
                (has_inputs ? " with an input" : " without inputs"));
        }
    }
    core::TxInput default_input;
    core::TxOutput default_output;
    check(default_input.vout == 0 && default_input.sequence == core::TxInput::SEQUENCE_FINAL,
          "default input has deterministic index and sequence");
    check(default_output.value == 0, "default output has deterministic amount");
}

std::unique_ptr<state::Storage> backend(const std::filesystem::path& path) {
    if (!use_rocksdb) return std::make_unique<state::MemoryStorage>();
    auto storage = std::make_unique<state::RocksDBStorage>((path / "state").string());
    expect(storage->open(), "open isolated RocksDB");
    return storage;
}

struct Fixture {
    std::filesystem::path path = test::make_temp_dir("geocoin-envelope");
    std::shared_ptr<state::ChainState> state;
    Transaction funding = reward(1);
    Fixture() {
        auto storage = backend(path);
        const auto& params = consensus::Params();
        const state::FoundationConfig foundation{params.foundation_address(), params.foundation_m(),
            params.foundation_n(), params.foundation_participants()};
        expect(storage->put_metadata("idx_epoch", bytes(32, '2')), "seed index epoch");
        expect(storage->put_metadata("chainstate_utxo_count", utils::encode_u64_le(0)), "seed UTXO count");
        expect(storage->put_metadata("foundation_cfg", state::records::serialize_foundation_config(foundation)),
               "seed foundation");
        expect(storage->put_metadata("interest_pool_balance", utils::encode_u64_le(0)), "seed pool");
        state = std::make_shared<state::ChainState>(std::move(storage));
        expect(state->apply_transaction(funding, 1, true), "apply funding coinbase through public state API");
        expect(state->set_height(100), "set funding maturity");
        expect(state->set_tip_hash(test::hash_from_seed(72)), "set preceding tip");
    }
    ~Fixture() {
        state.reset();
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
    void restart() {
        std::unique_ptr<state::Storage> storage;
        if (!use_rocksdb) storage = std::make_unique<state::MemoryStorage>(
            *dynamic_cast<state::MemoryStorage*>(state->get_storage()));
        state.reset();
        if (use_rocksdb) storage = backend(path);
        state = std::make_shared<state::ChainState>(std::move(storage));
    }
    Transaction payment() const {
        Transaction tx;
        tx.timestamp = 1700000000;
        tx.inputs = {{funding.get_hash(), 0}};
        tx.outputs = {{funding.outputs[0].value - fee, test::address_from_seed(71), {}}};
        return tx;
    }
    std::map<std::string, bytes> metadata() const {
        std::map<std::string, bytes> snapshot;
        for (const auto& key : state->get_storage()->get_keys_with_prefix("")) {
            expect(state->get_storage()->get_metadata(key, snapshot[key]), "read metadata snapshot");
        }
        return snapshot;
    }
};

void test_state_rejects(const std::string& name, const std::function<void(Transaction&)>& mutate,
                        bool coinbase = false) {
    const auto label = std::string(use_rocksdb ? "RocksDB: " : "MemoryStorage: ") + name;
    Fixture f;
    auto tx = coinbase ? reward() : f.payment();
    mutate(tx);
    if (!coinbase) tx.sign(test::make_key(42));
    const auto before = f.metadata();
    const auto count = f.state->get_utxo_count();
    check(!f.state->validate_transaction(tx), label + ": standalone state rejects");
    if (!coinbase) {
        network::Mempool pool(f.state);
        check(!pool.add_transaction(tx) && pool.empty(), label + ": mempool rejects");
    }
    consensus::ValidationState validation;
    check(!f.state->apply_transaction(tx, 101, coinbase, &validation) && validation.is_invalid(),
          label + ": direct application rejects before mutation");
    check(f.metadata() == before && f.state->get_utxo_count() == count &&
              f.state->has_utxo(f.funding.get_hash(), 0), label + ": rejection preserves input and metadata");
    try {
        f.restart();
        check(f.metadata() == before && f.state->get_utxo_count() == count &&
                  f.state->has_utxo(f.funding.get_hash(), 0), label + ": restart preserves input and metadata");
        auto valid = f.payment();
        valid.sign(test::make_key(42));
        check(f.state->validate_transaction(valid), label + ": valid payments still work after rejection");
    } catch (const std::exception&) {
        check(false, label + ": restart preserves input and metadata");
        check(false, label + ": valid payments still work after rejection");
    }
}

void test_valid_lifecycle() {
    const auto label = std::string(use_rocksdb ? "RocksDB: " : "MemoryStorage: ") + "boundary scripts";
    const auto path = test::make_temp_dir("geocoin-envelope-chain");
    auto chain = std::make_unique<test::TestChain>(backend(path));
    Transaction funding;
    for (Height height = 1; height <= 100; ++height) {
        auto block = chain->make_block();
        if (height == 1) funding = block.transactions.front();
        chain->append(block);
    }
    Transaction tx;
    tx.inputs = {{funding.get_hash(), 0}};
    tx.inputs[0].script_sig.assign(consensus::params::MAX_SCRIPT_SIG_SIZE, 1);
    tx.outputs = {{funding.outputs[0].value - fee, test::address_from_seed(71), {}}};
    tx.outputs[0].script_pubkey.assign(consensus::params::MAX_SCRIPT_PUBKEY_SIZE, 2);
    tx.sign(test::make_key(42));
    check(tx.verify() && readable(tx), label + ": valid signature and canonical bytes");
    {
        network::Mempool pool(chain->state);
        check(pool.add_transaction(tx), label + ": admitted to mempool");
        check(pool.get_transactions_for_mining(1000000).size() == 1, label + ": selected for mining");
    }
    auto block = chain->make_block();
    block.transactions[0] = Transaction::create_coinbase(101, test::address_from_seed(42),
        consensus::ProofOfWork::calculate_block_reward(101), fee, chain->state->get_foundation_address());
    block.transactions.push_back(tx);
    test::TestChain::seal(block, chain->chain->get_tip());
    chain->append(block);
    auto stored = chain->state->get_transaction(tx.get_hash());
    check(stored && stored->serialize() == tx.serialize(), label + ": accepted block retains exact transaction");
    chain->chain.reset();
    std::unique_ptr<state::Storage> storage;
    if (!use_rocksdb) storage = std::make_unique<state::MemoryStorage>(
        *dynamic_cast<state::MemoryStorage*>(chain->state->get_storage()));
    chain->state.reset();
    if (use_rocksdb) storage = backend(path);
    chain->state = std::make_shared<state::ChainState>(std::move(storage));
    chain->state->set_block_storage(chain->blocks);
    chain->chain = std::make_shared<core::Blockchain>(chain->state, chain->blocks);
    expect(chain->chain->initialize(), "restart accepted chain");
    stored = chain->state->get_transaction(tx.get_hash());
    check(stored && stored->serialize() == tx.serialize() && stored->verify(),
          label + ": restart retains exact transaction and signatures");
    expect(chain->state->revert_block(block), "undo boundary transaction");
    check(chain->state->has_utxo(funding.get_hash(), 0) && !chain->state->has_utxo(tx.get_hash(), 0),
          label + ": undo restores funding and removes created output");
    chain.reset();
    std::error_code error;
    std::filesystem::remove_all(path, error);
}

nlohmann::json rpc_call(rpc::RPCServer& server, const std::string& method, const nlohmann::json& params) {
    return rpc::RPCServerTestAccess::call(server,
        {{"jsonrpc", "2.0"}, {"id", "envelope"}, {"method", method}, {"params", params}});
}

void test_rpc_fields() {
    using nlohmann::json;
    Fixture f;
    rpc::RPCServer server;
    expect(server.set_context(rpc::RPCServerContext{.chain_state = f.state}), "set RPC state context");
    auto tx = f.payment();
    tx.inputs[0].script_sig = {1, 2};
    tx.outputs[0].script_pubkey = {3, 4};
    tx.sign(test::make_key(42));
    auto response = rpc_call(server, "decoderawtransaction", json::array({rpc::bytes_to_hex_string(tx.serialize())}));
    expect(response.contains("result"), "decode signed transaction through RPC dispatcher");
    auto decoded = response["result"];
    check(decoded["inputs"][0].value("script_sig_hex", "") == "0102", "RPC exposes exact input script");
    check(decoded["outputs"][0].value("script_pubkey_hex", "") == "0304", "RPC exposes exact output script");
    check(decoded["inputs"][0].value("signature_hash", "") == rpc::hash256_to_hex_string(tx.inputs[0].signature_hash),
          "RPC exposes signed input signature hash");
    check(decoded.value("witness_version", 0) == tx.witness_version, "RPC exposes witness format version");
    check(decoded.contains("witnesses") && decoded["witnesses"].size() == 1 &&
        decoded["witnesses"][0] == json{{"public_key_hash", rpc::hash256_to_hex_string(tx.witnesses[0].public_key_hash)},
            {"public_key_hex", rpc::bytes_to_hex_string(tx.witnesses[0].public_key)},
            {"signature_hex", rpc::bytes_to_hex_string(tx.witnesses[0].signature)}},
          "RPC exposes exact witness bytes and hash");
    tx.witnesses[0].public_key.clear();
    response = rpc_call(server, "decoderawtransaction", json::array({rpc::bytes_to_hex_string(tx.serialize())}));
    decoded = response["result"];
    check(decoded.contains("witnesses") && decoded["witnesses"].size() == 1 &&
        decoded["witnesses"][0].value("public_key_hex", "missing").empty() &&
        decoded["witnesses"][0].value("public_key_hash", "") == rpc::hash256_to_hex_string(tx.witnesses[0].public_key_hash),
          "RPC reports an omitted witness key without silently restoring it");

    tx = reward();
    const auto formatted = rpc::transaction_to_verbose_json(tx, tx.get_hash(), rpc::hash256_to_hex_string(tx.get_hash()), *f.state);
    check(formatted.contains("witnesses") && formatted["witnesses"].size() == tx.inputs.size() &&
        formatted["witnesses"][0].value("signature_hex", "missing").empty(),
          "RPC displays serialized empty witness slots for an unsigned object");

    // Decoder fixture only: an ANGLE input stores its signature in the body.
    tx.type = core::TransactionType::ANGLE_COMMITMENT;
    tx.commitment_network_id = consensus::Params().network_id();
    tx.multisig_address = test::address_from_seed(71);
    tx.signer_address = test::address_from_seed(42);
    tx.spending_tx_hash = test::hash_from_seed(73);
    tx.inputs[0].signature = {5, 6, 7};
    response = rpc_call(server, "decoderawtransaction", json::array({rpc::bytes_to_hex_string(tx.serialize())}));
    expect(response.contains("result"), "decode ANGLE body signature");
    decoded = response["result"];
    check(decoded["inputs"][0].value("signature_hex", "") == "050607" &&
        !decoded["inputs"][0].contains("signature_hash"), "RPC distinguishes ANGLE body signatures from signature hashes");

    for (const size_t size : {consensus::params::MAX_SCRIPT_SIG_SIZE, consensus::params::MAX_SCRIPT_SIG_SIZE + 1}) {
        const json request{{"sidechain_id", rpc::hash256_to_hex_string(test::hash_from_seed(74))},
            {"owner_address", test::address_from_seed(42).str()}, {"amount", 1000}, {"unlock_height", 200},
            {"inputs", json::array({json{{"txid", rpc::hash256_to_hex_string(f.funding.get_hash())}, {"vout", 0},
                {"script_sig_hex", std::string(size * 2, 'a')}}})},
            {"outputs", json::array({json{{"address", test::address_from_seed(71).str()}, {"value", 1000}}})},
            {"validate", false}};
        response = rpc_call(server, "createsidechainlockpayload", json::array({request}));
        if (size <= consensus::params::MAX_SCRIPT_SIG_SIZE) {
            check(response.contains("result"), "RPC accepts script at wire limit with validate=false");
            if (response.contains("result")) {
                response = rpc_call(server, "decoderawtransaction", json::array({response["result"]["tx_hex"]}));
                check(response.contains("result"), "RPC can decode its own boundary signing payload");
            }
        } else {
            check(response.contains("error") && response["error"].value("code", 0) == rpc::RPC_INVALID_PARAMETER,
                  "RPC rejects oversized script even with validate=false");
        }
    }
}
}

int main(int argc, char** argv) {
    try {
        consensus::SelectParams(consensus::Network::LOADNET);
        test_wire_limits();
        test_rpc_fields();
        for (const bool rocksdb : {false, true}) {
            use_rocksdb = rocksdb;
            test_state_rejects("oversized input script", [](auto& tx) {
                tx.inputs[0].script_sig.resize(consensus::params::MAX_SCRIPT_SIG_SIZE + 1, 1);
            });
            test_state_rejects("oversized output script", [](auto& tx) {
                tx.outputs[0].script_pubkey.resize(consensus::params::MAX_SCRIPT_PUBKEY_SIZE + 1, 1);
            });
            test_state_rejects("unsupported transaction version", [](auto& tx) { tx.version = 3; });
            test_state_rejects("dust output", [](auto& tx) { tx.outputs[0].value = 1; });
            test_state_rejects("coinbase extra witness", [](auto& tx) { tx.witnesses.resize(2); }, true);
            test_state_rejects("coinbase oversized witness signature", [](auto& tx) {
                tx.witnesses.resize(1);
                tx.witnesses[0].signature.resize(consensus::params::MAX_WITNESS_SIG_SIZE + 1, 1);
            }, true);
            test_valid_lifecycle();
        }
        size_t passed = 0;
        for (const auto& item : checks) if (item["passed"].get<bool>()) ++passed;
        const nlohmann::json result{{"passed", passed}, {"failed", checks.size() - passed}, {"checks", checks}};
        if (argc > 1) std::ofstream(argv[1]) << result.dump(2) << '\n';
        for (const auto& item : checks) if (!item["passed"].get<bool>()) std::cerr << "FAIL: " << item["name"] << '\n';
        std::cout << passed << '/' << checks.size() << " transaction envelope checks passed\n";
        return passed == checks.size() ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "transaction envelope test failed: " << error.what() << '\n';
        return 2;
    }
}
