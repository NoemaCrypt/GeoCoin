#include "chain_test_fixture.hpp"
#include "geocoin/node/chain_coordinator.hpp"
#include "geocoin/network/mempool.hpp"
#include "geocoin/utils/bytes.hpp"

#include <iostream>

using namespace geocoin;
using test::expect;

namespace {
class JournalFailure final : public state::MemoryStorage {
public:
    unsigned fail_on = 0;
    unsigned calls = 0;
    bool persistent = false;
    bool fail_height = false;
    std::string throw_put_key;
    bool put_metadata(const std::string& key, const bytes& value) override {
        if (!throw_put_key.empty() && key == throw_put_key) {
            throw_put_key.clear();
            throw std::runtime_error("injected state write exception");
        }
        if (fail_height && key == "chainstate_height") return false;
        if (fail_on && key == "chainstate_journal" &&
            ++calls >= fail_on && (persistent || calls == fail_on)) return false;
        return MemoryStorage::put_metadata(key, value);
    }
};

void check_position(const test::TestChain& fixture, Height height, const Hash256& hash) {
    expect(fixture.state->get_height() == height && fixture.chain->get_height() == height,
           "state and chain heights agree");
    expect(fixture.state->get_tip_hash() == hash && fixture.chain->get_tip()->get_block_hash() == hash,
           "state and chain hashes agree");
    Hash256 stored_hash{};
    Height stored_height = 0;
    expect(fixture.blocks->get_active_tip(stored_hash, stored_height) &&
               stored_height == height && stored_hash == hash, "durable active index agrees");
}

void check_disconnect_failure(bool forced, unsigned fail_on, bool persistent) {
    auto backend = std::make_unique<JournalFailure>();
    auto* faults = backend.get();
    test::TestChain fixture(std::move(backend));
    auto* genesis = fixture.chain->get_tip();
    for (int i = 0; i < 3; ++i) fixture.append(fixture.make_block());
    const auto old_tip = fixture.chain->get_tip()->get_block_hash();
    auto* target = genesis;
    if (!forced) {
        for (int i = 0; i < 4; ++i) {
            auto block = fixture.make_block(target);
            ++block.header.timestamp;
            test::TestChain::seal(block, target);
            target = fixture.accept(block);
        }
    }
    auto mempool = std::make_shared<network::Mempool>(fixture.state);
    std::vector<services::ChainMutationEvent> events;
    node::ChainCoordinator coordinator(fixture.chain, mempool,
        [&](services::ChainMutationEvent event) { events.push_back(std::move(event)); return true; });
    expect(coordinator.start(), "coordinator starts");
    faults->fail_on = fail_on;
    faults->persistent = persistent;
    if (forced) {
        const auto result = coordinator.activate_tip(target->get_block_hash());
        expect(!result.is_accepted(), "injected disconnect failure is reported");
        expect(result.active_height == fixture.chain->get_height() &&
                   result.active_tip_hash == fixture.chain->get_tip()->get_block_hash(),
               "failed operation reports its actual resulting tip");
        if (persistent && fail_on > 1) {
            expect(result.active_tip_changed && result.event_sequence != 0 && !events.empty(),
                   "incomplete compensation publishes the partial chain transition");
        }
    } else {
        consensus::ValidationState validation;
        expect(!fixture.chain->activate_best_chain(validation) && validation.is_error(),
               "automatic reorg reports storage failure");
    }
    coordinator.stop();
    if (!persistent || fail_on == 1) {
        check_position(fixture, 3, old_tip);
        expect(events.empty(), "fully compensated failure has no net transition");
    } else {
        const Height remaining = 4 - fail_on;
        check_position(fixture, remaining, fixture.chain->get_tip()->get_block_hash());
    }
    faults->fail_on = 0;
    consensus::ValidationState retry;
    expect(fixture.chain->switch_to_tip(target->get_block_hash(), retry), "retry succeeds after storage recovery");
    check_position(fixture, target->height, target->get_block_hash());
}

void check_metadata_failure_notification() {
    auto backend = std::make_unique<JournalFailure>();
    auto* faults = backend.get();
    test::TestChain fixture(std::move(backend));
    const auto genesis = fixture.chain->get_tip()->get_block_hash();
    fixture.append(fixture.make_block());
    std::vector<services::ChainMutationEvent> events;
    node::ChainCoordinator coordinator(fixture.chain, std::make_shared<network::Mempool>(fixture.state),
        [&](services::ChainMutationEvent event) { events.push_back(std::move(event)); return true; });
    expect(coordinator.start(), "coordinator starts for metadata failure");
    faults->fail_height = true;
    const auto result = coordinator.activate_tip(genesis);
    coordinator.stop();
    expect(!result.is_accepted() && !fixture.state->has_derived_state_snapshot(),
           "failed undo metadata cannot leave the snapshot marked usable");
    expect(!events.empty() && events.back().kind == services::ChainMutationEventKind::RESCAN_REQUIRED,
           "unavailable state is announced to downstream consumers");
}

void check_apply_exception_is_fail_closed_and_reported() {
    {
        auto backend = std::make_unique<JournalFailure>();
        auto* faults = backend.get();
        test::TestChain fixture(std::move(backend));
        const auto block = fixture.make_block();
        expect(block.transactions.size() == 1 && block.transactions.front().outputs.size() >= 2,
               "exception fixture has a multi-output coinbase");

        state::UTXO first_output;
        first_output.tx_hash = block.transactions.front().get_hash();
        first_output.vout = 0;
        state::UTXO failed_output = first_output;
        failed_output.vout = 1;
        faults->throw_put_key = failed_output.get_key();

        std::vector<services::ChainMutationEvent> events;
        node::ChainCoordinator coordinator(
            fixture.chain,
            std::make_shared<network::Mempool>(fixture.state),
            [&](services::ChainMutationEvent event) {
                events.push_back(std::move(event));
                return true;
            }
        );
        expect(coordinator.start(), "coordinator starts for transaction exception");
        const auto result = coordinator.process_block(block);
        coordinator.stop();

        expect(!result.is_accepted(), "transaction write exception is returned as a failed submission");
        expect(result.active_height == fixture.chain->get_height() &&
                   result.active_tip_hash == fixture.chain->get_tip()->get_block_hash(),
               "transaction exception reports the actual active chain position");
        expect(fixture.state->has_derived_state_snapshot(),
               "fully compensated transaction exception keeps the previous snapshot usable");
        expect(!fixture.state->has_utxo(first_output.tx_hash, first_output.vout) &&
                   !fixture.state->has_utxo(failed_output.tx_hash, failed_output.vout),
               "transaction exception rolls back every output written before the throw");
        expect(!fixture.state->get_storage()->has_metadata("chainstate_journal"),
               "fully compensated transaction exception clears its recovery marker");
        expect(events.empty(), "fully compensated transaction exception has no net transition");
    }

    {
        auto backend = std::make_unique<JournalFailure>();
        auto* faults = backend.get();
        test::TestChain fixture(std::move(backend));
        const auto block = fixture.make_block();
        faults->throw_put_key =
            "atx:" + utils::hash256_to_hex(block.transactions.front().get_hash());

        std::vector<services::ChainMutationEvent> events;
        node::ChainCoordinator coordinator(
            fixture.chain,
            std::make_shared<network::Mempool>(fixture.state),
            [&](services::ChainMutationEvent event) {
                events.push_back(std::move(event));
                return true;
            }
        );
        expect(coordinator.start(), "coordinator starts for late state exception");
        const auto result = coordinator.process_block(block);
        coordinator.stop();

        expect(!result.is_accepted(), "late state exception is returned as a failed submission");
        expect(result.active_height == fixture.chain->get_height() &&
                   result.active_tip_hash == fixture.chain->get_tip()->get_block_hash(),
               "late exception reports the actual active chain position");
        expect(!fixture.state->has_derived_state_snapshot(),
               "uncompensated late exception marks the snapshot unavailable");
        expect(fixture.state->get_storage()->has_metadata("chainstate_journal"),
               "uncompensated late exception retains its recovery marker");
        expect(!events.empty() &&
                   events.back().kind == services::ChainMutationEventKind::RESCAN_REQUIRED,
               "uncompensated late exception announces required downstream recovery");
    }
}

void check_restart(unsigned pending_count, bool invalid_pending, bool prune) {
    const auto directory = test::make_temp_dir("geocoin-recovery-state");
    {
        auto backend = std::make_unique<state::RocksDBStorage>(directory.string());
        expect(backend->open(), "open durable chainstate");
        test::TestChain fixture(std::move(backend));
        for (int i = 0; i < 3; ++i) fixture.append(fixture.make_block());
        // A completed disconnect must survive reopening before any further connect.
        consensus::ValidationState validation;
        expect(fixture.chain->switch_to_tip(fixture.chain->get_tip()->prev->get_block_hash(), validation),
               "disconnect before restart");
        auto* target = fixture.chain->get_tip();
        if (prune) expect(fixture.chain->prune_history(1, true), "archive and prune old history");
        for (unsigned i = 0; i < pending_count; ++i) {
            auto block = fixture.make_block(target, invalid_pending && i == 0 ? COIN : 0);
            ++block.header.timestamp;
            test::TestChain::seal(block, target);
            target = fixture.accept(block);
        }
        fixture.chain.reset();
        fixture.state.reset();
        fixture.blocks->close();
        expect(fixture.blocks->open((fixture.directory / "blocks").string()), "reopen block store");
        backend = std::make_unique<state::RocksDBStorage>(directory.string());
        expect(backend->open(), "reopen durable chainstate");
        bytes legacy;
        expect(backend->get_metadata("chainstate_height", legacy) && legacy.size() == 8 && legacy[0] == 2,
               "disconnect persists the canonical height in the same batch");
        fixture.state = std::make_shared<state::ChainState>(std::move(backend));
        expect(fixture.state->get_height() == 2, "reopened snapshot loads disconnected height");
        fixture.state->set_block_storage(fixture.blocks);
        fixture.chain = std::make_shared<core::Blockchain>(fixture.state, fixture.blocks);
        expect(fixture.chain->initialize(), "restart activates pending blocks from the saved snapshot");
        // Previously disconnected height 3 remains a valid candidate too.
        const Height expected = invalid_pending ? 3 : std::max<Height>(3, 2 + pending_count);
        check_position(fixture, expected, fixture.chain->get_tip()->get_block_hash());
    }
    std::error_code error;
    std::filesystem::remove_all(directory, error);
}
}

int main() {
    try {
        consensus::SelectParams(consensus::Network::LOADNET);
        for (bool forced : {false, true}) {
            for (unsigned phase : {1u, 2u, 3u}) {
                check_disconnect_failure(forced, phase, false);
                check_disconnect_failure(forced, phase, true);
            }
        }
        check_metadata_failure_notification();
        check_apply_exception_is_fail_closed_and_reported();
        for (bool prune : {false, true}) {
            check_restart(0, false, prune);
            check_restart(3, false, prune);
            check_restart(3, true, prune);
        }
        std::cout << "Disconnect compensation, durable height, events and pending startup passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
