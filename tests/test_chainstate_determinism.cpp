#include "chain_test_fixture.hpp"
#include "geocoin/utils/bytes.hpp"

#include <fstream>
#include <iostream>
#include <map>

namespace {
using namespace geocoin;
using test::expect;

std::string read_token(const std::filesystem::path& path) {
    std::ifstream input(path);
    std::string token;
    expect(static_cast<bool>(input >> token), "missing or empty deterministic fixture: " + path.string());
    return token;
}

void write_token(const std::filesystem::path& path, const std::string& token) {
    std::ofstream output(path);
    output << token << '\n';
    expect(output.good(), "write deterministic fixture");
}

// Canonical economic state for this coinbase/standard-spend fixture. Lengths
// are explicitly little-endian, records sorted; local epochs, undo history and
// witness caches are deliberately outside this consensus-state fingerprint.
std::string state_digest(const state::ChainState& state, const state::Storage& storage) {
    std::map<std::string, bytes> records;
    for (const auto& key : storage.get_all_keys()) {
        const bool utxo = key.rfind("utxo:", 0) == 0;
        if (utxo || key == "chainstate_height" || key == "chainstate_tip_hash" ||
            key == "chainstate_utxo_count" || key == "interest_pool_balance" || key == "foundation_cfg") {
            // The economic fingerprint excludes the physical storage namespace.
            expect(storage.get_metadata(key, records[utxo ? key.substr(5) : key]), "read persisted state record");
        }
    }
    const auto tip = state.get_tip_hash_snapshot();
    records["tip"] = bytes(tip.begin(), tip.end());
    bytes canonical;
    const auto append = [&](const auto& value) {
        const uint64_t length = value.size();
        for (int i = 0; i < 8; ++i) canonical.push_back(static_cast<byte>(length >> (8 * i)));
        canonical.insert(canonical.end(), value.begin(), value.end());
    };
    for (const auto& [key, value] : records) { append(key); append(value); }
    return utils::hash256_to_hex(crypto::sha256(canonical));
}

void replay_on_disk(const std::vector<core::Block>& blocks, const std::string& expected, bool split) {
    const auto directory = test::make_temp_dir("geocoin-determinism-state");
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
    } cleanup{directory};
    const auto open = [&]() -> std::unique_ptr<state::Storage> {
        auto backend = split
            ? std::make_unique<state::RocksDBStorage>((directory / "state").string(),
                                                    (directory / "addresses").string())
            : std::make_unique<state::RocksDBStorage>((directory / "state").string());
        expect(backend->open(), "open deterministic replay RocksDB");
        return backend;
    };
    auto backend = open();
    auto* persisted = backend.get();
    test::TestChain replay(std::move(backend));
    for (const auto& block : blocks) replay.append(block);
    expect(state_digest(*replay.state, *persisted) == expected, "MemoryStorage/RocksDB replay diverged");

    replay.chain.reset();
    replay.state.reset();
    backend = open();
    persisted = backend.get();
    replay.state = std::make_shared<state::ChainState>(std::move(backend));
    replay.state->set_block_storage(replay.blocks);
    replay.chain = std::make_shared<core::Blockchain>(replay.state, replay.blocks);
    expect(replay.chain->initialize(), "restart deterministic replay");
    expect(state_digest(*replay.state, *persisted) == expected, "restart changed consensus state");
}
}

int main(int argc, char** argv) {
    try {
        // Regeneration is an explicit developer action; ordinary CI never
        // creates fixtures or accepts a changed expected fingerprint.
        const bool generate = argc == 3 && std::string(argv[1]) == "--generate-fixture";
        expect(argc == 1 || generate, "usage: test_chainstate_determinism [--generate-fixture DIR]");
        const std::filesystem::path data = generate ? argv[2] : GEOCOIN_TEST_DATA_DIR;
        if (generate) std::filesystem::create_directories(data);
        consensus::SelectParams(consensus::Network::LOADNET);
        auto backend = std::make_unique<state::MemoryStorage>();
        auto* persisted = backend.get();
        test::TestChain chain(std::move(backend));
        std::vector<core::Block> blocks;
        for (int i = 0; i < 100; ++i) {
            blocks.push_back(chain.make_block());
            chain.append(blocks.back());
        }
        const Amount fee = consensus::params::MIN_TX_FEE;
        auto next = chain.make_block(nullptr, fee);
        core::Transaction payment;
        if (generate) {
            payment.version = 2;
            payment.timestamp = next.header.timestamp;
            core::TxInput input;
            input.prev_tx_hash = blocks.front().transactions.front().get_hash();
            input.vout = 0;
            payment.inputs.push_back(input);
            payment.outputs.push_back({COIN, test::address_from_seed(43), {}});
            payment.outputs.push_back({blocks.front().transactions.front().outputs.front().value - COIN - fee,
                                       test::address_from_seed(42), {}});
            payment.sign(test::make_key(42));
            write_token(data / "determinism-payment.hex", crypto::bytes_to_hex(payment.serialize()));
        } else {
            const auto serialized = crypto::hex_to_bytes(read_token(data / "determinism-payment.hex"));
            size_t offset = 0;
            payment = core::Transaction::deserialize(serialized, offset);
            expect(offset == serialized.size(), "frozen payment has no trailing bytes");
        }
        expect(payment.verify(), "frozen payment signature validates");
        next.transactions.push_back(payment);
        test::TestChain::seal(next, chain.chain->get_tip());
        blocks.push_back(next);
        chain.append(next);
        const std::string expected = state_digest(*chain.state, *persisted);
        if (generate) write_token(data / "determinism-state.sha256", expected);
        else expect(expected == read_token(data / "determinism-state.sha256"),
                    "consensus state differs from cross-platform golden fingerprint: " + expected);

        // A competing block removes the payment; restoring the original tip
        // must restore the same UTXOs, balances and persisted chain position.
        auto* parent = chain.chain->get_tip()->prev;
        auto fork = chain.make_block(parent);
        fork.header.timestamp += 1;
        test::TestChain::seal(fork, parent);
        chain.accept(fork);
        consensus::ValidationState validation;
        expect(chain.chain->switch_to_tip(fork.get_hash(), validation), "activate competing block");
        expect(state_digest(*chain.state, *persisted) != expected, "fork changes state");
        expect(chain.chain->switch_to_tip(next.get_hash(), validation), "restore payment block");
        expect(state_digest(*chain.state, *persisted) == expected, "reorg roundtrip changed state");
        replay_on_disk(blocks, expected, false);
        replay_on_disk(blocks, expected, true);
        std::cout << "Deterministic 101-block replay, spend, reorg and restart: " << expected << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
