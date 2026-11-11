#include "chain_test_fixture.hpp"
#include "geocoin/multisig/multisig.hpp"
#include "geocoin/multisig/angle_store.hpp"
#include "geocoin/network/mempool.hpp"
#include "geocoin/node/chain_coordinator.hpp"
#include "geocoin/storage/rocksdb_store.hpp"
#include "geocoin/utils/bytes.hpp"
#include "nlohmann_json.hpp"

#include <rocksdb/utilities/stackable_db.h>
#include <rocksdb/version.h>

#include <fstream>
#include <functional>
#include <iostream>
#include <thread>

namespace geocoin::storage {
struct BlockStorageTestAccess {
    static void wrap(BlockStorage& storage, bool chain,
                     const std::function<rocksdb::DB*(rocksdb::DB*)>& factory) {
        auto& store = chain ? storage.chain_db_ : storage.index_db_;
        store->db_.reset(factory(store->db_.release()));
    }
};
}

namespace {
using namespace geocoin;
using test::expect;
using Kind = services::ChainMutationEventKind;
nlohmann::json checks = nlohmann::json::array();

void check(bool passed, const std::string& name) {
    checks.push_back({{"name", name}, {"passed", passed}});
}

enum class Fault { NONE, IO_ERROR, MISSING, NON_STANDARD };
struct Probe {
    Fault fault = Fault::NONE;
    std::string body_key;
    std::string metadata_key;
    unsigned committed_transitions = 0;
    unsigned activate_after = 1;
    unsigned read_failures = 0;
    bool armed = false;

    bool active() const { return armed && committed_transitions >= activate_after; }
    void arm(Fault mode, const Hash256& body = {}, unsigned transitions = 1) {
        fault = mode;
        body_key = body == Hash256{} ? std::string{} : "b:" + utils::hash256_to_hex(body);
        metadata_key.clear();
        activate_after = transitions;
        committed_transitions = 0;
        read_failures = 0;
        armed = true;
    }
};

// Intercept real RocksDB reads only after the active-index transition has
// been durably completed. Validation, all writes and earlier reads stay real.
class ObservedDB final : public rocksdb::StackableDB {
public:
    ObservedDB(rocksdb::DB* db, std::shared_ptr<Probe> probe)
        : StackableDB(db), probe_(std::move(probe)) {}
    using StackableDB::Get;
#if ROCKSDB_MAJOR < 9
    // Before RocksDB 9, ordinary reads dispatch through this overload.
    // In 9+ it is final and forwards to the timestamp overload below.
    rocksdb::Status Get(const rocksdb::ReadOptions& options, rocksdb::ColumnFamilyHandle* family,
                        const rocksdb::Slice& key, rocksdb::PinnableSlice* value) override {
        return Get(options, family, key, value, nullptr);
    }
#endif
    rocksdb::Status Get(const rocksdb::ReadOptions& options, rocksdb::ColumnFamilyHandle* family,
                        const rocksdb::Slice& key, rocksdb::PinnableSlice* value,
                        std::string* timestamp) override {
        if (probe_->active() && key == probe_->body_key) {
            ++probe_->read_failures;
            if (probe_->fault == Fault::MISSING) return rocksdb::Status::NotFound();
            if (probe_->fault == Fault::NON_STANDARD) throw 31;
            return rocksdb::Status::IOError("injected post-commit block read failure");
        }
        return GetBaseDB()->Get(options, family, key, value, timestamp);
    }
    using StackableDB::Delete;
    rocksdb::Status Delete(const rocksdb::WriteOptions& options, rocksdb::ColumnFamilyHandle* family,
                           const rocksdb::Slice& key) override {
        const auto result = GetBaseDB()->Delete(options, family, key);
        if (result.ok() && probe_->armed && key == "active_transition") ++probe_->committed_transitions;
        return result;
    }
private:
    std::shared_ptr<Probe> probe_;
};

class ObservedState final : public state::MemoryStorage {
public:
    explicit ObservedState(std::shared_ptr<Probe> probe) : probe_(std::move(probe)) {}
    bool get_metadata(const std::string& key, bytes& value) const override {
        if (probe_->active() && !probe_->metadata_key.empty() && key == probe_->metadata_key) {
            ++probe_->read_failures;
            if (probe_->fault == Fault::NON_STANDARD) throw 37;
            throw storage::ReadError(storage::ReadStatus::IO_ERROR, "injected post-commit mempool read failure");
        }
        return MemoryStorage::get_metadata(key, value);
    }
private:
    std::shared_ptr<Probe> probe_;
};

void observe(test::TestChain& chain, const std::shared_ptr<Probe>& probe) {
    for (bool blocks : {false, true}) {
        storage::BlockStorageTestAccess::wrap(*chain.blocks, blocks,
            [&](auto* db) { return new ObservedDB(db, probe); });
    }
}

bool matches_gap(const services::ChainMutationEvent& event, uint64_t sequence, Height from_height) {
    return event.kind == Kind::RESCAN_REQUIRED && event.sequence == sequence &&
        event.gap.first_missing_sequence == sequence && event.gap.last_missing_sequence == sequence &&
        event.gap.chain_transition_lost && event.gap.rescan_from_height == from_height;
}

bool durable_tip_is(const test::TestChain& fixture, Height height, const Hash256& hash) {
    Height disk_height = 0;
    Hash256 disk_hash{};
    return fixture.state->get_height() == height && fixture.state->get_tip_hash() == hash &&
        fixture.chain->get_active_tip_snapshot().hash == hash &&
        fixture.blocks->get_active_tip(disk_hash, disk_height) && disk_height == height && disk_hash == hash;
}

void block_event_read_failure(bool activate, Fault fault) {
    auto probe = std::make_shared<Probe>();
    test::TestChain fixture;
    observe(fixture, probe);
    const auto block = fixture.make_block();
    if (activate) fixture.accept(block);
    std::vector<services::ChainMutationEvent> events;
    std::vector<std::thread::id> event_threads;
    node::ChainCoordinator coordinator(fixture.chain, std::make_shared<network::Mempool>(fixture.state),
        [&](services::ChainMutationEvent event) {
            event_threads.push_back(std::this_thread::get_id());
            events.push_back(std::move(event));
            return true;
        });
    expect(coordinator.start(), "start block-event coordinator");
    probe->arm(fault, block.get_hash());
    const std::string label = std::string(activate ? "activate: " : "submit: ") +
        (fault == Fault::MISSING ? "missing: " : fault == Fault::NON_STANDARD ? "non-standard: " : "I/O: ");
    const auto verify = [&](const auto& result) {
        check(probe->active() && probe->read_failures > 0,
              label + "read fault occurs after the durable active-index update");
        check(durable_tip_is(fixture, 1, block.get_hash()), label + "all chain positions retain the committed block");
        check(result.is_accepted() && result.active_tip_changed,
              label + "event preparation failure preserves the successful mutation result");
        check(result.active_height == 1 && result.active_tip_hash == block.get_hash(),
              label + "result describes the tip of this mutation");
        check(result.event_sequence == 1 && coordinator.latest_event_sequence() == 1,
              label + "committed mutation has exactly one event sequence");
        check(events.size() == 1 && matches_gap(events.front(), result.event_sequence, 1),
              label + "unavailable transition bodies produce a complete recovery marker");
    };
    if (activate) {
        verify(coordinator.activate_tip(block.get_hash()));
    } else {
        const auto result = coordinator.process_block(block);
        verify(result);
        check(result.block_is_active && result.block_height == 1,
              label + "block result retains its active placement after event failure");
    }
    probe->armed = false;
    check(fixture.state->has_derived_state_snapshot() && !fixture.chain->find_block_index(block.get_hash())->is_failed(),
          label + "observer preparation does not invalidate chain state or the block index");
    const auto duplicate = coordinator.process_block(block);
    check(duplicate.is_accepted() && !duplicate.active_tip_changed && duplicate.event_sequence == 0 && events.size() == 1,
          label + "retrying an already committed block does not duplicate its event");
    const auto next = fixture.make_block();
    const auto advanced = coordinator.process_block(next);
    check(advanced.is_accepted() && advanced.event_sequence == 2 && events.size() == 2 &&
              events.back().kind == Kind::ACTIVE_CHAIN_CHANGED &&
              events.back().transition.old_tip_hash == block.get_hash() &&
              events.back().transition.connected.size() == 1 && events.back().transition.connected.front().body_available,
          label + "normal ordered transitions resume after the read recovers");
    coordinator.stop();
    check(event_threads.size() == 2 && event_threads.front() != std::this_thread::get_id() &&
              event_threads.front() == event_threads.back(),
          label + "recovery publication and normal publication stay on the same writer thread");
}

void disconnect_event_read_failure() {
    auto probe = std::make_shared<Probe>();
    test::TestChain fixture;
    for (int i = 0; i < 3; ++i) fixture.append(fixture.make_block());
    const auto before = fixture.chain->get_active_tip_snapshot();
    const auto* target = before.index->prev->prev;
    observe(fixture, probe);
    std::vector<services::ChainMutationEvent> events;
    node::ChainCoordinator coordinator(fixture.chain, std::make_shared<network::Mempool>(fixture.state),
        [&](services::ChainMutationEvent event) { events.push_back(std::move(event)); return true; });
    expect(coordinator.start(), "start disconnect-event coordinator");
    probe->arm(Fault::IO_ERROR, before.hash, 2);
    const auto result = coordinator.activate_tip(target->get_block_hash());
    check(probe->committed_transitions == 2 && probe->read_failures > 0,
          "disconnect body read fails only after both disconnects have committed");
    check(result.is_accepted() && result.active_tip_changed && result.active_height == 1 &&
              durable_tip_is(fixture, 1, target->get_block_hash()),
          "disconnect event failure does not turn a committed rollback into a failed activation");
    check(events.size() == 1 && matches_gap(events.front(), result.event_sequence, 2),
          "disconnect recovery starts at the first removed height, not the old tip");
    probe->armed = false;
    coordinator.stop();
}

core::Transaction payment(const core::Transaction& funding) {
    core::Transaction tx;
    tx.timestamp = funding.timestamp + 1;
    // Exercise the height-sensitive survivor path even when the next block
    // does not touch this transaction's confirmed input.
    tx.lock_time = 1;
    tx.inputs.push_back({funding.get_hash(), 0});
    tx.outputs.push_back({funding.outputs.front().value - 10000, test::address_from_seed(43), {}});
    tx.sign(test::make_key(42));
    expect(tx.verify(), "payment has a real Falcon signature");
    return tx;
}

void commitment_cleanup_failure(Fault fault) {
    auto probe = std::make_shared<Probe>();
    test::TestChain fixture(std::make_unique<ObservedState>(probe));
    const auto* genesis = fixture.chain->get_tip();
    fixture.append(fixture.make_block());
    auto competing = fixture.make_block(genesis);
    ++competing.header.timestamp;
    test::TestChain::seal(competing, genesis);
    fixture.accept(competing);
    observe(fixture, probe);
    const auto config = multisig::create_multisig_address(1, 1, {test::address_from_seed(42)});
    const auto angle = core::Transaction::create_angle_commitment(
        Address(config.address), test::hash_from_seed(87), test::make_key(42), Address{},
        consensus::Params().network_id());
    auto pool = std::make_shared<network::Mempool>(fixture.state);
    std::vector<services::ChainMutationEvent> events;
    node::ChainCoordinator coordinator(fixture.chain, pool,
        [&](services::ChainMutationEvent event) { events.push_back(std::move(event)); return true; });
    expect(coordinator.start(), "start commitment-cleanup coordinator");
    expect(coordinator.admit_transaction(angle).is_accepted(), "admit a real signed angle commitment");
    expect(fixture.state->get_angle_store()->has_commitment(angle.get_hash()), "commitment enters the transient verifier store");
    probe->arm(fault);
    probe->metadata_key = "atx:" + utils::hash256_to_hex(angle.get_hash());
    // A competing tip forces a full survivor recheck after the disconnect.
    // Unrelated coinbase-only extensions correctly skip these registry reads.
    const auto result = coordinator.activate_tip(competing.get_hash());
    const std::string label = fault == Fault::IO_ERROR ? "commitment I/O: " : "commitment non-standard: ";
    check(result.is_accepted() && result.active_tip_changed && durable_tip_is(fixture, 1, competing.get_hash()),
          label + "cleanup after revalidation failure preserves the committed block result");
    check(probe->read_failures == 1,
          label + "clearing transient commitments does not repeat the failed database read");
    check(pool->empty() && !fixture.state->get_angle_store()->has_commitment(angle.get_hash()) &&
              fixture.state->get_angle_store()->get_by_multisig_address(angle.multisig_address).empty() &&
              fixture.state->get_angle_store()->get_by_spending_tx(angle.spending_tx_hash).empty(),
          label + "failed revalidation clears both the mempool and transient commitment store");
    check(events.size() == 2 && matches_gap(events.back(), result.event_sequence, 1),
          label + "commitment cleanup does not prevent the recovery event");
    probe->armed = false;
    pool->clear(); // Allow independent cleanup checks to run on the baseline.

    expect(coordinator.admit_transaction(angle).is_accepted(), "commitment can return after recovery");
    // Check ordinary removal independently of the post-commit path. No durable
    // state change is attempted while these historical reads are unavailable.
    probe->arm(fault);
    probe->committed_transitions = 1;
    probe->metadata_key = "atx:" + utils::hash256_to_hex(angle.get_hash());
    bool removed = false;
    try { removed = pool->remove_transaction(angle.get_hash()); } catch (...) {}
    check(removed && probe->read_failures == 0 && pool->empty() &&
              !fixture.state->get_angle_store()->has_commitment(angle.get_hash()),
          label + "ordinary removal completes all indexes without historical reads");
    probe->armed = false;
    check(coordinator.admit_transaction(angle).is_accepted(),
          label + "removal leaves no stale conflict or package index blocking readmission");
    coordinator.stop();
}

void mempool_event_failures() {
    auto probe = std::make_shared<Probe>();
    test::TestChain fixture(std::make_unique<ObservedState>(probe));
    core::Transaction funding;
    for (int i = 1; i <= 100; ++i) {
        const auto block = fixture.make_block();
        if (i == 1) funding = block.transactions.front();
        fixture.append(block);
    }
    observe(fixture, probe);
    auto pool = std::make_shared<network::Mempool>(fixture.state);
    std::vector<services::ChainMutationEvent> events;
    node::ChainCoordinator coordinator(fixture.chain, pool,
        [&](services::ChainMutationEvent event) { events.push_back(std::move(event)); return true; });
    expect(coordinator.start(), "start mempool-event coordinator");
    const auto tx = payment(funding);
    for (const auto fault : {Fault::IO_ERROR, Fault::NON_STANDARD}) {
        expect(coordinator.admit_transaction(tx).is_accepted(), "admit the unconfirmed payment");
        const auto before = fixture.chain->get_active_tip_snapshot();
        const auto next_sequence = coordinator.latest_event_sequence() + 1;
        const auto next = fixture.make_block();
        probe->arm(fault);
        probe->metadata_key = "atx:" + utils::hash256_to_hex(tx.get_hash());
        const auto result = coordinator.process_block(next);
        const std::string label = fault == Fault::IO_ERROR ? "mempool I/O: " : "mempool non-standard: ";
        check(probe->active() && probe->read_failures > 0,
              label + "revalidation read fails after the block commits");
        check(result.is_accepted() && result.active_tip_changed && result.block_is_active &&
                  durable_tip_is(fixture, before.height + 1, next.get_hash()),
              label + "revalidation failure preserves the committed result and active state");
        check(pool->size() == 0 && pool->get_transactions_for_relay().empty(),
              label + "unreconciled pending transactions are removed from mining and relay state");
        check(result.event_sequence == next_sequence && !events.empty() &&
                  matches_gap(events.back(), next_sequence, before.height + 1),
              label + "revalidation failure still publishes its ordered recovery event");
        probe->armed = false;
        // Baseline cleanup lets independent later cases run after recording the
        // defect. The implementation under test must already have emptied it.
        pool->clear();
    }
    expect(coordinator.admit_transaction(tx).is_accepted(), "payment can be resubmitted after local recovery");
    auto confirmed = fixture.make_block(nullptr, 10000);
    confirmed.transactions.push_back(tx);
    test::TestChain::seal(confirmed, fixture.chain->get_tip());
    const auto sequence = coordinator.latest_event_sequence() + 1;
    probe->arm(Fault::IO_ERROR, confirmed.get_hash());
    const auto result = coordinator.process_block(confirmed);
    check(result.is_accepted() && result.block_is_active && !pool->has_transaction(tx.get_hash()),
          "block event read failure cannot leave a confirmed payment in the mempool");
    check(pool->get_transactions_for_relay().empty(),
          "confirmed payment is not relayed after event preparation fails");
    check(result.event_sequence == sequence && !events.empty() && matches_gap(events.back(), sequence, 103),
          "confirmed-payment cleanup failure is announced with the committed height");
    probe->armed = false;
    check(!coordinator.admit_transaction(tx).is_accepted(), "already confirmed payment remains rejected after recovery");
    coordinator.stop();
}
}

int main(int argc, char** argv) {
    try {
        consensus::SelectParams(consensus::Network::LOADNET);
        for (bool activate : {false, true}) {
            for (auto fault : {Fault::IO_ERROR, Fault::MISSING, Fault::NON_STANDARD}) block_event_read_failure(activate, fault);
        }
        disconnect_event_read_failure();
        mempool_event_failures();
        commitment_cleanup_failure(Fault::IO_ERROR);
        commitment_cleanup_failure(Fault::NON_STANDARD);
        size_t failed = 0;
        for (const auto& result : checks) {
            if (!result["passed"].get<bool>()) {
                ++failed;
                std::cerr << "FAILED: " << result["name"] << '\n';
            }
        }
        const nlohmann::json report{{"checks", checks}, {"passed", checks.size() - failed}, {"failed", failed}};
        if (argc > 1) std::ofstream(argv[1]) << report.dump(2) << '\n';
        std::cout << "node commit event checks: " << checks.size() - failed << '/' << checks.size() << '\n';
        return failed ? 1 : 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 2;
    }
}
