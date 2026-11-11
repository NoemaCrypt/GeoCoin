#include "tcp_test_client.hpp"
#include "geocoin/consensus/chainparams.hpp"
#include "geocoin/consensus/consensus.hpp"
#include "geocoin/consensus/optimized_miner.hpp"
#include "geocoin/consensus/proof_of_work.hpp"
#include "geocoin/core/blockchain.hpp"
#include "geocoin/network/messages.hpp"
#include "geocoin/network/sync_manager.hpp"
#include "geocoin/state/state.hpp"
#include "geocoin/storage/block_storage.hpp"
#include "secure_channel_test_fixture.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef _WIN32
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace geocoin::network {

struct SyncManagerTestAccess {
    static void set_syncing(SyncManager& sync, Height target_height)
    {
        sync.status_.store(SyncStatus::SYNCING, std::memory_order_relaxed);
        sync.target_height_.store(target_height, std::memory_order_relaxed);
    }

    static void seed_download_queue(SyncManager& sync, Height height, const Hash256& hash)
    {
        std::lock_guard<std::mutex> lock(sync.queue_mutex_);
        HeaderEntry entry;
        entry.height = height;
        entry.header.nonce = static_cast<uint64_t>(height);
        sync.headers_index_[hash] = entry;
        sync.header_heights_[hash] = height;
        sync.best_header_by_height_[height] = hash;
        if (sync.download_queue_set_.insert(hash).second) {
            sync.download_queue_.emplace(height, hash);
        }
    }

    static void seed_non_best_download_queue(
        SyncManager& sync,
        Height height,
        const Hash256& queued_hash,
        const Hash256& best_hash
    )
    {
        std::lock_guard<std::mutex> lock(sync.queue_mutex_);
        HeaderEntry entry;
        entry.height = height;
        entry.header.nonce = static_cast<uint64_t>(height);
        sync.headers_index_[queued_hash] = entry;
        sync.header_heights_[queued_hash] = height;
        sync.best_header_by_height_[height] = best_hash;
        if (sync.download_queue_set_.insert(queued_hash).second) {
            sync.download_queue_.emplace(height, queued_hash);
        }
    }

    static void seed_header_only(
        SyncManager& sync,
        Height height,
        const Hash256& hash,
        const Hash256& best_hash = Hash256{}
    )
    {
        std::lock_guard<std::mutex> lock(sync.queue_mutex_);
        HeaderEntry entry;
        entry.height = height;
        entry.header.nonce = static_cast<uint64_t>(height);
        sync.headers_index_[hash] = entry;
        sync.header_heights_[hash] = height;
        sync.best_header_by_height_[height] = (best_hash == Hash256{}) ? hash : best_hash;
    }

    static void force_idle_watchdog_due(SyncManager& sync)
    {
        sync.last_getblocks_request_ms_.store(0, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(sync.peers_mutex_);
        for (auto& [address, info] : sync.sync_peers_) info.last_headers_request_ms = 0;
    }

    static void set_header_pruning_limits(SyncManager& sync, size_t max_index_size, size_t keep_depth)
    {
        std::lock_guard<std::mutex> lock(sync.queue_mutex_);
        sync.max_headers_index_size_ = max_index_size;
        sync.headers_prune_keep_depth_ = keep_depth;
    }

    static void set_side_header_budget(SyncManager& sync, size_t max_side_headers)
    {
        std::lock_guard<std::mutex> lock(sync.peers_mutex_);
        sync.max_side_headers_per_source_ = max_side_headers;
    }

    static size_t max_headers_index_size(const SyncManager& sync)
    {
        return sync.max_headers_index_size_;
    }

    static size_t max_side_headers_per_source(const SyncManager& sync)
    {
        return sync.max_side_headers_per_source_;
    }

    static void set_pending_limit(SyncManager& sync, size_t max_pending_blocks)
    {
        std::lock_guard<std::mutex> lock(sync.pending_mutex_);
        sync.max_pending_blocks_ = max_pending_blocks;
    }

    static size_t pending_count(SyncManager& sync)
    {
        std::lock_guard<std::mutex> lock(sync.pending_mutex_);
        return sync.pending_blocks_.size();
    }

    static void set_max_orphans(SyncManager& sync, size_t max_orphans)
    {
        std::lock_guard<std::mutex> lock(sync.orphan_mutex_);
        sync.max_orphan_blocks_ = max_orphans;
    }

    static void store_orphan(
        SyncManager& sync,
        const Hash256& hash,
        const core::Block& block,
        std::shared_ptr<Peer> peer
    )
    {
        sync.store_orphan_block(hash, block, std::move(peer));
    }

    static void seed_orphan_entry(
        SyncManager& sync,
        const Hash256& hash,
        const Hash256& prev_hash,
        std::shared_ptr<core::Block> block,
        std::shared_ptr<Peer> peer
    )
    {
        std::lock_guard<std::mutex> lock(sync.orphan_mutex_);
        sync.orphan_blocks_[hash] = SyncManager::OrphanBlockEntry{
            std::move(block),
            std::move(peer),
            1
        };
        sync.orphan_blocks_by_prev_.emplace(prev_hash, hash);
    }

    static std::vector<SyncManager::OrphanBlockEntry> take_orphan_children(
        SyncManager& sync,
        const Hash256& parent_hash
    )
    {
        return sync.take_orphan_children(parent_hash);
    }

    static void drain_orphan_blocks(SyncManager& sync, const Hash256& parent_hash)
    {
        sync.drain_orphan_blocks(parent_hash);
    }

    static size_t orphan_count(SyncManager& sync)
    {
        std::lock_guard<std::mutex> lock(sync.orphan_mutex_);
        return sync.orphan_blocks_.size();
    }

    static void prune_headers_state(SyncManager& sync, Height current_height)
    {
        std::lock_guard<std::mutex> lock(sync.queue_mutex_);
        sync.prune_headers_state_locked(current_height);
    }

    static bool has_header(SyncManager& sync, const Hash256& hash)
    {
        std::lock_guard<std::mutex> lock(sync.queue_mutex_);
        return sync.headers_index_.find(hash) != sync.headers_index_.end();
    }

    static bool queue_contains(SyncManager& sync, const Hash256& hash)
    {
        std::lock_guard<std::mutex> lock(sync.queue_mutex_);
        return sync.download_queue_set_.find(hash) != sync.download_queue_set_.end();
    }

    static size_t best_header_count(SyncManager& sync)
    {
        std::lock_guard<std::mutex> lock(sync.queue_mutex_);
        return sync.best_header_by_height_.size();
    }

    static void mark_downloading(
        SyncManager& sync,
        Height height,
        const Hash256& hash,
        std::shared_ptr<Peer> peer,
        std::chrono::seconds age
    ) {
        const uint64_t now_ms = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count());
        const uint64_t age_ms = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            age
        ).count());
        const uint64_t requested_ms = (age_ms >= now_ms) ? 1 : (now_ms - age_ms);
        const std::string peer_address = peer ? peer->get_address() : std::string{};
        {
            std::lock_guard<std::mutex> queue_lock(sync.queue_mutex_);
            sync.header_heights_[hash] = height;
            sync.best_header_by_height_[height] = hash;
            sync.download_attempts_[hash] = 1;
            sync.downloading_blocks_[hash] = SyncManager::BlockDownloadInfo{
                peer,
                peer_address,
                height,
                requested_ms,
                1
            };
        }
        if (peer) {
            std::lock_guard<std::mutex> peers_lock(sync.peers_mutex_);
            auto& info = sync.sync_peers_[peer->get_address()];
            info.peer = peer;
            info.height = height;
            info.last_request = std::chrono::steady_clock::now() - age;
            info.requested_blocks.insert(hash);
        }
    }

    static void register_sync_peer(
        SyncManager& sync,
        std::shared_ptr<Peer> peer,
        Height height,
        uint32_t bad_headers_score,
        size_t requested_count
    )
    {
        std::lock_guard<std::mutex> peers_lock(sync.peers_mutex_);
        auto& info = sync.sync_peers_[peer->get_address()];
        info.peer = peer;
        info.height = height;
        info.last_request = std::chrono::steady_clock::now();
        info.bad_headers_score = bad_headers_score;
        info.requested_blocks.clear();
        for (size_t i = 0; i < requested_count; ++i) {
            Hash256 hash{};
            hash[0] = static_cast<byte>(0x20 + (i & 0xffU));
            hash[1] = static_cast<byte>((i >> 8) & 0xffU);
            info.requested_blocks.insert(hash);
        }
    }

    static void set_peer_download_scores(
        SyncManager& sync,
        const std::string& address,
        uint32_t timeout_score,
        uint32_t invalid_block_score,
        uint64_t completed_blocks,
        double response_time_ewma_ms
    )
    {
        std::lock_guard<std::mutex> peers_lock(sync.peers_mutex_);
        auto it = sync.sync_peers_.find(address);
        if (it == sync.sync_peers_.end()) {
            return;
        }
        it->second.timeout_score = timeout_score;
        it->second.invalid_block_score = invalid_block_score;
        it->second.completed_blocks = completed_blocks;
        it->second.response_time_ewma_ms = response_time_ewma_ms;
    }

    static uint32_t peer_timeout_score(SyncManager& sync, const std::string& address)
    {
        std::lock_guard<std::mutex> peers_lock(sync.peers_mutex_);
        auto it = sync.sync_peers_.find(address);
        return it == sync.sync_peers_.end() ? 0 : it->second.timeout_score;
    }

    static bool avoids_peer_for_hash(SyncManager& sync, const Hash256& hash, const std::string& address)
    {
        std::lock_guard<std::mutex> queue_lock(sync.queue_mutex_);
        auto it = sync.download_avoid_peer_.find(hash);
        return it != sync.download_avoid_peer_.end() && it->second == address;
    }

    static void set_download_window_limits(
        SyncManager& sync,
        size_t current_window,
        size_t min_window,
        size_t max_window
    )
    {
        std::lock_guard<std::mutex> queue_lock(sync.queue_mutex_);
        sync.download_window_size_ = current_window;
        sync.min_download_window_size_ = min_window;
        sync.max_download_window_size_ = max_window;
    }

    static size_t download_window_size(SyncManager& sync)
    {
        std::lock_guard<std::mutex> queue_lock(sync.queue_mutex_);
        return sync.download_window_size_;
    }

    static std::shared_ptr<Peer> find_best_peer(SyncManager& sync)
    {
        return sync.find_best_peer();
    }

    static void request_blocks(SyncManager& sync, std::shared_ptr<Peer> peer, const std::vector<Hash256>& hashes)
    {
        sync.request_blocks(std::move(peer), hashes);
    }

    static void send_getblocks(SyncManager& sync, std::shared_ptr<Peer> peer)
    {
        sync.send_getblocks(std::move(peer));
    }

    static void send_getheaders(SyncManager& sync, std::shared_ptr<Peer> peer)
    {
        sync.send_getheaders(std::move(peer));
    }

    static void activate_headers_sync(SyncManager& sync, std::shared_ptr<Peer> peer, Height hinted_target)
    {
        sync.activate_headers_sync(std::move(peer), hinted_target);
    }
};

} // namespace geocoin::network

namespace geocoin::core {

struct BlockchainTestAccess {
    static chain::BlockIndex* add_index(
        Blockchain& blockchain,
        const Block& block,
        chain::BlockIndex* prev,
        Height height,
        uint64_t chain_work,
        chain::BlockStatus status
    )
    {
        auto index = std::make_unique<chain::BlockIndex>(block.header);
        index->hash = block.get_hash();
        index->prev = prev;
        index->height = height;
        index->status = status;
        index->chain_work = uint256(chain_work);
        index->sequence_id = blockchain.next_sequence_id_++;
        index->tx_count = static_cast<uint32_t>(block.transactions.size());
        index->chain_tx = (prev ? prev->chain_tx : 0) + index->tx_count;
        index->build_skip();

        chain::BlockIndex* ptr = index.get();
        blockchain.block_index_[ptr->get_block_hash()] = std::move(index);
        return ptr;
    }

    static void set_active_tip(Blockchain& blockchain, chain::BlockIndex* tip)
    {
        blockchain.chain_active_.set_tip(tip);
    }
};

} // namespace geocoin::core

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

geocoin::Hash256 hash_from_seed(uint8_t seed)
{
    geocoin::Hash256 hash{};
    for (size_t i = 0; i < hash.size(); ++i) {
        hash[i] = static_cast<uint8_t>(seed + i * 7);
    }
    return hash;
}

class SocketPair {
public:
    SocketPair()
    {
        int fds[2] = {-1, -1};
        if (!geocoin::test::make_test_socket_pair(fds)) {
            throw std::runtime_error("socketpair failed");
        }
        peer_fd_ = fds[0];
        read_fd_ = fds[1];
    }

    ~SocketPair()
    {
        if (read_fd_ >= 0) {
            geocoin::test::close_test_socket(read_fd_);
        }
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
        wire_buffer_.clear();
        if (!peer.activate_secure_channel(std::move(fixture.peer_keys))) {
            remote_channel_.reset();
            return false;
        }
        return true;
    }

    geocoin::bytes read_message(std::chrono::milliseconds timeout = std::chrono::milliseconds(1000))
    {
        if (read_fd_ < 0 || !remote_channel_) {
            return {};
        }

        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (true) {
            size_t record_size = 0;
            const auto size_status = geocoin::network::SecureChannel::inspect_record_size(
                wire_buffer_.data(), wire_buffer_.size(), record_size);
            if (size_status == geocoin::network::SecureRecordSizeStatus::INVALID) {
                wire_buffer_.clear();
                throw std::runtime_error("invalid secure record in socketpair test");
            }
            if (size_status == geocoin::network::SecureRecordSizeStatus::READY) {
                geocoin::bytes record(wire_buffer_.begin(), wire_buffer_.begin() + record_size);
                wire_buffer_.erase(wire_buffer_.begin(), wire_buffer_.begin() + record_size);
                geocoin::bytes plaintext;
                if (!remote_channel_->unprotect(record, plaintext)) {
                    throw std::runtime_error("secure record authentication failed in socketpair test");
                }
                return plaintext;
            }

            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                return {};
            }
            const auto remaining =
                std::chrono::duration_cast<std::chrono::microseconds>(deadline - now);
            fd_set read_fds;
            FD_ZERO(&read_fds);
            FD_SET(read_fd_, &read_fds);
            timeval tv;
            tv.tv_sec = static_cast<long>(remaining.count() / 1000000);
            tv.tv_usec = static_cast<long>(remaining.count() % 1000000);
            const int ready = ::select(read_fd_ + 1, &read_fds, nullptr, nullptr, &tv);
            if (ready <= 0) {
                return {};
            }

            geocoin::bytes chunk(8192);
            const ssize_t received = geocoin::test::receive_test_fd(read_fd_, chunk.data(), chunk.size(), 0);
            if (received <= 0) {
                return {};
            }
            chunk.resize(static_cast<size_t>(received));
            wire_buffer_.insert(wire_buffer_.end(), chunk.begin(), chunk.end());
        }
        return {};
    }

private:
    int peer_fd_ = -1;
    int read_fd_ = -1;
    std::unique_ptr<geocoin::network::SecureChannel> remote_channel_;
    geocoin::bytes wire_buffer_;
};

struct ChainFixture {
    std::filesystem::path path;
    std::shared_ptr<geocoin::storage::BlockStorage> block_storage;
    std::shared_ptr<geocoin::state::ChainState> chain_state;
    std::shared_ptr<geocoin::core::Blockchain> blockchain;
    geocoin::core::Block genesis;

    ChainFixture()
        : path(make_temp_dir("geocoin-sync-manager"))
        , block_storage(std::make_shared<geocoin::storage::BlockStorage>(path.string()))
        , chain_state(std::make_shared<geocoin::state::ChainState>(
              std::make_unique<geocoin::state::MemoryStorage>()))
        , genesis(geocoin::consensus::Params().get_genesis_block())
    {
        expect(block_storage->is_open(), "block storage opens");
        chain_state->set_block_storage(block_storage);
        blockchain = std::make_shared<geocoin::core::Blockchain>(chain_state, block_storage);
        expect(blockchain->initialize(), "blockchain initializes genesis");
        expect(blockchain->get_height() == 0, "fixture active chain starts at genesis");
    }

    ~ChainFixture()
    {
        block_storage->close();
        std::filesystem::remove_all(path);
    }
};

std::shared_ptr<geocoin::network::Peer> make_peer(
    SocketPair& sockets,
    geocoin::Height height,
    const std::string& address = "127.0.0.1:3030"
)
{
    auto peer = std::make_shared<geocoin::network::Peer>(address, sockets.take_peer_fd());
    expect(sockets.activate_peer_channel(*peer), "test peer secure channel activates");
    auto info = peer->get_info();
    info.height = height;
    peer->update_info(info);
    peer->set_state(geocoin::network::PeerState::READY);
    return peer;
}

geocoin::core::Block make_child_candidate(
    const geocoin::core::Block& prev,
    uint32_t bits
)
{
    geocoin::core::Block block;
    block.header.version = 1;
    block.header.timestamp = prev.header.timestamp + 1;
    block.header.prev_block_hash = prev.get_hash();
    block.header.merkle_root = hash_from_seed(0x41);
    block.header.witness_commitment = hash_from_seed(0x71);
    block.header.bits = bits;
    block.header.nonce = 0;
    return block;
}

geocoin::core::Block mine_easy_child_block(
    const geocoin::core::Block& prev,
    const geocoin::chain::BlockIndex* prev_index,
    uint8_t merkle_seed
)
{
    expect(prev_index != nullptr, "easy sync-manager child has previous index context");
    geocoin::core::Block block = make_child_candidate(
        prev,
        geocoin::consensus::calculate_next_difficulty(
            prev_index,
            prev.header.timestamp + 1
        )
    );
    block.header.merkle_root = hash_from_seed(merkle_seed);
    block.header.witness_commitment = hash_from_seed(static_cast<uint8_t>(merkle_seed + 0x20));

    std::atomic<bool> stop{true};
    std::atomic<uint64_t> hashes{0};
    geocoin::consensus::OptimizedMiner miner;
    expect(
        miner.mine_block_optimized(
            block,
            block.header.bits,
            static_cast<uint32_t>(prev_index->get_median_time_past()),
            prev.get_hash(),
            prev_index->height + 1,
            512,
            &stop,
            -1,
            &hashes
        ),
        "easy sync-manager child header mines within unit-test budget"
    );
    expect(
        geocoin::consensus::ProofOfWork::verify_proof_of_work(
            block.header,
            block.header.bits,
            static_cast<uint32_t>(prev_index->get_median_time_past()),
            prev.get_hash(), prev_index->height + 1
        ),
        "easy sync-manager child header verifies"
    );
    return block;
}

geocoin::core::Block mine_structurally_valid_child_block(
    const geocoin::core::Block& prev,
    const geocoin::chain::BlockIndex* prev_index,
    const geocoin::core::Block& body_template,
    uint8_t unique_tag
)
{
    geocoin::core::Block block = body_template;
    // The miner derives geo_tag from parent context. Commit the discriminator
    // in the body so two candidates for the same parent remain distinct.
    expect(!block.transactions.empty(), "pending body template contains a coinbase");
    block.transactions.front().timestamp += unique_tag;
    block.header.version = 1;
    block.header.timestamp = prev.header.timestamp + 1;
    block.header.prev_block_hash = prev.get_hash();
    block.header.bits = geocoin::consensus::calculate_next_difficulty(
        prev_index,
        block.header.timestamp
    );
    block.header.nonce = 0;
    block.header.merkle_root = block.calculate_merkle_root();
    block.header.witness_commitment = block.calculate_witness_commitment();

    std::atomic<bool> stop{true};
    std::atomic<uint64_t> hashes{0};
    geocoin::consensus::OptimizedMiner miner;
    expect(
        miner.mine_block_optimized(
            block,
            block.header.bits,
            static_cast<uint32_t>(prev_index->get_median_time_past()),
            prev.get_hash(),
            prev_index->height + 1,
            512,
            &stop,
            -1,
            &hashes
        ),
        "structurally valid pending block mines within unit-test budget"
    );
    return block;
}

geocoin::bytes expect_message_type(const geocoin::bytes& raw, geocoin::network::MessageType type, const char* message)
{
    geocoin::network::MessageHeader header;
    geocoin::bytes payload;
    expect(geocoin::network::parse_message(raw, header, payload), "framed network message parses");
    expect(header.type == type, message);
    return payload;
}

void test_start_stop_and_getheaders()
{
    ChainFixture fixture;
    geocoin::network::SyncManager sync(fixture.chain_state, fixture.blockchain);
    sync.set_max_parallel_downloads(0);

    expect(!sync.start_sync(nullptr, 1), "start_sync rejects null peer");
    SocketPair stale_sockets;
    auto stale_peer = make_peer(stale_sockets, 0);
    expect(!sync.start_sync(stale_peer, 0), "start_sync rejects peers that are not ahead");
    expect(sync.get_stats().status == geocoin::network::SyncStatus::COMPLETED,
           "equal-height peer marks initial sync complete");
    expect(sync.get_stats().target_height == 0,
           "equal-height peer initializes sync target to local height");

    SocketPair sockets;
    const geocoin::Height malicious_height =
        (std::numeric_limits<geocoin::Height>::max)();
    auto peer = make_peer(sockets, malicious_height);
    expect(sync.start_sync(peer, malicious_height),
           "start_sync probes a peer advertising an ahead height");
    expect(!sync.is_syncing(), "unvalidated discovery does not report pending validated block work");
    expect_message_type(sockets.read_message(), geocoin::network::MessageType::GETHEADERS,
                        "start_sync sends GETHEADERS");

    const auto stats = sync.get_stats();
    expect(stats.status == geocoin::network::SyncStatus::COMPLETED,
           "stats keep validated sync complete while probing an untrusted height");
    expect(stats.current_height == 0, "stats report current height");
    expect(stats.target_height == 0,
           "unvalidated VERSION height does not become the sync target");
    expect(stats.active_peers == 1, "stats report active peer");
    expect(stats.download_queue_size == 0, "stats report empty download queue");
    expect(stats.downloading_blocks == 0, "stats report no downloading blocks");
    expect(stats.pending_blocks == 0, "stats report no pending blocks");
    expect(stats.orphan_blocks == 0, "stats report no orphan blocks");
    sync.update();

    const auto headers_from_empty_locator = sync.handle_getheaders(peer, {}, geocoin::Hash256{});
    expect(!headers_from_empty_locator.empty(), "getheaders without locator returns genesis header");
    const auto headers_after_genesis =
        sync.handle_getheaders(peer, {fixture.genesis.get_hash()}, geocoin::Hash256{});
    expect(headers_after_genesis.empty(), "getheaders after genesis has no more headers");

    expect(sync.handle_headers(peer, {}), "empty headers are accepted as sync hint");
    expect(sync.get_stats().status == geocoin::network::SyncStatus::COMPLETED,
           "empty response completes discovery when no validated lead exists");
    expect(!sync.start_sync(peer, malicious_height),
           "disproved VERSION lead cannot restart header discovery indefinitely");
    expect(sockets.read_message(std::chrono::milliseconds(50)).empty(),
           "disproved VERSION lead does not emit another GETHEADERS");
    expect(sync.handle_headers(peer, {fixture.genesis.header}), "known headers are accepted without queueing");
    sync.stop_sync();
    expect(!sync.is_syncing(), "stop_sync leaves non-syncing state");
    expect(sync.get_stats().active_peers == 0, "stop_sync clears peers");
}

void test_null_blockchain_inv_guard()
{
    auto state = std::make_shared<geocoin::state::ChainState>(
        std::make_unique<geocoin::state::MemoryStorage>());
    geocoin::network::SyncManager sync(state, nullptr);

    sync.handle_inv(nullptr, {});
    sync.handle_inv(nullptr, {hash_from_seed(0xd0)});

    const auto stats = sync.get_stats();
    expect(stats.current_height == 0, "null blockchain stats fall back to chainstate height");
    expect(stats.download_queue_size == 0, "null blockchain inv guard leaves queue empty");
    expect(stats.orphan_blocks == 0, "null blockchain inv guard leaves orphan buffer empty");

    geocoin::core::Block block;
    expect(!sync.handle_block(nullptr, block), "null blockchain handle_block guard rejects block");
    expect(!sync.handle_headers(nullptr, {block.header}), "null blockchain handle_headers guard rejects headers");

    SocketPair sockets;
    auto peer = make_peer(sockets, 10, "127.0.0.10:3030");
    geocoin::network::SyncManagerTestAccess::activate_headers_sync(sync, peer, 10);
    geocoin::network::SyncManagerTestAccess::send_getheaders(sync, peer);
    geocoin::network::SyncManagerTestAccess::send_getblocks(sync, peer);
    expect(sockets.read_message(std::chrono::milliseconds(50)).empty(),
           "null blockchain guards do not emit sync requests");
}

void test_genesis_block_handling()
{
    ChainFixture fixture;
    geocoin::network::SyncManager sync(fixture.chain_state, fixture.blockchain);

    SocketPair sockets;
    auto peer = make_peer(sockets, 1);
    expect(sync.handle_block(peer, fixture.genesis), "duplicate genesis block is accepted");

    bool disconnected = false;
    std::string disconnect_reason;
    sync.set_peer_disconnect_callback([&](std::shared_ptr<geocoin::network::Peer> got_peer, const std::string& reason) {
        disconnected = (got_peer == peer);
        disconnect_reason = reason;
    });

    geocoin::core::Block wrong_genesis = fixture.genesis;
    wrong_genesis.header.merkle_root = hash_from_seed(0xc1);
    wrong_genesis.header.witness_commitment = hash_from_seed(0xc2);
    wrong_genesis.header.nonce += 1;
    expect(!sync.handle_headers(peer, {wrong_genesis.header}),
           "headers-first rejects a chain rooted at a foreign genesis before body download");
    expect(!sync.handle_block(peer, wrong_genesis), "wrong genesis block is rejected");
    expect(disconnected, "wrong genesis disconnects peer through callback");
    expect(disconnect_reason == "wrong-genesis-block", "wrong genesis disconnect reason is stable");
}

void test_headers_inv_and_block_queue()
{
    ChainFixture fixture;
    geocoin::network::SyncManager sync(fixture.chain_state, fixture.blockchain);
    geocoin::core::Block bad_difficulty_child = make_child_candidate(fixture.genesis, 0x2100ffff);
    const uint32_t expected_bits = geocoin::consensus::calculate_next_difficulty(
        fixture.blockchain->get_tip(),
        fixture.genesis.header.timestamp + 1
    );
    geocoin::core::Block bad_pow_child = make_child_candidate(fixture.genesis, expected_bits);

    SocketPair sockets;
    auto peer = make_peer(sockets, 2);
    expect(sync.start_sync(peer, 2), "start_sync accepts peer for header queue test");
    (void)sockets.read_message();

    geocoin::core::BlockHeader unknown_parent = bad_difficulty_child.header;
    unknown_parent.prev_block_hash = hash_from_seed(0x90);
    expect(!sync.handle_headers(peer, {unknown_parent}), "unknown parent header is rejected");
    expect(sockets.read_message(std::chrono::milliseconds(50)).empty(),
           "unknown parent recovery respects header request spacing");
    geocoin::network::SyncManagerTestAccess::force_idle_watchdog_due(sync);
    sync.update();
    expect_message_type(sockets.read_message(), geocoin::network::MessageType::GETHEADERS,
                        "unknown parent requests fresh headers");

    expect(!sync.handle_headers(peer, {bad_difficulty_child.header}), "bad difficulty header is rejected");
    expect(!sync.handle_headers(peer, {bad_pow_child.header}), "bad PoW header is rejected");

    bool block_callback_called = false;
    sync.set_block_callback([&](const geocoin::core::Block&, geocoin::Height) {
        block_callback_called = true;
    });
    expect(!sync.handle_block(peer, bad_pow_child), "known-parent invalid block is rejected by block validation");
    expect(!block_callback_called, "rejected known-parent block does not fire block callback");
    expect(fixture.blockchain->get_height() == 0, "rejected known-parent block does not advance chain");

    geocoin::core::Block orphan = bad_pow_child;
    orphan.header.prev_block_hash = hash_from_seed(0xa0);
    expect(!sync.handle_block(peer, orphan), "unsolicited orphan block is rejected");
    expect(sync.get_stats().orphan_blocks == 0,
           "unsolicited orphan cannot consume the bounded orphan buffer");

    SocketPair inv_sockets;
    auto inv_peer = make_peer(inv_sockets, 3);
    expect(sync.start_sync(inv_peer, 3), "start_sync accepts inv peer");
    (void)inv_sockets.read_message();
    sync.handle_inv(inv_peer, {hash_from_seed(0xb0)});
    expect(inv_sockets.read_message(std::chrono::milliseconds(50)).empty(),
           "unknown inv shares the outstanding headers refresh");

    sync.stop_sync();
    const auto stopped = sync.get_stats();
    expect(stopped.orphan_blocks == 0, "stop_sync clears orphan blocks");
    expect(stopped.download_queue_size == 0, "stop_sync clears download queue");
}

void test_handle_block_queue_cleanup_orphan_peer_and_pending_gap()
{
    ChainFixture fixture;
    geocoin::network::SyncManager sync(fixture.chain_state, fixture.blockchain);
    geocoin::network::SyncManagerTestAccess::set_syncing(sync, 8);

    SocketPair sockets;
    auto peer = make_peer(sockets, 8, "127.0.0.88:3030");

    geocoin::core::Block orphan = fixture.genesis;
    orphan.header.prev_block_hash = hash_from_seed(0x88);
    orphan.header.timestamp += 1;
    orphan.header.nonce += 1;
    const geocoin::Hash256 orphan_hash = orphan.get_hash();
    geocoin::network::SyncManagerTestAccess::seed_download_queue(sync, 1, orphan_hash);
    geocoin::network::SyncManagerTestAccess::mark_downloading(
        sync,
        1,
        orphan_hash,
        peer,
        std::chrono::seconds(0)
    );

    expect(!sync.handle_block(peer, orphan), "orphan with peer is buffered after queue cleanup");
    expect(sync.get_stats().download_queue_size == 0, "received orphan is removed from download queue");
    expect(sync.get_stats().downloading_blocks == 0, "received orphan is removed from downloading set");
    expect(sync.get_stats().orphan_blocks == 1, "received orphan is stored");
    expect_message_type(
        sockets.read_message(),
        geocoin::network::MessageType::GETHEADERS,
        "orphan with peer requests headers refresh"
    );

    geocoin::network::SyncManagerTestAccess::set_pending_limit(sync, 1);
    auto* genesis_index = fixture.blockchain->get_tip();

    geocoin::core::Block detached_prev = make_child_candidate(
        fixture.genesis,
        geocoin::consensus::Params().pow_limit_bits());
    detached_prev.header.merkle_root = hash_from_seed(0x89);
    auto* detached_index = geocoin::core::BlockchainTestAccess::add_index(
        *fixture.blockchain,
        detached_prev,
        genesis_index,
        2,
        2,
        geocoin::chain::BlockStatus::VALID_TREE
    );

    geocoin::core::Block gap_block = mine_structurally_valid_child_block(
        detached_prev,
        detached_index,
        fixture.genesis,
        0x8a
    );
    geocoin::network::SyncManagerTestAccess::seed_header_only(sync, 3, gap_block.get_hash());
    expect(sync.handle_block(peer, gap_block), "known future-height block is buffered as pending");
    expect(geocoin::network::SyncManagerTestAccess::pending_count(sync) == 1,
           "pending buffer stores out-of-order block");
    expect(sockets.read_message(std::chrono::milliseconds(50)).empty(),
           "large height gap shares the outstanding headers refresh");

    geocoin::core::Block farther_prev = make_child_candidate(
        fixture.genesis,
        geocoin::consensus::Params().pow_limit_bits());
    farther_prev.header.merkle_root = hash_from_seed(0x8b);
    auto* farther_index = geocoin::core::BlockchainTestAccess::add_index(
        *fixture.blockchain,
        farther_prev,
        genesis_index,
        4,
        3,
        geocoin::chain::BlockStatus::VALID_TREE
    );
    geocoin::core::Block farther_gap = mine_structurally_valid_child_block(
        farther_prev,
        farther_index,
        fixture.genesis,
        0x8c
    );
    geocoin::network::SyncManagerTestAccess::seed_header_only(sync, 5, farther_gap.get_hash());
    expect(sync.handle_block(peer, farther_gap), "second future-height block is accepted into bounded pending buffer");
    expect(geocoin::network::SyncManagerTestAccess::pending_count(sync) == 1,
           "pending buffer enforces configured max size");
    expect(geocoin::network::SyncManagerTestAccess::queue_contains(sync, farther_gap.get_hash()),
           "pending eviction restores demand for the missing best-header body");
    expect(sync.handle_block(peer, farther_gap) && sync.get_stats().download_queue_size == 1,
           "repeated pending eviction does not duplicate download demand");

    const auto alternate_gap = mine_structurally_valid_child_block(
        detached_prev, detached_index, fixture.genesis, 0x8d);
    expect(alternate_gap.get_hash() != gap_block.get_hash(), "pending replacement fixture uses distinct blocks");
    geocoin::network::SyncManagerTestAccess::seed_header_only(
        sync, 3, alternate_gap.get_hash(), gap_block.get_hash());
    expect(sync.handle_block(peer, alternate_gap), "retain an alternate body at the buffered height");
    expect(geocoin::network::SyncManagerTestAccess::queue_contains(sync, gap_block.get_hash()),
           "replacing a pending body preserves demand for the previous best-header body");
}

void test_orphan_store_take_and_drain_paths()
{
    ChainFixture fixture;
    geocoin::network::SyncManager sync(fixture.chain_state, fixture.blockchain);
    using Access = geocoin::network::SyncManagerTestAccess;

    SocketPair sockets;
    auto peer = make_peer(sockets, 5, "127.0.0.89:3030");
    const geocoin::Hash256 genesis_hash = fixture.genesis.get_hash();

    Access::store_orphan(sync, genesis_hash, fixture.genesis, peer);
    expect(Access::orphan_count(sync) == 0,
           "store_orphan ignores blocks already known to chainstate");

    geocoin::core::Block child_a = make_child_candidate(fixture.genesis, 0x2100ffff);
    child_a.header.merkle_root = hash_from_seed(0x8d);
    const geocoin::Hash256 child_a_hash = child_a.get_hash();
    Access::store_orphan(sync, child_a_hash, child_a, peer);
    Access::store_orphan(sync, child_a_hash, child_a, peer);
    expect(Access::orphan_count(sync) == 1,
           "store_orphan replaces duplicate without growing buffer");

    auto children = Access::take_orphan_children(sync, genesis_hash);
    expect(children.size() == 1 && children[0].block && children[0].block->get_hash() == child_a_hash,
           "take_orphan_children returns and removes matching child");
    expect(Access::orphan_count(sync) == 0,
           "take_orphan_children empties removed child from orphan map");
    children = Access::take_orphan_children(sync, genesis_hash);
    expect(children.empty(), "take_orphan_children returns empty after child removal");

    Access::set_max_orphans(sync, 1);
    geocoin::core::Block child_b = make_child_candidate(fixture.genesis, 0x2100ffff);
    child_b.header.merkle_root = hash_from_seed(0x8e);
    geocoin::core::Block child_c = make_child_candidate(fixture.genesis, 0x2100ffff);
    child_c.header.prev_block_hash = hash_from_seed(0x8f);
    child_c.header.merkle_root = hash_from_seed(0x90);
    Access::store_orphan(sync, child_b.get_hash(), child_b, peer);
    Access::store_orphan(sync, child_c.get_hash(), child_c, peer);
    expect(Access::orphan_count(sync) == 1,
           "store_orphan prunes oldest entries above configured orphan limit");

    const geocoin::Hash256 null_child_hash = hash_from_seed(0x91);
    Access::seed_orphan_entry(sync, null_child_hash, genesis_hash, nullptr, peer);

    geocoin::core::Block drain_child = make_child_candidate(fixture.genesis, 0x2100ffff);
    drain_child.header.merkle_root = hash_from_seed(0x92);
    Access::seed_orphan_entry(
        sync,
        drain_child.get_hash(),
        genesis_hash,
        std::make_shared<geocoin::core::Block>(drain_child),
        peer
    );
    const size_t before_drain = Access::orphan_count(sync);
    expect(before_drain >= 2, "drain fixture has null and block children");
    Access::drain_orphan_blocks(sync, genesis_hash);
    expect(Access::orphan_count(sync) <= before_drain - 2,
           "drain_orphan_blocks removes matching children and skips null child");
    expect(Access::take_orphan_children(sync, genesis_hash).empty(),
           "drain_orphan_blocks leaves no children for drained parent");

    Access::drain_orphan_blocks(sync, genesis_hash);
    expect(Access::take_orphan_children(sync, genesis_hash).empty(),
           "drain_orphan_blocks is a no-op with no children");
}

void test_getheaders_stop_hash_and_null_peer_guards()
{
    ChainFixture fixture;
    geocoin::network::SyncManager sync(fixture.chain_state, fixture.blockchain);
    const geocoin::Hash256 genesis_hash = fixture.genesis.get_hash();

    const auto stopped_at_genesis = sync.handle_getheaders(nullptr, {}, genesis_hash);
    expect(stopped_at_genesis.size() == 1, "getheaders can stop at genesis hash");
    expect(stopped_at_genesis.front().get_hash() == genesis_hash, "stop-hash result is genesis");

    const auto after_genesis = sync.handle_getheaders(nullptr, {genesis_hash}, geocoin::Hash256{});
    expect(after_genesis.empty(), "null peer getheaders accepts locator at active tip");

    geocoin::network::SyncManager no_chain(fixture.chain_state, nullptr);
    expect(no_chain.handle_getheaders(nullptr, {}, geocoin::Hash256{}).empty(),
           "getheaders without blockchain returns empty result");

    geocoin::network::SyncManagerTestAccess::send_getheaders(sync, nullptr);
    geocoin::network::SyncManagerTestAccess::send_getblocks(sync, nullptr);
}

void test_header_rejection_penalty_guards()
{
    ChainFixture fixture;
    geocoin::network::SyncManager sync(fixture.chain_state, fixture.blockchain);

    geocoin::core::BlockHeader unknown_parent = fixture.genesis.header;
    unknown_parent.prev_block_hash = hash_from_seed(0x31);
    unknown_parent.nonce += 1;

    expect(!sync.handle_headers(nullptr, {unknown_parent}),
           "invalid headers with null peer are rejected without disconnect callback");

    SocketPair sockets;
    auto unregistered_peer = make_peer(sockets, 5, "127.0.0.31:3030");
    expect(!sync.handle_headers(unregistered_peer, {unknown_parent}),
           "invalid headers from unregistered peer are rejected");
    expect_message_type(sockets.read_message(), geocoin::network::MessageType::GETHEADERS,
                        "invalid unregistered peer still receives a fresh headers request");
}

void test_valid_headers_index_and_download_queue_paths()
{
    ChainFixture fixture;
    geocoin::network::SyncManager sync(fixture.chain_state, fixture.blockchain);

    geocoin::core::Block parent = make_child_candidate(fixture.genesis, 0x2100ffff);
    parent.header.timestamp = fixture.genesis.header.timestamp + 1;
    parent.header.merkle_root = hash_from_seed(0x61);
    parent.header.witness_commitment = hash_from_seed(0x81);
    const geocoin::uint256 parent_work = geocoin::consensus::calculate_chainwork(
        fixture.blockchain->get_tip(),
        parent.header.bits
    );
    auto* parent_index = geocoin::core::BlockchainTestAccess::add_index(
        *fixture.blockchain,
        parent,
        fixture.blockchain->get_tip(),
        1,
        parent_work.data[0],
        geocoin::chain::BlockStatus::VALID_TREE
    );

    const auto child = mine_easy_child_block(
        parent,
        parent_index,
        0x62
    );

    geocoin::chain::BlockIndex child_index(child.header);
    child_index.hash = child.get_hash();
    child_index.prev = parent_index;
    child_index.height = 2;
    child_index.chain_work = geocoin::uint256(3);
    child_index.build_skip();

    const auto grandchild = mine_easy_child_block(
        child,
        &child_index,
        0x63
    );

    SocketPair sockets;
    auto peer = make_peer(sockets, 5, "127.0.0.62:3030");
    geocoin::network::SyncManagerTestAccess::register_sync_peer(sync, peer, 5, 2, 0);

    expect(sync.handle_headers(peer, {child.header}), "valid child header is accepted");
    expect(geocoin::network::SyncManagerTestAccess::has_header(sync, child.get_hash()),
           "valid child header is indexed");
    expect(geocoin::network::SyncManagerTestAccess::queue_contains(sync, child.get_hash()),
           "valid child header is queued for block download");

    auto stats = sync.get_stats();
    expect(stats.target_height >= 2, "valid child header raises sync target");
    expect(!parent_index->has_data() &&
               geocoin::network::SyncManagerTestAccess::queue_contains(sync, parent.get_hash()),
           "indexed parent without a body is restored to the download path");
    expect(stats.download_queue_size == 2, "valid child and its unsaved parent are queued");

    expect(sync.handle_headers(peer, {grandchild.header}),
           "valid grandchild header restores parent context from header index");
    expect(geocoin::network::SyncManagerTestAccess::has_header(sync, grandchild.get_hash()),
           "valid grandchild header is indexed");
    expect(geocoin::network::SyncManagerTestAccess::queue_contains(sync, grandchild.get_hash()),
           "valid grandchild header is queued for block download");

    stats = sync.get_stats();
    expect(stats.target_height >= 3, "valid grandchild header raises sync target");
    expect(stats.download_queue_size == 3, "two accepted headers and their unsaved anchor remain queued");
    expect(sockets.read_message(std::chrono::milliseconds(50)).empty(),
           "short accepted header batches do not request another headers page");
}

void test_header_state_pruning()
{
    ChainFixture fixture;

    {
        geocoin::network::SyncManager sync(fixture.chain_state, fixture.blockchain);
        const geocoin::Hash256 retained = hash_from_seed(0x40);
        geocoin::network::SyncManagerTestAccess::seed_header_only(sync, 1, retained);
        geocoin::network::SyncManagerTestAccess::set_header_pruning_limits(sync, 100, 100);
        geocoin::network::SyncManagerTestAccess::prune_headers_state(sync, 5);
        expect(geocoin::network::SyncManagerTestAccess::has_header(sync, retained),
               "header pruning keeps state when cutoff remains zero");
    }

    {
        geocoin::network::SyncManager sync(fixture.chain_state, fixture.blockchain);
        const geocoin::Hash256 old_a = hash_from_seed(0x41);
        const geocoin::Hash256 old_b = hash_from_seed(0x42);
        const geocoin::Hash256 overflow_old = hash_from_seed(0x43);
        const geocoin::Hash256 stale_non_best = hash_from_seed(0x44);
        const geocoin::Hash256 best_at_height = hash_from_seed(0x45);
        const geocoin::Hash256 retained = hash_from_seed(0x46);

        geocoin::network::SyncManagerTestAccess::seed_download_queue(sync, 1, old_a);
        geocoin::network::SyncManagerTestAccess::seed_download_queue(sync, 2, old_b);
        geocoin::network::SyncManagerTestAccess::seed_download_queue(sync, 3, overflow_old);
        geocoin::network::SyncManagerTestAccess::seed_non_best_download_queue(
            sync, 4, stale_non_best, best_at_height);
        geocoin::network::SyncManagerTestAccess::seed_download_queue(sync, 5, retained);
        geocoin::network::SyncManagerTestAccess::set_header_pruning_limits(sync, 1, 2);

        geocoin::network::SyncManagerTestAccess::prune_headers_state(sync, 5);

        expect(!geocoin::network::SyncManagerTestAccess::has_header(sync, old_a),
               "header pruning removes entries below depth cutoff");
        expect(!geocoin::network::SyncManagerTestAccess::has_header(sync, old_b),
               "header pruning removes second old entry below cutoff");
        expect(!geocoin::network::SyncManagerTestAccess::has_header(sync, overflow_old),
               "header pruning applies overflow cutoff");
        expect(!geocoin::network::SyncManagerTestAccess::has_header(sync, stale_non_best),
               "header pruning evicts non-best metadata to enforce the hard index cap");
        expect(!geocoin::network::SyncManagerTestAccess::queue_contains(sync, stale_non_best),
               "header pruning drops queued hash that is no longer best at its height");
        expect(geocoin::network::SyncManagerTestAccess::has_header(sync, retained),
               "header pruning keeps recent best header");
        expect(geocoin::network::SyncManagerTestAccess::queue_contains(sync, retained),
               "header pruning keeps recent queued best header");
        expect(geocoin::network::SyncManagerTestAccess::best_header_count(sync) == 2,
               "header pruning removes old best-height entries");
    }
}

void test_empty_headers_penalty_disconnects_ahead_peer()
{
    ChainFixture fixture;
    geocoin::network::SyncManager sync(fixture.chain_state, fixture.blockchain);

    SocketPair sockets;
    auto peer = make_peer(sockets, 8);
    expect(sync.start_sync(peer, 8), "start_sync accepts peer before empty header penalties");
    (void)sockets.read_message();

    int disconnects = 0;
    std::string last_reason;
    sync.set_peer_disconnect_callback(
        [&](std::shared_ptr<geocoin::network::Peer> got_peer, const std::string& reason) {
            if (got_peer == peer) {
                ++disconnects;
                last_reason = reason;
            }
        }
    );

    for (int i = 0; i < 8; ++i) {
        expect(sync.handle_headers(peer, {}), "empty headers from ahead peer remain protocol-level accepted");
    }
    expect(disconnects == 1, "repeated empty headers disconnect ahead peer through policy callback");
    expect(last_reason == "sync-invalid-headers", "empty header penalty uses invalid-headers reason");
}

void test_download_queue_dispatch_and_inv_dedupes()
{
    ChainFixture fixture;
    geocoin::network::SyncManager sync(fixture.chain_state, fixture.blockchain);
    sync.set_max_parallel_downloads(0);

    SocketPair sockets;
    auto peer = make_peer(sockets, 4);
    expect(sync.start_sync(peer, 4), "start_sync accepts peer for download queue dispatch");
    (void)sockets.read_message();

    const geocoin::Hash256 candidate_hash = hash_from_seed(0x44);
    geocoin::network::SyncManagerTestAccess::seed_download_queue(sync, 1, candidate_hash);
    geocoin::network::SyncManagerTestAccess::set_syncing(sync, 1);

    sync.handle_inv(peer, {fixture.genesis.get_hash(), candidate_hash});
    sync.update();

    const geocoin::bytes payload = expect_message_type(
        sockets.read_message(),
        geocoin::network::MessageType::GETDATA,
        "queued header hash is requested as GETDATA"
    );
    const auto getdata = geocoin::network::InvMessage::deserialize(payload);
    expect(getdata.items.size() == 1, "GETDATA contains one requested block");
    expect(getdata.items.front().type == geocoin::network::InvMessage::InvType::BLOCK,
           "GETDATA item requests a block");
    expect(getdata.items.front().hash == candidate_hash, "GETDATA requests queued hash");

    auto stats = sync.get_stats();
    expect(stats.download_queue_size == 0, "queued hash leaves download queue after request");
    expect(stats.downloading_blocks == 1, "requested hash is tracked as downloading");

    sync.handle_inv(peer, {candidate_hash});
    stats = sync.get_stats();
    expect(stats.download_queue_size == 0, "INV for downloading hash is not queued again");
    expect(stats.downloading_blocks == 1, "INV for downloading hash keeps one in-flight request");
}

void test_inv_known_headers_and_idle_watchdog()
{
    ChainFixture fixture;
    geocoin::network::SyncManager sync(fixture.chain_state, fixture.blockchain);
    sync.set_max_parallel_downloads(1);

    SocketPair sockets;
    auto peer = make_peer(sockets, 5);
    expect(sync.start_sync(peer, 5), "start_sync accepts peer for known header INV");
    (void)sockets.read_message();

    const geocoin::Hash256 queued_hash = hash_from_seed(0x81);
    geocoin::network::SyncManagerTestAccess::seed_header_only(sync, 1, queued_hash);
    sync.handle_inv(peer, {fixture.genesis.get_hash(), queued_hash});
    auto stats = sync.get_stats();
    expect(stats.status == geocoin::network::SyncStatus::SYNCING,
           "known header INV switches sync status to syncing");
    expect(stats.download_queue_size == 1, "known header INV queues block download");
    expect(stats.target_height == 1,
           "known header INV uses its validated index height, not peer VERSION height");

    sync.update();
    const geocoin::bytes payload = expect_message_type(
        sockets.read_message(),
        geocoin::network::MessageType::GETDATA,
        "known header INV is requested through GETDATA"
    );
    const auto getdata = geocoin::network::InvMessage::deserialize(payload);
    expect(getdata.items.size() == 1 && getdata.items.front().hash == queued_hash,
           "GETDATA requests known header hash");

    const geocoin::Hash256 non_best_hash = hash_from_seed(0x82);
    const geocoin::Hash256 best_hash = hash_from_seed(0x83);
    geocoin::network::SyncManagerTestAccess::seed_header_only(sync, 2, non_best_hash, best_hash);
    sync.handle_inv(peer, {non_best_hash});
    stats = sync.get_stats();
    expect(stats.download_queue_size == 0,
           "INV outside the global best-header path is ignored");

    sync.stop_sync();
    expect(sync.start_sync(peer, 5), "start_sync accepts peer for idle watchdog");
    (void)sockets.read_message();
    geocoin::network::SyncManagerTestAccess::set_syncing(sync, 1);
    geocoin::network::SyncManagerTestAccess::force_idle_watchdog_due(sync);
    sync.update();
    expect_message_type(
        sockets.read_message(),
        geocoin::network::MessageType::GETHEADERS,
        "idle sync watchdog re-sends GETHEADERS"
    );
}

void test_download_timeout_requeues_without_ready_peer()
{
    ChainFixture fixture;
    geocoin::network::SyncManager sync(fixture.chain_state, fixture.blockchain);

    SocketPair sockets;
    auto peer = make_peer(sockets, 3);
    const geocoin::Hash256 stale_hash = hash_from_seed(0x66);

    geocoin::network::SyncManagerTestAccess::set_syncing(sync, 3);
    geocoin::network::SyncManagerTestAccess::mark_downloading(
        sync,
        1,
        stale_hash,
        peer,
        std::chrono::seconds(31)
    );
    peer->disconnect();

    sync.update();
    const auto stats = sync.get_stats();
    expect(stats.download_queue_size == 1, "timed-out block is requeued when peer is unavailable");
    expect(stats.downloading_blocks == 0, "timed-out block is removed from downloading set");
}

void test_download_timeout_requeues_ready_peer_by_age()
{
    ChainFixture fixture;
    geocoin::network::SyncManager sync(fixture.chain_state, fixture.blockchain);

    SocketPair sockets;
    auto peer = make_peer(sockets, 3, "127.0.0.67:3030");
    const geocoin::Hash256 stale_hash = hash_from_seed(0x67);

    geocoin::network::SyncManagerTestAccess::set_syncing(sync, 3);
    geocoin::network::SyncManagerTestAccess::mark_downloading(
        sync,
        1,
        stale_hash,
        peer,
        std::chrono::seconds(31)
    );

    sync.update();
    const geocoin::bytes payload = expect_message_type(
        sockets.read_message(),
        geocoin::network::MessageType::GETDATA,
        "aged block request is re-issued to a ready peer"
    );
    const auto getdata = geocoin::network::InvMessage::deserialize(payload);
    expect(getdata.items.size() == 1 && getdata.items.front().hash == stale_hash,
           "aged block request reissues the same hash");
    const auto stats = sync.get_stats();
    expect(stats.download_queue_size == 0, "aged block request leaves queue after immediate retry");
    expect(stats.downloading_blocks == 1, "aged block request is tracked as downloading after retry");
}

void test_cross_peer_block_cannot_consume_another_peers_request()
{
    ChainFixture fixture;
    geocoin::network::SyncManager sync(fixture.chain_state, fixture.blockchain);

    SocketPair requested_sockets;
    SocketPair racing_sockets;
    auto requested_peer = make_peer(
        requested_sockets,
        1,
        "127.0.0.131:3030"
    );
    auto racing_peer = make_peer(
        racing_sockets,
        1,
        "127.0.0.132:3030"
    );

    const auto candidate = make_child_candidate(fixture.genesis, 0x2100ffff);
    const auto candidate_hash = candidate.get_hash();
    geocoin::network::SyncManagerTestAccess::mark_downloading(
        sync,
        1,
        candidate_hash,
        requested_peer,
        std::chrono::seconds(0)
    );

    expect(!sync.handle_block(racing_peer, candidate),
           "BLOCK from a peer that did not receive GETDATA is ignored");
    const auto stats = sync.get_stats();
    expect(stats.downloading_blocks == 1,
           "cross-peer BLOCK cannot clear another peer's in-flight request");
}

void test_download_scheduler_peer_selection_and_request_guards()
{
    {
        ChainFixture fixture;
        geocoin::network::SyncManager sync(fixture.chain_state, fixture.blockchain);
        sync.set_max_parallel_downloads(1);
        geocoin::network::SyncManagerTestAccess::set_syncing(sync, 10);

        const geocoin::Hash256 queued_hash = hash_from_seed(0x91);
        geocoin::network::SyncManagerTestAccess::seed_download_queue(sync, 1, queued_hash);
        sync.update();

        auto stats = sync.get_stats();
        expect(stats.download_queue_size == 1, "scheduler requeues block when no ready peer is available");
        expect(stats.downloading_blocks == 0, "scheduler does not mark block downloading without a peer");
    }

    {
        ChainFixture fixture;
        geocoin::network::SyncManager sync(fixture.chain_state, fixture.blockchain);
        sync.set_max_parallel_downloads(1);
        geocoin::network::SyncManagerTestAccess::set_syncing(sync, 10);

        SocketPair sockets;
        auto peer = make_peer(sockets, 10, "127.0.0.91:3030");
        geocoin::network::SyncManagerTestAccess::register_sync_peer(sync, peer, 10, 0, 0);

        const geocoin::Hash256 far_hash = hash_from_seed(0x92);
        geocoin::network::SyncManagerTestAccess::seed_download_queue(sync, 100, far_hash);
        sync.update();
        expect(sockets.read_message(std::chrono::milliseconds(50)).empty(),
               "scheduler does not request blocks outside the active download window");
        auto stats = sync.get_stats();
        expect(stats.download_queue_size == 1 && stats.downloading_blocks == 0,
               "out-of-window block remains queued");

        const geocoin::Hash256 stale_hash = hash_from_seed(0x93);
        const geocoin::Hash256 best_hash = hash_from_seed(0x94);
        geocoin::network::SyncManagerTestAccess::seed_non_best_download_queue(sync, 1, stale_hash, best_hash);
        sync.update();
        expect(sockets.read_message(std::chrono::milliseconds(50)).empty(),
               "scheduler skips queued headers that are not best for their height");
        stats = sync.get_stats();
        expect(stats.download_queue_size == 2 && stats.downloading_blocks == 0,
               "non-best queued header remains pending and unrequested");
    }

    {
        ChainFixture fixture;
        geocoin::network::SyncManager sync(fixture.chain_state, fixture.blockchain);
        sync.set_max_parallel_downloads(2);
        geocoin::network::SyncManagerTestAccess::set_syncing(sync, 10);

        SocketPair sockets;
        auto peer = make_peer(sockets, 10, "127.0.0.94:3030");
        const geocoin::Hash256 already_downloading = hash_from_seed(0x94);
        geocoin::network::SyncManagerTestAccess::register_sync_peer(sync, peer, 10, 0, 0);
        geocoin::network::SyncManagerTestAccess::seed_download_queue(sync, 1, already_downloading);
        geocoin::network::SyncManagerTestAccess::mark_downloading(
            sync, 1, already_downloading, peer, std::chrono::seconds(0));

        sync.update();
        expect(sockets.read_message(std::chrono::milliseconds(50)).empty(),
               "scheduler skips queue entries already marked as downloading");
        const auto stats = sync.get_stats();
        expect(stats.download_queue_size == 1 && stats.downloading_blocks == 1,
               "already-downloading queued entry remains unchanged");
    }

    {
        ChainFixture fixture;
        geocoin::network::SyncManager sync(fixture.chain_state, fixture.blockchain);

        geocoin::network::SyncManagerTestAccess::request_blocks(sync, nullptr, {hash_from_seed(0x95)});

        SocketPair sockets;
        auto peer = make_peer(sockets, 1, "127.0.0.95:3030");
        geocoin::network::SyncManagerTestAccess::request_blocks(sync, peer, {});
        expect(sockets.read_message(std::chrono::milliseconds(50)).empty(),
               "request_blocks guard does not send empty GETDATA");

        const geocoin::Hash256 requested_hash = hash_from_seed(0x96);
        geocoin::network::SyncManagerTestAccess::request_blocks(sync, peer, {requested_hash});
        const auto payload = expect_message_type(
            sockets.read_message(),
            geocoin::network::MessageType::GETDATA,
            "request_blocks sends GETDATA for explicit hash"
        );
        const auto getdata = geocoin::network::InvMessage::deserialize(payload);
        expect(getdata.items.size() == 1 && getdata.items.front().hash == requested_hash,
               "request_blocks GETDATA payload contains requested hash");
    }

    {
        ChainFixture fixture;
        geocoin::network::SyncManager sync(fixture.chain_state, fixture.blockchain);

        SocketPair worse_sockets;
        SocketPair busy_sockets;
        SocketPair best_sockets;
        auto worse = make_peer(worse_sockets, 10, "127.0.0.101:3030");
        auto busy = make_peer(busy_sockets, 10, "127.0.0.102:3030");
        auto best = make_peer(best_sockets, 10, "127.0.0.103:3030");

        geocoin::network::SyncManagerTestAccess::register_sync_peer(sync, worse, 10, 5, 0);
        geocoin::network::SyncManagerTestAccess::register_sync_peer(sync, busy, 10, 1, 3);
        geocoin::network::SyncManagerTestAccess::register_sync_peer(sync, best, 10, 1, 1);

        expect(geocoin::network::SyncManagerTestAccess::find_best_peer(sync) == best,
               "find_best_peer prefers lowest bad-header score and then fewest requests");

        geocoin::network::SyncManagerTestAccess::set_peer_download_scores(
            sync,
            best->get_address(),
            4,
            0,
            0,
            0.0
        );
        expect(geocoin::network::SyncManagerTestAccess::find_best_peer(sync) == busy,
               "find_best_peer penalizes peers with repeated block download timeouts");

        best->set_state(geocoin::network::PeerState::AUTHENTICATING);
        expect(geocoin::network::SyncManagerTestAccess::find_best_peer(sync) == busy,
               "find_best_peer skips peers that are no longer READY");
    }
}

void test_download_scheduler_starts_at_winning_fork_ancestor()
{
    ChainFixture fixture;

    auto* active_tip = fixture.blockchain->get_tip();
    uint64_t active_work = active_tip->chain_work.data[0];
    for (geocoin::Height height = 1; height <= 3; ++height) {
        geocoin::core::Block block = fixture.genesis;
        block.header.prev_block_hash = active_tip->get_block_hash();
        block.header.timestamp = fixture.genesis.header.timestamp + static_cast<uint32_t>(height);
        block.header.merkle_root = hash_from_seed(static_cast<uint8_t>(0xa0 + height));
        block.header.witness_commitment = hash_from_seed(static_cast<uint8_t>(0xb0 + height));
        block.header.nonce = height;
        active_tip = geocoin::core::BlockchainTestAccess::add_index(
            *fixture.blockchain,
            block,
            active_tip,
            height,
            ++active_work,
            geocoin::chain::BlockStatus::VALID_TREE
        );
    }
    geocoin::core::BlockchainTestAccess::set_active_tip(*fixture.blockchain, active_tip);

    geocoin::network::SyncManager sync(fixture.chain_state, fixture.blockchain);
    sync.set_max_parallel_downloads(3);
    geocoin::network::SyncManagerTestAccess::set_syncing(sync, 4);

    SocketPair sockets;
    auto peer = make_peer(sockets, 4, "127.0.0.121:3030");
    geocoin::network::SyncManagerTestAccess::register_sync_peer(sync, peer, 4, 0, 0);

    const geocoin::Hash256 fork2 = hash_from_seed(0xc2);
    const geocoin::Hash256 fork3 = hash_from_seed(0xc3);
    const geocoin::Hash256 fork4 = hash_from_seed(0xc4);
    geocoin::network::SyncManagerTestAccess::seed_download_queue(sync, 2, fork2);
    geocoin::network::SyncManagerTestAccess::seed_download_queue(sync, 3, fork3);
    geocoin::network::SyncManagerTestAccess::seed_download_queue(sync, 4, fork4);

    sync.update();
    const auto payload = expect_message_type(
        sockets.read_message(),
        geocoin::network::MessageType::GETDATA,
        "winning fork bodies are requested"
    );
    const auto getdata = geocoin::network::InvMessage::deserialize(payload);
    expect(getdata.items.size() == 3,
           "scheduler requests the full crossing-work fork path inside the window");
    expect(getdata.items[0].hash == fork2 &&
               getdata.items[1].hash == fork3 &&
               getdata.items[2].hash == fork4,
           "scheduler requests a winning fork parent-first, including ancestors below active tip");
    const auto stats = sync.get_stats();
    expect(stats.download_queue_size == 0 && stats.downloading_blocks == 3,
           "all winning-fork ancestors become in-flight downloads");
}

void test_batched_getdata_retry_avoidance_and_adaptive_window()
{
    {
        ChainFixture fixture;
        geocoin::network::SyncManager sync(fixture.chain_state, fixture.blockchain);
        sync.set_max_parallel_downloads(4);
        geocoin::network::SyncManagerTestAccess::set_syncing(sync, 10);

        SocketPair sockets;
        auto peer = make_peer(sockets, 10, "127.0.0.120:3030");
        geocoin::network::SyncManagerTestAccess::register_sync_peer(sync, peer, 10, 0, 0);

        std::vector<geocoin::Hash256> hashes;
        for (geocoin::Height height = 1; height <= 4; ++height) {
            auto hash = hash_from_seed(static_cast<uint8_t>(0x40 + height));
            hashes.push_back(hash);
            geocoin::network::SyncManagerTestAccess::seed_download_queue(sync, height, hash);
        }

        sync.update();
        const auto payload = expect_message_type(
            sockets.read_message(),
            geocoin::network::MessageType::GETDATA,
            "scheduler batches adjacent block requests into one GETDATA"
        );
        const auto getdata = geocoin::network::InvMessage::deserialize(payload);
        expect(getdata.items.size() == hashes.size(), "batched GETDATA contains all eligible hashes");
        for (size_t i = 0; i < hashes.size(); ++i) {
            expect(getdata.items[i].hash == hashes[i], "batched GETDATA preserves height order");
        }
        const auto stats = sync.get_stats();
        expect(stats.download_queue_size == 0 && stats.downloading_blocks == 4,
               "batched GETDATA tracks every requested block as in-flight");
    }

    {
        ChainFixture fixture;
        geocoin::network::SyncManager sync(fixture.chain_state, fixture.blockchain);
        sync.set_max_parallel_downloads(1);
        geocoin::network::SyncManagerTestAccess::set_syncing(sync, 3);

        SocketPair slow_sockets;
        SocketPair fast_sockets;
        auto slow = make_peer(slow_sockets, 3, "127.0.0.121:3030");
        auto fast = make_peer(fast_sockets, 3, "127.0.0.122:3030");
        geocoin::network::SyncManagerTestAccess::register_sync_peer(sync, fast, 3, 0, 0);

        const geocoin::Hash256 stale_hash = hash_from_seed(0x68);
        geocoin::network::SyncManagerTestAccess::mark_downloading(
            sync,
            1,
            stale_hash,
            slow,
            std::chrono::seconds(31)
        );

        sync.update();
        expect(slow_sockets.read_message(std::chrono::milliseconds(50)).empty(),
               "timed-out block is not immediately retried on the same peer when another peer is ready");
        const auto payload = expect_message_type(
            fast_sockets.read_message(),
            geocoin::network::MessageType::GETDATA,
            "timed-out block is retried on a different ready peer"
        );
        const auto getdata = geocoin::network::InvMessage::deserialize(payload);
        expect(getdata.items.size() == 1 && getdata.items.front().hash == stale_hash,
               "retry GETDATA requests the timed-out hash");
        expect(geocoin::network::SyncManagerTestAccess::peer_timeout_score(sync, slow->get_address()) == 1,
               "timed-out peer receives a download timeout penalty");
        expect(geocoin::network::SyncManagerTestAccess::avoids_peer_for_hash(
                   sync,
                   stale_hash,
                   slow->get_address()),
               "timed-out hash records the peer to avoid on retry");
    }

    {
        ChainFixture fixture;
        geocoin::network::SyncManager sync(fixture.chain_state, fixture.blockchain);
        sync.set_max_parallel_downloads(4);
        geocoin::network::SyncManagerTestAccess::set_syncing(sync, 100);
        geocoin::network::SyncManagerTestAccess::set_download_window_limits(sync, 64, 64, 128);

        SocketPair sockets;
        auto peer = make_peer(sockets, 100, "127.0.0.123:3030");
        geocoin::network::SyncManagerTestAccess::register_sync_peer(sync, peer, 100, 0, 0);
        geocoin::network::SyncManagerTestAccess::mark_downloading(
            sync,
            1,
            hash_from_seed(0x71),
            peer,
            std::chrono::seconds(0)
        );
        geocoin::network::SyncManagerTestAccess::mark_downloading(
            sync,
            2,
            hash_from_seed(0x72),
            peer,
            std::chrono::seconds(0)
        );
        for (geocoin::Height height = 3; height <= 70; ++height) {
            geocoin::network::SyncManagerTestAccess::seed_download_queue(
                sync,
                height,
                hash_from_seed(static_cast<uint8_t>(height))
            );
        }

        sync.update();
        expect(geocoin::network::SyncManagerTestAccess::download_window_size(sync) == 128,
               "download window grows while queue is deep and downloads are healthy");

        geocoin::network::SyncManager shrink(fixture.chain_state, fixture.blockchain);
        geocoin::network::SyncManagerTestAccess::set_syncing(shrink, 100);
        geocoin::network::SyncManagerTestAccess::set_download_window_limits(shrink, 128, 64, 128);
        SocketPair shrink_sockets;
        auto penalized = make_peer(shrink_sockets, 100, "127.0.0.124:3030");
        geocoin::network::SyncManagerTestAccess::register_sync_peer(shrink, penalized, 100, 0, 0);
        geocoin::network::SyncManagerTestAccess::set_peer_download_scores(
            shrink,
            penalized->get_address(),
            3,
            0,
            0,
            0.0
        );

        shrink.update();
        expect(geocoin::network::SyncManagerTestAccess::download_window_size(shrink) == 64,
               "download window shrinks when peer timeout pressure is high");
    }
}

void test_update_periodic_debug_snapshot()
{
    ChainFixture fixture;
    geocoin::network::SyncManager sync(fixture.chain_state, fixture.blockchain);
    geocoin::network::SyncManagerTestAccess::set_syncing(sync, 0);

    for (int i = 0; i < 120; ++i) {
        sync.update();
    }

    const auto stats = sync.get_stats();
    expect(stats.status == geocoin::network::SyncStatus::SYNCING,
           "periodic update snapshot keeps explicit syncing status");
}

void test_inv_known_header_without_prior_start_updates_target()
{
    ChainFixture fixture;
    geocoin::network::SyncManager sync(fixture.chain_state, fixture.blockchain);

    SocketPair sockets;
    auto peer = make_peer(sockets, 7, "127.0.0.77:3030");
    const geocoin::Hash256 known_header = hash_from_seed(0x77);
    geocoin::network::SyncManagerTestAccess::seed_header_only(sync, 1, known_header);

    sync.handle_inv(peer, {known_header});

    const auto stats = sync.get_stats();
    expect(stats.status == geocoin::network::SyncStatus::SYNCING,
           "known header INV can start syncing without prior start_sync");
    expect(stats.target_height == 1,
           "known header INV raises target only to its validated header height");
    expect(stats.download_queue_size == 1, "known header INV queues block download");
}

void test_legacy_getblocks_locator_uses_genesis()
{
    ChainFixture fixture;

    {
        geocoin::network::SyncManager sync(fixture.chain_state, fixture.blockchain);
        SocketPair sockets;
        auto peer = make_peer(sockets, 1);
        geocoin::network::SyncManagerTestAccess::send_getblocks(sync, peer);

        const geocoin::bytes payload = expect_message_type(
            sockets.read_message(),
            geocoin::network::MessageType::GETBLOCKS,
            "legacy GETBLOCKS helper sends GETBLOCKS"
        );
        const auto getblocks = geocoin::network::GetBlocksMessage::deserialize(payload);
        expect(getblocks.version == 1, "GETBLOCKS locator version is stable");
        expect(getblocks.locator_hashes.size() == 1, "genesis-only chain locator has one hash");
        expect(getblocks.locator_hashes.front() == fixture.genesis.get_hash(),
               "genesis-only GETBLOCKS locator uses genesis hash");
    }

    geocoin::chain::BlockIndex* prev_index = fixture.blockchain->get_tip();
    geocoin::core::Block prev_block = fixture.genesis;
    std::vector<geocoin::Hash256> active_hashes{fixture.genesis.get_hash()};
    for (geocoin::Height height = 1; height <= 3; ++height) {
        geocoin::core::Block block = make_child_candidate(prev_block, 0x2100ffff);
        block.header.timestamp = prev_block.header.timestamp + static_cast<uint32_t>(height);
        block.header.merkle_root = block.calculate_merkle_root();
        block.header.witness_commitment = block.calculate_witness_commitment();
        int32_t file = 0;
        uint32_t pos = 0;
        expect(fixture.block_storage->save_block_with_pos(block, height, file, pos, false),
               "synthetic locator block saves");
        expect(fixture.block_storage->set_active_block_and_tip(height, block.get_hash()),
               "synthetic locator active height mapping saves");
        auto* index = geocoin::core::BlockchainTestAccess::add_index(
            *fixture.blockchain,
            block,
            prev_index,
            height,
            static_cast<uint64_t>(height + 1),
            geocoin::chain::BlockStatus::HAVE_DATA | geocoin::chain::BlockStatus::VALID_TREE
        );
        index->block_pos.file = file;
        index->block_pos.pos = pos;
        prev_index = index;
        prev_block = block;
        active_hashes.push_back(block.get_hash());
    }
    geocoin::core::BlockchainTestAccess::set_active_tip(*fixture.blockchain, prev_index);

    geocoin::network::SyncManager sync(fixture.chain_state, fixture.blockchain);
    SocketPair sockets;
    auto peer = make_peer(sockets, 4, "127.0.0.104:3030");
    geocoin::network::SyncManagerTestAccess::send_getblocks(sync, peer);
    const geocoin::bytes payload = expect_message_type(
        sockets.read_message(),
        geocoin::network::MessageType::GETBLOCKS,
        "multi-height GETBLOCKS helper sends locator"
    );
    const auto getblocks = geocoin::network::GetBlocksMessage::deserialize(payload);
    expect(getblocks.locator_hashes.size() >= 2, "multi-height locator contains tip and older blocks");
    expect(getblocks.locator_hashes.front() == active_hashes.back(), "multi-height locator starts at active tip");
    expect(getblocks.locator_hashes.back() == active_hashes.front(), "multi-height locator includes genesis");
}

std::vector<std::pair<geocoin::core::Block, geocoin::chain::BlockIndex*>> build_synthetic_active_chain(
    ChainFixture& fixture,
    geocoin::Height tip_height
)
{
    std::vector<std::pair<geocoin::core::Block, geocoin::chain::BlockIndex*>> chain{
        {fixture.genesis, fixture.blockchain->get_tip()}};
    for (geocoin::Height height = 1; height <= tip_height; ++height) {
        geocoin::core::Block block = make_child_candidate(chain.back().first, 0x2100ffff);
        auto* index = geocoin::core::BlockchainTestAccess::add_index(
            *fixture.blockchain,
            block,
            chain.back().second,
            height,
            1000000ULL * (height + 1),
            geocoin::chain::BlockStatus::HAVE_DATA | geocoin::chain::BlockStatus::VALID_TREE
        );
        chain.emplace_back(block, index);
    }
    geocoin::core::BlockchainTestAccess::set_active_tip(*fixture.blockchain, chain.back().second);
    return chain;
}

void test_headers_below_reorg_floor_are_not_indexed()
{
    using Access = geocoin::network::SyncManagerTestAccess;
    ChainFixture fixture;
    const geocoin::Height depth = geocoin::consensus::params::MAX_REORG_DEPTH;
    const auto active = build_synthetic_active_chain(fixture, depth + 10);
    geocoin::network::SyncManager sync(fixture.chain_state, fixture.blockchain);
    SocketPair sockets;
    auto peer = make_peer(sockets, depth + 10, "127.0.0.121:3030");
    Access::register_sync_peer(sync, peer, depth + 10, 0, 0);

    // Old history has little cumulative work, so it is the cheapest place to
    // mine an unbounded side branch; activation would exceed the reorg limit.
    const auto deep = mine_easy_child_block(active[1].first, active[1].second, 0x51);
    expect(!sync.handle_headers(peer, {deep.header}), "fork below the reorg limit is rejected");
    expect(!Access::has_header(sync, deep.get_hash()), "fork below the reorg limit is not indexed");

    const auto& near_tip = active[active.size() - 5];
    const auto shallow = mine_easy_child_block(near_tip.first, near_tip.second, 0x52);
    expect(sync.handle_headers(peer, {shallow.header}), "fork within the reorg limit is accepted");
    expect(Access::has_header(sync, shallow.get_hash()), "fork within the reorg limit is indexed");
}

void test_side_branch_header_budget()
{
    using Access = geocoin::network::SyncManagerTestAccess;
    ChainFixture fixture;
    const auto active = build_synthetic_active_chain(fixture, 3);
    geocoin::network::SyncManager sync(fixture.chain_state, fixture.blockchain);
    Access::set_side_header_budget(sync, 2);

    SocketPair first_sockets;
    SocketPair reconnect_sockets;
    SocketPair extension_sockets;
    SocketPair other_sockets;
    auto first = make_peer(first_sockets, 3, "127.0.0.131:3030");
    auto reconnect = make_peer(reconnect_sockets, 3, "127.0.0.131:3031");
    auto extension_peer = make_peer(extension_sockets, 3, "127.0.0.131:3032");
    auto other = make_peer(other_sockets, 3, "127.0.0.132:3030");
    for (const auto& peer : {first, reconnect, extension_peer, other}) {
        Access::register_sync_peer(sync, peer, 3, 0, 0);
    }

    // Siblings below the active tip never become the best header.
    std::vector<geocoin::core::Block> siblings;
    for (uint8_t seed = 0; seed < 4; ++seed) {
        siblings.push_back(mine_easy_child_block(
            active[1].first, active[1].second, static_cast<uint8_t>(0x60 + seed)));
    }
    expect(sync.handle_headers(first, {siblings[0].header}), "first side header fits the budget");
    expect(sync.handle_headers(first, {siblings[0].header}), "an already indexed side header is not charged again");
    expect(sync.handle_headers(first, {siblings[1].header}), "second side header fits the budget");
    expect(!sync.handle_headers(reconnect, {siblings[2].header}),
           "reconnecting from the same address does not reset the side-header budget");
    expect(!Access::has_header(sync, siblings[2].get_hash()), "over-budget side header is not indexed");
    expect(sync.handle_headers(other, {siblings[3].header}), "another address has its own side-header budget");

    const auto extension = mine_easy_child_block(active.back().first, active.back().second, 0x70);
    expect(sync.handle_headers(extension_peer, {extension.header}),
           "a header that extends the best tip is not charged to the side-header budget");
    expect(Access::has_header(sync, extension.get_hash()), "best-tip extension is indexed");
}

void test_easy_diff_networks_tighten_header_limits()
{
    using Access = geocoin::network::SyncManagerTestAccess;
    ChainFixture fixture;
    geocoin::network::SyncManager sync(fixture.chain_state, fixture.blockchain);
    expect(geocoin::consensus::Params().allow_min_difficulty_blocks() ||
               geocoin::consensus::Params().fixed_difficulty(),
           "sync unit tests run on an easy-diff network");
    expect(Access::max_side_headers_per_source(sync) <= 256,
           "easy-diff networks use a tighter per-source side-header budget");
    expect(Access::max_headers_index_size(sync) <= 32768,
           "easy-diff networks keep a smaller header index for side branches");

    const auto active = build_synthetic_active_chain(fixture, 3);
    Access::set_side_header_budget(sync, 0);
    SocketPair sockets;
    auto peer = make_peer(sockets, 3, "127.0.0.141:3030");
    Access::register_sync_peer(sync, peer, 3, 0, 0);
    const auto side = mine_easy_child_block(active[1].first, active[1].second, 0x81);
    expect(!sync.handle_headers(peer, {side.header}),
           "a zero side-header budget rejects a cheap fork without indexing it");
    expect(!Access::has_header(sync, side.get_hash()),
           "an over-budget cheap header is not stored");
}

void test_global_best_header_path_queues_crossing_fork_ancestors()
{
    ChainFixture fixture;
    geocoin::network::SyncManager sync(fixture.chain_state, fixture.blockchain);

    const uint64_t active_work = fixture.blockchain->get_tip()->chain_work.data[0];
    auto make_parent = [&](uint8_t seed, uint64_t work) {
        geocoin::core::Block parent = make_child_candidate(fixture.genesis, 0x2100ffff);
        parent.header.timestamp = fixture.genesis.header.timestamp + 1;
        parent.header.merkle_root = hash_from_seed(seed);
        parent.header.witness_commitment = hash_from_seed(static_cast<uint8_t>(seed + 0x20));
        auto* index = geocoin::core::BlockchainTestAccess::add_index(
            *fixture.blockchain,
            parent,
            fixture.blockchain->get_tip(),
            1,
            work,
            geocoin::chain::BlockStatus::VALID_TREE
        );
        return std::pair<geocoin::core::Block, geocoin::chain::BlockIndex*>{parent, index};
    };

    auto [parent_a, parent_a_index] = make_parent(0x20, active_work + 3);
    auto [parent_b, parent_b_index] = make_parent(0x30, active_work);

    const auto a2 = mine_easy_child_block(
        parent_a,
        parent_a_index,
        0x21
    );
    const auto b2 = mine_easy_child_block(
        parent_b,
        parent_b_index,
        0x31
    );
    geocoin::chain::BlockIndex b2_index(b2.header);
    b2_index.hash = b2.get_hash();
    b2_index.prev = parent_b_index;
    b2_index.height = 2;
    b2_index.chain_work = geocoin::consensus::calculate_chainwork(parent_b_index, b2.header.bits);

    const auto b3 = mine_easy_child_block(
        b2,
        &b2_index,
        0x32
    );
    geocoin::chain::BlockIndex b3_index(b3.header);
    b3_index.hash = b3.get_hash();
    b3_index.prev = &b2_index;
    b3_index.height = 3;
    b3_index.chain_work = geocoin::consensus::calculate_chainwork(&b2_index, b3.header.bits);

    const auto b4 = mine_easy_child_block(
        b3,
        &b3_index,
        0x33
    );

    SocketPair sockets;
    auto peer = make_peer(sockets, 4, "127.0.0.120:3030");
    geocoin::network::SyncManagerTestAccess::register_sync_peer(sync, peer, 4, 0, 0);

    geocoin::core::BlockHeader future = a2.header;
    future.timestamp = static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count() + geocoin::consensus::params::MAX_FUTURE_BLOCK_TIME + 1
    );
    expect(!sync.handle_headers(peer, {future}),
           "headers-first rejects a future header before queueing its body");
    expect(!geocoin::network::SyncManagerTestAccess::has_header(sync, future.get_hash()),
           "future header is not retained in the header index");

    expect(sync.handle_headers(peer, {a2.header}), "first fork header is accepted");
    expect(geocoin::network::SyncManagerTestAccess::queue_contains(sync, a2.get_hash()),
           "first globally best fork is queued");

    expect(sync.handle_headers(peer, {b2.header}), "weaker fork parent header is retained");
    expect(!geocoin::network::SyncManagerTestAccess::queue_contains(sync, b2.get_hash()),
           "weaker fork is not downloaded before it wins globally");
    expect(sync.handle_headers(peer, {b3.header}), "weaker fork continuation is retained");
    expect(sync.handle_headers(peer, {b4.header}), "crossing-work fork tip is accepted");

    expect(!geocoin::network::SyncManagerTestAccess::queue_contains(sync, a2.get_hash()),
           "stale best-fork request is removed after a global work crossing");
    expect(geocoin::network::SyncManagerTestAccess::queue_contains(sync, b2.get_hash()),
           "winning fork queues its previously non-best parent");
    expect(geocoin::network::SyncManagerTestAccess::queue_contains(sync, b3.get_hash()),
           "winning fork queues its intermediate ancestor");
    expect(geocoin::network::SyncManagerTestAccess::queue_contains(sync, b4.get_hash()),
           "winning fork queues its tip");
}

void test_getheaders_skips_side_fork_locator()
{
    ChainFixture fixture;
    auto* genesis_index = fixture.blockchain->get_tip();
    const uint64_t genesis_work = genesis_index->chain_work.data[0];

    geocoin::core::Block active1 = fixture.genesis;
    active1.header.prev_block_hash = fixture.genesis.get_hash();
    active1.header.timestamp += 1;
    active1.header.merkle_root = hash_from_seed(0xd1);
    auto* active1_index = geocoin::core::BlockchainTestAccess::add_index(
        *fixture.blockchain,
        active1,
        genesis_index,
        1,
        genesis_work + 1,
        geocoin::chain::BlockStatus::VALID_TREE
    );

    geocoin::core::Block active2 = active1;
    active2.header.prev_block_hash = active1.get_hash();
    active2.header.timestamp += 1;
    active2.header.merkle_root = hash_from_seed(0xd2);
    auto* active2_index = geocoin::core::BlockchainTestAccess::add_index(
        *fixture.blockchain,
        active2,
        active1_index,
        2,
        genesis_work + 2,
        geocoin::chain::BlockStatus::VALID_TREE
    );

    geocoin::core::Block side1 = fixture.genesis;
    side1.header.prev_block_hash = fixture.genesis.get_hash();
    side1.header.timestamp += 1;
    side1.header.merkle_root = hash_from_seed(0xe1);
    geocoin::core::BlockchainTestAccess::add_index(
        *fixture.blockchain,
        side1,
        genesis_index,
        1,
        genesis_work + 1,
        geocoin::chain::BlockStatus::VALID_TREE
    );
    geocoin::core::BlockchainTestAccess::set_active_tip(*fixture.blockchain, active2_index);

    geocoin::network::SyncManager sync(fixture.chain_state, fixture.blockchain);
    const auto headers = sync.handle_getheaders(
        nullptr,
        {side1.get_hash(), fixture.genesis.get_hash()},
        geocoin::Hash256{}
    );
    expect(headers.size() == 2,
           "GETHEADERS ignores a known side-fork locator and starts after the active common ancestor");
    expect(headers[0].get_hash() == active1.get_hash() &&
               headers[1].get_hash() == active2.get_hash(),
           "GETHEADERS returns every active fork ancestor in order");
}

void test_orphan_buffer_deduplicates_and_enforces_limit()
{
    ChainFixture fixture;
    geocoin::network::SyncManager sync(fixture.chain_state, fixture.blockchain);

    const geocoin::Hash256 unknown_parent = hash_from_seed(0xe0);
    auto make_orphan = [&](uint32_t nonce) {
        geocoin::core::Block block = fixture.genesis;
        block.header.prev_block_hash = unknown_parent;
        block.header.timestamp = fixture.genesis.header.timestamp + 100 + nonce;
        block.header.nonce = nonce;
        return block;
    };

    const auto first = make_orphan(1);
    const auto second = make_orphan(2);
    geocoin::network::SyncManagerTestAccess::store_orphan(sync, first.get_hash(), first, nullptr);
    geocoin::network::SyncManagerTestAccess::store_orphan(sync, second.get_hash(), second, nullptr);
    expect(sync.get_stats().orphan_blocks == 2, "two unique orphans are tracked");
    geocoin::network::SyncManagerTestAccess::store_orphan(sync, second.get_hash(), second, nullptr);
    expect(sync.get_stats().orphan_blocks == 2, "duplicate orphan does not grow buffer");

    for (uint32_t nonce = 3; nonce <= 30; ++nonce) {
        const auto orphan = make_orphan(nonce);
        geocoin::network::SyncManagerTestAccess::store_orphan(
            sync,
            orphan.get_hash(),
            orphan,
            nullptr
        );
    }

    expect(sync.get_stats().orphan_blocks == 16, "orphan buffer enforces memory-safe max size");
    sync.stop_sync();
    expect(sync.get_stats().orphan_blocks == 0, "stop_sync clears saturated orphan buffer");
}

} // namespace

int main()
{
    try {
        geocoin::consensus::SelectParams(geocoin::consensus::Network::TESTNET);
        test_start_stop_and_getheaders();
        test_null_blockchain_inv_guard();
        test_genesis_block_handling();
        test_headers_inv_and_block_queue();
        test_handle_block_queue_cleanup_orphan_peer_and_pending_gap();
        test_orphan_store_take_and_drain_paths();
        test_getheaders_stop_hash_and_null_peer_guards();
        test_header_rejection_penalty_guards();
        test_valid_headers_index_and_download_queue_paths();
        test_header_state_pruning();
        test_empty_headers_penalty_disconnects_ahead_peer();
        test_download_queue_dispatch_and_inv_dedupes();
        test_inv_known_headers_and_idle_watchdog();
        test_download_timeout_requeues_without_ready_peer();
        test_download_timeout_requeues_ready_peer_by_age();
        test_cross_peer_block_cannot_consume_another_peers_request();
        test_download_scheduler_peer_selection_and_request_guards();
        test_download_scheduler_starts_at_winning_fork_ancestor();
        test_batched_getdata_retry_avoidance_and_adaptive_window();
        test_update_periodic_debug_snapshot();
        test_inv_known_header_without_prior_start_updates_target();
        test_legacy_getblocks_locator_uses_genesis();
        test_global_best_header_path_queues_crossing_fork_ancestors();
        test_headers_below_reorg_floor_are_not_indexed();
        test_side_branch_header_budget();
        test_easy_diff_networks_tighten_header_limits();
        test_getheaders_skips_side_fork_locator();
        test_orphan_buffer_deduplicates_and_enforces_limit();
        std::cout << "test_sync_manager_units: ok\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "test_sync_manager_units failed: " << e.what() << "\n";
        return 1;
    }
}
