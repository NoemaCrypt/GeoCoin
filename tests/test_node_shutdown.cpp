#include "chain_test_fixture.hpp"
#include "geocoin/node/node.hpp"
#include "geocoin/node/node_connection_plan.hpp"
#include "nlohmann_json.hpp"
#include "tcp_test_client.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace geocoin;
using namespace std::chrono_literals;
using test::expect;
nlohmann::json checks = nlohmann::json::array();

void check(bool passed, const std::string& name) {
    checks.push_back({{"name", name}, {"passed", passed}});
}

template <class Predicate>
bool wait_until(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

node::NodeConfig config_at(const std::string& directory) {
    node::NodeConfig config;
    config.data_dir = directory;
    config.network = "degree-load";
    config.enable_auto_connect = false;
    config.enable_p2p_listen = false;
    return config;
}

core::Block next_block(node::Node& node) {
    const auto* previous = node.get_blockchain()->get_tip();
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

// The Python parent bounds each process. A lock-order regression can deadlock
// a real callback and stop(), so the test must not try to detach live Node threads.
void observer_during_stop(const std::string& directory, const std::string& operation) {
    std::mutex mutex;
    std::condition_variable ready;
    bool entered = false;
    bool release = false;
    bool callback_ok = false;
    size_t callbacks = 0;
    uint64_t delivered_sequence = 0;
    node::Node node(config_at(directory));
    expect(node.start(test::make_key(44)), "start node for shutdown observer");
    const auto original_config = node.get_config();
    node.set_chain_mutation_event_callback([&](const services::ChainMutationEvent& event) {
        {
            std::unique_lock lock(mutex);
            entered = true;
            ready.notify_all();
            ready.wait(lock, [&] { return release; });
        }
        ++callbacks;
        delivered_sequence = event.sequence;
        if (operation == "config") {
            const auto config = node.get_config();
            callback_ok = config.network == original_config.network &&
                config.data_dir == original_config.data_dir &&
                config.mining_threads == original_config.mining_threads;
        } else if (operation == "connect") {
            callback_ok = !node.connect_to_peer("127.0.0.1:1");
        } else if (operation == "start") {
            callback_ok = node.start_with_result(test::make_key(45)).failure ==
                node::NodeStartFailure::INVALID_LIFECYCLE;
        } else {
            callback_ok = !node.is_running();
        }
    });

    const auto block = next_block(node);
    const auto submitted = node.process_block(block);
    check(submitted.is_accepted() && submitted.block_is_active && submitted.event_sequence != 0,
          "real block commits before observer shutdown");
    {
        std::unique_lock lock(mutex);
        expect(ready.wait_for(lock, 3s, [&] { return entered; }), "observer entered");
    }
    auto stopped = std::async(std::launch::async, [&] { node.stop(); });
    expect(wait_until([&] { return !node.is_running(); }), "shutdown closes running gate");
    check(stopped.wait_for(50ms) == std::future_status::timeout,
          "stop waits for the in-flight observer");
    check(node.process_block(block).outcome == services::SubmissionOutcome::UNAVAILABLE &&
              node.activate_tip(block.get_hash()).outcome == services::SubmissionOutcome::UNAVAILABLE &&
              node.admit_transaction(core::Transaction{}).outcome == services::SubmissionOutcome::UNAVAILABLE,
          "shutdown rejects new chain and mempool mutations before draining");

    std::atomic<bool> second_entered{false};
    auto also_stopped = std::async(std::launch::async, [&] {
        second_entered = true;
        node.stop();
    });
    expect(wait_until([&] { return second_entered.load(); }), "second stop entered");
    check(also_stopped.wait_for(50ms) == std::future_status::timeout,
          "a concurrent stop also waits until observer delivery finishes");
    {
        std::lock_guard lock(mutex);
        release = true;
    }
    ready.notify_all();
    stopped.get();
    also_stopped.get();
    check(callback_ok, "observer can use the " + operation + " service during shutdown");
    check(callbacks == 1 && delivered_sequence == submitted.event_sequence,
          "shutdown delivers the committed event once with its original sequence");
    check(!node.is_running() && node.get_status().current_height == 1 &&
              node.get_chain_state()->get_tip_hash() == block.get_hash(),
          "completed shutdown retains the committed chain state");
    check(node.start_with_result(test::make_key(46)).failure == node::NodeStartFailure::INVALID_LIFECYCLE,
          "stopped Node remains non-restartable");
    node.stop();
    check(node.get_peer_manager()->get_connection_diagnostics().outbound_attempts.empty(),
          "shutdown does not create a manual outbound attempt");
}

// Fill a local listener's accept backlog. The node then has one genuine TCP
// connection in progress, with a bounded timeout, when stop() is requested.
class StalledListener {
public:
    StalledListener() : listener_(1) {
        bool full = false;
        for (size_t i = 0; i < 16; ++i) {
            test::TcpStream socket;
            bool timed_out = false;
            if (!socket.connect_within("127.0.0.1", port(), 100, timed_out)) {
                expect(timed_out, "fill local TCP accept backlog");
                full = true;
                break;
            }
            sockets_.push_back(std::move(socket));
        }
        expect(full, "local listener backlog is full");
    }

    uint16_t port() const { return listener_.port(); }

private:
    test::BoundListener listener_;
    std::vector<test::TcpStream> sockets_;
};

void cancel_pending_connections(const std::string& directory, bool connect_only) {
    const auto plan = node::detail::build_peer_bootstrap_plan(config_at(directory), connect_only, false);
    check(plan.default_seed_nodes.empty(), "LOADNET bootstrap contains no public seed endpoints");
    // Keep this regression entirely local even on the original faulty code.
    if (!plan.default_seed_nodes.empty()) return;
    StalledListener listener;
    std::vector<std::string> targets;
    for (unsigned i = 1; i <= 3; ++i) {
        targets.push_back("127.0.0." + std::to_string(i) + ":" + std::to_string(listener.port()));
    }
    auto config = config_at(directory);
    config.enable_auto_connect = true;
    config.outbound_connect_timeout_ms = 500;
    config.dns_seed_delay_ms = 5000;
    if (connect_only) config.connect_nodes = targets;
    else config.seed_nodes = targets;
    node::Node node(config);
    expect(node.start(test::make_key(44)), "start node with local connection targets");
    const auto peers = node.get_peer_manager();
    expect(wait_until([&] {
        const auto attempts = peers->get_connection_diagnostics().outbound_attempts;
        return std::any_of(attempts.begin(), attempts.end(), [&](const auto& attempt) {
            return attempt.address == targets.front() && attempt.in_progress;
        });
    }), "first outbound connection is in progress");
    const auto started = std::chrono::steady_clock::now();
    node.stop();
    const auto attempts = peers->get_connection_diagnostics().outbound_attempts;
    check(attempts.size() == 1 && attempts.front().address == targets.front(),
          "shutdown cancels remaining connection targets after the in-flight attempt");
    check(attempts.size() == 1 && !attempts.front().in_progress && attempts.front().fail_count == 1,
          "in-flight connection completes with accurate failure diagnostics");
    check(std::chrono::steady_clock::now() - started < 1500ms,
          "shutdown does not wait for the remaining TCP timeouts or DNS delay");
    check(!node.is_running() && peers->get_peer_count() == 0,
          "connection worker is joined and peers are disconnected");
    check(std::filesystem::exists(std::filesystem::path(node.get_config().data_dir) / "addresses/peers.dat"),
          "connection shutdown still persists the peer address cache");
}

void network_bootstrap_plans() {
    for (const auto network : {consensus::Network::MAIN, consensus::Network::TESTNET,
                               consensus::Network::LOADNET}) {
        consensus::SelectParams(network);
        auto config = config_at("unused");
        config.network = consensus::Params().network_id();
        config.seed_nodes = {"127.0.0.1:14300"};
        const bool testnet = network == consensus::Network::TESTNET;
        const auto plan = node::detail::build_peer_bootstrap_plan(config, false, testnet);
        const std::string label = config.network + ": ";
        if (network == consensus::Network::LOADNET) {
            check(plan.default_seed_nodes.empty(), label + "public discovery is disabled");
        } else {
            const std::string suffix = testnet ? ":13030" : ":3030";
            check(!plan.default_seed_nodes.empty() &&
                      std::all_of(plan.default_seed_nodes.begin(), plan.default_seed_nodes.end(),
                          [&](const auto& endpoint) { return endpoint.ends_with(suffix); }),
                  label + "public seeds retain their network-specific ports");
        }
        const std::string local = "127.0.0.1:" + std::to_string(consensus::Params().default_port());
        check(!plan.base_priority_targets.empty() &&
                  plan.base_priority_targets.front() == config.seed_nodes.front() &&
                  std::find(plan.local_seed_nodes.begin(), plan.local_seed_nodes.end(), local) !=
                      plan.local_seed_nodes.end(),
              label + "explicit peers keep priority and local fallback uses the network port");
        config.connect_nodes = {"127.0.0.2:14300", "127.0.0.2:14300", "127.0.0.3:14300"};
        const auto only = node::detail::build_peer_bootstrap_plan(config, true, testnet);
        check(only.default_seed_nodes.empty() && only.local_seed_nodes.empty() &&
                  only.base_priority_targets == std::vector<std::string>{"127.0.0.2:14300", "127.0.0.3:14300"},
              label + "connect-only mode retains just the unique explicit targets");
    }
}
}

int main(int argc, char** argv) {
    if (argc != 4) return 2;
    try {
        consensus::SelectParams(consensus::Network::LOADNET);
        const std::string scenario = argv[1];
        if (scenario == "bootstrap") {
            network_bootstrap_plans();
        } else if (scenario == "connect-only" || scenario == "seed-targets") {
            cancel_pending_connections(argv[2], scenario == "connect-only");
        } else {
            observer_during_stop(argv[2], scenario);
        }
    } catch (const std::exception& error) {
        check(false, std::string("unexpected exception: ") + error.what());
    }
    size_t passed = 0;
    for (const auto& result : checks) {
        if (result["passed"].get<bool>()) ++passed;
        else std::cerr << "FAILED: " << result["name"] << '\n';
    }
    const nlohmann::json report{{"checks", checks}, {"passed", passed}, {"failed", checks.size() - passed}};
    std::ofstream(argv[3]) << report.dump(2) << '\n';
    std::cout << argv[1] << ": " << passed << '/' << checks.size() << '\n';
    return passed == checks.size() ? 0 : 1;
}
