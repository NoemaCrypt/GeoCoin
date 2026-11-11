#include "geocoin/node/node.hpp"
#include "geocoin/core/merkle.hpp"
#include "geocoin/consensus/consensus.hpp"
#include "geocoin/consensus/optimized_miner.hpp"
#include "geocoin/consensus/proof_of_work.hpp"
#include "geocoin/rpc/rpc_mining_helpers.hpp"
#include "geocoin/wallet/wallet.hpp"
#include "secure_channel_test_fixture.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

#ifndef _WIN32
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace geocoin::core {

struct BlockchainTestAccess {
    static Hash256 set_fake_active_tip(Blockchain& blockchain, Height height)
    {
        std::lock_guard lock(blockchain.mutex_);
        auto* prev = blockchain.chain_active_.tip();

        Block block;
        block.header.version = 1;
        block.header.prev_block_hash = prev ? prev->get_block_hash() : Hash256{};
        block.header.timestamp = static_cast<uint32_t>(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()
            ).count() - 1
        );
        block.header.bits = prev ? prev->get_bits() : consensus::ProofOfWork::get_initial_bits();

        auto index = std::make_unique<chain::BlockIndex>(block.header);
        index->hash = block.get_hash();
        index->prev = prev;
        index->height = height;
        index->status = chain::BlockStatus::VALID_SCRIPTS;
        index->chain_work = prev ? prev->chain_work + uint256(1) : uint256(1);
        index->build_skip();

        const Hash256 hash = index->hash;
        auto* index_ptr = index.get();
        blockchain.block_index_[hash] = std::move(index);
        blockchain.chain_active_.set_tip(index_ptr);
        return hash;
    }
};

} // namespace geocoin::core

namespace geocoin::state {

struct ChainStateTestAccess {
    static void seed_interest_pool(ChainState& state, Amount amount, const Address& holder)
    {
        std::lock_guard<std::recursive_mutex> lock(state.mutex_);
        state.interest_pool_balance_ = amount;

        UTXO utxo;
        utxo.tx_hash = {};
        utxo.tx_hash[0] = 0xe1;
        utxo.vout = 0;
        utxo.value = 200 * COIN;
        utxo.address = holder;
        utxo.height = 1;
        utxo.is_coinbase = false;
        state.add_utxo(utxo);
    }
};

} // namespace geocoin::state

namespace geocoin::node {

struct NodeTestAccess {
    static core::Block create_mining_block(Node& node)
    {
        return node.create_mining_block();
    }

    static uint32_t calculate_next_difficulty(Node& node, Timestamp block_timestamp)
    {
        return node.calculate_next_difficulty(block_timestamp);
    }

    static Height get_best_peer_height(Node& node, Height floor_height)
    {
        return node.get_best_peer_height(floor_height);
    }

    static void set_fake_mining_stats(
        Node& node,
        size_t mining_threads,
        uint64_t total_hashes,
        uint64_t last_reported_hashes,
        double current_hashrate,
        std::chrono::milliseconds elapsed
    ) {
        node.mining_ = true;
        node.mining_threads_.clear();
        node.mining_threads_.resize(mining_threads);
        {
            std::lock_guard<std::mutex> lock(node.mining_stats_mutex_);
            node.total_hashes_.store(total_hashes, std::memory_order_relaxed);
            node.last_reported_hashes_ = last_reported_hashes;
            node.current_hashrate_ = current_hashrate;
            node.mining_last_hashrate_time_ = std::chrono::steady_clock::now() - elapsed;
        }
    }

    static void clear_fake_mining(Node& node)
    {
        node.mining_ = false;
        node.mining_threads_.clear();
        std::lock_guard<std::mutex> lock(node.mining_stats_mutex_);
        node.current_hashrate_ = 0.0;
        node.last_reported_hashes_ = 0;
        node.total_hashes_.store(0, std::memory_order_relaxed);
    }

    // Mirrors what a mining worker does when it terminates on an exception
    // (and what the update loop does on a runtime failure): the flag drops
    // while the std::thread objects stay joinable in mining_threads_.
    static void simulate_worker_self_termination(Node& node)
    {
        node.mining_.store(false, std::memory_order_release);
    }

    static size_t tracked_mining_threads(Node& node)
    {
        std::lock_guard<std::mutex> lock(node.mining_control_mutex_);
        return node.mining_threads_.size();
    }
};

} // namespace geocoin::node

namespace geocoin::network {

struct PeerManagerTestAccess {
    static bool force_authenticated_ready(
        PeerManager& manager,
        const std::string& address,
        Height height
    ) {
        std::lock_guard<std::mutex> lock(manager.mutex_);
        auto it = manager.peers_.find(address);
        if (it == manager.peers_.end() || !it->second.peer) {
            return false;
        }

        auto secure_fixture = geocoin::test::make_secure_channel_fixture();
        if (!it->second.peer->activate_secure_channel(std::move(secure_fixture.peer_keys))) {
            return false;
        }
        it->second.channel_binding = secure_fixture.channel_binding;
        it->second.secure_established = true;

        auto session = std::make_shared<AuthSession>();
        session->session_id = {};
        session->role = AuthRole::PROVER;
        session->status = AuthStatus::COMPLETED;
        session->current_round = 3;
        session->started_at = std::chrono::steady_clock::now();
        session->last_activity = session->started_at;
        session->channel_binding = secure_fixture.channel_binding;
        it->second.auth_session = std::move(session);

        auto info = it->second.peer->get_info();
        info.height = height;
        it->second.peer->update_info(info);
        it->second.peer->set_state(PeerState::READY);
        it->second.last_message_at = std::chrono::steady_clock::now();
        it->second.last_useful_message_at = it->second.last_message_at;
        return true;
    }
};

} // namespace geocoin::network

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
        key[i] = static_cast<uint8_t>(seed + i);
    }
    return key;
}

geocoin::Hash256 hash_from_seed(uint8_t seed)
{
    geocoin::Hash256 hash{};
    for (size_t i = 0; i < hash.size(); ++i) {
        hash[i] = static_cast<geocoin::byte>(seed + i * 17);
    }
    return hash;
}

class SocketPair {
public:
    SocketPair()
    {
#ifndef _WIN32
        int fds[2] = {-1, -1};
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
            throw std::runtime_error("socketpair failed");
        }
        peer_fd_ = fds[0];
        read_fd_ = fds[1];
#else
        throw std::runtime_error("socketpair test is not supported on Windows");
#endif
    }

    ~SocketPair()
    {
#ifndef _WIN32
        if (peer_fd_ >= 0) {
            ::close(peer_fd_);
        }
        if (read_fd_ >= 0) {
            ::close(read_fd_);
        }
#endif
    }

    int take_peer_fd()
    {
        int fd = peer_fd_;
        peer_fd_ = -1;
        return fd;
    }

    bool activate_peer_channel(geocoin::network::Peer& peer)
    {
        auto fixture = geocoin::test::make_secure_channel_fixture();
        remote_channel_ = std::move(fixture.remote_channel);
        if (!peer.activate_secure_channel(std::move(fixture.peer_keys))) {
            remote_channel_.reset();
            return false;
        }
        return true;
    }

private:
    int peer_fd_ = -1;
    int read_fd_ = -1;
    std::unique_ptr<geocoin::network::SecureChannel> remote_channel_;
};

geocoin::node::NodeConfig base_config(const std::filesystem::path& dir)
{
    geocoin::node::NodeConfig config;
    config.data_dir = dir.string();
    config.enable_auto_connect = false;
    config.enable_p2p_listen = false;
    config.enable_mining = false;
    return config;
}

void test_not_running_rejects_start_mining()
{
    const auto dir = make_temp_dir("geocoin-node-mining-not-running");
    std::error_code ec;
    try {
        geocoin::wallet::Wallet wallet;
        expect(wallet.initialize(make_key(42)), "wallet initializes for non-running overload");

        geocoin::node::Node node(base_config(dir));
        expect(!node.start_mining(), "start_mining rejects non-running node");
        expect(!node.start_mining(0, wallet.get_address()),
               "start_mining overload rejects non-running node after clamping zero threads");
        expect(node.get_config().mining_threads == 1,
               "start_mining overload clamps zero requested threads to one");
        node.stop_mining();
        expect(!node.is_mining(), "stop_mining is a no-op when mining is inactive");
        std::filesystem::remove_all(dir, ec);
    } catch (...) {
        std::filesystem::remove_all(dir, ec);
        throw;
    }
}

void test_running_without_address_rejects_start_mining()
{
    const auto dir = make_temp_dir("geocoin-node-mining-no-address");
    std::error_code ec;
    try {
        geocoin::node::Node node(base_config(dir));
        expect(node.start(make_key(1)), "isolated node starts");
        expect(!node.start_mining(), "start_mining rejects missing mining address");
        expect(!node.get_status().mining, "node remains non-mining after missing address");
        node.stop();
        std::filesystem::remove_all(dir, ec);
    } catch (...) {
        std::filesystem::remove_all(dir, ec);
        throw;
    }
}

void test_sync_target_blocks_start_mining()
{
    const auto dir = make_temp_dir("geocoin-node-mining-sync-target");
    std::error_code ec;
    try {
        geocoin::wallet::Wallet wallet;
        expect(wallet.initialize(make_key(43)), "wallet initializes for sync target guard");

        auto config = base_config(dir);
        config.network = "degree-load";
        config.mining_address = wallet.get_address();
        const size_t safe_thread_limit = geocoin::node::cpu_mining_thread_limit();
        config.mining_threads = safe_thread_limit + 10;

        geocoin::node::Node node(config);
        expect(node.start(make_key(3)), "isolated node starts for sync target guard");
        auto block = geocoin::node::NodeTestAccess::create_mining_block(node);
        const auto parent = node.get_blockchain()->get_active_tip_snapshot();
        std::atomic<bool> mining{true};
        std::atomic<uint64_t> hashes{0};
        geocoin::consensus::OptimizedMiner miner;
        expect(miner.mine_block_optimized(block, block.header.bits,
                   static_cast<uint32_t>(parent.median_time_past), parent.hash, parent.height + 1,
                   4096, &mining, -1, &hashes), "mine a valid header for the sync guard");

        SocketPair sockets;
        auto peer = std::make_shared<geocoin::network::Peer>("127.0.0.1:3030", sockets.take_peer_fd());
        expect(sockets.activate_peer_channel(*peer),
               "sync guard peer uses an active secure channel");
        auto info = peer->get_info();
        info.height = 2;
        peer->update_info(info);
        peer->set_state(geocoin::network::PeerState::READY);

        expect(node.get_sync_manager()->start_sync(peer, 2), "sync manager accepts ahead peer");
        expect(!node.get_status().mining_blocked_by_sync,
               "unvalidated discovery cannot pause mining while waiting for headers");
        expect(node.get_sync_manager()->handle_headers(peer, {block.header}),
               "sync guard receives a real validated header before blocking mining");
        const auto status = node.get_status();
        expect(status.syncing, "node status reports sync in progress");
        expect(status.mining_blocked_by_sync, "status blocks mining by sync target");
        expect(!node.start_mining(), "start_mining rejects while sync target is ahead");
        expect(node.get_config().mining_threads == safe_thread_limit,
               "start_mining clamps thread count before sync guard returns");
        expect(!node.is_mining(), "sync target rejection does not start mining thread");

        node.stop();
        std::filesystem::remove_all(dir, ec);
    } catch (...) {
        std::filesystem::remove_all(dir, ec);
        throw;
    }
}

void test_zero_threads_rejects_start_mining()
{
    const auto dir = make_temp_dir("geocoin-node-mining-zero-threads");
    std::error_code ec;
    try {
        geocoin::wallet::Wallet wallet;
        expect(wallet.initialize(make_key(42)), "wallet initializes");

        auto config = base_config(dir);
        config.mining_address = wallet.get_address();
        config.mining_threads = 0;

        geocoin::node::Node node(config);
        expect(node.start(make_key(2)), "isolated node starts for zero-thread guard");
        expect(!node.start_mining(), "start_mining rejects zero mining threads");
        expect(!node.get_status().mining, "node remains non-mining after zero-thread rejection");
        node.stop();
        std::filesystem::remove_all(dir, ec);
    } catch (...) {
        std::filesystem::remove_all(dir, ec);
        throw;
    }
}

void test_mining_block_template_helpers()
{
    const auto dir = make_temp_dir("geocoin-node-mining-template");
    std::error_code ec;
    try {
        geocoin::wallet::Wallet wallet;
        expect(wallet.initialize(make_key(44)), "wallet initializes for mining template");

        auto config = base_config(dir);
        config.mining_address = wallet.get_address();
        config.mining_threads = 1;

        geocoin::node::Node node(config);
        expect(node.start(make_key(5)), "isolated node starts for mining template");
        const auto tip = node.get_blockchain()->get_tip();
        expect(tip != nullptr, "mining template has active tip");

        const auto block = geocoin::node::NodeTestAccess::create_mining_block(node);
        expect(block.header.version == 1, "mining template sets block version");
        expect(block.header.prev_block_hash == tip->get_block_hash(),
               "mining template points to active tip");
        expect(block.header.bits == geocoin::node::NodeTestAccess::calculate_next_difficulty(
                   node,
                   block.header.timestamp
               ),
               "mining template uses node difficulty helper");
        expect(block.header.geo_tag != 0, "mining template computes geo tag from previous tip");
        expect(block.transactions.size() == 1, "empty mempool template contains only coinbase");
        expect(block.transactions[0].is_coinbase(), "first mining transaction is coinbase");
        expect(!block.transactions[0].outputs.empty(), "coinbase has at least one output");
        expect(block.transactions[0].outputs[0].address == wallet.get_address(),
               "coinbase pays configured mining address");

        std::vector<geocoin::Hash256> tx_hashes;
        tx_hashes.reserve(block.transactions.size());
        for (const auto& tx : block.transactions) {
            tx_hashes.push_back(tx.get_hash());
        }
        expect(block.header.merkle_root == geocoin::core::compute_merkle_root(tx_hashes),
               "mining template merkle root matches transactions");
        expect(block.header.witness_commitment == block.calculate_witness_commitment(),
               "mining template witness commitment matches transactions");

        const auto due_height =
            geocoin::consensus::params::INTEREST_POOL_ACTIVATION_HEIGHT +
            geocoin::consensus::params::INTEREST_DISTRIBUTION_INTERVAL;
        const auto fake_tip_hash = geocoin::core::BlockchainTestAccess::set_fake_active_tip(
            *node.get_blockchain(),
            due_height - 1
        );
        node.get_chain_state()->set_height(due_height - 1);
        node.get_chain_state()->set_tip_hash(fake_tip_hash);
        geocoin::state::ChainStateTestAccess::seed_interest_pool(
            *node.get_chain_state(),
            100 * geocoin::COIN,
            wallet.get_address()
        );

        const auto expect_due_interest = [&](const geocoin::core::Block& due_block, const char* label) {
            expect(due_block.header.prev_block_hash == fake_tip_hash, label);
            expect(due_block.transactions.size() >= 2, label);
            expect(due_block.transactions[1].type ==
                       geocoin::core::TransactionType::INTEREST_DISTRIBUTION,
                   label);
            const auto count = std::count_if(
                due_block.transactions.begin(),
                due_block.transactions.end(),
                [](const auto& tx) {
                    return tx.type == geocoin::core::TransactionType::INTEREST_DISTRIBUTION;
                }
            );
            expect(count == 1, label);
            expect(due_block.header.witness_commitment == due_block.calculate_witness_commitment(), label);
        };

        const auto due_node_block = geocoin::node::NodeTestAccess::create_mining_block(node);
        expect_due_interest(due_node_block, "node due-height template includes one interest tx at index 1");

        const auto due_rpc_block = geocoin::rpc::build_block_template(
            node.get_blockchain().get(),
            node.get_chain_state().get(),
            node.get_mempool().get(),
            wallet.get_address()
        );
        expect_due_interest(due_rpc_block, "RPC due-height template includes one interest tx at index 1");

        node.stop();
        std::filesystem::remove_all(dir, ec);
    } catch (...) {
        std::filesystem::remove_all(dir, ec);
        throw;
    }
}

void test_committed_chain_events_are_fifo()
{
    const auto dir = make_temp_dir("geocoin-node-chain-events");
    std::error_code ec;
    try {
        geocoin::wallet::Wallet wallet;
        expect(wallet.initialize(make_key(0x5a)), "chain-event wallet initializes");

        auto config = base_config(dir);
        config.network = "degree-test";
        config.mining_address = wallet.get_address();
        geocoin::node::Node node(config);
        expect(node.start(make_key(0x5b)), "chain-event node starts");

        std::mutex events_mutex;
        std::condition_variable events_ready;
        std::vector<std::pair<uint64_t, geocoin::Height>> events;
        node.set_chain_mutation_event_callback(
            [&](const geocoin::services::ChainMutationEvent& event) {
                if (event.kind !=
                    geocoin::services::ChainMutationEventKind::ACTIVE_CHAIN_CHANGED) {
                    return;
                }
                {
                    std::lock_guard<std::mutex> lock(events_mutex);
                    events.emplace_back(event.sequence, event.transition.new_height);
                }
                events_ready.notify_one();
            }
        );

        auto mine_and_submit = [&]() {
            auto block = geocoin::node::NodeTestAccess::create_mining_block(node);
            const auto parent = node.get_blockchain()->get_active_tip_snapshot();
            expect(parent.index != nullptr, "chain-event template has parent index");
            std::atomic<bool> mining{true};
            std::atomic<uint64_t> hashes{0};
            geocoin::consensus::OptimizedMiner miner;
            expect(miner.mine_block_optimized(
                       block,
                       block.header.bits,
                       static_cast<uint32_t>(parent.median_time_past),
                       parent.hash,
                       parent.height + 1,
                       4096,
                       &mining,
                       -1,
                       &hashes),
                   "chain-event block mines within unit-test budget");
            const auto submission = node.process_block(block);
            expect(submission.is_accepted() && submission.active_tip_changed &&
                       submission.block_is_active,
                   "mined block commits through the node mutation port");
            return submission.event_sequence;
        };

        const uint64_t first_sequence = mine_and_submit();
        const uint64_t second_sequence = mine_and_submit();
        expect(second_sequence > first_sequence,
               "coordinator assigns monotonically increasing event sequences");

        {
            std::unique_lock<std::mutex> lock(events_mutex);
            expect(events_ready.wait_for(lock, std::chrono::seconds(2), [&]() {
                       return events.size() == 2;
                   }),
                   "event dispatcher drains both committed block events");
            expect(events[0] == std::make_pair(first_sequence, geocoin::Height{1}) &&
                       events[1] == std::make_pair(second_sequence, geocoin::Height{2}),
                   "chain observers receive commits in writer order");
        }

        node.clear_chain_mutation_event_callback();
        node.stop();
        std::filesystem::remove_all(dir, ec);
    } catch (...) {
        std::filesystem::remove_all(dir, ec);
        throw;
    }
}

void test_start_stop_mining_with_tiny_cooperative_batch()
{
    const auto dir = make_temp_dir("geocoin-node-mining-start-stop-tiny");
    std::error_code ec;
    try {
        geocoin::wallet::Wallet wallet;
        expect(wallet.initialize(make_key(45)), "wallet initializes for tiny mining lifecycle");

        auto config = base_config(dir);
        config.mining_address = wallet.get_address();
        config.mining_threads = 1;
        config.mining_sha256_backend = "invalid-test-backend";
        config.mining_cooperative_batch_iterations = 1;
        config.mining_cooperative_pause_us = 20000;

        geocoin::node::Node node(config);
        expect(node.start(make_key(6)), "isolated node starts for tiny mining lifecycle");
        expect(node.start_mining(), "start_mining succeeds with one tiny cooperative worker");
        expect(node.is_mining(), "node reports mining after successful start");
        expect(node.get_config().mining_sha256_backend == "auto",
               "unknown mining backend falls back to auto");
        geocoin::wallet::Wallet other_wallet;
        expect(other_wallet.initialize(make_key(46)), "alternate payout wallet initializes");
        expect(!node.start_mining(2, other_wallet.get_address()),
               "duplicate mining start is rejected");
        const auto active_config = node.get_config();
        expect(active_config.mining_threads == 1 &&
                   active_config.mining_address == wallet.get_address(),
               "rejected mining start cannot mutate the active session configuration");

        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        auto status = node.get_status();
        expect(status.mining, "status reports tiny miner active");
        expect(status.mining_threads == 1, "status reports one tiny mining thread");

        std::thread first_stop([&node]() { node.stop_mining(); });
        std::thread second_stop([&node]() { node.stop_mining(); });
        first_stop.join();
        second_stop.join();
        expect(!node.is_mining(), "concurrent stop_mining calls serialize and clear mining flag");
        status = node.get_status();
        expect(!status.mining, "status reports miner stopped");
        expect(status.hashrate == 0.0, "stopped miner reports zero hashrate");

        expect(node.start_mining(), "mining can restart after a concurrent managed stop");
        node.stop_mining();

        node.stop();
        std::filesystem::remove_all(dir, ec);
    } catch (...) {
        std::filesystem::remove_all(dir, ec);
        throw;
    }
}

void test_restart_after_worker_self_termination_reaps_stale_threads()
{
    const auto dir = make_temp_dir("geocoin-node-mining-worker-exit");
    std::error_code ec;
    try {
        geocoin::wallet::Wallet wallet;
        expect(wallet.initialize(make_key(47)), "wallet initializes for worker-exit lifecycle");

        auto config = base_config(dir);
        config.mining_address = wallet.get_address();
        config.mining_threads = 1;
        config.mining_cooperative_batch_iterations = 1;
        config.mining_cooperative_pause_us = 20000;

        geocoin::node::Node node(config);
        expect(node.start(make_key(7)), "isolated node starts for worker-exit lifecycle");
        expect(node.start_mining(), "first mining session starts");
        expect(geocoin::node::NodeTestAccess::tracked_mining_threads(node) == 1,
               "first session tracks one worker");

        // The worker exits on its own; nobody calls stop_mining() in between.
        geocoin::node::NodeTestAccess::simulate_worker_self_termination(node);
        for (int i = 0; i < 100 && node.get_status().mining; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        expect(!node.is_mining(), "self-terminated session reports mining stopped");

        // Regression: restarting used to append new workers behind the stale
        // joinable thread objects, so get_status() over-reported the thread
        // count and stale handles lingered until the next stop_mining().
        expect(node.start_mining(), "mining restarts after a worker self-terminated");
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const auto status = node.get_status();
        expect(status.mining, "restarted session reports mining");
        expect(status.mining_threads == 1, "restarted session reports only its own worker");
        expect(geocoin::node::NodeTestAccess::tracked_mining_threads(node) == 1,
               "stale worker handles are reaped before a new session starts");

        node.stop_mining();
        expect(!node.is_mining(), "restarted session stops cleanly");
        node.stop();
        std::filesystem::remove_all(dir, ec);
    } catch (...) {
        std::filesystem::remove_all(dir, ec);
        throw;
    }
}

void test_status_ibd_and_pruning_guards()
{
    const auto dir = make_temp_dir("geocoin-node-status-pruning");
    std::error_code ec;
    try {
        {
            geocoin::node::Node node(base_config(dir));
            expect(!node.is_running(), "fresh node is not running before start");
            const auto info = node.get_pruned_data_info();
            expect(!info.enabled && info.first_available_height == 0,
                   "full non-started node reports unpruned data info");
            expect(!node.is_height_pruned_for_api(0), "full node never reports height pruned");
            geocoin::Height out_height = 123;
            expect(!node.is_block_pruned_for_api({}, &out_height),
                   "full non-started node reports block unpruned");
            expect(out_height == 123, "unpruned missing block leaves output height untouched");
        }

        auto config = base_config(dir);
        config.node_type = geocoin::node::NodeType::PRUNED;
        config.pruned_block_retention = 10;
        config.pruned_delete_old_blocks = false;
        config.pruned_delete_old_indexes = false;

        geocoin::node::Node node(config);
        expect(node.start(make_key(4)), "isolated pruned node starts");
        const auto status = node.get_status();
        expect(status.running, "started node status reports running");
        expect(status.current_height == 0, "started isolated node begins at genesis height");
        expect(status.target_height == 0, "started isolated node has no sync target");
        expect(status.initial_sync_complete, "started isolated node has no peer sync pending");
        expect(!status.mining && status.hashrate == 0.0, "non-mining status has zero hashrate");
        expect(node.is_initial_block_download(), "genesis-only node reports initial block download");

        const auto pruned = node.get_pruned_data_info();
        expect(pruned.enabled && pruned.retention == 10 && pruned.tip_height == 0,
               "pruned node reports configured retention and tip");
        expect(pruned.first_available_height == 0, "genesis pruned node first available height is zero");
        expect(!node.is_height_pruned_for_api(0), "genesis height is available on pruned node");

        geocoin::Height missing_height = 77;
        expect(!node.is_block_pruned_for_api(hash_from_seed(0x44), &missing_height),
               "missing block is not reported as pruned");
        expect(missing_height == 77, "missing block leaves output height untouched");

        const std::string error = node.describe_pruned_api_error(5);
        expect(error.find("10") != std::string::npos, "pruned error includes retention");
        expect(error.find("5") != std::string::npos, "pruned error includes requested height");

        node.stop();
        std::filesystem::remove_all(dir, ec);
    } catch (...) {
        std::filesystem::remove_all(dir, ec);
        throw;
    }
}

void test_status_treats_authenticated_peer_height_as_untrusted_hint()
{
    const auto dir = make_temp_dir("geocoin-node-status-peer-height");
    std::error_code ec;
    try {
        geocoin::node::Node node(base_config(dir));
        expect(node.start(make_key(0x54)), "isolated node starts for authenticated peer status test");

        SocketPair sockets;
        auto peer_manager = node.get_peer_manager();
        const std::string address = "127.0.0.88:3030";
        expect(peer_manager->add_inbound_peer(sockets.take_peer_fd(), address, make_key(0x55)),
               "inbound peer can be attached for status height test");
        expect(geocoin::network::PeerManagerTestAccess::force_authenticated_ready(
                   *peer_manager,
                   address,
                   17
               ),
               "test peer can be marked authenticated and ready");

        expect(geocoin::node::NodeTestAccess::get_best_peer_height(node, 3) == 17,
               "best peer height uses authenticated peer height");
        const auto status = node.get_status();
        expect(status.current_height == 0, "started status peer-height test begins at genesis height");
        expect(status.connected_peers == 1, "status reports connected peer count");
        expect(status.authenticated_peers == 1, "status reports authenticated peer count");
        expect(status.best_peer_height == 17,
               "status exposes the peer-advertised height as diagnostics");
        expect(status.target_height == 0,
               "peer-advertised height does not become a validated sync target");
        expect(status.initial_sync_complete,
               "an advertisement alone does not falsify validated sync completion");
        expect(!status.mining_blocked_by_sync,
               "an advertisement alone cannot permanently disable mining");

        node.stop();
        std::filesystem::remove_all(dir, ec);
    } catch (...) {
        std::filesystem::remove_all(dir, ec);
        throw;
    }
}

void test_status_hashrate_without_starting_miner()
{
    const auto dir = make_temp_dir("geocoin-node-status-hashrate");
    std::error_code ec;
    try {
        geocoin::node::Node node(base_config(dir));

        geocoin::node::NodeTestAccess::set_fake_mining_stats(
            node,
            2,
            1000,
            100,
            10.0,
            std::chrono::milliseconds(1000)
        );
        auto status = node.get_status();
        expect(status.mining, "fake mining status reports mining");
        expect(status.mining_threads == 2, "fake mining status reports thread count");
        expect(status.hashrate > 400.0 && status.hashrate < 500.0,
               "hashrate is smoothed from positive hash delta");

        geocoin::node::NodeTestAccess::set_fake_mining_stats(
            node,
            1,
            1000,
            2000,
            100.0,
            std::chrono::milliseconds(1000)
        );
        status = node.get_status();
        expect(status.mining_threads == 1, "fake mining status updates thread count");
        expect(status.hashrate > 40.0 && status.hashrate < 60.0,
               "hashrate reset branch treats wrapped counters as zero delta");

        geocoin::node::NodeTestAccess::clear_fake_mining(node);
        status = node.get_status();
        expect(!status.mining, "fake mining status can be cleared");
        expect(status.hashrate == 0.0, "non-mining status resets reported hashrate");
        expect(status.mining_threads == 0, "non-mining status reports zero threads");

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
        test_not_running_rejects_start_mining();
        test_running_without_address_rejects_start_mining();
        test_sync_target_blocks_start_mining();
        test_zero_threads_rejects_start_mining();
        test_mining_block_template_helpers();
        test_committed_chain_events_are_fifo();
        test_start_stop_mining_with_tiny_cooperative_batch();
        test_restart_after_worker_self_termination_reaps_stale_threads();
        test_status_ibd_and_pruning_guards();
        test_status_treats_authenticated_peer_height_as_untrusted_hint();
        test_status_hashrate_without_starting_miner();
        std::cout << "test_node_mining_guards: ok\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "test_node_mining_guards failed: " << e.what() << "\n";
        return 1;
    }
}
