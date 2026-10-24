#include "geocoin/consensus/consensus.hpp"
#include "geocoin/consensus/chainparams.hpp"
#include "geocoin/consensus/proof_of_work.hpp"
#include "geocoin/chain/block_index.hpp"
#include "geocoin/core/blockchain.hpp"
#include "geocoin/state/state.hpp"
#include "geocoin/storage/block_storage.hpp"
#include "geocoin/utils/metrics.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

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

    static void add_candidate(Blockchain& blockchain, chain::BlockIndex* index)
    {
        blockchain.block_index_candidates_.insert(index);
    }

    static chain::BlockIndex* find_most_work_chain(Blockchain& blockchain)
    {
        return blockchain.find_most_work_chain();
    }

    static bool is_chain_valid(Blockchain& blockchain, chain::BlockIndex* tip)
    {
        return blockchain.is_chain_valid(tip);
    }

    static void mark_failed_descendants(Blockchain& blockchain, chain::BlockIndex* failed_root)
    {
        blockchain.mark_failed_descendants(failed_root);
    }

    static bool collect_active_chain_entries(
        const Blockchain& blockchain,
        std::vector<std::pair<Height, Hash256>>& entries,
        Hash256& tip_hash,
        Height& tip_height
    )
    {
        return blockchain.collect_active_chain_entries(entries, tip_hash, tip_height);
    }

    static bool active_chain_index_matches(const Blockchain& blockchain)
    {
        return blockchain.active_chain_index_matches();
    }

    static bool persist_active_chain_index(Blockchain& blockchain)
    {
        return blockchain.persist_active_chain_index();
    }

    static bool recover_interrupted_active_chain_transition(
        Blockchain& blockchain,
        bool has_pruned_anchor = false
    )
    {
        return blockchain.recover_interrupted_active_chain_transition(has_pruned_anchor);
    }

    static bool load_block_from_disk(Blockchain& blockchain, chain::BlockIndex* index, Block& block)
    {
        return blockchain.load_block_from_disk(index, block);
    }

    static void set_policy_checkpoint(
        Blockchain& blockchain,
        Height height,
        const Hash256& hash,
        bool allow_deep_reorg = false
    )
    {
        blockchain.has_policy_checkpoint_ = true;
        blockchain.policy_checkpoint_height_ = height;
        blockchain.policy_checkpoint_hash_ = hash;
        blockchain.allow_deep_reorg_ = allow_deep_reorg;
    }

    static void record_block_reject(
        Blockchain& blockchain,
        const consensus::ValidationState& state,
        bool policy
    )
    {
        blockchain.record_block_reject_(state, policy);
    }

    static bool check_chain_transition(
        const Blockchain& blockchain,
        const chain::BlockIndex* target,
        consensus::ValidationState& state
    )
    {
        return blockchain.check_chain_transition(target, state);
    }
};

} // namespace geocoin::core

namespace geocoin::storage {

struct BlockStorageTestAccess {
    static bool put(BlockStorage& storage, const std::string& key, const std::string& value, bool sync = true)
    {
        return storage.db_put(key, value, sync);
    }

    static bool erase(BlockStorage& storage, const std::string& key, bool sync = true)
    {
        return storage.db_delete(key, sync);
    }

    static std::string block_index_key(const BlockStorage& storage, const Hash256& hash)
    {
        return storage.block_index_key(hash);
    }

    static std::string chainwork_key(const BlockStorage& storage, const Hash256& hash)
    {
        return storage.chainwork_key(hash);
    }

    static std::string consensus_ruleset_key(const BlockStorage& storage)
    {
        return storage.consensus_ruleset_key();
    }
};

} // namespace geocoin::storage

namespace {

void expect(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

geocoin::Hash256 hash_from_seed(uint8_t seed)
{
    geocoin::Hash256 hash{};
    for (size_t i = 0; i < hash.size(); ++i) {
        hash[i] = static_cast<uint8_t>(seed + i * 31);
    }
    return hash;
}

class TempDir {
public:
    explicit TempDir(const std::string& name)
        : path_(std::filesystem::temp_directory_path() /
                (name + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())))
    {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    ~TempDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

geocoin::core::Block make_coinbase_block(
    geocoin::Height height,
    const geocoin::Hash256& prev_hash,
    const geocoin::Address& miner,
    const geocoin::Address& foundation
)
{
    geocoin::core::Block block;
    block.header.version = 1;
    block.header.prev_block_hash = prev_hash;
    block.header.timestamp = static_cast<uint32_t>(2000000000 + height);
    block.header.bits = geocoin::consensus::ProofOfWork::get_initial_bits();
    block.header.geo_tag = 0;
    block.header.nonce = height;

    const geocoin::Amount reward = geocoin::consensus::ProofOfWork::calculate_block_reward(height);
    block.transactions.push_back(
        geocoin::core::Transaction::create_coinbase(height, miner, reward, 0, foundation)
    );
    block.header.merkle_root = block.calculate_merkle_root();
    block.header.witness_commitment = block.calculate_witness_commitment();
    return block;
}

geocoin::core::Block make_block_one(
    const geocoin::Hash256& prev_hash,
    const geocoin::Address& miner,
    const geocoin::Address& foundation
)
{
    return make_coinbase_block(1, prev_hash, miner, foundation);
}

void test_null_and_empty_guards()
{
    using Access = geocoin::core::BlockchainTestAccess;

    auto state = std::make_shared<geocoin::state::ChainState>(
        std::make_unique<geocoin::state::MemoryStorage>());
    geocoin::core::Blockchain no_storage_chain(state, nullptr);

    geocoin::core::Block block;
    expect(!no_storage_chain.get_block(hash_from_seed(1), block), "null storage get_block fails");
    expect(!no_storage_chain.get_block_by_height(0, block), "null storage get_block_by_height fails");
    uint64_t pruned = 1;
    expect(!no_storage_chain.prune_block_index_below_height(1, &pruned), "null storage prune fails");
    expect(pruned == 0, "null storage prune resets count");

    geocoin::consensus::ValidationState validation;
    expect(!no_storage_chain.activate_best_chain(validation), "empty blockchain has no best chain");

    auto tips = no_storage_chain.get_chain_tips();
    expect(tips.empty(), "empty block index has no chain tips");

    no_storage_chain.set_policy_checkpoint_options(10, 2, true);
    auto policy = no_storage_chain.get_policy_checkpoint_info();
    expect(policy.interval == 10 && policy.confirmations == 2 && policy.allow_deep_reorg,
           "policy checkpoint options roundtrip");
    no_storage_chain.maybe_update_policy_checkpoint();
    no_storage_chain.load_policy_checkpoint();
    no_storage_chain.set_ibd_mode(true);
    no_storage_chain.set_ibd_mode(false);

    geocoin::core::Blockchain null_state_chain(nullptr, nullptr);
    geocoin::consensus::ValidationState connect_state;
    expect(!null_state_chain.connect_block(block, connect_state, nullptr),
           "connect_block rejects null chainstate");
    expect(connect_state.is_error() && connect_state.get_reject_reason() == "chainstate is null",
           "connect_block null chainstate reports reason");
    geocoin::consensus::ValidationState disconnect_state;
    expect(!null_state_chain.disconnect_block(block, disconnect_state, nullptr),
           "disconnect_block rejects null chainstate");
    expect(disconnect_state.is_error() && disconnect_state.get_reject_reason() == "chainstate is null",
           "disconnect_block null chainstate reports reason");

    geocoin::consensus::ValidationState debug_only_state;
    debug_only_state.invalid(
        geocoin::consensus::RejectCode::INVALID,
        "",
        "debug-only-reject"
    );
    Access::record_block_reject(no_storage_chain, debug_only_state, false);
    geocoin::consensus::ValidationState unknown_state;
    Access::record_block_reject(no_storage_chain, unknown_state, true);
    auto rejects = no_storage_chain.get_block_reject_stats();
    expect(rejects.consensus["debug-only-reject"] == 1,
           "reject stats fall back to debug message");
    expect(rejects.policy["unknown"] == 1,
           "reject stats fall back to unknown reason");
}

void test_block_index_helpers()
{
    std::vector<std::unique_ptr<geocoin::chain::BlockIndex>> chain;
    chain.reserve(16);

    for (geocoin::Height h = 0; h < 16; ++h) {
        geocoin::core::BlockHeader header;
        header.version = 1;
        header.timestamp = static_cast<uint32_t>(1000 + ((h * 37) % 113));
        header.bits = geocoin::consensus::ProofOfWork::get_initial_bits();
        header.nonce = h;
        header.prev_block_hash = (h == 0) ? geocoin::Hash256{} : chain.back()->get_block_hash();
        header.merkle_root = hash_from_seed(static_cast<uint8_t>(0x20 + h));
        header.witness_commitment = hash_from_seed(static_cast<uint8_t>(0x40 + h));

        auto index = std::make_unique<geocoin::chain::BlockIndex>(header);
        index->height = h;
        index->prev = (h == 0) ? nullptr : chain.back().get();
        index->chain_work = geocoin::uint256(h + 1);
        index->sequence_id = static_cast<uint32_t>(h);
        index->status = geocoin::chain::BlockStatus::VALID_TREE |
                        geocoin::chain::BlockStatus::HAVE_DATA;
        index->block_pos = geocoin::chain::DiskBlockPos(static_cast<int32_t>(h), static_cast<uint32_t>(h * 10));
        index->build_skip();
        chain.push_back(std::move(index));
    }

    auto* tip = chain.back().get();
    expect(tip->get_ancestor(0) == chain.front().get(), "ancestor resolves genesis");
    expect(tip->get_ancestor(7) == chain[7].get(), "ancestor resolves middle height");
    expect(tip->get_ancestor(15) == tip, "ancestor resolves self");
    expect(tip->get_ancestor(16) == nullptr, "ancestor above tip is rejected");
    const geocoin::chain::BlockIndex* const_tip = tip;
    expect(const_tip->get_ancestor(5) == chain[5].get(), "const ancestor resolves");
    expect(tip->skip != nullptr, "skip pointer is built on long chain");

    geocoin::core::BlockHeader damaged_header;
    damaged_header.timestamp = 999;
    geocoin::chain::BlockIndex damaged(damaged_header);
    damaged.height = 3;
    expect(damaged.get_ancestor(1) == nullptr, "ancestor detects damaged prev chain");

    std::vector<geocoin::Timestamp> times;
    for (int i = 5; i < 16; ++i) {
        times.push_back(chain[static_cast<size_t>(i)]->get_block_time());
    }
    std::sort(times.begin(), times.end());
    expect(tip->get_median_time_past() == times[times.size() / 2], "median time past uses last 11 blocks");

    geocoin::chain::BlockIndex single(damaged_header);
    single.height = 0;
    expect(single.get_median_time_past() == damaged_header.timestamp, "single index median is own timestamp");

    expect(tip->has_data(), "block status has data flag");
    expect(!tip->has_undo(), "block status missing undo flag");
    expect(tip->is_valid(geocoin::chain::BlockStatus::VALID_HEADER),
           "validity level includes lower levels");
    expect(tip->is_valid(geocoin::chain::BlockStatus::VALID_TREE),
           "validity level recognizes its current level");
    expect(!tip->is_valid(geocoin::chain::BlockStatus::VALID_TRANSACTIONS),
           "validity level does not confuse overlapping enum bits");
    tip->status |= geocoin::chain::BlockStatus::HAVE_UNDO;
    expect(tip->has_undo(), "block status OR adds undo flag");
    tip->status |= geocoin::chain::BlockStatus::FAILED_VALID;
    expect(tip->is_failed(), "failed status mask is recognized");
    expect(!tip->is_valid(geocoin::chain::BlockStatus::VALID_HEADER),
           "failed block is never reported valid");
    expect(!tip->block_pos.is_null(), "disk block position is non-null after assignment");
    tip->block_pos.set_null();
    expect(tip->block_pos.is_null(), "disk block position can be nulled");

    geocoin::chain::BlockIndex weaker(damaged_header);
    weaker.chain_work = geocoin::uint256(1);
    weaker.sequence_id = 1;
    geocoin::chain::BlockIndex stronger(damaged_header);
    stronger.chain_work = geocoin::uint256(2);
    stronger.sequence_id = 2;
    expect(weaker < stronger, "operator< orders lower chainwork first");
    geocoin::chain::BlockIndexWorkComparator comparator;
    expect(comparator(&stronger, &weaker), "work comparator prioritizes higher chainwork");
    expect(!comparator(&stronger, &stronger), "work comparator rejects identical pointers");
    stronger.chain_work = weaker.chain_work;
    stronger.sequence_id = 0;
    expect(comparator(&stronger, &weaker), "work comparator ties by lower sequence id");
    expect(tip->to_string().find("BlockIndex(height=15") != std::string::npos,
           "to_string includes height");

    geocoin::chain::Chain active_chain;
    expect(active_chain.empty(), "new active chain is empty");
    expect(active_chain.height() == -1, "empty active chain has sentinel height");
    expect(active_chain.get_locator().empty(), "empty active chain locator is empty");
    expect(active_chain.find_fork(nullptr) == nullptr, "find_fork rejects null index");

    active_chain.set_tip(nullptr);
    expect(active_chain.empty(), "set_tip null clears active chain");

    geocoin::chain::BlockIndex overflow_tip(damaged_header);
    overflow_tip.height = std::numeric_limits<geocoin::Height>::max();
    active_chain.set_tip(&overflow_tip);
    expect(active_chain.empty(), "set_tip rejects height overflow");

    active_chain.set_tip(tip);
    expect(active_chain.size() == chain.size(), "set_tip builds full chain vector");
    expect(active_chain.genesis() == chain.front().get(), "active chain exposes genesis");
    expect(active_chain.tip() == tip, "active chain exposes tip");
    expect(active_chain.contains(tip), "active chain contains tip");
    expect(!active_chain.contains(nullptr), "active chain does not contain null");
    expect(!active_chain.contains(&weaker), "active chain does not contain foreign index");
    expect(active_chain.next(chain[5].get()) == chain[6].get(), "active chain next resolves middle index");
    expect(active_chain.next(tip) == nullptr, "active chain next rejects tip");
    expect(active_chain.next(&weaker) == nullptr, "active chain next rejects foreign index");

    const auto default_locator = active_chain.get_locator();
    expect(!default_locator.empty() && default_locator.front() == tip->get_block_hash(),
           "default locator starts at tip");
    expect(default_locator.back() == chain.front()->get_block_hash(),
           "default locator reaches genesis");
    expect(default_locator.size() < chain.size(), "long locator switches to exponential stepping");
    const auto middle_locator = active_chain.get_locator(chain[10].get());
    expect(!middle_locator.empty() && middle_locator.front() == chain[10]->get_block_hash(),
           "explicit locator starts at requested index");

    geocoin::chain::BlockIndex orphan_high(damaged_header);
    orphan_high.height = 20;
    expect(active_chain.find_fork(&orphan_high) == nullptr,
           "find_fork rejects taller disconnected index");
}

void test_initialize_connect_disconnect_and_accessors()
{
    TempDir dir("geocoin-blockchain-units");
    auto storage = std::make_shared<geocoin::storage::BlockStorage>(dir.path().string());
    expect(storage->is_open(), "block storage opens");

    auto state = std::make_shared<geocoin::state::ChainState>(
        std::make_unique<geocoin::state::MemoryStorage>());
    state->set_block_storage(storage);

    geocoin::core::Blockchain blockchain(state, storage);
    blockchain.set_policy_checkpoint_options(1, 1, false);
    expect(blockchain.initialize(), "blockchain initializes with genesis");
    expect(blockchain.get_height() == 0, "initialized chain height is genesis");
    expect(state->get_height() == 0, "chainstate height is genesis");
    expect(blockchain.get_tip() != nullptr, "genesis tip exists");

    geocoin::Hash256 genesis_hash{};
    expect(blockchain.get_active_block_hash_by_height(0, genesis_hash), "active genesis hash resolves");
    expect(blockchain.has_block_index(genesis_hash), "genesis block index exists");
    expect(blockchain.find_block_index(genesis_hash) != nullptr, "find_block_index returns genesis");
    expect(blockchain.get_block_storage() == storage, "block storage accessor returns same storage");
    expect(blockchain.get_chain_state() == state, "chain state accessor returns same state");

    blockchain.set_policy_checkpoint_options(0, 1, false);
    blockchain.maybe_update_policy_checkpoint();
    blockchain.set_policy_checkpoint_options(1, 0, false);
    blockchain.maybe_update_policy_checkpoint();
    blockchain.set_policy_checkpoint_options(100, 10, false);
    blockchain.maybe_update_policy_checkpoint();
    blockchain.set_policy_checkpoint_options(1, 1, false);

    geocoin::core::Block genesis;
    expect(blockchain.get_block(genesis_hash, genesis), "get_block loads genesis");
    geocoin::core::Block by_height;
    expect(blockchain.get_block_by_height(0, by_height), "get_block_by_height loads genesis");
    expect(by_height.get_hash() == genesis_hash, "height 0 block hash matches genesis");

    // A body-less block would fail check_block immediately. Its invalid PoW
    // must win so peers cannot force expensive body/signature validation with
    // headers for which they performed no work.
    geocoin::core::Block invalid_pow;
    const auto* genesis_index = blockchain.get_tip();
    expect(genesis_index != nullptr, "PoW ordering test has genesis index");
    invalid_pow.header.version = 1;
    invalid_pow.header.prev_block_hash = genesis_hash;
    invalid_pow.header.timestamp = genesis_index->get_block_time() + 1;
    invalid_pow.header.bits = geocoin::consensus::calculate_next_difficulty(
        genesis_index,
        invalid_pow.header.timestamp
    );
    while (geocoin::consensus::ProofOfWork::verify_proof_of_work(
        invalid_pow.header,
        invalid_pow.header.bits,
        static_cast<uint32_t>(genesis_index->get_median_time_past()),
        genesis_hash, 1)) {
        ++invalid_pow.header.nonce;
    }
    geocoin::consensus::ValidationState invalid_pow_state;
    expect(!blockchain.process_new_block(invalid_pow, invalid_pow_state),
           "invalid PoW block is rejected");
    expect(invalid_pow_state.get_reject_reason() == "bad-pow",
           "PoW is checked before the malformed block body");

    auto tips = blockchain.get_chain_tips();
    expect(tips.size() == 1 && tips[0].active && tips[0].height == 0, "genesis is the only active tip");
    auto policy = blockchain.get_policy_checkpoint_info();
    expect(policy.has_checkpoint && policy.height == 0 && policy.hash == genesis_hash,
           "genesis policy checkpoint is present");

    expect(storage->set_policy_checkpoint(genesis_hash, 99), "test stores stale policy checkpoint");
    blockchain.load_policy_checkpoint();
    policy = blockchain.get_policy_checkpoint_info();
    expect(!policy.has_checkpoint, "stale policy checkpoint above tip is ignored");

    expect(storage->set_policy_checkpoint(hash_from_seed(0xa9), 0), "test stores mismatched policy checkpoint");
    blockchain.load_policy_checkpoint();
    policy = blockchain.get_policy_checkpoint_info();
    expect(!policy.has_checkpoint, "mismatched policy checkpoint hash is ignored");

    expect(storage->set_policy_checkpoint(genesis_hash, 0), "test restores genesis policy checkpoint");
    blockchain.load_policy_checkpoint();
    policy = blockchain.get_policy_checkpoint_info();
    expect(policy.has_checkpoint && policy.height == 0 && policy.hash == genesis_hash,
           "valid policy checkpoint reloads");
    blockchain.maybe_update_policy_checkpoint();
    blockchain.load_policy_checkpoint();

    geocoin::consensus::ValidationState missing_tip_state;
    expect(!blockchain.switch_to_tip(hash_from_seed(0xee), missing_tip_state),
           "switch_to_tip rejects missing target");
    auto rejects = blockchain.get_block_reject_stats();
    expect(!rejects.consensus.empty(), "missing switch target rejection is counted");

    geocoin::core::Block invalid;
    invalid.header.timestamp = 2000000000;
    geocoin::consensus::ValidationState invalid_state;
    expect(!blockchain.process_new_block(invalid, invalid_state), "invalid block is rejected");
    rejects = blockchain.get_block_reject_stats();
    expect(!rejects.consensus.empty(), "invalid block rejection is counted");

    geocoin::consensus::ValidationState missing_prev_state;
    geocoin::chain::BlockIndex* missing_prev_index = nullptr;
    const auto missing_prev_block = make_coinbase_block(
        1,
        hash_from_seed(0xea),
        geocoin::Address("GEOminer-missing-prev"),
        state->get_foundation_address()
    );
    expect(!blockchain.accept_block(missing_prev_block, missing_prev_state, &missing_prev_index),
           "accept_block rejects unknown prev block");
    expect(missing_prev_index == nullptr, "accept_block leaves out index null on missing prev");

    uint64_t missing_anchor_pruned = 123;
    expect(!blockchain.prune_block_index_below_height(2, &missing_anchor_pruned),
           "prune index rejects missing active anchor");
    expect(missing_anchor_pruned == 0, "failed prune resets pruned count");

    const auto block1 = make_block_one(
        genesis_hash,
        geocoin::Address("GEOminer-block1"),
        state->get_foundation_address()
    );
    int32_t file = 0;
    uint32_t pos = 0;
    expect(storage->save_block_with_pos(block1, 1, file, pos, false), "block1 saves to storage");

    geocoin::chain::BlockIndex block1_index(block1.header);
    block1_index.hash = block1.get_hash();
    block1_index.prev = blockchain.get_tip();
    block1_index.height = 1;
    block1_index.status = geocoin::chain::BlockStatus::HAVE_DATA;
    block1_index.chain_work = geocoin::consensus::calculate_chainwork(block1_index.prev, block1.header.bits);

    geocoin::consensus::ValidationState connect_state;
    expect(blockchain.connect_tip(connect_state, &block1_index), "connect_tip applies block1");
    expect(blockchain.get_height() == 1, "active chain height advances");
    expect(state->get_height() == 1, "chainstate height advances");
    blockchain.maybe_update_policy_checkpoint();
    policy = blockchain.get_policy_checkpoint_info();
    expect(policy.has_checkpoint && policy.height == 0,
           "policy checkpoint does not advance to finalized genesis height");
    geocoin::Hash256 active_hash{};
    expect(blockchain.get_active_block_hash_by_height(1, active_hash) && active_hash == block1.get_hash(),
           "active block1 hash resolves");
    expect(blockchain.get_block_by_height(1, by_height) && by_height.get_hash() == block1.get_hash(),
           "active block1 loads by height");

    const auto block2 = make_coinbase_block(
        2,
        block1.get_hash(),
        geocoin::Address("GEOminer-block2"),
        state->get_foundation_address()
    );
    expect(storage->save_block_with_pos(block2, 2, file, pos, false), "block2 saves to storage");

    geocoin::chain::BlockIndex block2_index(block2.header);
    block2_index.hash = block2.get_hash();
    block2_index.prev = &block1_index;
    block2_index.height = 2;
    block2_index.status = geocoin::chain::BlockStatus::HAVE_DATA;
    block2_index.chain_work = geocoin::consensus::calculate_chainwork(block2_index.prev, block2.header.bits);

    geocoin::consensus::ValidationState connect_state2;
    expect(blockchain.connect_tip(connect_state2, &block2_index), "connect_tip applies block2");
    expect(blockchain.get_height() == 2, "active chain height advances to block2");
    blockchain.maybe_update_policy_checkpoint();
    policy = blockchain.get_policy_checkpoint_info();
    expect(policy.has_checkpoint && policy.height == 1 && policy.hash == block1.get_hash(),
           "policy checkpoint advances to finalized block1");
    geocoin::Hash256 stored_checkpoint_hash{};
    geocoin::Height stored_checkpoint_height = 0;
    expect(storage->get_policy_checkpoint(stored_checkpoint_hash, stored_checkpoint_height),
           "policy checkpoint persists to storage");
    expect(stored_checkpoint_hash == block1.get_hash() && stored_checkpoint_height == 1,
           "stored policy checkpoint matches block1");

    uint64_t pruned_count = 1;
    expect(blockchain.prune_block_index_below_height(0, &pruned_count) && pruned_count == 0,
           "prune index below zero is a no-op");
    expect(state->prune_block_bodies_below_height(1),
           "archive active ancestors before compacting their indexes");
    expect(blockchain.prune_block_index_below_height(2, &pruned_count) && pruned_count >= 1,
           "blockchain prunes old index records through storage");
    geocoin::storage::BlockStorage::PrunedIndexAnchor anchor;
    expect(storage->get_pruned_index_anchor(anchor), "blockchain pruning stores pruned anchor");
    expect(anchor.height == 1 && anchor.hash == block1.get_hash(),
           "blockchain pruning uses active block before retention boundary as anchor");

    geocoin::consensus::ValidationState disconnect_state;
    expect(blockchain.disconnect_tip(disconnect_state), "disconnect_tip reverts block2");
    expect(blockchain.get_height() == 1, "active chain height returns to block1");
    expect(blockchain.disconnect_tip(disconnect_state), "disconnect_tip reverts block1");
    expect(blockchain.get_height() == 0, "active chain height returns to genesis");
    expect(state->get_height() == 0, "chainstate height returns to genesis");
    expect(!blockchain.disconnect_tip(disconnect_state), "disconnecting genesis is rejected");
}

void test_restart_repairs_stale_active_tip_to_most_work()
{
    TempDir dir("geocoin-blockchain-stale-active");
    geocoin::Hash256 genesis_hash{};
    geocoin::Hash256 block1_hash{};

    {
        auto storage = std::make_shared<geocoin::storage::BlockStorage>(dir.path().string());
        expect(storage->is_open(), "block storage opens for stale active setup");

        auto state = std::make_shared<geocoin::state::ChainState>(
            std::make_unique<geocoin::state::MemoryStorage>());
        state->set_block_storage(storage);

        geocoin::core::Blockchain blockchain(state, storage);
        expect(blockchain.initialize(), "setup blockchain initializes genesis");
        expect(blockchain.get_active_block_hash_by_height(0, genesis_hash), "setup genesis active hash");

        const auto block1 = make_block_one(
            genesis_hash,
            geocoin::Address("GEOminer-stale-active"),
            state->get_foundation_address()
        );
        block1_hash = block1.get_hash();
        expect(storage->save_block(block1, 1, false), "block1 is persisted as more-work candidate");

        geocoin::Hash256 active_tip_hash{};
        geocoin::Height active_tip_height = 999;
        expect(storage->get_active_tip(active_tip_hash, active_tip_height), "setup active tip exists");
        expect(active_tip_hash == genesis_hash && active_tip_height == 0,
               "setup leaves persisted active tip on genesis");
        storage->close();
    }

    {
        auto storage = std::make_shared<geocoin::storage::BlockStorage>(dir.path().string());
        expect(storage->is_open(), "block storage reopens for stale active test");

        auto state = std::make_shared<geocoin::state::ChainState>(
            std::make_unique<geocoin::state::MemoryStorage>());
        state->set_block_storage(storage);

        geocoin::core::Blockchain blockchain(state, storage);
        expect(blockchain.initialize(), "restart switches stale active tip to most-work chain");
        expect(blockchain.get_height() == 1, "restart advances active height to persisted block1");

        geocoin::Hash256 active_hash{};
        expect(blockchain.get_active_block_hash_by_height(1, active_hash) && active_hash == block1_hash,
               "active height index points to block1 after repair");

        geocoin::Hash256 stored_tip_hash{};
        geocoin::Height stored_tip_height = 0;
        expect(storage->get_active_tip(stored_tip_hash, stored_tip_height),
               "repaired active tip is persisted");
        expect(stored_tip_hash == block1_hash && stored_tip_height == 1,
               "persisted active tip is rewritten to block1");
        storage->close();
    }
}

void test_restart_does_not_bypass_policy_checkpoint_with_more_work_fork()
{
    using Access = geocoin::core::BlockchainTestAccess;
    TempDir dir("geocoin-blockchain-restart-checkpoint-guard");
    geocoin::Hash256 honest1_hash{};
    geocoin::Hash256 honest2_hash{};

    {
        auto storage = std::make_shared<geocoin::storage::BlockStorage>(dir.path().string());
        expect(storage->is_open(), "checkpoint restart setup storage opens");
        auto state = std::make_shared<geocoin::state::ChainState>(
            std::make_unique<geocoin::state::MemoryStorage>());
        state->set_block_storage(storage);
        geocoin::core::Blockchain blockchain(state, storage);
        expect(blockchain.initialize(), "checkpoint restart setup initializes");

        geocoin::Hash256 genesis_hash{};
        expect(blockchain.get_active_block_hash_by_height(0, genesis_hash),
               "checkpoint restart setup resolves genesis");
        const auto foundation = state->get_foundation_address();

        auto connect_saved = [&](const geocoin::core::Block& block,
                                 geocoin::chain::BlockIndex* prev,
                                 geocoin::Height height) {
            int32_t file = 0;
            uint32_t pos = 0;
            expect(storage->save_block_with_pos(block, height, file, pos, false),
                   "checkpoint restart active block saves");
            auto* index = Access::add_index(
                blockchain,
                block,
                prev,
                height,
                static_cast<uint64_t>(height + 1),
                geocoin::chain::BlockStatus::HAVE_DATA
            );
            index->block_pos.file = file;
            index->block_pos.pos = pos;
            geocoin::consensus::ValidationState connect_state;
            expect(blockchain.connect_tip(connect_state, index),
                   "checkpoint restart active block connects");
            return index;
        };

        const auto honest1 = make_coinbase_block(
            1, genesis_hash, geocoin::Address("GEOhonest-restart-1"), foundation);
        auto* honest1_index = connect_saved(honest1, blockchain.get_tip(), 1);
        honest1_hash = honest1.get_hash();
        const auto honest2 = make_coinbase_block(
            2, honest1_hash, geocoin::Address("GEOhonest-restart-2"), foundation);
        connect_saved(honest2, honest1_index, 2);
        honest2_hash = honest2.get_hash();
        expect(storage->set_policy_checkpoint(honest1_hash, 1),
               "checkpoint restart stores non-genesis policy checkpoint");

        const auto attacker1 = make_coinbase_block(
            1, genesis_hash, geocoin::Address("GEOattacker-restart-1"), foundation);
        const auto attacker2 = make_coinbase_block(
            2, attacker1.get_hash(), geocoin::Address("GEOattacker-restart-2"), foundation);
        const auto attacker3 = make_coinbase_block(
            3, attacker2.get_hash(), geocoin::Address("GEOattacker-restart-3"), foundation);
        expect(storage->save_block(attacker1, 1, false), "checkpoint restart attacker1 saves");
        expect(storage->save_block(attacker2, 2, false), "checkpoint restart attacker2 saves");
        expect(storage->save_block(attacker3, 3, false), "checkpoint restart attacker3 saves");
        storage->close();
    }

    {
        auto storage = std::make_shared<geocoin::storage::BlockStorage>(dir.path().string());
        expect(storage->is_open(), "checkpoint restart storage reopens");
        auto state = std::make_shared<geocoin::state::ChainState>(
            std::make_unique<geocoin::state::MemoryStorage>());
        state->set_block_storage(storage);
        geocoin::core::Blockchain blockchain(state, storage);

        expect(blockchain.initialize(),
               "restart keeps running while quarantining a forbidden more-work fork");
        expect(blockchain.get_height() == 2 && blockchain.get_tip()->get_block_hash() == honest2_hash,
               "restart preserves checkpoint-compatible active tip");
        const auto checkpoint = blockchain.get_policy_checkpoint_info();
        expect(checkpoint.has_checkpoint && checkpoint.height == 1 && checkpoint.hash == honest1_hash,
               "restart preserves the non-genesis policy checkpoint");
        storage->close();
    }
}

void test_restart_rejects_active_tip_height_mismatch()
{
    TempDir dir("geocoin-blockchain-active-tip-mismatch");
    geocoin::Hash256 genesis_hash{};

    {
        auto storage = std::make_shared<geocoin::storage::BlockStorage>(dir.path().string());
        expect(storage->is_open(), "block storage opens for height mismatch setup");

        auto state = std::make_shared<geocoin::state::ChainState>(
            std::make_unique<geocoin::state::MemoryStorage>());
        state->set_block_storage(storage);

        geocoin::core::Blockchain blockchain(state, storage);
        expect(blockchain.initialize(), "setup blockchain initializes for height mismatch");
        expect(blockchain.get_active_block_hash_by_height(0, genesis_hash), "setup genesis hash for mismatch");
        expect(storage->set_active_tip(genesis_hash, 42), "test corrupts active tip height");
        storage->close();
    }

    {
        auto storage = std::make_shared<geocoin::storage::BlockStorage>(dir.path().string());
        expect(storage->is_open(), "block storage reopens for height mismatch test");

        auto state = std::make_shared<geocoin::state::ChainState>(
            std::make_unique<geocoin::state::MemoryStorage>());
        state->set_block_storage(storage);

        geocoin::core::Blockchain blockchain(state, storage);
        expect(!blockchain.initialize(), "restart rejects active tip height mismatch");
        storage->close();
    }
}

void test_restart_rejects_unknown_active_tip()
{
    TempDir dir("geocoin-blockchain-active-tip-missing");

    {
        auto storage = std::make_shared<geocoin::storage::BlockStorage>(dir.path().string());
        expect(storage->is_open(), "block storage opens for missing active tip setup");

        auto state = std::make_shared<geocoin::state::ChainState>(
            std::make_unique<geocoin::state::MemoryStorage>());
        state->set_block_storage(storage);

        geocoin::core::Blockchain blockchain(state, storage);
        expect(blockchain.initialize(), "setup blockchain initializes for missing active tip");
        expect(storage->set_active_tip(hash_from_seed(0x9a), 0), "test corrupts active tip hash");
        storage->close();
    }

    {
        auto storage = std::make_shared<geocoin::storage::BlockStorage>(dir.path().string());
        expect(storage->is_open(), "block storage reopens for missing active tip test");

        auto state = std::make_shared<geocoin::state::ChainState>(
            std::make_unique<geocoin::state::MemoryStorage>());
        state->set_block_storage(storage);

        geocoin::core::Blockchain blockchain(state, storage);
        expect(!blockchain.initialize(), "restart rejects active tip hash missing from block index");
        storage->close();
    }
}

void test_restart_rejects_active_tip_without_body()
{
    TempDir dir("geocoin-blockchain-active-tip-no-body");
    geocoin::Hash256 pruned_hash{};

    {
        auto storage = std::make_shared<geocoin::storage::BlockStorage>(dir.path().string());
        expect(storage->is_open(), "block storage opens for no-body active tip setup");

        auto state = std::make_shared<geocoin::state::ChainState>(
            std::make_unique<geocoin::state::MemoryStorage>());
        state->set_block_storage(storage);

        geocoin::core::Blockchain blockchain(state, storage);
        expect(blockchain.initialize(), "setup blockchain initializes for no-body active tip");
        expect(storage->prune_block_body_at_height(0, &pruned_hash), "test prunes active genesis body");
        expect(pruned_hash == geocoin::consensus::Params().get_genesis_hash(),
               "pruned body is network genesis");
        storage->close();
    }

    {
        auto storage = std::make_shared<geocoin::storage::BlockStorage>(dir.path().string());
        expect(storage->is_open(), "block storage reopens for no-body active tip test");

        auto state = std::make_shared<geocoin::state::ChainState>(
            std::make_unique<geocoin::state::MemoryStorage>());
        state->set_block_storage(storage);

        geocoin::core::Blockchain blockchain(state, storage);
        expect(!blockchain.initialize(), "restart rejects active tip without block body");
        storage->close();
    }
}

void test_restart_rejects_incomplete_split_write()
{
    TempDir dir("geocoin-blockchain-incomplete-split-write");

    {
        auto storage = std::make_shared<geocoin::storage::BlockStorage>(dir.path().string());
        expect(storage->is_open(), "block storage opens for incomplete split-write setup");

        auto state = std::make_shared<geocoin::state::ChainState>(
            std::make_unique<geocoin::state::MemoryStorage>());
        state->set_block_storage(storage);

        geocoin::core::Blockchain blockchain(state, storage);
        expect(blockchain.initialize(), "setup blockchain initializes before split-write marker");
        using StorageAccess = geocoin::storage::BlockStorageTestAccess;
        expect(StorageAccess::put(*storage, "split_write_pending", "1"),
               "test seeds incomplete split-write marker");
        storage->close();
    }

    {
        auto storage = std::make_shared<geocoin::storage::BlockStorage>(dir.path().string());
        expect(storage->is_open(), "block storage reopens for incomplete split-write test");
        expect(storage->has_incomplete_split_write(),
               "reopened storage still has the split-write marker");

        auto state = std::make_shared<geocoin::state::ChainState>(
            std::make_unique<geocoin::state::MemoryStorage>());
        state->set_block_storage(storage);

        geocoin::core::Blockchain blockchain(state, storage);
        expect(!blockchain.initialize(), "restart rejects incomplete split write");
        storage->close();
    }
}

void test_restart_rewrites_incomplete_active_chain_index()
{
    TempDir dir("geocoin-blockchain-incomplete-active-index");
    geocoin::Hash256 genesis_hash{};
    geocoin::Hash256 block1_hash{};

    {
        auto storage = std::make_shared<geocoin::storage::BlockStorage>(dir.path().string());
        expect(storage->is_open(), "block storage opens for incomplete active index setup");

        auto state = std::make_shared<geocoin::state::ChainState>(
            std::make_unique<geocoin::state::MemoryStorage>());
        state->set_block_storage(storage);

        geocoin::core::Blockchain blockchain(state, storage);
        expect(blockchain.initialize(), "setup blockchain initializes for incomplete active index");
        expect(blockchain.get_active_block_hash_by_height(0, genesis_hash),
               "setup reads genesis active hash");

        const auto block1 = make_block_one(
            genesis_hash,
            geocoin::Address("GEOminer-incomplete-active-index"),
            state->get_foundation_address()
        );
        block1_hash = block1.get_hash();
        expect(storage->save_block(block1, 1, false), "block1 persists for incomplete active index");
        expect(storage->set_active_tip(block1_hash, 1), "test stores active tip without height entry");

        geocoin::Hash256 missing_active_hash{};
        expect(!storage->get_active_block_hash(1, missing_active_hash),
               "setup leaves active height 1 missing");
        storage->close();
    }

    {
        auto storage = std::make_shared<geocoin::storage::BlockStorage>(dir.path().string());
        expect(storage->is_open(), "block storage reopens for incomplete active index test");

        auto state = std::make_shared<geocoin::state::ChainState>(
            std::make_unique<geocoin::state::MemoryStorage>());
        state->set_block_storage(storage);

        geocoin::core::Blockchain blockchain(state, storage);
        expect(blockchain.initialize(), "restart repairs incomplete active chain index");

        geocoin::Hash256 active_hash{};
        expect(storage->get_active_block_hash(1, active_hash) && active_hash == block1_hash,
               "restart rewrites missing active height entry");
        expect(blockchain.get_height() == 1, "restart keeps block1 active");
        storage->close();
    }
}

void test_restart_loads_legacy_body_only_block_records()
{
    TempDir dir("geocoin-blockchain-body-only-legacy");
    geocoin::Hash256 genesis_hash{};
    geocoin::Hash256 block1_hash{};

    {
        auto storage = std::make_shared<geocoin::storage::BlockStorage>(dir.path().string());
        expect(storage->is_open(), "block storage opens for body-only legacy setup");

        auto state = std::make_shared<geocoin::state::ChainState>(
            std::make_unique<geocoin::state::MemoryStorage>());
        state->set_block_storage(storage);

        geocoin::core::Blockchain blockchain(state, storage);
        expect(blockchain.initialize(), "setup blockchain initializes for body-only legacy test");
        expect(blockchain.get_active_block_hash_by_height(0, genesis_hash),
               "setup reads genesis hash for body-only legacy test");

        const auto block1 = make_block_one(
            genesis_hash,
            geocoin::Address("GEOminer-body-only-legacy"),
            state->get_foundation_address()
        );
        block1_hash = block1.get_hash();
        expect(storage->save_block(block1, 1, false),
               "body-only legacy setup saves block1 body");
        expect(storage->set_active_block(1, block1_hash),
               "body-only legacy setup stores active height 1");
        expect(storage->set_active_tip(block1_hash, 1),
               "body-only legacy setup stores active tip");

        using StorageAccess = geocoin::storage::BlockStorageTestAccess;
        expect(StorageAccess::erase(*storage, StorageAccess::block_index_key(*storage, genesis_hash)),
               "body-only legacy setup removes genesis block-index record");
        expect(StorageAccess::erase(*storage, StorageAccess::block_index_key(*storage, block1_hash)),
               "body-only legacy setup removes block1 block-index record");
        expect(StorageAccess::erase(*storage, StorageAccess::chainwork_key(*storage, block1_hash)),
               "body-only legacy setup removes block1 chainwork record");
        storage->close();
    }

    {
        auto storage = std::make_shared<geocoin::storage::BlockStorage>(dir.path().string());
        expect(storage->is_open(), "block storage reopens for body-only legacy test");

        auto state = std::make_shared<geocoin::state::ChainState>(
            std::make_unique<geocoin::state::MemoryStorage>());
        state->set_block_storage(storage);

        geocoin::core::Blockchain blockchain(state, storage);
        expect(blockchain.initialize(), "restart rebuilds indexes from legacy body-only records");
        expect(blockchain.get_height() == 1, "body-only legacy restart restores active height");

        geocoin::Hash256 active_hash{};
        expect(blockchain.get_active_block_hash_by_height(1, active_hash) && active_hash == block1_hash,
               "body-only legacy restart restores active block1 hash");
        expect(blockchain.has_block_index(genesis_hash) && blockchain.has_block_index(block1_hash),
               "body-only legacy restart recreates in-memory block indexes");
        storage->close();
    }
}

void test_restart_rejects_missing_or_mismatched_consensus_ruleset()
{
    using StorageAccess = geocoin::storage::BlockStorageTestAccess;
    TempDir dir("geocoin-blockchain-consensus-ruleset");

    {
        auto storage = std::make_shared<geocoin::storage::BlockStorage>(dir.path().string());
        expect(storage->is_open(), "ruleset setup storage opens");
        auto state = std::make_shared<geocoin::state::ChainState>(
            std::make_unique<geocoin::state::MemoryStorage>());
        state->set_block_storage(storage);
        geocoin::core::Blockchain blockchain(state, storage);
        expect(blockchain.initialize(), "ruleset setup blockchain initializes");
        storage->close();
    }

    {
        auto storage = std::make_shared<geocoin::storage::BlockStorage>(dir.path().string());
        expect(storage->is_open(), "missing-ruleset storage reopens");
        expect(StorageAccess::erase(*storage, StorageAccess::consensus_ruleset_key(*storage)),
               "ruleset marker is removed for compatibility guard test");
        auto state = std::make_shared<geocoin::state::ChainState>(
            std::make_unique<geocoin::state::MemoryStorage>());
        state->set_block_storage(storage);
        geocoin::core::Blockchain blockchain(state, storage);
        expect(!blockchain.initialize(), "non-empty database without ruleset is rejected");
        storage->close();
    }

    {
        auto storage = std::make_shared<geocoin::storage::BlockStorage>(dir.path().string());
        expect(storage->is_open(), "mismatched-ruleset storage reopens");
        expect(storage->set_consensus_ruleset_id("discarded-test-rules"),
               "obsolete ruleset marker is stored for mismatch test");
        auto state = std::make_shared<geocoin::state::ChainState>(
            std::make_unique<geocoin::state::MemoryStorage>());
        state->set_block_storage(storage);
        geocoin::core::Blockchain blockchain(state, storage);
        expect(!blockchain.initialize(), "database from another consensus ruleset is rejected");
        storage->close();
    }
}

void test_restart_rejects_orphan_block_index_record()
{
    TempDir dir("geocoin-blockchain-orphan-index-record");
    geocoin::Hash256 genesis_hash{};

    {
        auto storage = std::make_shared<geocoin::storage::BlockStorage>(dir.path().string());
        expect(storage->is_open(), "block storage opens for orphan index setup");

        auto state = std::make_shared<geocoin::state::ChainState>(
            std::make_unique<geocoin::state::MemoryStorage>());
        state->set_block_storage(storage);

        geocoin::core::Blockchain blockchain(state, storage);
        expect(blockchain.initialize(), "setup blockchain initializes for orphan index test");
        expect(blockchain.get_active_block_hash_by_height(0, genesis_hash),
               "setup reads genesis active hash for orphan index test");

        const auto orphan = make_coinbase_block(
            1,
            hash_from_seed(0xbb),
            geocoin::Address("GEOminer-orphan-index"),
            state->get_foundation_address()
        );
        expect(storage->save_block(orphan, 1, false), "orphan block index record persists");
        storage->close();
    }

    {
        auto storage = std::make_shared<geocoin::storage::BlockStorage>(dir.path().string());
        expect(storage->is_open(), "block storage reopens for orphan index test");

        auto state = std::make_shared<geocoin::state::ChainState>(
            std::make_unique<geocoin::state::MemoryStorage>());
        state->set_block_storage(storage);

        geocoin::core::Blockchain blockchain(state, storage);
        expect(!blockchain.initialize(), "restart rejects orphan block index record");
        storage->close();
    }
}

void test_restart_rejects_compacted_pruned_index_without_snapshot()
{
    using Access = geocoin::core::BlockchainTestAccess;

    TempDir dir("geocoin-blockchain-pruned-anchor-no-snapshot");
    geocoin::Hash256 block1_hash{};

    {
        auto storage = std::make_shared<geocoin::storage::BlockStorage>(dir.path().string());
        expect(storage->is_open(), "block storage opens for pruned-anchor setup");

        auto state = std::make_shared<geocoin::state::ChainState>(
            std::make_unique<geocoin::state::MemoryStorage>());
        state->set_block_storage(storage);

        geocoin::core::Blockchain blockchain(state, storage);
        blockchain.set_policy_checkpoint_options(100, 100, false);
        expect(blockchain.initialize(), "setup blockchain initializes for pruned-anchor test");

        geocoin::Hash256 genesis_hash{};
        expect(blockchain.get_active_block_hash_by_height(0, genesis_hash),
               "setup reads genesis hash for pruned-anchor test");

        int32_t file = 0;
        uint32_t pos = 0;
        const auto block1 = make_block_one(
            genesis_hash,
            geocoin::Address("GEOminer-pruned-anchor-1"),
            state->get_foundation_address()
        );
        block1_hash = block1.get_hash();
        expect(storage->save_block_with_pos(block1, 1, file, pos, false),
               "pruned-anchor setup saves block1");
        auto* block1_index = Access::add_index(
            blockchain,
            block1,
            blockchain.get_tip(),
            1,
            2,
            geocoin::chain::BlockStatus::HAVE_DATA
        );
        geocoin::consensus::ValidationState connect_block1_state;
        expect(blockchain.connect_tip(connect_block1_state, block1_index),
               "pruned-anchor setup connects block1");

        const auto block2 = make_coinbase_block(
            2,
            block1_hash,
            geocoin::Address("GEOminer-pruned-anchor-2"),
            state->get_foundation_address()
        );
        expect(storage->save_block_with_pos(block2, 2, file, pos, false),
               "pruned-anchor setup saves block2");
        auto* block2_index = Access::add_index(
            blockchain,
            block2,
            block1_index,
            2,
            3,
            geocoin::chain::BlockStatus::HAVE_DATA
        );
        geocoin::consensus::ValidationState connect_block2_state;
        expect(blockchain.connect_tip(connect_block2_state, block2_index),
               "pruned-anchor setup connects block2");

        uint64_t pruned = 0;
        expect(blockchain.prune_history(1, true, nullptr, &pruned) && pruned >= 1,
               "pruned-anchor setup archives bodies and compacts index below height 2");
        expect(storage->set_policy_checkpoint(hash_from_seed(0xb8), 1),
               "pruned-anchor setup stores invalid checkpoint so anchor fallback is exercised");
        storage->close();
    }

    {
        auto storage = std::make_shared<geocoin::storage::BlockStorage>(dir.path().string());
        expect(storage->is_open(), "block storage reopens for pruned-anchor test");

        auto state = std::make_shared<geocoin::state::ChainState>(
            std::make_unique<geocoin::state::MemoryStorage>());
        state->set_block_storage(storage);

        geocoin::core::Blockchain blockchain(state, storage);
        expect(!blockchain.initialize(),
               "restart rejects compacted pruned index without validated snapshot");
        auto policy = blockchain.get_policy_checkpoint_info();
        expect(policy.has_checkpoint && policy.height == 1 && policy.hash == block1_hash,
               "pruned-anchor restart falls back to anchor policy checkpoint before rejecting rebuild");
        storage->close();
    }
}

void test_restart_rejects_wrong_genesis_datadir()
{
    TempDir dir("geocoin-blockchain-wrong-genesis");
    {
        auto storage = std::make_shared<geocoin::storage::BlockStorage>(dir.path().string());
        expect(storage->is_open(), "block storage opens for wrong genesis setup");

        geocoin::core::Block wrong_genesis;
        wrong_genesis.header.version = 1;
        wrong_genesis.header.prev_block_hash = {};
        wrong_genesis.header.timestamp = 2000000100;
        wrong_genesis.header.bits = geocoin::consensus::ProofOfWork::get_initial_bits();
        wrong_genesis.header.geo_tag = 123;
        wrong_genesis.header.nonce = 7;
        wrong_genesis.transactions.push_back(
            geocoin::core::Transaction::create_coinbase(
                0,
                geocoin::Address("GEOwrong-genesis"),
                geocoin::consensus::ProofOfWork::calculate_block_reward(0)
            )
        );
        wrong_genesis.header.merkle_root = wrong_genesis.calculate_merkle_root();
        wrong_genesis.header.witness_commitment = wrong_genesis.calculate_witness_commitment();
        expect(wrong_genesis.get_hash() != geocoin::consensus::Params().get_genesis_hash(),
               "wrong genesis hash differs from network genesis");
        expect(storage->save_block(wrong_genesis, 0, false), "wrong genesis block persists");
        storage->close();
    }

    {
        auto storage = std::make_shared<geocoin::storage::BlockStorage>(dir.path().string());
        expect(storage->is_open(), "block storage reopens for wrong genesis test");

        auto state = std::make_shared<geocoin::state::ChainState>(
            std::make_unique<geocoin::state::MemoryStorage>());
        state->set_block_storage(storage);

        geocoin::core::Blockchain blockchain(state, storage);
        expect(!blockchain.initialize(), "restart rejects datadir with wrong genesis");
        storage->close();
    }
}

void test_linear_ancestry_validation()
{
    using Access = geocoin::core::BlockchainTestAccess;
    auto state = std::make_shared<geocoin::state::ChainState>(
        std::make_unique<geocoin::state::MemoryStorage>());
    geocoin::core::Blockchain blockchain(state, nullptr);
    auto metric = GEO_METRIC_COUNTER("blockchain.ancestry_entries_checked_total");
    const auto before = metric->value();
    geocoin::chain::BlockIndex* tip = nullptr;
    geocoin::chain::BlockIndex* middle = nullptr;
    constexpr geocoin::Height length = 10000;
    for (geocoin::Height h = 0; h < length; ++h) {
        geocoin::core::Block block;
        block.header.nonce = h;
        if (tip) block.header.prev_block_hash = tip->get_block_hash();
        tip = Access::add_index(blockchain, block, tip, h, h + 1, geocoin::chain::BlockStatus::HAVE_DATA);
        if (h == length / 2) middle = tip;
        expect(Access::is_chain_valid(blockchain, tip), "new index inherits valid ancestry");
    }
    expect(metric->value() - before <= 2 * length, "10000 appends require only linear ancestry visits");
    middle->status |= geocoin::chain::BlockStatus::FAILED_VALID;
    Access::mark_failed_descendants(blockchain, middle);
    expect(!Access::is_chain_valid(blockchain, tip), "cached ancestry is invalidated with failed descendants");
    expect(Access::is_chain_valid(blockchain, middle->prev), "unaffected prefix remains valid");
}

void test_chain_tip_classification_candidates_and_active_index_guards()
{
    using Access = geocoin::core::BlockchainTestAccess;
    constexpr auto have_data = geocoin::chain::BlockStatus::HAVE_DATA;

    auto state = std::make_shared<geocoin::state::ChainState>(
        std::make_unique<geocoin::state::MemoryStorage>());
    geocoin::core::Blockchain blockchain(state, nullptr);
    expect(Access::active_chain_index_matches(blockchain),
           "null storage active chain index is treated as matching");
    expect(Access::persist_active_chain_index(blockchain),
           "null storage active chain index persistence is a no-op");

    const auto foundation = state->get_foundation_address();
    auto make_tip_block = [&](geocoin::Height height,
                              const geocoin::Hash256& prev_hash,
                              const std::string& label,
                              uint64_t nonce) {
        auto block = make_coinbase_block(
            height,
            prev_hash,
            geocoin::Address(label),
            foundation
        );
        block.header.nonce = nonce;
        return block;
    };

    auto* genesis = Access::add_index(
        blockchain,
        make_tip_block(0, geocoin::Hash256{}, "GEOtip-genesis", 100),
        nullptr,
        0,
        1,
        have_data
    );
    auto* active1 = Access::add_index(
        blockchain,
        make_tip_block(1, genesis->get_block_hash(), "GEOtip-active1", 101),
        genesis,
        1,
        2,
        have_data
    );
    auto* active2 = Access::add_index(
        blockchain,
        make_tip_block(2, active1->get_block_hash(), "GEOtip-active2", 102),
        active1,
        2,
        3,
        have_data
    );
    auto* fork1 = Access::add_index(
        blockchain,
        make_tip_block(1, genesis->get_block_hash(), "GEOtip-fork1", 103),
        genesis,
        1,
        40,
        have_data
    );
    auto* fork2 = Access::add_index(
        blockchain,
        make_tip_block(2, fork1->get_block_hash(), "GEOtip-fork2", 104),
        fork1,
        2,
        50,
        have_data
    );
    auto* headers_only = Access::add_index(
        blockchain,
        make_tip_block(1, genesis->get_block_hash(), "GEOtip-headers", 105),
        genesis,
        1,
        100,
        geocoin::chain::BlockStatus::VALID_HEADER
    );
    auto* invalid = Access::add_index(
        blockchain,
        make_tip_block(1, genesis->get_block_hash(), "GEOtip-invalid", 106),
        genesis,
        1,
        90,
        have_data | geocoin::chain::BlockStatus::FAILED_VALID
    );
    auto* orphan_root = Access::add_index(
        blockchain,
        make_tip_block(0, geocoin::Hash256{}, "GEOtip-orphan-root", 107),
        nullptr,
        0,
        4,
        have_data
    );
    auto* orphan_child = Access::add_index(
        blockchain,
        make_tip_block(1, orphan_root->get_block_hash(), "GEOtip-orphan-child", 108),
        orphan_root,
        1,
        5,
        have_data
    );
    (void)orphan_child;

    Access::set_active_tip(blockchain, active2);

    const auto tips = blockchain.get_chain_tips();
    auto find_status = [&](const std::string& status) -> const geocoin::core::ChainTipInfo* {
        for (const auto& tip : tips) {
            if (tip.status == status) {
                return &tip;
            }
        }
        return nullptr;
    };

    const auto* active_tip = find_status("active");
    expect(active_tip && active_tip->hash == active2->get_block_hash() &&
               active_tip->height == 2 && active_tip->branch_length == 0,
           "chain tips classify active tip");
    const auto* valid_fork = find_status("valid-fork");
    expect(valid_fork && valid_fork->hash == fork2->get_block_hash() &&
               valid_fork->height == 2 && valid_fork->branch_length == 2,
           "chain tips classify valid fork and branch length");
    const auto* headers_tip = find_status("headers-only");
    expect(headers_tip && headers_tip->hash == headers_only->get_block_hash() &&
               headers_tip->branch_length == 1,
           "chain tips classify headers-only branch");
    const auto* invalid_tip = find_status("invalid");
    expect(invalid_tip && invalid_tip->hash == invalid->get_block_hash(),
           "chain tips classify invalid branch");
    const auto* no_common = find_status("no-common-fork");
    expect(no_common && no_common->height == 1 && no_common->branch_length == 2,
           "chain tips classify disconnected branch");

    Access::add_candidate(blockchain, active2);
    Access::add_candidate(blockchain, fork2);
    Access::add_candidate(blockchain, headers_only);
    Access::add_candidate(blockchain, invalid);
    expect(Access::is_chain_valid(blockchain, active2), "active data chain is valid");
    expect(!Access::is_chain_valid(blockchain, nullptr), "null chain is invalid");
    expect(!Access::is_chain_valid(blockchain, headers_only), "headers-only chain is invalid");
    expect(!Access::is_chain_valid(blockchain, invalid), "failed chain is invalid");
    expect(Access::find_most_work_chain(blockchain) == fork2,
           "most-work selection skips invalid and headers-only candidates");
    geocoin::consensus::ValidationState invalid_switch_state;
    expect(!blockchain.switch_to_tip(headers_only->get_block_hash(), invalid_switch_state),
           "switch_to_tip rejects headers-only target");
    expect(invalid_switch_state.is_error() &&
               invalid_switch_state.get_reject_reason() == "target chain is not valid",
           "switch_to_tip invalid target reports reason");

    Access::mark_failed_descendants(blockchain, fork1);
    expect(geocoin::chain::has_status(fork2->status, geocoin::chain::BlockStatus::FAILED_CHILD),
           "failed descendants are marked");
    expect(Access::find_most_work_chain(blockchain) == active2,
           "failed descendants are removed from best-chain candidates");

    TempDir dir("geocoin-blockchain-active-index-private");
    auto storage = std::make_shared<geocoin::storage::BlockStorage>(dir.path().string());
    expect(storage->is_open(), "active-index test storage opens");
    auto stored_state = std::make_shared<geocoin::state::ChainState>(
        std::make_unique<geocoin::state::MemoryStorage>());
    stored_state->set_block_storage(storage);
    geocoin::core::Blockchain stored_chain(stored_state, storage);
    expect(!Access::active_chain_index_matches(stored_chain),
           "storage-backed active index rejects missing tip");

    auto* stored_genesis = Access::add_index(
        stored_chain,
        make_tip_block(0, geocoin::Hash256{}, "GEOstored-genesis", 109),
        nullptr,
        0,
        1,
        have_data
    );
    auto* stored_tip = Access::add_index(
        stored_chain,
        make_tip_block(1, stored_genesis->get_block_hash(), "GEOstored-tip", 110),
        stored_genesis,
        1,
        2,
        have_data
    );
    Access::set_active_tip(stored_chain, stored_tip);
    expect(!Access::active_chain_index_matches(stored_chain),
           "storage-backed active index rejects absent persisted entries");

    std::vector<std::pair<geocoin::Height, geocoin::Hash256>> entries;
    geocoin::Hash256 tip_hash{};
    geocoin::Height tip_height = 99;
    expect(Access::collect_active_chain_entries(stored_chain, entries, tip_hash, tip_height),
           "active chain entries collect from in-memory chain");
    expect(entries.size() == 2 && entries[0].first == 0 && entries[0].second == stored_genesis->get_block_hash() &&
               entries[1].first == 1 && entries[1].second == stored_tip->get_block_hash() &&
               tip_hash == stored_tip->get_block_hash() && tip_height == 1,
           "active chain entries preserve heights and tip");

    expect(Access::persist_active_chain_index(stored_chain),
           "active chain index persists successfully");
    expect(Access::active_chain_index_matches(stored_chain),
           "active chain index matches after persistence");

    expect(storage->set_active_block(1, hash_from_seed(0xf1)),
           "test corrupts active height entry");
    expect(!Access::active_chain_index_matches(stored_chain),
           "active index mismatch detects wrong height hash");
    expect(Access::persist_active_chain_index(stored_chain),
           "active chain index can be repaired");
    expect(storage->set_active_tip(hash_from_seed(0xf2), 1),
           "test corrupts active tip hash");
    expect(!Access::active_chain_index_matches(stored_chain),
           "active index mismatch detects wrong tip hash");
}

void test_blockchain_accept_connect_and_reorg_guards()
{
    using Access = geocoin::core::BlockchainTestAccess;
    constexpr auto have_data = geocoin::chain::BlockStatus::HAVE_DATA;

    auto state = std::make_shared<geocoin::state::ChainState>(
        std::make_unique<geocoin::state::MemoryStorage>());
    geocoin::core::Blockchain blockchain(state, nullptr);
    const auto foundation = state->get_foundation_address();

    auto make_test_block = [&](geocoin::Height height,
                               const geocoin::Hash256& prev_hash,
                               const std::string& label,
                               uint64_t nonce) {
        auto block = make_coinbase_block(
            height,
            prev_hash,
            geocoin::Address(label),
            foundation
        );
        block.header.nonce = nonce;
        block.header.merkle_root = block.calculate_merkle_root();
        block.header.witness_commitment = block.calculate_witness_commitment();
        return block;
    };

    geocoin::consensus::ValidationState connect_null_state;
    expect(!blockchain.connect_tip(connect_null_state, nullptr),
           "connect_tip rejects null index");
    expect(connect_null_state.is_error() &&
               connect_null_state.get_reject_reason() == "null index",
           "connect_tip null index reports reason");

    geocoin::core::Block loaded;
    expect(!Access::load_block_from_disk(blockchain, nullptr, loaded),
           "load_block_from_disk rejects null index");

    const auto known_block = make_test_block(0, geocoin::Hash256{}, "GEOaccept-known", 700);
    auto* known = Access::add_index(blockchain, known_block, nullptr, 0, 1, have_data);
    Access::add_candidate(blockchain, known);
    geocoin::chain::BlockIndex* duplicate_index = nullptr;
    geocoin::consensus::ValidationState duplicate_state;
    expect(blockchain.accept_block(known_block, duplicate_state, &duplicate_index),
           "accept_block treats duplicate known block as accepted");
    expect(duplicate_index == known, "duplicate accept returns existing block index");

    geocoin::consensus::ValidationState missing_body_state;
    expect(!blockchain.connect_tip(missing_body_state, known),
           "connect_tip rejects indexed block without storage body");
    expect(missing_body_state.is_error() &&
               missing_body_state.get_reject_reason() == "failed to load block from disk",
           "connect_tip missing body reports reason");

    const auto failed_prev_block = make_test_block(0, geocoin::Hash256{}, "GEOfailed-prev", 701);
    auto* failed_prev = Access::add_index(
        blockchain,
        failed_prev_block,
        nullptr,
        0,
        2,
        have_data | geocoin::chain::BlockStatus::FAILED_VALID
    );
    geocoin::chain::BlockIndex* duplicate_failed_index = failed_prev;
    geocoin::consensus::ValidationState duplicate_failed_state;
    expect(!blockchain.accept_block(
               failed_prev_block,
               duplicate_failed_state,
               &duplicate_failed_index),
           "accept_block rejects a block already known to be invalid");
    expect(duplicate_failed_index == nullptr && duplicate_failed_state.is_invalid() &&
               duplicate_failed_state.get_reject_reason() == "duplicate-invalid",
           "known-invalid duplicate has stable rejection and no output index");
    const auto child_of_failed = make_test_block(
        1,
        failed_prev->get_block_hash(),
        "GEOchild-of-failed",
        702
    );
    geocoin::chain::BlockIndex* rejected_child_index = nullptr;
    geocoin::consensus::ValidationState failed_prev_state;
    expect(!blockchain.accept_block(child_of_failed, failed_prev_state, &rejected_child_index),
           "accept_block rejects child of failed prev block");
    expect(rejected_child_index == nullptr, "failed-prev accept leaves out index null");
    expect(failed_prev_state.is_invalid() &&
               failed_prev_state.get_reject_reason() == "bad-prevblk-failed",
           "failed-prev accept reports consensus reason");

    auto* failed_descendant = Access::add_index(
        blockchain,
        make_test_block(1, failed_prev->get_block_hash(), "GEOfailed-descendant", 703),
        failed_prev,
        1,
        3,
        have_data
    );
    expect(!Access::is_chain_valid(blockchain, failed_descendant),
           "chain validity rejects failed ancestor");
    Access::add_candidate(blockchain, failed_descendant);
    expect(Access::find_most_work_chain(blockchain) == known,
           "best-chain selection skips candidate with failed ancestor");

    geocoin::consensus::ValidationState empty_disconnect_state;
    geocoin::core::Blockchain empty_chain(state, nullptr);
    geocoin::consensus::ValidationState no_best_state;
    expect(!empty_chain.activate_best_chain(no_best_state),
           "activate_best_chain rejects empty candidate set");
    expect(no_best_state.is_error() && no_best_state.get_reject_reason() == "no best chain found",
           "activate_best_chain empty candidate set reports reason");
    expect(empty_chain.disconnect_tip(empty_disconnect_state),
           "disconnect_tip succeeds on empty active chain");

    geocoin::core::Blockchain no_common_chain(state, nullptr);
    const auto no_common_genesis_block = make_test_block(0, geocoin::Hash256{}, "GEOcommon-genesis", 710);
    auto* no_common_genesis = Access::add_index(no_common_chain, no_common_genesis_block, nullptr, 0, 1, have_data);
    auto* no_common_active = Access::add_index(
        no_common_chain,
        make_test_block(1, no_common_genesis->get_block_hash(), "GEOcommon-active", 711),
        no_common_genesis,
        1,
        2,
        have_data
    );
    auto* disconnected_candidate = Access::add_index(
        no_common_chain,
        make_test_block(2, geocoin::Hash256{}, "GEOdisconnected-best", 712),
        nullptr,
        2,
        50,
        have_data
    );
    Access::set_active_tip(no_common_chain, no_common_active);
    Access::add_candidate(no_common_chain, disconnected_candidate);
    geocoin::consensus::ValidationState no_common_state;
    expect(!no_common_chain.activate_best_chain(no_common_state),
           "activate_best_chain rejects candidate without common fork");
    expect(no_common_state.is_invalid() &&
               no_common_state.get_reject_reason() == "reorg-no-common-fork",
           "no-common-fork rejection reason is stable");

    geocoin::consensus::ValidationState switch_no_common_state;
    expect(!no_common_chain.switch_to_tip(disconnected_candidate->get_block_hash(), switch_no_common_state),
           "switch_to_tip rejects target without common fork");
    expect(switch_no_common_state.is_invalid() &&
               switch_no_common_state.get_reject_reason() == "reorg-no-common-fork",
           "forced no-common-fork rejection reason is stable");
}

void test_blockchain_policy_checkpoint_and_depth_guards()
{
    using Access = geocoin::core::BlockchainTestAccess;
    constexpr auto have_data = geocoin::chain::BlockStatus::HAVE_DATA;

    auto state = std::make_shared<geocoin::state::ChainState>(
        std::make_unique<geocoin::state::MemoryStorage>());
    const auto foundation = state->get_foundation_address();

    auto make_test_block = [&](geocoin::Height height,
                               const geocoin::Hash256& prev_hash,
                               const std::string& label,
                               uint64_t nonce) {
        auto block = make_coinbase_block(
            height,
            prev_hash,
            geocoin::Address(label),
            foundation
        );
        block.header.nonce = nonce;
        block.header.merkle_root = block.calculate_merkle_root();
        block.header.witness_commitment = block.calculate_witness_commitment();
        return block;
    };

    {
        geocoin::core::Blockchain blockchain(state, nullptr);
        auto* genesis = Access::add_index(
            blockchain,
            make_test_block(0, geocoin::Hash256{}, "GEOpolicy-genesis", 720),
            nullptr,
            0,
            100,
            have_data
        );
        auto* active1 = Access::add_index(
            blockchain,
            make_test_block(1, genesis->get_block_hash(), "GEOpolicy-active1", 721),
            genesis,
            1,
            100,
            have_data
        );

        Access::set_active_tip(blockchain, active1);
        Access::set_policy_checkpoint(blockchain, 1, active1->get_block_hash());
        Access::add_candidate(blockchain, genesis);

        geocoin::consensus::ValidationState tip_below_state;
        expect(!blockchain.activate_best_chain(tip_below_state),
               "activate_best_chain rejects candidate below policy checkpoint");
        expect(tip_below_state.is_invalid() &&
                   tip_below_state.get_reject_reason() == "policy-checkpoint-tip-below",
               "tip-below checkpoint rejection reason is stable");
    }

    {
        geocoin::core::Blockchain blockchain(state, nullptr);
        auto* genesis = Access::add_index(
            blockchain,
            make_test_block(0, geocoin::Hash256{}, "GEOpolicy2-genesis", 730),
            nullptr,
            0,
            1,
            have_data
        );
        auto* active1 = Access::add_index(
            blockchain,
            make_test_block(1, genesis->get_block_hash(), "GEOpolicy2-active1", 731),
            genesis,
            1,
            2,
            have_data
        );
        auto* fork1 = Access::add_index(
            blockchain,
            make_test_block(1, genesis->get_block_hash(), "GEOpolicy2-fork1", 732),
            genesis,
            1,
            20,
            have_data
        );
        auto* fork2 = Access::add_index(
            blockchain,
            make_test_block(2, fork1->get_block_hash(), "GEOpolicy2-fork2", 733),
            fork1,
            2,
            30,
            have_data
        );

        Access::set_active_tip(blockchain, active1);
        Access::set_policy_checkpoint(blockchain, 1, active1->get_block_hash());
        Access::add_candidate(blockchain, fork2);

        geocoin::consensus::ValidationState mismatch_state;
        expect(!blockchain.activate_best_chain(mismatch_state),
               "activate_best_chain rejects fork diverging before policy checkpoint");
        expect(mismatch_state.is_invalid() &&
                   mismatch_state.get_reject_reason() == "policy-checkpoint-mismatch",
               "checkpoint mismatch rejection reason is stable");

        Access::add_candidate(blockchain, active1);
        geocoin::consensus::ValidationState quarantined_state;
        expect(blockchain.activate_best_chain(quarantined_state),
               "forbidden more-work fork is quarantined when an allowed active candidate exists");
        expect(blockchain.get_tip() == active1,
               "quarantined fork does not block the checkpoint-compatible active chain");

        geocoin::consensus::ValidationState switch_active_state;
        expect(blockchain.switch_to_tip(active1->get_block_hash(), switch_active_state),
               "switch_to_tip accepts already-active target");

        geocoin::consensus::ValidationState switch_mismatch_state;
        expect(!blockchain.switch_to_tip(fork2->get_block_hash(), switch_mismatch_state),
               "switch_to_tip rejects target diverging before policy checkpoint");
        expect(switch_mismatch_state.is_invalid() &&
                   switch_mismatch_state.get_reject_reason() == "policy-checkpoint-mismatch",
               "forced checkpoint mismatch rejection reason is stable");
    }

    {
        geocoin::core::Blockchain blockchain(state, nullptr);
        auto* genesis = Access::add_index(
            blockchain,
            make_test_block(0, geocoin::Hash256{}, "GEOdeep-genesis", 740),
            nullptr,
            0,
            1,
            have_data
        );

        geocoin::chain::BlockIndex* active_tip = genesis;
        for (uint64_t h = 1; h <= geocoin::consensus::params::MAX_REORG_DEPTH + 1; ++h) {
            active_tip = Access::add_index(
                blockchain,
                make_test_block(
                    static_cast<geocoin::Height>(h),
                    active_tip->get_block_hash(),
                    "GEOdeep-active-" + std::to_string(h),
                    740 + h
                ),
                active_tip,
                static_cast<geocoin::Height>(h),
                h + 1,
                have_data
            );
        }

        auto* fork1 = Access::add_index(
            blockchain,
            make_test_block(1, genesis->get_block_hash(), "GEOdeep-fork1", 900),
            genesis,
            1,
            static_cast<uint64_t>(geocoin::consensus::params::MAX_REORG_DEPTH) + 1000,
            have_data
        );

        Access::set_active_tip(blockchain, active_tip);
        Access::add_candidate(blockchain, fork1);
        geocoin::consensus::ValidationState too_deep_state;
        expect(!blockchain.activate_best_chain(too_deep_state),
               "activate_best_chain rejects too-deep reorg");
        expect(too_deep_state.is_invalid() &&
                   too_deep_state.get_reject_reason() == "reorg-too-deep",
               "too-deep reorg rejection reason is stable");

        geocoin::consensus::ValidationState switch_too_deep_state;
        expect(!blockchain.switch_to_tip(fork1->get_block_hash(), switch_too_deep_state),
               "switch_to_tip rejects too-deep forced reorg");
        expect(switch_too_deep_state.is_invalid() &&
                   switch_too_deep_state.get_reject_reason() == "reorg-too-deep",
               "forced too-deep reorg rejection reason is stable");

        Access::set_policy_checkpoint(
            blockchain,
            0,
            genesis->get_block_hash(),
            true
        );
        geocoin::consensus::ValidationState override_state;
        expect(Access::check_chain_transition(blockchain, fork1, override_state),
               "explicit emergency override allows deep local-policy transition");
    }
}

void test_sparse_active_chain_index_guards()
{
    using Access = geocoin::core::BlockchainTestAccess;
    constexpr auto have_data = geocoin::chain::BlockStatus::HAVE_DATA;

    TempDir dir("geocoin-blockchain-sparse-active-index");
    auto storage = std::make_shared<geocoin::storage::BlockStorage>(dir.path().string());
    expect(storage->is_open(), "sparse active-index storage opens");

    auto state = std::make_shared<geocoin::state::ChainState>(
        std::make_unique<geocoin::state::MemoryStorage>());
    state->set_block_storage(storage);
    geocoin::core::Blockchain blockchain(state, storage);
    const auto foundation = state->get_foundation_address();

    auto sparse_block = make_coinbase_block(
        2,
        geocoin::Hash256{},
        geocoin::Address("GEOsparse-tip"),
        foundation
    );
    sparse_block.header.nonce = 950;
    auto* sparse_tip = Access::add_index(
        blockchain,
        sparse_block,
        nullptr,
        2,
        10,
        have_data
    );
    Access::set_active_tip(blockchain, sparse_tip);

    std::vector<std::pair<geocoin::Height, geocoin::Hash256>> entries;
    geocoin::Hash256 tip_hash{};
    geocoin::Height tip_height = 0;
    expect(Access::collect_active_chain_entries(blockchain, entries, tip_hash, tip_height),
           "sparse active chain entries collect");
    expect(entries.size() == 1 && entries[0].first == 2 &&
               entries[0].second == sparse_tip->get_block_hash() &&
               tip_hash == sparse_tip->get_block_hash() && tip_height == 2,
           "sparse active entries only include present heights");

    expect(storage->set_active_tip(sparse_tip->get_block_hash(), 2),
           "sparse active tip persists for comparison");
    expect(storage->set_active_block(2, sparse_tip->get_block_hash()),
           "sparse active height persists for comparison");
    expect(Access::active_chain_index_matches(blockchain),
           "sparse active index accepts missing lower heights with no stored hashes");

    expect(storage->set_active_block(0, hash_from_seed(0xd1)),
           "test injects stored hash into sparse lower height");
    expect(!Access::active_chain_index_matches(blockchain),
           "sparse active index rejects unexpected lower stored hash");
}

void test_storage_backed_forced_reorg_success()
{
    using Access = geocoin::core::BlockchainTestAccess;
    constexpr auto have_data = geocoin::chain::BlockStatus::HAVE_DATA;

    TempDir dir("geocoin-blockchain-forced-reorg-success");
    auto storage = std::make_shared<geocoin::storage::BlockStorage>(dir.path().string());
    expect(storage->is_open(), "forced reorg storage opens");

    auto state = std::make_shared<geocoin::state::ChainState>(
        std::make_unique<geocoin::state::MemoryStorage>());
    state->set_block_storage(storage);

    geocoin::core::Blockchain blockchain(state, storage);
    blockchain.set_policy_checkpoint_options(100, 100, false);
    expect(blockchain.initialize(), "forced reorg chain initializes genesis");

    geocoin::Hash256 genesis_hash{};
    expect(blockchain.get_active_block_hash_by_height(0, genesis_hash),
           "forced reorg genesis active hash resolves");
    auto* genesis = blockchain.get_tip();
    expect(genesis != nullptr, "forced reorg genesis index exists");

    int32_t file = 0;
    uint32_t pos = 0;
    const auto block1 = make_block_one(
        genesis_hash,
        geocoin::Address("GEOforced-reorg-block1"),
        state->get_foundation_address()
    );
    expect(storage->save_block_with_pos(block1, 1, file, pos, false),
           "forced reorg block1 saves");
    auto* block1_index = Access::add_index(
        blockchain,
        block1,
        genesis,
        1,
        2,
        have_data
    );
    geocoin::consensus::ValidationState connect_block1_state;
    expect(blockchain.connect_tip(connect_block1_state, block1_index),
           "forced reorg block1 connects");

    const auto old_block2 = make_coinbase_block(
        2,
        block1.get_hash(),
        geocoin::Address("GEOforced-reorg-old2"),
        state->get_foundation_address()
    );
    expect(storage->save_block_with_pos(old_block2, 2, file, pos, false),
           "forced reorg old block2 saves");
    auto* old_block2_index = Access::add_index(
        blockchain,
        old_block2,
        block1_index,
        2,
        3,
        have_data
    );
    geocoin::consensus::ValidationState connect_old_state;
    expect(blockchain.connect_tip(connect_old_state, old_block2_index),
           "forced reorg old block2 connects");

    const auto new_block2 = make_coinbase_block(
        2,
        block1.get_hash(),
        geocoin::Address("GEOforced-reorg-new2"),
        state->get_foundation_address()
    );
    expect(new_block2.get_hash() != old_block2.get_hash(),
           "forced reorg target block differs from old tip");
    expect(storage->save_block_with_pos(new_block2, 2, file, pos, false),
           "forced reorg new block2 saves");
    auto* new_block2_index = Access::add_index(
        blockchain,
        new_block2,
        block1_index,
        2,
        50,
        have_data
    );

    geocoin::consensus::ValidationState switch_state;
    expect(blockchain.switch_to_tip(new_block2_index->get_block_hash(), switch_state),
           "storage-backed forced reorg switches to target tip");
    expect(blockchain.get_tip() == new_block2_index && blockchain.get_height() == 2,
           "forced reorg updates active in-memory tip");
    expect(state->get_height() == 2 && state->get_tip_hash() == new_block2.get_hash(),
           "forced reorg updates chainstate tip");

    geocoin::Hash256 active_hash{};
    expect(storage->get_active_block_hash(2, active_hash) && active_hash == new_block2.get_hash(),
           "forced reorg persists active height entry");
    geocoin::Hash256 active_tip_hash{};
    geocoin::Height active_tip_height = 0;
    expect(storage->get_active_tip(active_tip_hash, active_tip_height) &&
               active_tip_hash == new_block2.get_hash() && active_tip_height == 2,
           "forced reorg persists active tip");
}

void test_storage_backed_activate_best_chain_reorg_success()
{
    using Access = geocoin::core::BlockchainTestAccess;
    constexpr auto have_data = geocoin::chain::BlockStatus::HAVE_DATA;

    TempDir dir("geocoin-blockchain-activate-reorg-success");
    auto storage = std::make_shared<geocoin::storage::BlockStorage>(dir.path().string());
    expect(storage->is_open(), "activate reorg storage opens");

    auto state = std::make_shared<geocoin::state::ChainState>(
        std::make_unique<geocoin::state::MemoryStorage>());
    state->set_block_storage(storage);

    geocoin::core::Blockchain blockchain(state, storage);
    blockchain.set_policy_checkpoint_options(100, 100, false);
    expect(blockchain.initialize(), "activate reorg chain initializes genesis");

    geocoin::Hash256 genesis_hash{};
    expect(blockchain.get_active_block_hash_by_height(0, genesis_hash),
           "activate reorg genesis active hash resolves");
    auto* genesis = blockchain.get_tip();
    expect(genesis != nullptr, "activate reorg genesis index exists");

    int32_t file = 0;
    uint32_t pos = 0;
    const auto block1 = make_block_one(
        genesis_hash,
        geocoin::Address("GEOactivate-reorg-block1"),
        state->get_foundation_address()
    );
    expect(storage->save_block_with_pos(block1, 1, file, pos, false),
           "activate reorg block1 saves");
    auto* block1_index = Access::add_index(
        blockchain,
        block1,
        genesis,
        1,
        2,
        have_data
    );
    block1_index->chain_work = genesis->chain_work + geocoin::uint256(1);
    geocoin::consensus::ValidationState connect_block1_state;
    expect(blockchain.connect_tip(connect_block1_state, block1_index),
           "activate reorg block1 connects");

    const auto old_block2 = make_coinbase_block(
        2,
        block1.get_hash(),
        geocoin::Address("GEOactivate-reorg-old2"),
        state->get_foundation_address()
    );
    expect(storage->save_block_with_pos(old_block2, 2, file, pos, false),
           "activate reorg old block2 saves");
    auto* old_block2_index = Access::add_index(
        blockchain,
        old_block2,
        block1_index,
        2,
        3,
        have_data
    );
    old_block2_index->chain_work = block1_index->chain_work + geocoin::uint256(1);
    geocoin::consensus::ValidationState connect_old_state;
    expect(blockchain.connect_tip(connect_old_state, old_block2_index),
           "activate reorg old block2 connects");

    const auto new_block2 = make_coinbase_block(
        2,
        block1.get_hash(),
        geocoin::Address("GEOactivate-reorg-new2"),
        state->get_foundation_address()
    );
    expect(storage->save_block_with_pos(new_block2, 2, file, pos, false),
           "activate reorg new block2 saves");
    auto* new_block2_index = Access::add_index(
        blockchain,
        new_block2,
        block1_index,
        2,
        100,
        have_data
    );
    new_block2_index->chain_work = block1_index->chain_work + geocoin::uint256(100);
    Access::add_candidate(blockchain, new_block2_index);

    geocoin::consensus::ValidationState activate_state;
    expect(blockchain.activate_best_chain(activate_state),
           "activate_best_chain switches to the most-work fork");
    expect(blockchain.get_tip() == new_block2_index && blockchain.get_height() == 2,
           "activate_best_chain updates active in-memory tip");
    expect(state->get_height() == 2 && state->get_tip_hash() == new_block2.get_hash(),
           "activate_best_chain updates chainstate tip");

    geocoin::Hash256 active_hash{};
    expect(storage->get_active_block_hash(2, active_hash) && active_hash == new_block2.get_hash(),
           "activate_best_chain persists active height entry");
    geocoin::Hash256 active_tip_hash{};
    geocoin::Height active_tip_height = 0;
    expect(storage->get_active_tip(active_tip_hash, active_tip_height) &&
               active_tip_hash == new_block2.get_hash() && active_tip_height == 2,
           "activate_best_chain persists active tip");
}

void test_interrupted_active_chain_transition_recovery()
{
    using Access = geocoin::core::BlockchainTestAccess;
    constexpr auto have_data = geocoin::chain::BlockStatus::HAVE_DATA;
    using Kind = geocoin::storage::ActiveChainTransitionKind;
    using ReadResult = geocoin::storage::ActiveChainTransitionReadResult;

    TempDir dir("geocoin-blockchain-transition-recovery");
    auto storage = std::make_shared<geocoin::storage::BlockStorage>(dir.path().string());
    expect(storage->is_open(), "transition recovery storage opens");

    auto state = std::make_shared<geocoin::state::ChainState>(
        std::make_unique<geocoin::state::MemoryStorage>());
    state->set_block_storage(storage);
    geocoin::core::Blockchain blockchain(state, storage);
    expect(blockchain.initialize(), "transition recovery chain initializes");

    auto* genesis = blockchain.get_tip();
    expect(genesis != nullptr, "transition recovery genesis exists");
    const auto block1 = make_coinbase_block(
        1,
        genesis->get_block_hash(),
        geocoin::Address("GEOtransition-recovery-1"),
        state->get_foundation_address()
    );
    int32_t file = 0;
    uint32_t pos = 0;
    expect(storage->save_block_with_pos(block1, 1, file, pos, false),
           "transition recovery block1 saves");
    auto* block1_index = Access::add_index(
        blockchain,
        block1,
        genesis,
        1,
        2,
        have_data
    );
    geocoin::consensus::ValidationState connect_state;
    expect(blockchain.connect_tip(connect_state, block1_index),
           "transition recovery block1 connects");

    const auto block2 = make_coinbase_block(
        2,
        block1.get_hash(),
        geocoin::Address("GEOtransition-recovery-2"),
        state->get_foundation_address()
    );
    expect(storage->save_block_with_pos(block2, 2, file, pos, false),
           "transition recovery block2 saves");
    (void)Access::add_index(
        blockchain,
        block2,
        block1_index,
        2,
        3,
        have_data
    );

    geocoin::storage::ActiveChainTransitionRecord transition;
    transition.kind = Kind::CONNECT;
    transition.old_tip_exists = true;
    transition.old_height = 1;
    transition.old_tip_hash = block1.get_hash();
    transition.new_height = 2;
    transition.new_tip_hash = block2.get_hash();

    // Simulate a crash where the active index reached the new tip but the
    // derived state database remained at the old tip.
    expect(storage->begin_active_chain_transition(transition),
           "rollback recovery transition intent persists");
    expect(storage->set_active_block_and_tip(2, block2.get_hash()),
           "rollback recovery active index advances");
    expect(Access::recover_interrupted_active_chain_transition(blockchain),
           "transition recovery rolls active index back");

    geocoin::Hash256 active_hash{};
    geocoin::Height active_height = 0;
    expect(storage->get_active_tip(active_hash, active_height) &&
               active_height == 1 && active_hash == block1.get_hash(),
           "rollback recovery restores old active tip");
    expect(!storage->get_active_block_hash(2, active_hash),
           "rollback recovery removes uncommitted active height");

    // Simulate the normal crash window: ChainState committed, active index did
    // not. Recovery must roll the index forward without rebuilding history.
    expect(storage->begin_active_chain_transition(transition),
           "roll-forward recovery transition intent persists");
    expect(state->apply_block(block2),
           "roll-forward recovery derived state advances");
    expect(Access::recover_interrupted_active_chain_transition(blockchain),
           "transition recovery rolls active index forward");
    expect(storage->get_active_tip(active_hash, active_height) &&
               active_height == 2 && active_hash == block2.get_hash(),
           "roll-forward recovery publishes committed active tip");

    geocoin::storage::ActiveChainTransitionRecord loaded;
    expect(storage->get_active_chain_transition(loaded) == ReadResult::NOT_FOUND,
           "successful transition recovery clears journal");
}

} // namespace

int main()
{
    try {
        test_null_and_empty_guards();
        test_block_index_helpers();
        test_initialize_connect_disconnect_and_accessors();
        test_restart_repairs_stale_active_tip_to_most_work();
        test_restart_does_not_bypass_policy_checkpoint_with_more_work_fork();
        test_restart_rejects_active_tip_height_mismatch();
        test_restart_rejects_unknown_active_tip();
        test_restart_rejects_active_tip_without_body();
        test_restart_rejects_incomplete_split_write();
        test_restart_rewrites_incomplete_active_chain_index();
        test_restart_loads_legacy_body_only_block_records();
        test_restart_rejects_missing_or_mismatched_consensus_ruleset();
        test_restart_rejects_orphan_block_index_record();
        test_restart_rejects_compacted_pruned_index_without_snapshot();
        test_restart_rejects_wrong_genesis_datadir();
        test_chain_tip_classification_candidates_and_active_index_guards();
        test_linear_ancestry_validation();
        test_blockchain_accept_connect_and_reorg_guards();
        test_blockchain_policy_checkpoint_and_depth_guards();
        test_sparse_active_chain_index_guards();
        test_storage_backed_forced_reorg_success();
        test_storage_backed_activate_best_chain_reorg_success();
        test_interrupted_active_chain_transition_recovery();
        std::cout << "Blockchain unit tests passed\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "Blockchain unit tests failed: " << ex.what() << "\n";
        return 1;
    }
}
