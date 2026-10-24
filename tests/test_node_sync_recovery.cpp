#include "chain_test_fixture.hpp"
#include "secure_channel_test_fixture.hpp"
#include "geocoin/network/messages.hpp"
#include "geocoin/network/sync_manager.hpp"
#include "nlohmann_json.hpp"

#include <fstream>
#include <iostream>
#include <limits>
#include <set>

namespace geocoin::network {
struct SyncManagerTestAccess {
    static void prefer_peer(SyncManager& sync, const std::shared_ptr<Peer>& peer) {
        std::lock_guard<std::mutex> lock(sync.peers_mutex_);
        sync.sync_peers_.at(peer->get_address()).completed_blocks = 100;
        sync.sync_peers_.at(peer->get_address()).bad_headers_score = 0;
    }
    static bool request_blocks(SyncManager& sync, const std::shared_ptr<Peer>& peer,
                               const std::vector<Hash256>& hashes) {
        return sync.request_blocks(peer, hashes);
    }
    static void block_request_window_due(SyncManager& sync) {
        std::lock_guard<std::mutex> lock(sync.peers_mutex_);
        for (auto& [address, info] : sync.sync_peers_) {
            for (auto& requested_ms : info.recent_block_requests_ms) requested_ms = 0;
        }
    }
    static void headers_retry_due(SyncManager& sync) {
        std::lock_guard<std::mutex> lock(sync.peers_mutex_);
        for (auto& [address, info] : sync.sync_peers_) info.last_headers_request_ms = 0;
    }
    static void headers_interval_due(SyncManager& sync) {
        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
        std::lock_guard<std::mutex> lock(sync.peers_mutex_);
        for (auto& [address, info] : sync.sync_peers_) info.last_headers_request_ms = now_ms - 1000;
    }
    static PeerSyncInfo peer_info(SyncManager& sync, const std::shared_ptr<Peer>& peer) {
        std::lock_guard<std::mutex> lock(sync.peers_mutex_);
        return sync.sync_peers_.at(peer->get_address());
    }
    static std::shared_ptr<Peer> requester(SyncManager& sync, const Hash256& hash) {
        std::lock_guard<std::mutex> lock(sync.queue_mutex_);
        const auto it = sync.downloading_blocks_.find(hash);
        return it == sync.downloading_blocks_.end() ? nullptr : it->second.peer;
    }
    static uint32_t attempts(SyncManager& sync, const Hash256& hash) {
        std::lock_guard<std::mutex> lock(sync.queue_mutex_);
        const auto it = sync.download_attempts_.find(hash);
        return it == sync.download_attempts_.end() ? 0 : it->second;
    }
    static bool has_header(SyncManager& sync, const Hash256& hash) {
        std::lock_guard<std::mutex> lock(sync.queue_mutex_);
        return sync.headers_index_.contains(hash);
    }
    static bool has_retry_metadata(SyncManager& sync, const Hash256& hash) {
        std::lock_guard<std::mutex> lock(sync.queue_mutex_);
        return sync.download_attempts_.contains(hash) || sync.download_avoid_peer_.contains(hash);
    }
    static void local_retry_due(SyncManager& sync, bool due) {
        sync.next_local_block_retry_ms_.store(due ? 0 : (std::numeric_limits<uint64_t>::max)(),
                                              std::memory_order_relaxed);
    }
    static void prune_header_history(SyncManager& sync, Height current_height) {
        std::lock_guard<std::mutex> lock(sync.queue_mutex_);
        sync.headers_prune_keep_depth_ = 0;
        sync.prune_headers_state_locked(current_height);
    }
    static void expire_requests(SyncManager& sync) {
        {
            std::lock_guard<std::mutex> lock(sync.queue_mutex_);
            for (auto& [hash, info] : sync.downloading_blocks_) info.requested_ms = 1;
        }
        sync.check_timeouts();
    }
};
}

namespace geocoin::storage {
struct BlockStorageTestAccess {
    static void refuse_writes(BlockStorage& storage, bool refused) {
        std::lock_guard<std::recursive_mutex> lock(storage.db_write_mutex_);
        storage.split_write_failed_ = refused;
    }
};
}

namespace {
using namespace geocoin;
using network::SyncManager;
using Access = network::SyncManagerTestAccess;
using test::expect;
nlohmann::json checks = nlohmann::json::array();
void check(bool ok, const std::string& name) { checks.push_back({{"name", name}, {"passed", ok}}); }

class StateWriteFailure final : public state::MemoryStorage {
public:
    bool fail_commit = false;
    bool fail_journal = false;
    unsigned journal_failures_remaining = 0;
    unsigned journal_failures = 0;
    bool commit_batch() override {
        if (fail_commit) { fail_commit = false; return false; }
        return MemoryStorage::commit_batch();
    }
    bool put_metadata(const std::string& key, const bytes& value) override {
        if (key == "chainstate_journal" && (fail_journal || journal_failures_remaining > 0)) {
            fail_journal = false;
            if (journal_failures_remaining > 0) --journal_failures_remaining;
            ++journal_failures;
            return false;
        }
        return MemoryStorage::put_metadata(key, value);
    }
};

// The real Peer and encrypted framing are retained; only the byte stream is local.
class RecordingTransport final : public network::PeerTransport {
public:
    explicit RecordingTransport(std::unique_ptr<network::SecureChannel> remote)
        : remote_(std::move(remote)) {}
    bool connect(std::chrono::milliseconds) override { open_ = true; return true; }
    void close() noexcept override { open_ = false; }
    bool is_open() const noexcept override { return open_; }
    std::ptrdiff_t receive(bytes& buffer, size_t) override { buffer.clear(); return 0; }
    std::ptrdiff_t send(const bytes& wire) override {
        if (!open_) return -1;
        bytes message, payload;
        network::MessageHeader header;
        expect(remote_->unprotect(wire, message) && network::parse_message(message, header, payload),
               "decode real encrypted request");
        if (header.type == network::MessageType::GETHEADERS) {
            ++header_requests;
            last_header_locator = network::GetBlocksMessage::deserialize(payload).locator_hashes;
        }
        if (header.type == network::MessageType::GETDATA) {
            const auto request = network::InvMessage::deserialize(payload);
            for (const auto& item : request.items) requested.push_back(item.hash);
        }
        return static_cast<std::ptrdiff_t>(wire.size());
    }
    std::vector<Hash256> requested;
    size_t header_requests = 0;
    std::vector<Hash256> last_header_locator;
private:
    bool open_ = true;
    std::unique_ptr<network::SecureChannel> remote_;
};

struct Remote {
    std::shared_ptr<network::Peer> peer;
    RecordingTransport* transport;
    explicit Remote(Height height, const std::string& address = "127.0.0.1:31001") {
        auto channel = test::make_secure_channel_fixture();
        auto stream = std::make_unique<RecordingTransport>(std::move(channel.remote_channel));
        transport = stream.get();
        peer = std::make_shared<network::Peer>(address, std::move(stream));
        expect(peer->activate_secure_channel(std::move(channel.peer_keys)), "activate local peer channel");
        auto info = peer->get_info();
        info.height = height;
        info.services = network::NODE_NETWORK;
        peer->update_info(info);
        peer->set_state(network::PeerState::READY);
    }
};

struct Branch {
    std::vector<core::Block> blocks;
    std::vector<std::unique_ptr<chain::BlockIndex>> indices;
    Branch(test::TestChain& chain, const chain::BlockIndex* parent, size_t count, byte seed) {
        for (size_t i = 0; i < count; ++i) {
            auto block = chain.make_block(parent);
            block.transactions[0].outputs[0].address = test::address_from_seed(seed);
            test::TestChain::seal(block, parent);
            auto index = std::make_unique<chain::BlockIndex>(block.header);
            index->hash = block.get_hash();
            index->prev = const_cast<chain::BlockIndex*>(parent);
            index->height = parent->height + 1;
            index->chain_work = consensus::calculate_chainwork(parent, block.header.bits);
            index->build_skip();
            parent = index.get();
            indices.push_back(std::move(index));
            blocks.push_back(std::move(block));
        }
    }
    void announce(SyncManager& sync, Remote& remote) const {
        expect(sync.start_sync(remote.peer, indices.back()->height), "start real header discovery");
        std::vector<core::BlockHeader> headers;
        for (const auto& block : blocks) headers.push_back(block.header);
        expect(sync.handle_headers(remote.peer, headers), "validate complete branch headers");
        sync.update();
        expect(!remote.transport->requested.empty(), "request validated branch bodies");
    }
};

void test_header_discovery_burst() {
    test::TestChain chain;
    SyncManager sync(chain.state, chain.chain);
    Remote remote(0);
    Branch branch(chain, chain.chain->get_tip(), 100, 61);
    expect(!sync.start_sync(remote.peer, 0), "peers connect at genesis before mining");

    // The miner's BLOCK announcements can arrive before the first body is
    // applied. Every unknown parent used to provoke another GETHEADERS.
    for (size_t i = 1; i < branch.blocks.size(); ++i) {
        sync.handle_block(remote.peer, branch.blocks[i]);
    }
    for (const auto& block : branch.blocks) {
        sync.handle_inv(remote.peer, {block.get_hash()});
    }
    check(remote.transport->header_requests == 1,
          "burst announcements share one outstanding header discovery request");
    check(sync.get_stats().target_height == 0,
          "burst discovery does not trust an unvalidated target height");
    check(!sync.is_syncing(), "unvalidated announcements do not pause mining while discovery is queued");
    check(sync.get_stats().orphan_blocks == 0,
          "unsolicited burst bodies do not consume the orphan buffer");
}

void test_new_block_uses_a_peer_with_validated_headers() {
    test::TestChain chain;
    SyncManager sync(chain.state, chain.chain);
    Remote lagging(1), source(1, "127.0.0.1:31002");
    Branch branch(chain, chain.chain->get_tip(), 1, 67);
    expect(sync.start_sync(lagging.peer, 1), "probe an older peer before a new block exists");
    expect(sync.handle_headers(lagging.peer, {}), "older peer has no new block yet");
    Access::prefer_peer(sync, lagging.peer);
    branch.announce(sync, source);
    check(lagging.transport->requested.empty() &&
              source.transport->requested == std::vector<Hash256>{branch.blocks[0].get_hash()},
          "new block is requested from its header source instead of a faster lagging peer");
    expect(sync.handle_block(source.peer, branch.blocks[0]), "the selected source supplies the new block");
    check(chain.chain->get_height() == 1, "source selection avoids waiting for an unavailable block");
}

void test_block_download_request_budget() {
    test::TestChain chain;
    SyncManager sync(chain.state, chain.chain);
    Remote first(1), second(1, "127.0.0.1:31002");
    Branch branch(chain, chain.chain->get_tip(), 1, 66);
    const auto& block = branch.blocks.front();
    size_t sent = 0;
    // Fill a receive-policy window without advancing wall time. The sender
    // must leave room for other GETDATA consumers instead of reaching 32.
    for (size_t i = 0; i < 32; ++i) {
        if (Access::request_blocks(sync, first.peer, {block.get_hash()})) ++sent;
    }
    check(sent > 0 && sent < 32 && first.transport->requested.size() == sent,
          "block requests are bounded below the shared GETDATA receive limit");
    check(Access::request_blocks(sync, second.peer, {block.get_hash()}),
          "one peer's exhausted request window does not throttle another peer");
    second.peer->disconnect();
    expect(sync.start_sync(first.peer, 1) && sync.handle_headers(first.peer, {block.header}),
           "queue a real validated block after exhausting the request window");
    sync.update();
    check(sync.get_stats().download_queue_size == 1 && sync.get_stats().downloading_blocks == 0 &&
              first.transport->requested.size() == sent,
          "rate-limited downloads retain queue demand without repeated sends");
    Access::block_request_window_due(sync);
    sync.update();
    check(first.transport->requested.size() == sent + 1 && sync.get_stats().downloading_blocks == 1,
          "queued block resumes when the request window replenishes");
    expect(sync.handle_block(first.peer, block), "apply the block after request budget recovery");
    check(chain.chain->get_height() == 1 && first.peer->is_connected(),
          "request pacing preserves connection and download progress");
}

void test_lost_initial_headers_response() {
    test::TestChain chain;
    SyncManager sync(chain.state, chain.chain);
    Remote remote(0);
    Branch branch(chain, chain.chain->get_tip(), 2, 62);
    sync.handle_inv(remote.peer, {branch.blocks.back().get_hash()});
    expect(remote.transport->header_requests == 1, "initial discovery request sent");
    expect(sync.get_stats().target_height == 0, "no headers or bodies received yet");
    Access::headers_retry_due(sync);
    sync.update();
    check(remote.transport->header_requests == 2,
          "lost initial HEADERS response is retried without a validated target");
    check(sync.get_stats().status == network::SyncStatus::COMPLETED &&
              sync.get_stats().target_height == 0,
          "silent discovery peer cannot keep an unvalidated sync target active");
    Access::headers_retry_due(sync);
    sync.update();
    check(remote.transport->header_requests == 3,
          "discovery retry continues after download status becomes completed");
    expect(sync.handle_headers(remote.peer, {branch.blocks[0].header, branch.blocks[1].header}),
           "delayed headers response supplies a validated target");
    sync.update();
    for (const auto& block : branch.blocks) {
        expect(sync.handle_block(remote.peer, block), "download resumes after a delayed response");
    }
    check(chain.chain->get_height() == 2, "lost response recovery reaches the announced tip");
}

void test_queued_discovery_after_completion() {
    test::TestChain chain;
    SyncManager sync(chain.state, chain.chain);
    Remote remote(0);
    Branch branch(chain, chain.chain->get_tip(), 2, 63);
    sync.handle_inv(remote.peer, {branch.blocks[0].get_hash()});
    // A second announcement arrives after the remote produced its first reply.
    sync.handle_inv(remote.peer, {branch.blocks[1].get_hash()});
    expect(sync.handle_headers(remote.peer, {branch.blocks[0].header}), "receive first reply");
    expect(sync.handle_block(remote.peer, branch.blocks[0]), "apply first announced block");
    expect(!sync.is_syncing(), "first body completes the validated download target");
    check(remote.transport->header_requests == 1, "new announcement is deferred during a pending reply");
    Access::headers_interval_due(sync);
    sync.update();
    check(remote.transport->header_requests == 2,
          "queued discovery is sent even after the previous target completes");
    expect(sync.handle_headers(remote.peer, {branch.blocks[1].header}), "discover the later block");
    expect(sync.handle_block(remote.peer, branch.blocks[1]), "apply the later block");
    Access::headers_retry_due(sync);
    sync.update();
    check(remote.transport->header_requests == 2,
          "answered discovery is not retried without new demand");
    check(chain.chain->get_height() == 2, "queued discovery reaches the later tip");
}

void test_header_request_pacing_and_reconnect() {
    test::TestChain chain;
    SyncManager sync(chain.state, chain.chain);
    Remote first(0);
    Remote second(0, "127.0.0.1:31002");
    Branch branch(chain, chain.chain->get_tip(), 2, 64);
    sync.handle_inv(first.peer, {branch.blocks[0].get_hash()});
    sync.handle_inv(second.peer, {branch.blocks[0].get_hash()});
    check(first.transport->header_requests == 1 && second.transport->header_requests == 1,
          "discovery pacing is independent for each peer");
    expect(sync.handle_headers(first.peer, {}), "empty reply completes the first request");
    sync.handle_inv(first.peer, {branch.blocks[1].get_hash()});
    check(first.transport->header_requests == 1,
          "immediate replies cannot turn block announcements into a request storm");
    Access::headers_interval_due(sync);
    sync.update();
    check(first.transport->header_requests == 2 && second.transport->header_requests == 1,
          "deferred request is sent after spacing while another peer's request stays pending");

    expect(sync.handle_headers(first.peer, {}), "peer exhausts the old connection's discovery");
    first.peer->disconnect();
    Remote replacement(2, first.peer->get_address());
    check(sync.start_sync(replacement.peer, 2) && replacement.transport->header_requests == 1,
          "reconnected peer does not inherit old exhausted or pending discovery state");
    sync.stop_sync();
    Access::headers_retry_due(sync);
    sync.update();
    check(replacement.transport->header_requests == 1 && second.transport->header_requests == 1,
          "stop_sync cancels queued discoveries and response retries");
}

void test_header_locator_ahead_of_bodies() {
    test::TestChain chain;
    SyncManager sync(chain.state, chain.chain);
    constexpr size_t page_size = 2;
    Remote remote(page_size + 1);
    Branch branch(chain, chain.chain->get_tip(), page_size + 1, 65);
    expect(sync.start_sync(remote.peer, page_size + 1), "start paginated header discovery");
    std::vector<core::BlockHeader> headers;
    for (size_t i = 0; i < page_size; ++i) headers.push_back(branch.blocks[i].header);
    expect(sync.handle_headers(remote.peer, headers), "validate headers before their bodies");
    sync.handle_inv(remote.peer, {branch.blocks.back().get_hash()});
    check(remote.transport->header_requests == 1, "header pagination observes request spacing");
    Access::headers_interval_due(sync);
    sync.update();
    check(remote.transport->header_requests == 2 &&
              !remote.transport->last_header_locator.empty() &&
              remote.transport->last_header_locator.front() == branch.blocks[page_size - 1].get_hash(),
          "next header page starts after the validated page while active height is still zero");
    expect(sync.handle_headers(remote.peer, {branch.blocks.back().header}), "validate final header page");
    check(sync.get_stats().target_height == page_size + 1,
          "header pagination advances beyond the first full page");
}

void test_reverse_fork_download() {
    test::TestChain chain;
    const auto* genesis = chain.chain->get_tip();
    for (size_t i = 0; i < 3; ++i) chain.append(chain.make_block());
    Branch branch(chain, genesis, 4, 52);
    SyncManager sync(chain.state, chain.chain);
    Remote remote(4);
    branch.announce(sync, remote);
    for (size_t i = branch.blocks.size(); i-- > 1;) {
        expect(!sync.handle_block(remote.peer, branch.blocks[i]), "buffer a requested child before its parent");
    }
    check(sync.get_stats().orphan_blocks == 3, "reverse fork retains its three requested descendants");
    check(sync.handle_block(remote.peer, branch.blocks[0]), "accept the first body on the competing branch");
    check(chain.chain->get_tip()->get_block_hash() == branch.blocks.back().get_hash(),
          "accepted side-branch parent drains descendants and activates the winning fork");
    check(sync.get_stats().orphan_blocks == 0, "winning fork leaves no stranded orphan bodies");
    check(sync.get_stats().status == network::SyncStatus::COMPLETED, "reverse fork download completes");
    core::Block stored;
    check(chain.blocks->get_block(branch.blocks.back().get_hash(), stored, true) &&
          stored.serialize() == branch.blocks.back().serialize(), "winning fork tip is durably readable");
}

void test_parent_accepted_before_network_reply() {
    test::TestChain chain;
    Branch branch(chain, chain.chain->get_tip(), 2, 53);
    SyncManager sync(chain.state, chain.chain);
    Remote remote(2);
    branch.announce(sync, remote);
    expect(!sync.handle_block(remote.peer, branch.blocks[1]), "buffer requested child");
    // RPC/import and the sync response can refer to the same accepted parent.
    chain.append(branch.blocks[0]);
    check(sync.handle_block(remote.peer, branch.blocks[0]), "duplicate parent response is accepted");
    check(chain.chain->get_height() == 2 && sync.get_stats().orphan_blocks == 0,
          "already accepted parent still releases its waiting child");
    check(sync.get_stats().status == network::SyncStatus::COMPLETED, "duplicate parent response completes sync");
}

void test_failed_body_retry() {
    test::TestChain chain;
    Branch branch(chain, chain.chain->get_tip(), 3, 54);
    SyncManager sync(chain.state, chain.chain);
    Remote first(3), second(3, "127.0.0.2:31001");
    branch.announce(sync, first);
    expect(sync.start_sync(second.peer, 3), "register another available peer");
    expect(!sync.handle_block(first.peer, branch.blocks[2]), "buffer grandchild before parent retry");
    expect(!sync.handle_block(first.peer, branch.blocks[1]), "buffer child before parent retry");
    auto damaged = branch.blocks[0];
    damaged.transactions[0].outputs[0].value += 1; // Header still names the correct original body.
    check(!sync.handle_block(first.peer, damaged), "reject body inconsistent with the validated header");
    check(Access::peer_info(sync, first.peer).invalid_block_score == 1,
          "bad body is accounted against its source peer");
    check(sync.get_stats().download_queue_size == 1, "failed body is requeued despite buffered descendants");
    sync.update();
    check(Access::requester(sync, branch.blocks[0].get_hash()) == second.peer,
          "missing parent is requested from the other peer");
    check(Access::attempts(sync, branch.blocks[0].get_hash()) == 2, "body retry preserves its attempt history");
    check(Access::requester(sync, branch.blocks[0].get_hash()) == second.peer &&
          sync.handle_block(second.peer, branch.blocks[0]), "accept correct body from retry");
    check(chain.chain->get_height() == 3 && sync.get_stats().orphan_blocks == 0,
          "body retry unblocks and applies the complete buffered branch");
    check(Access::attempts(sync, branch.blocks[0].get_hash()) == 0 &&
          Access::peer_info(sync, second.peer).requested_blocks.empty(),
          "successful retry releases scheduler and peer accounting");
}

void test_late_unassigned_child() {
    test::TestChain chain;
    Branch branch(chain, chain.chain->get_tip(), 3, 57);
    SyncManager sync(chain.state, chain.chain);
    Remote remote(3);
    branch.announce(sync, remote);
    Access::expire_requests(sync);
    expect(sync.get_stats().download_queue_size == 3, "timeout requeues requested headers");
    check(!sync.handle_block(remote.peer, branch.blocks[1]), "late unassigned child is not admitted as an orphan");
    check(sync.get_stats().download_queue_size == 3 && sync.get_stats().orphan_blocks == 0,
          "late child response preserves queued demand for all missing bodies");
    sync.update();
    for (const auto& block : branch.blocks) if (Access::requester(sync, block.get_hash())) {
        (void)sync.handle_block(remote.peer, block);
    }
    check(chain.chain->get_height() == 3, "timed-out branch completes after the delayed response");
}

void test_stale_failed_body_is_not_requeued() {
    test::TestChain chain;
    Branch old_branch(chain, chain.chain->get_tip(), 1, 58);
    Branch new_branch(chain, chain.chain->get_tip(), 2, 59);
    SyncManager sync(chain.state, chain.chain);
    Remote remote(2);
    old_branch.announce(sync, remote);
    new_branch.announce(sync, remote);
    auto damaged = old_branch.blocks[0];
    damaged.transactions[0].outputs[0].value += 1;
    check(!sync.handle_block(remote.peer, damaged), "stale branch bad body still rejects");
    check(sync.get_stats().download_queue_size == 0 &&
          Access::attempts(sync, damaged.get_hash()) == 0,
          "retry neither resurrects a superseded branch nor retains its attempt counter");
    for (const auto& block : new_branch.blocks) expect(sync.handle_block(remote.peer, block), "apply new best branch");
    check(chain.chain->get_tip()->get_block_hash() == new_branch.blocks.back().get_hash(),
          "failed stale response does not disturb the winning branch");
}

void test_bounded_orphan_buffer_progress() {
    test::TestChain chain;
    Branch branch(chain, chain.chain->get_tip(), 20, 56);
    SyncManager sync(chain.state, chain.chain);
    Remote remote(20);
    branch.announce(sync, remote);
    size_t cursor = 0;
    std::set<Hash256> received;
    auto deliver_requested = [&] {
        const auto requests = remote.transport->requested;
        for (; cursor < requests.size(); ++cursor) {
            const auto& hash = requests[cursor];
            if (hash == branch.blocks[0].get_hash() || !Access::requester(sync, hash)) continue;
            for (const auto& block : branch.blocks) if (block.get_hash() == hash) {
                (void)sync.handle_block(remote.peer, block);
                received.insert(hash);
                break;
            }
        }
    };
    for (size_t turn = 0; turn < 10 && received.size() < 19; ++turn) {
        deliver_requested();
        sync.update();
    }
    expect(received.size() == 19, "all requested descendants arrive before the first parent");
    check(sync.get_stats().orphan_blocks == 16, "orphan retention respects the production limit under pressure");
    check(sync.get_stats().download_queue_size + sync.get_stats().downloading_blocks > 1,
          "evicted descendants remain queued or requested for recovery");
    expect(sync.handle_block(remote.peer, branch.blocks[0]), "accept delayed first parent");
    for (size_t turn = 0; turn < 10 && chain.chain->get_height() < 20; ++turn) {
        sync.update();
        deliver_requested();
    }
    check(chain.chain->get_height() == 20 && sync.get_stats().orphan_blocks == 0,
          "bounded orphan eviction does not strand the validated chain");
}

void test_local_submission_error(services::SubmissionOutcome outcome, bool exception = false) {
    const std::string prefix = exception ? "exception: " :
        (outcome == services::SubmissionOutcome::UNAVAILABLE ? "unavailable: " : "internal error: ");
    test::TestChain chain;
    Branch branch(chain, chain.chain->get_tip(), 1, 55);
    SyncManager sync(chain.state, chain.chain);
    Remote remote(1);
    branch.announce(sync, remote);
    sync.set_block_submitter([&](const core::Block&) {
        if (exception) throw std::runtime_error("local-test-service-exception");
        services::BlockSubmissionResult result;
        result.outcome = outcome;
        result.reject_reason = "local-test-service-failure";
        return result;
    });
    bool all_failed = true;
    for (size_t i = 0; i < 4; ++i) {
        all_failed = !sync.handle_block(remote.peer, branch.blocks[0]) && all_failed;
        if (i < 3) sync.update();
    }
    check(all_failed, prefix + "submission failure is returned");
    const auto info = Access::peer_info(sync, remote.peer);
    check(info.invalid_block_score == 0 && info.failed_blocks == 0,
          prefix + "local service failure does not penalize the valid peer");
    check(remote.peer->is_connected(), prefix + "four local failures do not disconnect the peer");
    check(sync.get_stats().download_queue_size == 1, prefix + "valid body remains eligible for retry");
    sync.set_block_submitter({});
    sync.update();
    check(Access::requester(sync, branch.blocks[0].get_hash()) == remote.peer,
          prefix + "request resumes when the service is available");
    if (Access::requester(sync, branch.blocks[0].get_hash())) sync.handle_block(remote.peer, branch.blocks[0]);
    check(chain.chain->get_height() == 1, prefix + "valid block applies after local recovery");
}

void test_shorter_stronger_target(bool taller_target_first) {
    test::TestChain chain;
    for (size_t i = 0; i < 4; ++i) chain.append(chain.make_block());
    SyncManager sync(chain.state, chain.chain);
    Remote remote(8);
    expect(sync.start_sync(remote.peer, 8), "register peer for work-based scheduling");
    if (taller_target_first) {
        Branch taller(chain, chain.chain->get_tip(), 4, 61);
        taller.announce(sync, remote);
    }

    // A controlled chain-work anchor isolates scheduling from difficulty.
    // LOADNET's fixed difficulty cannot naturally produce a shorter winner.
    // Headers, bodies, encrypted GETDATA and the reorg still use production code.
    auto* anchor = chain.chain->find_block_index(chain.chain->get_tip()->get_ancestor(1)->get_block_hash());
    anchor->chain_work = chain.chain->get_tip()->chain_work + uint256(20);
    Branch shorter(chain, anchor, 2, 62);
    std::vector<core::BlockHeader> headers;
    for (const auto& block : shorter.blocks) headers.push_back(block.header);
    expect(sync.handle_headers(remote.peer, headers), "validate shorter stronger headers");
    const std::string prefix = taller_target_first ? "replaced taller target: " : "shorter winner: ";
    check(sync.is_syncing(), prefix + "greater work keeps synchronization active below local height");
    check(sync.get_stats().target_height == 4, prefix + "target forgets the obsolete taller branch");
    expect(sync.handle_headers(remote.peer, {}), "process exhausted header discovery");
    check(sync.is_syncing(), prefix + "empty headers do not stop a stronger pending branch");
    (void)sync.start_sync(remote.peer, 4);
    check(sync.is_syncing(), prefix + "an equal-height peer does not stop pending work");
    sync.update();
    check(Access::requester(sync, shorter.blocks[0].get_hash()) == remote.peer,
          prefix + "scheduler requests the shorter branch ancestor");
    for (const auto& block : shorter.blocks) (void)sync.handle_block(remote.peer, block);
    check(chain.chain->get_tip()->get_block_hash() == shorter.blocks.back().get_hash(),
          prefix + "stronger branch becomes active");
    check(sync.get_stats().status == network::SyncStatus::COMPLETED && sync.get_stats().target_height == 3,
          prefix + "sync completes at the new shorter active height");
}

void test_failed_header_branch_reselection(bool retain_alternative) {
    test::TestChain chain;
    const auto* genesis = chain.chain->get_tip();
    auto bad = chain.make_block(genesis, 1);
    chain::BlockIndex bad_index(bad.header);
    bad_index.hash = bad.get_hash();
    bad_index.prev = const_cast<chain::BlockIndex*>(genesis);
    bad_index.height = 1;
    bad_index.chain_work = consensus::calculate_chainwork(genesis, bad.header.bits);
    Branch children(chain, &bad_index, 3, 63);
    Branch alternative(chain, genesis, 2, 64);
    SyncManager sync(chain.state, chain.chain);
    Remote remote(4);
    expect(sync.start_sync(remote.peer, 4), "discover branch with invalid reward");
    std::vector<core::BlockHeader> headers{bad.header};
    for (const auto& block : children.blocks) headers.push_back(block.header);
    expect(sync.handle_headers(remote.peer, headers), "invalid reward is not visible in headers");
    if (retain_alternative) {
        expect(sync.handle_headers(remote.peer, {alternative.blocks[0].header, alternative.blocks[1].header}),
               "retain lower-work alternative headers");
    }
    sync.update();
    expect(!sync.handle_block(remote.peer, children.blocks.back()), "retain requested descendant before failed parent");
    expect(!sync.handle_block(remote.peer, bad), "reject the committed excessive coinbase reward");
    expect(chain.chain->find_block_index(bad.get_hash())->is_failed(), "Blockchain marks consensus failure");
    const std::string prefix = retain_alternative ? "fallback headers: " : "failed only branch: ";
    check(!Access::has_header(sync, bad.get_hash()) &&
          !Access::has_header(sync, children.blocks.back().get_hash()),
          prefix + "remove failed header and all header descendants");
    check(sync.get_stats().orphan_blocks == 0 && sync.get_stats().downloading_blocks == 0,
          prefix + "release orphan bodies and requests for the failed branch");
    check(Access::peer_info(sync, remote.peer).requested_blocks.empty(),
          prefix + "release peer request slots for the failed branch");
    check(sync.get_stats().target_height == (retain_alternative ? 2 : 0),
          prefix + "target is recomputed from eligible headers");
    check(sync.is_syncing() == retain_alternative,
          prefix + "status follows remaining valid work");
    check(!sync.handle_headers(remote.peer, {bad.header}),
          prefix + "known failed header is rejected when advertised again");
    check(!sync.handle_headers(remote.peer, {children.blocks[0].header}),
          prefix + "headers cannot extend a known failed parent");
    if (retain_alternative) {
        sync.update();
        check(Access::requester(sync, alternative.blocks[0].get_hash()) == remote.peer,
              prefix + "scheduler selects the retained alternative without another announcement");
        for (const auto& block : alternative.blocks) (void)sync.handle_block(remote.peer, block);
        check(chain.chain->get_tip()->get_block_hash() == alternative.blocks.back().get_hash() && !sync.is_syncing(),
              prefix + "alternative branch reaches the active tip and completes");
    } else {
        chain.append(chain.make_block());
    }
    check(!sync.handle_block(remote.peer, bad),
          prefix + "known failed body is not accepted as a successful duplicate below the active tip");
}

void test_invalid_header_batch_is_atomic() {
    test::TestChain chain;
    Branch branch(chain, chain.chain->get_tip(), 2, 65);
    SyncManager sync(chain.state, chain.chain);
    Remote remote(2);
    expect(sync.start_sync(remote.peer, 2), "start partial header response test");
    auto invalid = branch.blocks[1].header;
    invalid.prev_block_hash = chain.chain->get_tip()->get_block_hash();
    check(!sync.handle_headers(remote.peer, {branch.blocks[0].header, invalid}),
          "non-contiguous header suffix is rejected");
    check(!Access::has_header(sync, branch.blocks[0].get_hash()) && sync.get_stats().target_height == 0,
          "invalid header batch leaves no partially committed prefix");
    expect(sync.handle_headers(remote.peer, {branch.blocks[0].header}), "valid prefix can be submitted independently");
    sync.update();
    check(Access::requester(sync, branch.blocks[0].get_hash()) == remote.peer,
          "resubmitted valid prefix remains downloadable");
    (void)sync.handle_block(remote.peer, branch.blocks[0]);
    check(chain.chain->get_height() == 1 && !sync.is_syncing(),
          "accepted header prefix can complete normally");
}

void test_failed_ancestor_during_reorg(bool pruned_headers) {
    test::TestChain chain;
    const auto* genesis = chain.chain->get_tip();
    for (size_t i = 0; i < 3; ++i) chain.append(chain.make_block());
    const Hash256 old_tip = chain.chain->get_tip()->get_block_hash();
    auto bad = chain.make_block(genesis, 1);
    chain::BlockIndex bad_index(bad.header);
    bad_index.hash = bad.get_hash();
    bad_index.prev = const_cast<chain::BlockIndex*>(genesis);
    bad_index.height = 1;
    bad_index.chain_work = consensus::calculate_chainwork(genesis, bad.header.bits);
    Branch descendants(chain, &bad_index, 4, 66);
    SyncManager sync(chain.state, chain.chain);
    Remote remote(5);
    expect(sync.start_sync(remote.peer, 5), "start invalid ancestor reorg test");
    std::vector<core::BlockHeader> headers{bad.header};
    for (const auto& block : descendants.blocks) headers.push_back(block.header);
    expect(sync.handle_headers(remote.peer, headers), "retain candidate branch headers");
    sync.update();
    if (pruned_headers) Access::prune_header_history(sync, 3);
    expect(!sync.handle_block(remote.peer, descendants.blocks.back()), "buffer final descendant");
    expect(sync.handle_block(remote.peer, bad), "retain side-branch body before contextual activation");
    expect(sync.handle_block(remote.peer, descendants.blocks[0]), "retain side-branch height two");
    expect(sync.handle_block(remote.peer, descendants.blocks[1]), "retain side-branch height three");
    check(!sync.handle_block(remote.peer, descendants.blocks[2]),
          "winning fork is rejected when connecting its invalid ancestor");
    check(chain.chain->get_tip()->get_block_hash() == old_tip && chain.state->get_tip_hash() == old_tip,
          "failed reorg restores the original active chain and UTXO tip");
    bool all_removed = !Access::has_header(sync, bad.get_hash());
    for (const auto& block : descendants.blocks) all_removed &= !Access::has_header(sync, block.get_hash());
    check(all_removed && sync.get_stats().orphan_blocks == 0 && sync.get_stats().downloading_blocks == 0,
          "failed ancestor removes indexed and header-only descendants together");
    check(!sync.is_syncing() && sync.get_stats().target_height == 3,
          "failed reorg returns sync status to the restored active chain");
    check(!sync.handle_block(remote.peer, bad), "invalid side-branch duplicate remains rejected");
}

void test_body_write_failure_retry() {
    test::TestChain chain;
    Branch branch(chain, chain.chain->get_tip(), 2, 71);
    SyncManager sync(chain.state, chain.chain);
    Remote remote(2);
    branch.announce(sync, remote);
    storage::BlockStorageTestAccess::refuse_writes(*chain.blocks, true);
    check(!sync.handle_block(remote.peer, branch.blocks[0]), "block body write failure is returned");
    const auto* index = chain.chain->find_block_index(branch.blocks[0].get_hash());
    check(index && !index->has_data() && !index->is_failed(), "failed body write leaves a retryable index without data");
    storage::BlockStorageTestAccess::refuse_writes(*chain.blocks, false);
    check(sync.get_stats().download_queue_size == 1, "index without a saved body does not consume retry demand");
    (void)sync.handle_block(remote.peer, branch.blocks[1]);
    sync.update();
    check(Access::requester(sync, branch.blocks[0].get_hash()) == remote.peer,
          "missing indexed body is requested again after write recovery");
    if (Access::requester(sync, branch.blocks[0].get_hash())) (void)sync.handle_block(remote.peer, branch.blocks[0]);
    check(chain.chain->get_height() == 2 && chain.state->get_height() == 2 && !sync.is_syncing(),
          "write recovery applies the parent and its pending child");
    core::Block stored;
    check(chain.blocks->get_block(branch.blocks[0].get_hash(), stored, true), "retried parent body is durable");
    check(Access::peer_info(sync, remote.peer).invalid_block_score == 0,
          "body storage failure does not penalize the peer");
}

void test_missing_indexed_body_discovery() {
    test::TestChain chain;
    Branch branch(chain, chain.chain->get_tip(), 2, 72);
    storage::BlockStorageTestAccess::refuse_writes(*chain.blocks, true);
    consensus::ValidationState failure;
    expect(!chain.chain->process_new_block(branch.blocks[0], failure) && failure.is_error(),
           "create a validated index whose body write failed");
    storage::BlockStorageTestAccess::refuse_writes(*chain.blocks, false);
    SyncManager sync(chain.state, chain.chain);
    Remote remote(2);
    sync.handle_inv(remote.peer, {branch.blocks[0].get_hash()});
    check(remote.transport->header_requests == 1, "INV for an indexed missing body starts header discovery");
    branch.announce(sync, remote);
    check(Access::requester(sync, branch.blocks[0].get_hash()) == remote.peer,
          "known header prefix with a missing body is included in the best download path");
    for (const auto& block : branch.blocks) if (Access::requester(sync, block.get_hash())) {
        (void)sync.handle_block(remote.peer, block);
    }
    check(chain.chain->get_height() == 2 && !sync.is_syncing(), "header discovery recovers the missing indexed ancestor");
}

void test_missing_side_body_duplicate() {
    test::TestChain chain;
    const auto* genesis = chain.chain->get_tip();
    for (size_t i = 0; i < 2; ++i) chain.append(chain.make_block());
    Branch branch(chain, genesis, 1, 73);
    SyncManager sync(chain.state, chain.chain);
    Remote remote(2);
    storage::BlockStorageTestAccess::refuse_writes(*chain.blocks, true);
    expect(!sync.handle_block(remote.peer, branch.blocks[0]), "side body write initially fails");
    storage::BlockStorageTestAccess::refuse_writes(*chain.blocks, false);
    check(sync.handle_block(remote.peer, branch.blocks[0]), "repeated side body can be accepted after write recovery");
    core::Block stored;
    check(chain.blocks->get_block(branch.blocks[0].get_hash(), stored, true) &&
          chain.chain->find_block_index(branch.blocks[0].get_hash())->has_data(),
          "duplicate shortcut does not acknowledge an unsaved side body");
}

void test_saved_body_state_retry(bool journal) {
    auto backend = std::make_unique<StateWriteFailure>();
    auto* faults = backend.get();
    test::TestChain chain(std::move(backend));
    Branch branch(chain, chain.chain->get_tip(), 2, 74);
    SyncManager sync(chain.state, chain.chain);
    Remote remote(2);
    branch.announce(sync, remote);
    faults->fail_commit = !journal;
    faults->fail_journal = journal;
    const std::string prefix = journal ? "state journal failure: " : "state commit failure: ";
    check(!sync.handle_block(remote.peer, branch.blocks[0]), prefix + "submission reports local failure");
    check(chain.chain->get_height() == 0 && chain.state->get_height() == 0 &&
          chain.chain->find_block_index(branch.blocks[0].get_hash())->has_data(),
          prefix + "body is retained while active state is rolled back");
    check(sync.get_stats().download_queue_size == 1, prefix + "saved candidate remains queued for application");
    (void)sync.handle_block(remote.peer, branch.blocks[1]);
    const auto request_count = remote.transport->requested.size();
    sync.update();
    check(chain.chain->get_height() == 2 && chain.state->get_height() == 2 && !sync.is_syncing(),
          prefix + "update retries the saved body and drains its pending child");
    check(remote.transport->requested.size() == request_count, prefix + "saved body is reused without another GETDATA");
    check(Access::peer_info(sync, remote.peer).invalid_block_score == 0,
          prefix + "local state failure does not penalize the peer");
}

void test_saved_shorter_fork_retry() {
    auto backend = std::make_unique<StateWriteFailure>();
    auto* faults = backend.get();
    test::TestChain chain(std::move(backend));
    for (size_t i = 0; i < 3; ++i) chain.append(chain.make_block());
    // Scheduling fixture with controlled work, as in test_shorter_stronger_target.
    auto* anchor = chain.chain->find_block_index(chain.chain->get_tip()->get_ancestor(1)->get_block_hash());
    anchor->chain_work = chain.chain->get_tip()->chain_work + uint256(20);
    Branch shorter(chain, anchor, 1, 75);
    SyncManager sync(chain.state, chain.chain);
    Remote remote(4);
    expect(sync.start_sync(remote.peer, 4) && sync.handle_headers(remote.peer, {shorter.blocks[0].header}),
           "discover a stronger shorter candidate");
    sync.update();
    faults->fail_journal = true;
    check(!sync.handle_block(remote.peer, shorter.blocks[0]), "local reorg failure is reported for the shorter fork");
    expect(chain.chain->get_height() == 3 && chain.chain->find_block_index(shorter.blocks[0].get_hash())->has_data(),
           "failed reorg retains the shorter body and restores the old tip");
    check(sync.handle_block(remote.peer, shorter.blocks[0]) && chain.chain->get_height() == 2 && !sync.is_syncing(),
          "repeated saved side body retries activation instead of taking the duplicate shortcut");
}

void test_superseded_retry_metadata(bool timeout_before_switch) {
    test::TestChain chain;
    Branch old_branch(chain, chain.chain->get_tip(), 1, 76);
    Branch best_branch(chain, chain.chain->get_tip(), 2, 77);
    SyncManager sync(chain.state, chain.chain);
    Remote remote(2);
    old_branch.announce(sync, remote);
    if (timeout_before_switch) Access::expire_requests(sync);
    best_branch.announce(sync, remote);
    if (!timeout_before_switch) Access::expire_requests(sync);
    check(!Access::has_retry_metadata(sync, old_branch.blocks[0].get_hash()),
          timeout_before_switch ? "replacing a queued branch clears obsolete retry metadata" :
                                  "expiring a superseded request clears obsolete retry metadata");
    check(Access::attempts(sync, best_branch.blocks[0].get_hash()) == 1,
          "retry cleanup preserves the live best branch attempt count");
}

void test_saved_body_retry_without_peer() {
    auto backend = std::make_unique<StateWriteFailure>();
    auto* faults = backend.get();
    test::TestChain chain(std::move(backend));
    Branch branch(chain, chain.chain->get_tip(), 1, 78);
    SyncManager sync(chain.state, chain.chain);
    Remote remote(1);
    size_t submissions = 0;
    sync.set_block_submitter([&](const core::Block& block) {
        ++submissions;
        consensus::ValidationState state;
        services::BlockSubmissionResult result;
        const bool accepted = chain.chain->process_new_block(block, state);
        result.outcome = accepted ? services::SubmissionOutcome::ACCEPTED :
            (state.is_error() ? services::SubmissionOutcome::INTERNAL_ERROR : services::SubmissionOutcome::REJECTED);
        result.reject_reason = state.get_reject_reason();
        return result;
    });
    branch.announce(sync, remote);
    faults->journal_failures_remaining = 2;
    expect(!sync.handle_block(remote.peer, branch.blocks[0]), "first local write attempt fails");
    remote.peer->disconnect();
    sync.update();
    check(faults->journal_failures == 2 && submissions == 2,
          "saved body retry uses the configured mutation service without a peer");
    Access::local_retry_due(sync, false);
    for (size_t i = 0; i < 10; ++i) sync.update();
    check(submissions == 2 && sync.get_stats().download_queue_size == 1,
          "local retry waits for its deadline without losing queued work");
    chain.blocks->close();
    Access::local_retry_due(sync, true);
    sync.update();
    check(submissions == 2 && sync.get_stats().download_queue_size == 1,
          "temporarily unavailable local body stays queued without a false submission");
    expect(chain.blocks->open((chain.directory / "blocks").string()), "reopen local block storage");
    Access::local_retry_due(sync, true);
    sync.update();
    check(submissions == 3 && chain.chain->get_height() == 1 && !sync.is_syncing(),
          "saved body applies after local storage recovery while the peer remains offline");
    check(remote.transport->requested.size() == 1, "local recovery never redownloads the saved body");
}

void test_saved_candidate_rediscovery() {
    auto backend = std::make_unique<StateWriteFailure>();
    auto* faults = backend.get();
    test::TestChain chain(std::move(backend));
    Branch branch(chain, chain.chain->get_tip(), 1, 79);
    faults->fail_journal = true;
    consensus::ValidationState failure;
    expect(!chain.chain->process_new_block(branch.blocks[0], failure), "retain an unapplied saved candidate");
    SyncManager sync(chain.state, chain.chain);
    Remote remote(1);
    sync.handle_inv(remote.peer, {branch.blocks[0].get_hash()});
    check(remote.transport->header_requests == 1, "INV does not ignore a saved candidate with greater work");
    expect(sync.handle_headers(remote.peer, {branch.blocks[0].header}), "accept known saved candidate header");
    check(sync.is_syncing() && sync.get_stats().download_queue_size == 1,
          "known saved header restores the missing activation demand");
    sync.update();
    check(chain.chain->get_height() == 1 && !sync.is_syncing() && remote.transport->requested.empty(),
          "known saved candidate is activated without downloading it again");
}

void test_pruned_retry_metadata() {
    test::TestChain chain;
    Branch branch(chain, chain.chain->get_tip(), 3, 80);
    SyncManager sync(chain.state, chain.chain);
    Remote remote(3);
    branch.announce(sync, remote);
    Access::expire_requests(sync);
    for (const auto& block : branch.blocks) chain.append(block);
    Access::prune_header_history(sync, chain.chain->get_height());
    check(!Access::has_retry_metadata(sync, branch.blocks[0].get_hash()) &&
          !Access::has_retry_metadata(sync, branch.blocks[1].get_hash()),
          "header pruning removes obsolete retry metadata");
    check(Access::attempts(sync, branch.blocks.back().get_hash()) == 1,
          "pruning preserves retry metadata for retained queued headers");
    const auto requests = remote.transport->requested.size();
    sync.update();
    check(!sync.is_syncing() && !Access::has_retry_metadata(sync, branch.blocks.back().get_hash()) &&
          remote.transport->requested.size() == requests,
          "blocks accepted outside sync clear queued work and complete without redownload");
}

void test_incomplete_header_anchor(bool saved_parent) {
    auto backend = std::make_unique<StateWriteFailure>();
    auto* faults = backend.get();
    test::TestChain chain(std::move(backend));
    Branch branch(chain, chain.chain->get_tip(), 2, 81);
    faults->fail_journal = saved_parent;
    storage::BlockStorageTestAccess::refuse_writes(*chain.blocks, !saved_parent);
    consensus::ValidationState failure;
    expect(!chain.chain->process_new_block(branch.blocks[0], failure), "retain an incomplete header anchor");
    storage::BlockStorageTestAccess::refuse_writes(*chain.blocks, false);
    SyncManager sync(chain.state, chain.chain);
    Remote remote(2);
    expect(sync.start_sync(remote.peer, 2) && sync.handle_headers(remote.peer, {branch.blocks[1].header}),
           "validate a child header anchored at a previously indexed parent");
    sync.update();
    sync.update();
    check(saved_parent ? chain.chain->get_height() >= 1 : Access::requester(sync, branch.blocks[0].get_hash()) == remote.peer,
          saved_parent ? "header anchor with a saved body is retried locally" :
                         "header anchor with a missing body is restored to the download path");
    for (const auto& block : branch.blocks) if (Access::requester(sync, block.get_hash())) {
        (void)sync.handle_block(remote.peer, block);
    }
    check(chain.chain->get_height() == 2 && chain.state->get_tip_hash() == branch.blocks.back().get_hash() && !sync.is_syncing(),
          saved_parent ? "saved header anchor does not strand its announced child" :
                         "unsaved header anchor does not strand its announced child");
}
}

int main(int argc, char** argv) {
    try {
        consensus::SelectParams(consensus::Network::LOADNET);
        test_new_block_uses_a_peer_with_validated_headers();
        test_header_discovery_burst();
        test_block_download_request_budget();
        test_lost_initial_headers_response();
        test_queued_discovery_after_completion();
        test_header_request_pacing_and_reconnect();
        test_header_locator_ahead_of_bodies();
        test_reverse_fork_download();
        test_parent_accepted_before_network_reply();
        test_failed_body_retry();
        test_late_unassigned_child();
        test_stale_failed_body_is_not_requeued();
        test_bounded_orphan_buffer_progress();
        test_local_submission_error(services::SubmissionOutcome::UNAVAILABLE);
        test_local_submission_error(services::SubmissionOutcome::INTERNAL_ERROR);
        test_local_submission_error(services::SubmissionOutcome::INTERNAL_ERROR, true);
        test_shorter_stronger_target(false);
        test_shorter_stronger_target(true);
        test_failed_header_branch_reselection(false);
        test_failed_header_branch_reselection(true);
        test_invalid_header_batch_is_atomic();
        test_failed_ancestor_during_reorg(false);
        test_failed_ancestor_during_reorg(true);
        test_body_write_failure_retry();
        test_missing_indexed_body_discovery();
        test_missing_side_body_duplicate();
        test_saved_body_state_retry(false);
        test_saved_body_state_retry(true);
        test_saved_shorter_fork_retry();
        test_superseded_retry_metadata(false);
        test_superseded_retry_metadata(true);
        test_saved_body_retry_without_peer();
        test_saved_candidate_rediscovery();
        test_pruned_retry_metadata();
        test_incomplete_header_anchor(false);
        test_incomplete_header_anchor(true);
        size_t passed = 0;
        for (const auto& check : checks) if (check["passed"].get<bool>()) ++passed;
        if (argc > 1) std::ofstream(argv[1]) << nlohmann::json{
            {"passed", passed}, {"failed", checks.size()-passed}, {"checks", checks}}.dump(2) << '\n';
        for (const auto& check : checks) if (!check["passed"].get<bool>()) std::cerr << "FAIL: " << check["name"] << '\n';
        std::cout << passed << '/' << checks.size() << " node sync recovery checks passed\n";
        return passed == checks.size() ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "node sync recovery test failed: " << error.what() << '\n';
        return 2;
    }
}
