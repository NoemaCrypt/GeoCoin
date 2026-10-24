#include "chain_test_fixture.hpp"
#include "geocoin/node/chain_event_dispatcher.hpp"
#include "geocoin/node/node.hpp"
#include "nlohmann_json.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
using namespace geocoin;
using namespace std::chrono_literals;
using test::expect;
using Event = services::ChainMutationEvent;
using Kind = services::ChainMutationEventKind;
nlohmann::json checks = nlohmann::json::array();

void check(bool passed, const std::string& name) {
    checks.push_back({{"name", name}, {"passed", passed}});
}

Event event_at(uint64_t sequence, Kind kind = Kind::TRANSACTION_ACCEPTED, Height fork = 0) {
    Event event;
    event.sequence = sequence;
    event.kind = kind;
    event.transition.fork_height = fork;
    event.approximate_bytes = 1;
    return event;
}

void failed_consumer_coalesces_pending_work() {
    std::mutex mutex;
    std::condition_variable cv;
    bool first_seen = false;
    bool release_first = false;
    std::vector<Event> delivered;
    node::ChainEventDispatcher dispatcher([&](const Event& event) {
        std::unique_lock lock(mutex);
        if (event.sequence == 1 && event.kind != Kind::RESCAN_REQUIRED) {
            first_seen = true;
            cv.notify_all();
            cv.wait_for(lock, 3s, [&] { return release_first; });
            throw std::runtime_error("injected observer failure");
        }
        delivered.push_back(event);
        cv.notify_all();
    });
    expect(dispatcher.start(), "start dispatcher for queue recovery");
    expect(dispatcher.publish(event_at(1, Kind::ACTIVE_CHAIN_CHANGED, 10)), "publish first event");
    {
        std::unique_lock lock(mutex);
        expect(cv.wait_for(lock, 2s, [&] { return first_seen; }), "consumer receives first event");
    }
    bool published = true;
    const auto started = std::chrono::steady_clock::now();
    for (uint64_t sequence = 2; sequence <= 300; ++sequence) {
        // This later reorganization is already represented by a saturation
        // marker when the earlier in-flight event fails.
        published = dispatcher.publish(sequence == 260
            ? event_at(sequence, Kind::ACTIVE_CHAIN_CHANGED, 4)
            : event_at(sequence)) && published;
    }
    check(published && std::chrono::steady_clock::now() - started < 500ms,
          "blocked observer does not block the writer, including saturated publications");
    {
        std::lock_guard lock(mutex);
        release_first = true;
    }
    cv.notify_all();
    {
        std::unique_lock lock(mutex);
        cv.wait_for(lock, 3s, [&] {
            return std::any_of(delivered.begin(), delivered.end(), [](const Event& event) {
                return event.kind == Kind::RESCAN_REQUIRED;
            });
        });
    }
    expect(dispatcher.publish(event_at(301)), "publish after recovery");
    {
        std::unique_lock lock(mutex);
        cv.wait_for(lock, 2s, [&] { return !delivered.empty() && delivered.back().sequence == 301; });
    }
    dispatcher.stop();
    const auto gap = std::find_if(delivered.begin(), delivered.end(), [](const Event& event) {
        return event.kind == Kind::RESCAN_REQUIRED;
    });
    check(gap != delivered.end() && gap->gap.first_missing_sequence == 1 &&
              gap->gap.last_missing_sequence == 300 && gap->sequence == 300,
          "consumer failure and existing overflow become one complete sequence range");
    check(gap != delivered.end() && gap->gap.chain_transition_lost && gap->gap.rescan_from_height == 5,
          "recovery keeps the earliest fork height across failed, queued and overflowed work");
    check(delivered.size() == 2 && delivered.front().kind == Kind::RESCAN_REQUIRED &&
              delivered.back().sequence == 301,
          "later event delivery resumes only after the recovery barrier");
}

void persistent_failure_stops_and_preserves_recovery() {
    std::mutex mutex;
    std::condition_variable cv;
    bool available = false;
    size_t failed_markers = 0;
    std::vector<Event> delivered;
    node::ChainEventDispatcher dispatcher([&](const Event& event) {
        std::lock_guard lock(mutex);
        if (!available) {
            if (event.kind == Kind::RESCAN_REQUIRED) ++failed_markers;
            cv.notify_all();
            throw 7; // Exercise the non-standard exception path too.
        }
        delivered.push_back(event);
        cv.notify_all();
    });
    expect(dispatcher.start(), "start dispatcher for persistent failure");
    expect(dispatcher.publish(event_at(50, Kind::ACTIVE_CHAIN_CHANGED, 7)), "publish last chain event");
    {
        std::unique_lock lock(mutex);
        check(cv.wait_for(lock, 3s, [&] { return failed_markers > 0; }),
              "failure of the last event schedules recovery without any later publication");
        const auto before = failed_markers;
        check(!cv.wait_for(lock, 150ms, [&] { return failed_markers > before; }),
              "persistent observer failure does not spin through recovery attempts");
    }
    const auto started = std::chrono::steady_clock::now();
    dispatcher.stop();
    check(std::chrono::steady_clock::now() - started < 500ms && !dispatcher.is_running(),
          "shutdown interrupts the retry delay and finishes despite a failing consumer");
    check(!dispatcher.publish(event_at(51)), "stopped dispatcher rejects new publications");
    {
        std::lock_guard lock(mutex);
        available = true;
    }
    expect(dispatcher.start(), "restart dispatcher after consumer recovery");
    {
        std::unique_lock lock(mutex);
        cv.wait_for(lock, 2s, [&] { return !delivered.empty(); });
    }
    dispatcher.stop();
    check(delivered.size() == 1 && delivered.front().kind == Kind::RESCAN_REQUIRED &&
              delivered.front().gap.first_missing_sequence == 50 &&
              delivered.front().gap.last_missing_sequence == 50 &&
              delivered.front().gap.chain_transition_lost && delivered.front().gap.rescan_from_height == 8,
          "dispatcher restart retains the undelivered recovery range and fork height");
}

void failed_recovery_includes_publications_during_delivery() {
    std::mutex mutex;
    std::condition_variable cv;
    bool first_marker_seen = false;
    bool release_marker = false;
    size_t original_attempts = 0;
    size_t marker_attempts = 0;
    std::vector<Event> delivered;
    node::ChainEventDispatcher dispatcher([&](const Event& event) {
        std::unique_lock lock(mutex);
        if (event.kind == Kind::ACTIVE_CHAIN_CHANGED && event.sequence == 1) {
            ++original_attempts;
            throw std::runtime_error("injected initial delivery failure");
        }
        if (event.kind == Kind::RESCAN_REQUIRED && ++marker_attempts == 1) {
            first_marker_seen = true;
            cv.notify_all();
            cv.wait_for(lock, 3s, [&] { return release_marker; });
            throw std::runtime_error("injected recovery delivery failure");
        }
        delivered.push_back(event);
        cv.notify_all();
    });
    expect(dispatcher.start(), "start dispatcher for in-flight recovery");
    expect(dispatcher.publish(event_at(1, Kind::ACTIVE_CHAIN_CHANGED, 10)), "publish initial transition");
    {
        std::unique_lock lock(mutex);
        expect(cv.wait_for(lock, 3s, [&] { return first_marker_seen; }), "first recovery attempt begins");
    }
    const auto started = std::chrono::steady_clock::now();
    check(dispatcher.publish(event_at(2)) && dispatcher.publish(event_at(3, Kind::ACTIVE_CHAIN_CHANGED, 2)) &&
              std::chrono::steady_clock::now() - started < 500ms,
          "writer can publish while a recovery callback is in flight");
    {
        std::lock_guard lock(mutex);
        release_marker = true;
    }
    cv.notify_all();
    {
        std::unique_lock lock(mutex);
        cv.wait_for(lock, 3s, [&] { return !delivered.empty(); });
    }
    expect(dispatcher.publish(event_at(4)), "publish after repeated recovery");
    {
        std::unique_lock lock(mutex);
        cv.wait_for(lock, 2s, [&] { return !delivered.empty() && delivered.back().sequence == 4; });
    }
    dispatcher.stop();
    check(!delivered.empty() && delivered.front().kind == Kind::RESCAN_REQUIRED &&
              delivered.front().gap.first_missing_sequence == 1 && delivered.front().gap.last_missing_sequence == 3 &&
              delivered.front().gap.chain_transition_lost && delivered.front().gap.rescan_from_height == 3,
          "failed recovery absorbs newer publications and their earlier rescan height");
    check(delivered.size() == 2 && delivered.back().sequence == 4,
          "publications during failed recovery cannot overtake its replacement marker");
    check(original_attempts == 1 && marker_attempts == 2,
          "recovery retries the marker without repeating partially applied original callbacks");
}

void shutdown_drains_a_new_failure() {
    std::mutex mutex;
    std::condition_variable cv;
    bool first_seen = false;
    bool release_first = false;
    std::vector<Event> delivered;
    node::ChainEventDispatcher dispatcher([&](const Event& event) {
        std::unique_lock lock(mutex);
        if (event.kind == Kind::TRANSACTION_ACCEPTED && event.sequence == 1) {
            first_seen = true;
            cv.notify_all();
            cv.wait_for(lock, 3s, [&] { return release_first; });
            throw 23;
        }
        delivered.push_back(event);
    });
    expect(dispatcher.start(), "start dispatcher for shutdown drain");
    expect(dispatcher.publish(event_at(1)) && dispatcher.publish(event_at(2)), "publish work before shutdown");
    {
        std::unique_lock lock(mutex);
        expect(cv.wait_for(lock, 2s, [&] { return first_seen; }), "shutdown fixture callback begins");
    }
    const auto started = std::chrono::steady_clock::now();
    std::thread stopper([&] { dispatcher.stop(); });
    while (dispatcher.is_running() && std::chrono::steady_clock::now() - started < 1s) {
        std::this_thread::sleep_for(1ms);
    }
    check(!dispatcher.is_running() && !dispatcher.publish(event_at(3)),
          "shutdown closes publication before draining the in-flight callback");
    {
        std::lock_guard lock(mutex);
        release_first = true;
    }
    cv.notify_all();
    stopper.join();
    check(std::chrono::steady_clock::now() - started < 500ms,
          "new failure during shutdown receives an immediate recovery attempt");
    check(delivered.size() == 1 && delivered.front().kind == Kind::RESCAN_REQUIRED &&
              delivered.front().gap.first_missing_sequence == 1 && delivered.front().gap.last_missing_sequence == 2 &&
              !delivered.front().gap.chain_transition_lost && delivered.front().gap.rescan_from_height == 0,
          "shutdown drains failed transaction events with a mempool-only recovery marker");
}

core::Block next_block(node::Node& node) {
    auto chain = node.get_blockchain();
    const auto* previous = chain->get_tip();
    core::Block block;
    block.header.version = 1;
    block.header.prev_block_hash = previous->get_block_hash();
    block.header.timestamp = previous->get_block_time() + consensus::params::TARGET_BLOCK_TIME;
    block.header.bits = consensus::calculate_next_difficulty(previous, block.header.timestamp);
    block.transactions.push_back(core::Transaction::create_coinbase(
        previous->height + 1, test::address_from_seed(42),
        consensus::ProofOfWork::calculate_block_reward(previous->height + 1),
        0, node.get_chain_state()->get_foundation_address()));
    block.transactions.front().timestamp = block.header.timestamp;
    test::TestChain::seal(block, previous);
    return block;
}

void node_observers_are_independent(bool standard_exception) {
    const auto directory = test::make_temp_dir("geocoin-node-event-recovery");
    {
        std::mutex mutex;
        std::condition_variable cv;
        std::vector<Event> delivered;
        bool throw_chain_callback = false;
        size_t transaction_callbacks = 0;
        size_t chain_failures = 0;
        const auto fail = [&] {
            if (standard_exception) throw std::runtime_error("injected node observer failure");
            throw 19;
        };
        node::NodeConfig config;
        config.data_dir = directory.string();
        config.network = "degree-load";
        config.enable_auto_connect = false;
        config.enable_p2p_listen = false;
        node::Node node(config);
        expect(node.start(test::make_key(44)), "start real node for observer recovery");
        node.set_transaction_callback([&](const core::Transaction&, Height, uint64_t) {
            std::lock_guard lock(mutex);
            ++transaction_callbacks;
            fail();
        });
        node.set_chain_mutation_event_callback([&](const Event& event) {
            std::lock_guard lock(mutex);
            if (throw_chain_callback && event.kind == Kind::ACTIVE_CHAIN_CHANGED) {
                ++chain_failures;
                fail();
            }
            delivered.push_back(event);
            cv.notify_all();
        });

        const auto first = next_block(node);
        const auto first_result = node.process_block(first);
        const std::string label = standard_exception ? "std exception: " : "non-standard exception: ";
        check(first_result.is_accepted() && first_result.block_is_active,
              label + "observer failure does not change the committed block result");
        {
            std::unique_lock lock(mutex);
            cv.wait_for(lock, 1s, [&] { return !delivered.empty(); });
            check(transaction_callbacks == 1, label + "transaction observer was invoked");
            check(!delivered.empty() && delivered.front().kind == Kind::ACTIVE_CHAIN_CHANGED &&
                      delivered.front().sequence == first_result.event_sequence &&
                      delivered.front().transition.new_tip_hash == first.get_hash(),
                  label + "transaction observer failure does not suppress the complete chain event");
            throw_chain_callback = true;
        }
        node.clear_transaction_callback();
        const auto second = next_block(node);
        const auto second_result = node.process_block(second);
        {
            std::unique_lock lock(mutex);
            cv.wait_for(lock, 3s, [&] {
                return !delivered.empty() && delivered.back().kind == Kind::RESCAN_REQUIRED;
            });
            check(chain_failures == 1, label + "chain observer failed exactly once on the second block");
            check(!delivered.empty() && delivered.back().kind == Kind::RESCAN_REQUIRED &&
                      delivered.back().gap.first_missing_sequence == second_result.event_sequence &&
                      delivered.back().gap.last_missing_sequence == second_result.event_sequence &&
                      delivered.back().gap.chain_transition_lost && delivered.back().gap.rescan_from_height == 2,
                  label + "chain observer receives a recovery marker after its exception");
        }
        check(second_result.is_accepted() && node.is_running() &&
                  node.get_blockchain()->get_active_tip_snapshot().hash == second.get_hash(),
              label + "node and committed state remain usable after both observer failures");
        node.stop();
    }
    std::error_code error;
    std::filesystem::remove_all(directory, error);
}
}

int main(int argc, char** argv) {
    try {
        consensus::SelectParams(consensus::Network::LOADNET);
        failed_consumer_coalesces_pending_work();
        persistent_failure_stops_and_preserves_recovery();
        failed_recovery_includes_publications_during_delivery();
        shutdown_drains_a_new_failure();
        node_observers_are_independent(true);
        node_observers_are_independent(false);
        size_t failed = 0;
        for (const auto& result : checks) {
            if (!result["passed"].get<bool>()) {
                ++failed;
                std::cerr << "FAILED: " << result["name"] << '\n';
            }
        }
        const nlohmann::json report{{"checks", checks}, {"passed", checks.size() - failed}, {"failed", failed}};
        if (argc > 1) std::ofstream(argv[1]) << report.dump(2) << '\n';
        std::cout << "node event recovery checks: " << checks.size() - failed << '/' << checks.size() << '\n';
        return failed ? 1 : 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 2;
    }
}
