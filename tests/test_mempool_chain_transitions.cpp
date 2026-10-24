#include "chain_test_fixture.hpp"
#include "geocoin/network/mempool.hpp"
#include "geocoin/node/chain_coordinator.hpp"
#include "nlohmann_json.hpp"

#include <fstream>
#include <iostream>

namespace {
using namespace geocoin;
using test::expect;
nlohmann::json checks = nlohmann::json::array();

void check(bool result, const std::string& name) {
    checks.push_back({{"name", name}, {"passed", result}});
}

core::Transaction payment(const core::Transaction& parent, uint32_t output,
                          uint8_t owner, uint8_t recipient, Amount fee) {
    core::Transaction tx;
    tx.timestamp = parent.timestamp + 1;
    tx.inputs.push_back({parent.get_hash(), output});
    tx.outputs.push_back({parent.outputs.at(output).value - fee, test::address_from_seed(recipient), {}});
    tx.sign(test::make_key(owner));
    expect(tx.verify(), "payment has a valid Falcon signature");
    return tx;
}

void maturity_after_height_change(bool rocksdb) {
    const auto directory = test::make_temp_dir("geocoin-mempool-maturity");
    {
        std::unique_ptr<state::Storage> storage;
        if (rocksdb) {
            auto backend = std::make_unique<state::RocksDBStorage>((directory / "state").string());
            expect(backend->open(), "open maturity RocksDB");
            storage = std::move(backend);
        } else {
            storage = std::make_unique<state::MemoryStorage>();
        }
        auto state = std::make_shared<state::ChainState>(std::move(storage));
        auto funding = core::Transaction::create_coinbase(10, test::address_from_seed(42), 10 * COIN);
        expect(state->apply_transaction(funding, 10, true) && state->set_height(109), "seed mature funding");
        const auto tx = payment(funding, 0, 42, 43, 10000);
        network::Mempool pool(state);
        expect(pool.add_transaction(tx), "mature payment is admitted");
        expect(state->set_height(107), "move below the reward maturity boundary");
        const std::string label = rocksdb ? "RocksDB: " : "MemoryStorage: ";
        check(pool.get_transactions_for_mining(1000000).empty(),
              label + "mining skips a payment that became immature");
        pool.remove_block_transactions(core::Block{});
        check(!pool.has_transaction(tx.get_hash()), label + "reconciliation evicts an immature payment");
        check(!state->apply_transaction(tx, 108, false) && state->has_utxo(funding.get_hash(), 0),
              label + "block application rejects the immature payment without consuming funds");
        expect(state->set_height(109), "restore reward maturity");
        check(pool.add_transaction(tx), label + "mature payment can be admitted again after eviction");
    }
    std::error_code error;
    std::filesystem::remove_all(directory, error);
}

void pure_disconnect_cleans_mempool() {
    test::TestChain chain;
    core::Transaction funding;
    for (int height = 1; height <= 100; ++height) {
        const auto block = chain.make_block();
        if (height == 1) funding = block.transactions.front();
        chain.append(block);
    }
    const auto previous_tip = chain.chain->get_tip()->get_block_hash();
    const auto parent = payment(funding, 0, 42, 43, 10000);
    auto block = chain.make_block(nullptr, 10000);
    block.transactions.push_back(parent);
    test::TestChain::seal(block, chain.chain->get_tip());
    chain.append(block);
    const auto child = payment(parent, 0, 43, 44, 20000);
    auto pool = std::make_shared<network::Mempool>(chain.state);
    std::vector<services::ChainMutationEvent> events;
    node::ChainCoordinator coordinator(chain.chain, pool, [&](services::ChainMutationEvent event) {
        events.push_back(std::move(event));
        return true;
    });
    expect(coordinator.start(), "start chain coordinator");
    expect(coordinator.admit_transaction(child).is_accepted(), "admit child of confirmed payment");
    const auto transition = coordinator.activate_tip(previous_tip);
    expect(transition.is_accepted() && chain.chain->get_height() == 100, "disconnect to the preceding tip");
    check(!pool->has_transaction(child.get_hash()), "pure disconnect removes a child whose funding left the chain");
    check(pool->has_transaction(parent.get_hash()), "pure disconnect re-admits the valid disconnected parent");
    const auto selected = pool->get_transactions_for_mining(1000000);
    check(selected.size() == 1 && selected.front().get_hash() == parent.get_hash(),
          "template after pure disconnect contains only the valid parent");
    check(!events.empty() && events.back().kind == services::ChainMutationEventKind::ACTIVE_CHAIN_CHANGED,
          "pure disconnect still publishes the chain transition");

    auto replacement = chain.make_block();
    ++replacement.header.timestamp;
    test::TestChain::seal(replacement, chain.chain->get_tip());
    auto* replacement_tip = chain.accept(replacement);
    const auto replacement_next = chain.make_block(replacement_tip);
    replacement_tip = chain.accept(replacement_next);
    expect(coordinator.activate_tip(replacement_tip->get_block_hash()).is_accepted(),
           "connect a replacement branch containing two blocks");
    check(pool->has_transaction(parent.get_hash()) && !pool->has_transaction(child.get_hash()),
          "multi-block reconciliation preserves an independent valid pending payment");
    check(chain.chain->get_height() == 102 &&
              pool->get_transactions_for_mining(1000000).size() == 1,
          "replacement branch leaves a usable mining template");
    coordinator.stop();
}
}

int main(int argc, char** argv) {
    try {
        consensus::SelectParams(consensus::Network::LOADNET);
        maturity_after_height_change(false);
        maturity_after_height_change(true);
        pure_disconnect_cleans_mempool();
        size_t failed = 0;
        for (const auto& result : checks) {
            if (!result["passed"].get<bool>()) {
                ++failed;
                std::cerr << "FAILED: " << result["name"] << '\n';
            }
        }
        const nlohmann::json report{{"checks", checks}, {"passed", checks.size() - failed}, {"failed", failed}};
        if (argc > 1) std::ofstream(argv[1]) << report.dump(2) << '\n';
        std::cout << "mempool transition checks: " << checks.size() - failed << '/' << checks.size() << '\n';
        return failed ? 1 : 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 2;
    }
}
