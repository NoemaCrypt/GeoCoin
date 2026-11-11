#include "geocoin/consensus/consensus.hpp"
#include "geocoin/crypto/addresses.hpp"
#include "geocoin/node/chain_coordinator.hpp"
#include "geocoin/node/chain_event_dispatcher.hpp"
#include "geocoin/node/node.hpp"
#include "geocoin/network/addrman.hpp"
#include "rpc_test_utils.hpp"
#include "tcp_test_client.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace geocoin::node {

struct NodeTestAccess {
    static bool addrman_contains(Node& node, const std::string& address)
    {
        std::lock_guard<std::mutex> lock(node.addrman_mutex_);
        if (!node.addrman_) {
            return false;
        }
        const auto addresses = node.addrman_->get_all();
        return std::find(addresses.begin(), addresses.end(), address) != addresses.end();
    }

    static bool reconnect_target_contains(Node& node, const std::string& address)
    {
        std::lock_guard<std::mutex> lock(node.outbound_reconnect_targets_mutex_);
        return node.outbound_reconnect_targets_.count(address) > 0;
    }

    static void publish_block_side_effects(Node& node, const core::Block& block, Height height)
    {
        services::ChainMutationEvent event;
        event.sequence = node.chain_coordinator_->latest_event_sequence() + 1;
        event.kind = services::ChainMutationEventKind::ACTIVE_CHAIN_CHANGED;
        event.transition.sequence = event.sequence;
        event.transition.new_height = height;
        event.transition.new_tip_hash = block.get_hash();
        services::ChainBlockChange change;
        change.height = height;
        change.hash = block.get_hash();
        change.block = block;
        change.body_available = true;
        event.transition.connected.push_back(std::move(change));
        node.handle_chain_mutation_event(event);
    }
};

} // namespace geocoin::node

namespace {

void expect(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::filesystem::path make_temp_dir(const std::string& name)
{
    const auto path = std::filesystem::temp_directory_path()
        / (name + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
    return path;
}

geocoin::bytes make_key(uint8_t seed)
{
    geocoin::bytes key(32);
    for (size_t i = 0; i < key.size(); ++i) {
        key[i] = static_cast<geocoin::byte>(seed + i);
    }
    return key;
}

geocoin::Address address_from_key(uint8_t seed)
{
    geocoin::Address address;
    expect(geocoin::crypto::compute_address_from_private_key(make_key(seed), address),
           "address derives from private key");
    return address;
}

geocoin::core::Transaction make_signed_standard_spend(
    const geocoin::core::Transaction& funding,
    uint8_t sender_seed,
    const geocoin::Address& recipient,
    geocoin::Amount amount,
    geocoin::Amount fee
)
{
    expect(!funding.outputs.empty(), "funding transaction has output");
    expect(funding.outputs[0].value > amount + fee, "funding output can pay amount plus fee");

    geocoin::core::Transaction tx;
    tx.version = 2;
    tx.type = geocoin::core::TransactionType::STANDARD;
    tx.timestamp = 123456789;

    geocoin::core::TxInput input;
    input.prev_tx_hash = funding.get_hash();
    input.vout = 0;
    tx.inputs.push_back(input);

    tx.outputs.push_back({amount, recipient, {}});
    tx.outputs.push_back({funding.outputs[0].value - amount - fee, funding.outputs[0].address, {}});
    tx.sign_input(0, make_key(sender_seed));
    return tx;
}

geocoin::node::NodeConfig base_config(const std::filesystem::path& dir)
{
    geocoin::node::NodeConfig config;
    config.data_dir = dir.string();
    config.enable_auto_connect = false;
    config.enable_p2p_listen = false;
    config.enable_mining = false;
    config.outbound_connect_timeout_ms = 1;
    return config;
}

void test_chain_event_dispatcher_never_blocks_writer_and_reports_gap()
{
    std::mutex mutex;
    std::condition_variable cv;
    bool release_first = false;
    bool first_seen = false;
    bool publisher_done = false;
    bool all_published = true;
    std::vector<geocoin::services::ChainMutationEvent> delivered;

    geocoin::node::ChainEventDispatcher dispatcher(
        [&](const geocoin::services::ChainMutationEvent& event) {
            std::unique_lock<std::mutex> lock(mutex);
            delivered.push_back(event);
            if (event.sequence == 1) {
                first_seen = true;
                cv.notify_all();
                cv.wait(lock, [&]() { return release_first; });
            }
            cv.notify_all();
        }
    );

    expect(dispatcher.start(), "chain event dispatcher starts");
    geocoin::services::ChainMutationEvent first;
    first.sequence = 1;
    first.approximate_bytes = 1;
    expect(dispatcher.publish(first), "first observer event publishes");

    {
        std::unique_lock<std::mutex> lock(mutex);
        expect(cv.wait_for(lock, std::chrono::seconds(2), [&]() { return first_seen; }),
               "dispatcher consumer receives first event");
    }

    std::thread publisher([&]() {
        for (uint64_t sequence = 2; sequence <= 301; ++sequence) {
            geocoin::services::ChainMutationEvent event;
            event.sequence = sequence;
            event.approximate_bytes = 1;
            if (sequence == 258) {
                event.kind = geocoin::services::ChainMutationEventKind::ACTIVE_CHAIN_CHANGED;
                event.transition.fork_height = 10;
            }
            all_published = dispatcher.publish(std::move(event)) && all_published;
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            publisher_done = true;
        }
        cv.notify_all();
    });

    bool non_blocking = false;
    {
        std::unique_lock<std::mutex> lock(mutex);
        non_blocking = cv.wait_for(
            lock,
            std::chrono::milliseconds(500),
            [&]() { return publisher_done; }
        );
        release_first = true;
    }
    cv.notify_all();
    publisher.join();

    {
        std::unique_lock<std::mutex> lock(mutex);
        (void)cv.wait_for(lock, std::chrono::seconds(5), [&]() {
            return std::any_of(delivered.begin(), delivered.end(), [](const auto& event) {
                return event.kind == geocoin::services::ChainMutationEventKind::RESCAN_REQUIRED;
            });
        });
    }
    dispatcher.stop();

    const auto gap = std::find_if(delivered.begin(), delivered.end(), [](const auto& event) {
        return event.kind == geocoin::services::ChainMutationEventKind::RESCAN_REQUIRED;
    });
    expect(non_blocking, "saturated observer queue never blocks the chain writer");
    expect(all_published, "dispatcher records saturated publications for recovery");
    expect(gap != delivered.end(), "dispatcher emits a recovery marker after saturation");
    expect(gap->gap.first_missing_sequence == 258 &&
               gap->gap.last_missing_sequence == 301,
           "recovery marker describes the complete missing sequence range");
    expect(gap->gap.chain_transition_lost && gap->gap.rescan_from_height == 11,
           "recovery marker preserves the earliest safe chain rescan height");
}

class SingleAcceptServer {
public:
    SingleAcceptServer() = default;

    ~SingleAcceptServer()
    {
        stop();
    }

    uint16_t port() const
    {
        return listener_.port();
    }

    void start()
    {
        running_.store(true, std::memory_order_relaxed);
        thread_ = std::thread([this]() {
            listener_.accept_into(socket_);
            while (running_.load(std::memory_order_relaxed)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
            }
        });
    }

    void stop()
    {
        if (!running_.exchange(false, std::memory_order_relaxed) && !thread_.joinable()) {
            return;
        }
        socket_.close();
        listener_.close();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

private:
    geocoin::test::BoundListener listener_{1};
    geocoin::test::TcpStream socket_;
    std::atomic<bool> running_{false};
    std::thread thread_;
};

bool wait_for_outbound_attempt(
    geocoin::node::Node& node,
    const std::string& target,
    geocoin::network::PeerOutboundAttemptInfo* out = nullptr
)
{
    for (int i = 0; i < 60; ++i) {
        const auto diagnostics = node.get_peer_manager()->get_connection_diagnostics();
        const auto it = std::find_if(
            diagnostics.outbound_attempts.begin(),
            diagnostics.outbound_attempts.end(),
            [&](const auto& attempt) {
                return attempt.address == target;
            }
        );
        if (it != diagnostics.outbound_attempts.end() && !it->in_progress) {
            if (out) {
                *out = *it;
            }
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

bool wait_for_connected_peer(
    geocoin::node::Node& node,
    const std::string& target
)
{
    for (int i = 0; i < 60; ++i) {
        auto peer = node.get_peer_manager()->get_peer(target);
        if (peer && peer->is_connected()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

bool wait_for_reconnect_target(
    geocoin::node::Node& node,
    const std::string& target
)
{
    for (int i = 0; i < 60; ++i) {
        if (geocoin::node::NodeTestAccess::reconnect_target_contains(node, target)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

void test_connect_to_peer_rejects_when_not_running()
{
    const auto dir = make_temp_dir("geocoin-node-connections-not-running");
    std::error_code ec;
    try {
        geocoin::node::Node node(base_config(dir));
        const std::string target =
            "127.0.0.1:" + std::to_string(geocoin::test::find_free_port());
        expect(!node.connect_to_peer(target), "connect_to_peer rejects before node start");
        expect(!node.is_running(), "node remains stopped after rejected manual connect");
        std::filesystem::remove_all(dir, ec);
    } catch (...) {
        std::filesystem::remove_all(dir, ec);
        throw;
    }
}

void test_connect_only_autoconnect_records_manual_attempt()
{
    const auto dir = make_temp_dir("geocoin-node-connections-connect-only");
    std::error_code ec;
    try {
        const std::string target =
            "127.0.0.1:" + std::to_string(geocoin::test::find_free_port());

        auto config = base_config(dir);
        config.enable_auto_connect = true;
        config.connect_nodes = {target, target};
        config.max_outbound_peers = 1;

        geocoin::node::Node node(config);
        expect(node.start(make_key(0x44)), "connect-only node starts");
        expect(node.get_status().running, "connect-only status reports running");

        geocoin::network::PeerOutboundAttemptInfo attempt;
        expect(wait_for_outbound_attempt(node, target, &attempt),
               "connect-only loop records outbound attempt");
        expect(attempt.manual, "connect-only outbound attempt is marked manual");
        expect(attempt.fail_count >= 1, "failed connect-only target increments fail count");
        expect(!attempt.last_error.empty(), "failed connect-only target records last error");
        expect(node.get_peer_manager()->get_stats().outbound_peers == 0,
               "failed connect-only target leaves no outbound peers");

        node.stop();
        expect(!node.is_running(), "connect-only node stops cleanly");

        const std::filesystem::path peers_file =
            std::filesystem::path(node.get_config().data_dir) / "addresses" / "peers.dat";
        expect(std::filesystem::exists(peers_file),
               "connection loop persists peers.dat on clean shutdown");

        std::filesystem::remove_all(dir, ec);
    } catch (...) {
        std::filesystem::remove_all(dir, ec);
        throw;
    }
}

void test_seed_autoconnect_records_config_seed_attempt()
{
    const auto dir = make_temp_dir("geocoin-node-connections-seed");
    std::error_code ec;
    try {
        const std::string target =
            "127.0.0.1:" + std::to_string(geocoin::test::find_free_port());

        auto config = base_config(dir);
        config.enable_auto_connect = true;
        config.seed_nodes = {target, target};
        config.max_outbound_peers = 1;
        config.dns_seed_delay_ms = 5000;

        geocoin::node::Node node(config);
        expect(node.start(make_key(0x45)), "seed auto-connect node starts");
        const auto status = node.get_status();
        expect(status.running, "seed auto-connect status reports running");
        expect(status.current_height == 0, "seed auto-connect starts from genesis height");
        expect(status.hashrate == 0.0, "seed auto-connect does not mine");
        expect(node.is_initial_block_download(), "genesis-only seed node reports IBD");

        geocoin::network::PeerOutboundAttemptInfo attempt;
        expect(wait_for_outbound_attempt(node, target, &attempt),
               "seed auto-connect loop records outbound attempt");
        expect(attempt.manual, "configured seed target is prioritized as manual target");
        expect(attempt.fail_count >= 1, "failed seed target increments fail count");
        expect(!attempt.last_error.empty(), "failed seed target records last error");
        expect(node.get_peer_manager()->get_stats().outbound_peers == 0,
               "failed seed target leaves no outbound peers");

        node.stop();
        expect(!node.is_running(), "seed auto-connect node stops cleanly");
        std::filesystem::remove_all(dir, ec);
    } catch (...) {
        std::filesystem::remove_all(dir, ec);
        throw;
    }
}

void test_seed_autoconnect_success_records_addrman()
{
    const auto dir = make_temp_dir("geocoin-node-connections-seed-success");
    std::error_code ec;
    try {
        SingleAcceptServer acceptor;
        acceptor.start();
        const std::string target = "127.0.0.1:" + std::to_string(acceptor.port());

        auto config = base_config(dir);
        config.enable_auto_connect = true;
        config.seed_nodes = {target};
        config.max_outbound_peers = 1;
        config.outbound_connect_timeout_ms = 250;
        config.dns_seed_delay_ms = 5000;

        geocoin::node::Node node(config);
        expect(node.start(make_key(0x46)), "seed success auto-connect node starts");
        expect(wait_for_connected_peer(node, target),
               "seed success auto-connect creates outbound peer");
        expect(wait_for_reconnect_target(node, target),
               "successful seed connection completes reconnect bookkeeping");
        expect(geocoin::node::NodeTestAccess::addrman_contains(node, target),
               "successful seed connection records address in AddrMan");

        node.stop();
        acceptor.stop();
        std::filesystem::remove_all(dir, ec);
    } catch (...) {
        std::filesystem::remove_all(dir, ec);
        throw;
    }
}

void test_manual_connect_success_records_addrman()
{
    const auto dir = make_temp_dir("geocoin-node-connections-manual-success");
    std::error_code ec;
    try {
        SingleAcceptServer acceptor;
        acceptor.start();
        const std::string target = "127.0.0.1:" + std::to_string(acceptor.port());

        auto config = base_config(dir);
        config.enable_auto_connect = false;
        config.max_outbound_peers = 1;
        config.outbound_connect_timeout_ms = 250;

        geocoin::node::Node node(config);
        expect(node.start(make_key(0x49)), "manual success node starts");
        expect(node.connect_to_peer(target), "manual connect_to_peer succeeds");
        expect(wait_for_connected_peer(node, target), "manual connect creates outbound peer");
        expect(geocoin::node::NodeTestAccess::addrman_contains(node, target),
               "manual connect records address in AddrMan");
        expect(geocoin::node::NodeTestAccess::reconnect_target_contains(node, target),
               "manual connect records reconnect target");

        const auto diagnostics = node.get_peer_manager()->get_connection_diagnostics();
        const auto it = std::find_if(
            diagnostics.outbound_attempts.begin(),
            diagnostics.outbound_attempts.end(),
            [&](const auto& attempt) {
                return attempt.address == target;
            }
        );
        expect(it != diagnostics.outbound_attempts.end() && it->manual &&
                   it->fail_count == 0 && !it->in_backoff && it->last_success_age_ms >= 0 &&
                   it->last_error.empty(),
               "manual connect records successful outbound diagnostics");

        node.stop();
        acceptor.stop();
        std::filesystem::remove_all(dir, ec);
    } catch (...) {
        std::filesystem::remove_all(dir, ec);
        throw;
    }
}

void test_submit_transaction_lookup_and_block_callbacks()
{
    const auto dir = make_temp_dir("geocoin-node-submit-callbacks");
    std::error_code ec;
    try {
        auto config = base_config(dir);
        config.enable_auto_connect = false;
        config.enable_p2p_listen = false;
        config.mempool_max_size = 1024 * 1024;

        geocoin::node::Node node(config);
        expect(!node.submit_transaction(geocoin::core::Transaction{}),
               "submit_transaction rejects while node is stopped");
        expect(node.start(make_key(0x47)), "facade node starts");
        const auto duplicate_start = node.start_with_result(make_key(0x48));
        expect(!duplicate_start.ok() &&
                   duplicate_start.failure == geocoin::node::NodeStartFailure::INVALID_LIFECYCLE,
               "node rejects duplicate start with a typed lifecycle failure");
        expect(!duplicate_start.automatic_chain_data_recovery_allowed(),
               "a lifecycle failure can never authorize automatic chain recovery");

        const geocoin::Address sender = address_from_key(0x51);
        const geocoin::Address recipient = address_from_key(0x52);
        const geocoin::Amount funding_value = 10 * geocoin::COIN;
        const geocoin::Amount send_value = 2 * geocoin::COIN;
        // One Falcon STANDARD input with two outputs remains below this small
        // base relay fee (0.00005 GC).
        const geocoin::Amount fee = geocoin::consensus::params::MIN_TX_FEE * 10;

        auto chain_state = node.get_chain_state();
        const auto funding = geocoin::core::Transaction::create_coinbase(1, sender, funding_value);
        expect(chain_state->apply_transaction(funding, 1, true), "node facade funding transaction applies");
        chain_state->set_height(geocoin::consensus::params::COINBASE_MATURITY + 5);
        expect(node.get_balance(sender) >= funding.outputs[0].value,
               "node facade get_balance sees chain state");

        geocoin::core::Block genesis;
        expect(node.get_block_by_height(0, genesis), "node facade reads genesis by height");
        geocoin::core::Block same_genesis;
        expect(node.get_block(genesis.get_hash(), same_genesis), "node facade reads genesis by hash");
        const auto unchanged_activation = node.activate_tip(genesis.get_hash());
        expect(unchanged_activation.is_accepted(), "active tip activation uses the mutation port");
        expect(!unchanged_activation.active_tip_changed,
               "activating the current tip does not publish a false transition");

        const auto spend = make_signed_standard_spend(funding, 0x51, recipient, send_value, fee);
        expect(node.submit_transaction(spend), "node facade accepts valid mempool transaction");

        geocoin::core::Transaction found;
        expect(node.get_transaction(spend.get_hash(), found), "node facade finds mempool transaction");
        expect(found.get_hash() == spend.get_hash(), "node facade returns the same mempool tx");
        expect(!node.submit_transaction(spend), "node facade rejects duplicate mempool transaction");

        size_t callback_count = 0;
        geocoin::Height callback_height = 0;
        uint64_t callback_timestamp = 0;
        node.set_transaction_callback([&](const geocoin::core::Transaction& tx,
                                          geocoin::Height height,
                                          uint64_t timestamp) {
            if (tx.get_hash() == spend.get_hash()) {
                ++callback_count;
                callback_height = height;
                callback_timestamp = timestamp;
            }
        });

        geocoin::core::Block accepted;
        accepted.header.version = 1;
        accepted.header.prev_block_hash = genesis.get_hash();
        accepted.header.timestamp = 987654;
        accepted.header.bits = geocoin::consensus::ProofOfWork::get_initial_bits();
        accepted.header.nonce = 42;
        accepted.transactions = {spend};
        accepted.header.merkle_root = accepted.calculate_merkle_root();
        accepted.header.witness_commitment = accepted.calculate_witness_commitment();

        // Production publishes these effects only after ChainCoordinator has
        // committed an active tip. This test exercises the private observer
        // fan-out without restoring the old public post-acceptance bypass.
        geocoin::node::NodeTestAccess::publish_block_side_effects(
            node,
            accepted,
            geocoin::consensus::params::COINBASE_MATURITY + 6
        );
        expect(callback_count == 1, "accepted block notifies transaction callback once");
        expect(callback_height == geocoin::consensus::params::COINBASE_MATURITY + 6,
               "callback receives accepted block height");
        expect(callback_timestamp == accepted.header.timestamp, "callback receives block timestamp");

        node.clear_transaction_callback();
        geocoin::node::NodeTestAccess::publish_block_side_effects(
            node,
            accepted,
            geocoin::consensus::params::COINBASE_MATURITY + 7
        );
        expect(callback_count == 1, "cleared transaction callback is not called again");

        const geocoin::Height height_before_stop = chain_state->get_height();
        const geocoin::Hash256 tip_before_stop = chain_state->get_tip_hash();
        node.stop();
        const auto stopped_restart = node.start_with_result(make_key(0x49));
        expect(!stopped_restart.ok() &&
                   stopped_restart.failure == geocoin::node::NodeStartFailure::INVALID_LIFECYCLE,
               "stopped node instance rejects unsafe in-place restart with a typed failure");
        expect(chain_state->get_height() == height_before_stop &&
                   chain_state->get_tip_hash() == tip_before_stop,
               "rejected restart leaves live chain state unchanged");
        const auto stopped_submission = node.process_block(accepted);
        expect(stopped_submission.outcome == geocoin::services::SubmissionOutcome::UNAVAILABLE,
               "stopped node rejects block submission through mutation port");
        const auto stopped_activation = node.activate_tip(genesis.get_hash());
        expect(stopped_activation.outcome == geocoin::services::SubmissionOutcome::UNAVAILABLE,
               "stopped node rejects tip activation through mutation port");
        expect(callback_count == 1, "stopped node ignores accepted block notification");

        std::filesystem::remove_all(dir, ec);
    } catch (...) {
        std::filesystem::remove_all(dir, ec);
        throw;
    }
}

} // namespace

int main()
{
    try {
        test_connect_to_peer_rejects_when_not_running();
        test_chain_event_dispatcher_never_blocks_writer_and_reports_gap();
        test_connect_only_autoconnect_records_manual_attempt();
        test_seed_autoconnect_records_config_seed_attempt();
        test_seed_autoconnect_success_records_addrman();
        test_manual_connect_success_records_addrman();
        test_submit_transaction_lookup_and_block_callbacks();
        std::cout << "test_node_connections_units: ok\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "test_node_connections_units failed: " << e.what() << "\n";
        return 1;
    }
}
