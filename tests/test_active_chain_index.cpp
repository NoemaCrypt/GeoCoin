#include "chain_test_fixture.hpp"
#include "geocoin/utils/metrics.hpp"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>
#include <vector>

namespace {
std::string hash_to_hex(const geocoin::Hash256& hash)
{
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (const auto byte : hash) out << std::setw(2) << static_cast<unsigned>(byte);
    return out.str();
}

void check_index(const geocoin::test::TestChain& fixture)
{
    using namespace geocoin;
    Hash256 hash{};
    Height height = 0;
    test::expect(fixture.blocks->get_active_tip(hash, height), "persisted active tip exists");
    test::expect(hash == fixture.chain->get_tip()->get_block_hash() &&
                     height == fixture.chain->get_height(), "persisted tip matches chain");
    for (auto* index = fixture.chain->get_tip(); index; index = index->prev) {
        test::expect(fixture.blocks->get_active_block_hash(index->height, hash) &&
                         hash == index->get_block_hash(), "persisted height matches active branch");
    }
    test::expect(!fixture.blocks->get_active_block_hash(height + 1, hash), "no stale height above tip");
}

void check_pruned_transaction_survives_state_restart()
{
    using namespace geocoin;
    const auto state_directory = test::make_temp_dir("geocoin-active-tx-pruned-restart");
    {
        auto backend = std::make_unique<state::RocksDBStorage>(state_directory.string());
        test::expect(backend->open(), "open persistent state for pruned transaction test");
        test::TestChain fixture(std::move(backend));
        std::vector<core::Block> blocks;
        for (int i = 0; i < 3; ++i) {
            blocks.push_back(fixture.make_block());
            fixture.append(blocks.back());
        }
        const auto txid = blocks.front().transactions.front().get_hash();
        const auto address = blocks.front().transactions.front().outputs.front().address;
        test::expect(fixture.state->get_transaction(txid) != nullptr,
                     "active transaction is visible before body pruning");
        test::expect(fixture.blocks->prune_block_body_at_height(1),
                     "prune the active block body while retaining its indexes");

        fixture.chain.reset();
        fixture.state.reset();
        fixture.blocks->close();
        test::expect(fixture.blocks->open((fixture.directory / "blocks").string()),
                     "reopen pruned block storage");

        auto reopened_backend = std::make_unique<state::RocksDBStorage>(state_directory.string());
        test::expect(reopened_backend->open(), "reopen persistent state after pruning");
        auto reopened = std::make_shared<state::ChainState>(std::move(reopened_backend));
        reopened->set_block_storage(fixture.blocks);
        const auto recovered = reopened->get_transaction(txid);
        test::expect(recovered && recovered->get_hash() == txid,
                     "durable active marker resolves raw transaction after pruning and restart");
        const auto refs = reopened->get_address_txids(address);
        test::expect(std::any_of(refs.begin(), refs.end(), [&](const auto& ref) {
            return ref.txid == txid && ref.height == 1;
        }), "raw transaction rebuild restores active address query references");
    }
    std::error_code error;
    std::filesystem::remove_all(state_directory, error);
}

void check_undo_retention_policy()
{
    using namespace geocoin;
    for (const bool retain_all : {false, true}) {
        test::TestChain fixture;
        fixture.state->set_undo_retention_depth(retain_all ? 0 : 2);
        std::vector<core::Block> blocks;
        for (int i = 0; i < 3; ++i) {
            blocks.push_back(fixture.make_block());
            fixture.append(blocks.back());
        }
        auto* storage = fixture.state->get_storage();
        const auto undo_key = [&](size_t index) {
            return "undo:" + hash_to_hex(blocks[index].get_hash());
        };
        test::expect(storage->has_metadata(undo_key(0)) == retain_all,
                     "undo outside the configured reorg window follows retention policy");
        test::expect(storage->has_metadata(undo_key(1)) && storage->has_metadata(undo_key(2)),
                     "undo inside the configured reorg window remains available");
        if (!retain_all) {
            state::BlockUndo legacy_undo;
            legacy_undo.height = 0;
            legacy_undo.block_hash.fill(0xa5);
            const std::string legacy_key = "undo:" + hash_to_hex(legacy_undo.block_hash);
            test::expect(storage->put_metadata(legacy_key, legacy_undo.serialize()),
                         "inject an undo record left by an older retention policy");
            test::expect(storage->has_metadata(legacy_key),
                         "legacy expired undo exists before startup maintenance");
            fixture.state->set_block_storage(fixture.blocks);
            test::expect(storage->has_metadata(legacy_key), "attaching storage cannot prune before policy is applied");
            fixture.state->set_undo_retention_depth(2);
            test::expect(!storage->has_metadata(legacy_key),
                         "startup maintenance removes every expired undo record");
            test::expect(storage->has_metadata(undo_key(1)) &&
                             storage->has_metadata(undo_key(2)),
                         "startup maintenance preserves the configured reorg window");
        }
    }
}

void check_concurrent_chainstate_readers_during_reorg()
{
    using namespace geocoin;
    test::TestChain fixture;
    const auto active = fixture.make_block();
    fixture.append(active);
    auto* active_index = fixture.chain->get_tip();
    auto* genesis = active_index->prev;

    auto alternate = fixture.make_block(genesis);
    ++alternate.transactions.front().timestamp;
    alternate.transactions.front().outputs.front().address = test::address_from_seed(43);
    test::TestChain::seal(alternate, genesis);
    auto* alternate_index = fixture.accept(alternate);

    std::atomic<bool> stop{false};
    std::atomic<bool> reader_failed{false};
    std::vector<std::thread> readers;
    for (int i = 0; i < 3; ++i) {
        readers.emplace_back([&] {
            try {
                while (!stop.load(std::memory_order_relaxed)) {
                    (void)fixture.state->get_balance(active.transactions.front().outputs.front().address);
                    (void)fixture.state->get_utxos_for_address(active.transactions.front().outputs.front().address);
                    (void)fixture.state->get_transaction(active.transactions.front().get_hash());
                    (void)fixture.state->calculate_transaction_fee(active.transactions.front());
                    (void)fixture.state->get_foundation_config();
                    state::SidechainDeposit deposit;
                    (void)fixture.state->get_sidechain_deposit({}, deposit);
                    state::SidechainRegistryEntry registry;
                    (void)fixture.state->get_sidechain_registry({}, registry);
                    (void)fixture.state->nft_exists({});
                }
            } catch (...) {
                reader_failed = true;
            }
        });
    }

    for (int i = 0; i < 32; ++i) {
        consensus::ValidationState validation;
        const auto target = (i % 2 == 0) ? alternate_index : active_index;
        test::expect(fixture.chain->switch_to_tip(target->get_block_hash(), validation),
                     "reorg succeeds while public ChainState readers are active");
    }
    stop = true;
    for (auto& reader : readers) reader.join();
    test::expect(!reader_failed.load(), "concurrent public ChainState reads remain stable during reorg");
}
}

int main()
{
    using namespace geocoin;
    using test::expect;
    try {
        consensus::SelectParams(consensus::Network::LOADNET);
        test::TestChain fixture;
        auto writes = GEO_METRIC_COUNTER("storage.active_index_entries_written_total");
        auto rebuilds = GEO_METRIC_COUNTER("storage.active_index_rebuilds_total");
        const auto initial_writes = writes->value();
        const auto initial_rebuilds = rebuilds->value();
        auto visits = GEO_METRIC_COUNTER("blockchain.ancestry_entries_checked_total");
        const auto initial_visits = visits->value();
        for (int i = 0; i < 24; ++i) fixture.append(fixture.make_block());
        expect(visits->value() - initial_visits <= 24 * 4,
               "appending N blocks must perform only O(N) ancestry checks");
        expect(writes->value() - initial_writes == 24,
               "appending N blocks must write N active height entries, independent of chain height");
        expect(rebuilds->value() == initial_rebuilds, "ordinary append must not rebuild the index");
        check_index(fixture);

        auto* old_tip = fixture.chain->get_tip();
        auto* fork = old_tip->prev->prev->prev;
        auto* branch = fork;
        for (int i = 0; i < 4; ++i) {
            auto block = fixture.make_block(branch);
            // Distinguish the fork even if coinbase timestamps fall in the same second.
            block.header.timestamp += 1;
            test::TestChain::seal(block, branch);
            branch = fixture.accept(block);
        }
        const auto before_reorg = writes->value();
        consensus::ValidationState validation;
        expect(fixture.chain->activate_best_chain(validation), "activate longer fork");
        expect(fixture.chain->get_tip() == branch, "longer fork becomes active");
        expect(writes->value() - before_reorg == 4, "reorg writes only new branch heights");
        check_index(fixture);

        const auto before_switch = writes->value();
        expect(fixture.chain->switch_to_tip(old_tip->get_block_hash(), validation), "switch back to shorter branch");
        expect(writes->value() - before_switch == 3, "forced switch writes only changed heights");
        expect(rebuilds->value() == initial_rebuilds, "reorg must not rebuild unchanged prefix");
        check_index(fixture);

        fixture.chain.reset();
        fixture.blocks->close();
        expect(fixture.blocks->open((fixture.directory / "blocks").string()), "reopen block storage");
        fixture.chain = std::make_shared<core::Blockchain>(fixture.state, fixture.blocks);
        expect(fixture.chain->initialize(), "restart with incremental index");
        check_index(fixture);
        check_pruned_transaction_survives_state_restart();
        check_undo_retention_policy();
        check_concurrent_chainstate_readers_during_reorg();
        std::cout << "Linear active index writes, reorg, truncation and restart passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
