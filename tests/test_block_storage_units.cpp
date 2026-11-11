#include "geocoin/consensus/consensus.hpp"
#include "geocoin/consensus/proof_of_work.hpp"
#include "geocoin/core/block.hpp"
#include "geocoin/core/transaction.hpp"
#include "geocoin/crypto/geo_area.hpp"
#include "geocoin/storage/block_storage.hpp"
#include "geocoin/storage/read_status.hpp"
#include "geocoin/storage/witness_payload.hpp"
#include "geocoin/utils/varint.hpp"
#include "test_utils.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef _WIN32
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace geocoin::storage {

struct BlockStorageTestAccess {
    static bool get(const BlockStorage& storage, const std::string& key, std::string& value)
    {
        return storage.db_get(key, value);
    }

    static bool put(BlockStorage& storage, const std::string& key, const std::string& value, bool sync = true)
    {
        return storage.db_put(key, value, sync);
    }

    static bool erase(BlockStorage& storage, const std::string& key, bool sync = true)
    {
        return storage.db_delete(key, sync);
    }

    static std::string block_key(const BlockStorage& storage, const Hash256& hash)
    {
        return storage.block_key(hash);
    }

    static std::string block_index_key(const BlockStorage& storage, const Hash256& hash)
    {
        return storage.block_index_key(hash);
    }

    static std::string active_height_key(const BlockStorage& storage, Height height)
    {
        return storage.active_height_key(height);
    }

    static std::string active_tip_key(const BlockStorage& storage)
    {
        return storage.active_tip_key();
    }

    static std::string active_transition_key(const BlockStorage& storage)
    {
        return storage.active_transition_key();
    }

    static std::string policy_checkpoint_key(const BlockStorage& storage)
    {
        return storage.policy_checkpoint_key();
    }

    static std::string best_block_key(const BlockStorage& storage)
    {
        return storage.best_block_key();
    }

    static std::string pruned_index_anchor_key(const BlockStorage& storage)
    {
        return storage.pruned_index_anchor_key();
    }

    static std::string chainwork_key(const BlockStorage& storage, const Hash256& hash)
    {
        return storage.chainwork_key(hash);
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
        hash[i] = static_cast<uint8_t>(seed + i * 23);
    }
    return hash;
}

std::string hash_to_hex(const geocoin::Hash256& hash)
{
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (const auto byte : hash) {
        oss << std::setw(2) << static_cast<int>(byte);
    }
    return oss.str();
}

std::string hash_bytes(const geocoin::Hash256& hash)
{
    return std::string(reinterpret_cast<const char*>(hash.data()), hash.size());
}

std::string witness_payload_key_for(const geocoin::Hash256& hash)
{
    return "witness:" + hash_to_hex(hash);
}

std::string witness_payload_hash_key_for(const geocoin::Hash256& hash)
{
    return "witnessh:" + hash_to_hex(hash);
}

std::string bytes_as_string(const geocoin::bytes& data)
{
    return std::string(reinterpret_cast<const char*>(data.data()), data.size());
}

std::string legacy_height_hash_record(uint32_t height, const geocoin::Hash256& hash)
{
    std::string value(4 + hash.size(), '\0');
    std::memcpy(value.data(), &height, 4);
    std::memcpy(value.data() + 4, hash.data(), hash.size());
    return value;
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

geocoin::core::Transaction make_coinbase_tx(geocoin::Height height)
{
    return geocoin::core::Transaction::create_coinbase(
        height,
        geocoin::Address("GEOminer" + std::to_string(height)),
        50 * geocoin::COIN
    );
}

geocoin::core::Transaction make_witness_tx()
{
    geocoin::core::Transaction tx;
    tx.version = 2;
    tx.type = geocoin::core::TransactionType::STANDARD;
    tx.timestamp = 1234;

    geocoin::core::TxInput input;
    input.prev_tx_hash = hash_from_seed(0x31);
    input.vout = 0;
    tx.inputs.push_back(input);

    geocoin::core::TxOutput output;
    output.value = geocoin::COIN;
    output.address = geocoin::Address("GEOrecipient");
    tx.outputs.push_back(output);

    geocoin::core::TxWitness witness;
    witness.public_key_hash = hash_from_seed(0x32);
    witness.signature = {1, 2, 3, 4, 5};
    tx.witness_version = geocoin::consensus::params::WITNESS_FORMAT_VERSION_CURRENT;
    tx.witnesses.push_back(witness);
    return tx;
}

geocoin::core::Block make_block(
    const geocoin::Hash256& prev_hash,
    geocoin::Height height,
    std::vector<geocoin::core::Transaction> txs,
    uint64_t nonce_salt = 0
)
{
    geocoin::core::Block block;
    block.header.version = 1;
    block.header.prev_block_hash = prev_hash;
    block.header.timestamp = static_cast<uint32_t>(1000 + height + nonce_salt);
    block.header.bits = geocoin::consensus::ProofOfWork::get_initial_bits();
    block.header.geo_tag = 0;
    block.header.nonce = height * 100 + nonce_salt;
    block.transactions = std::move(txs);
    block.header.merkle_root = block.calculate_merkle_root();
    block.header.witness_commitment = block.calculate_witness_commitment();
    return block;
}

bool contains_hash(const std::vector<geocoin::Hash256>& hashes, const geocoin::Hash256& hash)
{
    return std::find(hashes.begin(), hashes.end(), hash) != hashes.end();
}

bool block_has_material_witness(const geocoin::core::Block& block)
{
    for (const auto& tx : block.transactions) {
        for (const auto& witness : tx.witnesses) {
            if (!witness.signature.empty() || !witness.public_key.empty()) {
                return true;
            }
        }
    }
    return false;
}

void append_varint(geocoin::bytes& out, uint64_t value)
{
    const auto encoded = geocoin::utils::encode_varint(value);
    out.insert(out.end(), encoded.begin(), encoded.end());
}

geocoin::bytes witness_payload_prefix(uint64_t witness_count)
{
    geocoin::bytes payload;
    append_varint(payload, 1);
    payload.push_back(geocoin::consensus::params::WITNESS_FORMAT_VERSION_CURRENT);
    append_varint(payload, witness_count);
    return payload;
}

void append_witness_hash_and_flags(
    geocoin::bytes& payload,
    const geocoin::Hash256& public_key_hash,
    uint8_t flags
)
{
    payload.insert(payload.end(), public_key_hash.begin(), public_key_hash.end());
    payload.push_back(flags);
}

void save_block(
    geocoin::storage::BlockStorage& storage,
    const geocoin::core::Block& block,
    geocoin::Height height
)
{
    int32_t file = -1;
    uint32_t pos = 0;
    expect(storage.save_block_with_pos(block, height, file, pos, false), "save_block_with_pos failed");
    expect(file == 0, "block storage virtual file should be zero");
}

template<class Fn>
void expect_read_error(Fn&& fn, const char* message,
                       geocoin::storage::ReadStatus status = geocoin::storage::ReadStatus::CORRUPT) {
    bool caught = false;
    try { fn(); } catch (const geocoin::storage::ReadError& error) { caught = error.status() == status; }
    expect(caught, message);
}

void test_closed_storage_guards()
{
    geocoin::storage::BlockStorage storage;
    expect(!storage.is_open(), "default BlockStorage must be closed");
    expect_read_error([&] { storage.get_best_height(); }, "closed height read reports I/O error", geocoin::storage::ReadStatus::IO_ERROR);
    expect_read_error([&] { storage.get_block_count(); }, "closed count read reports I/O error", geocoin::storage::ReadStatus::IO_ERROR);
    expect(storage.get_size() == 0, "closed storage size is zero");

    geocoin::core::Block block;
    geocoin::Hash256 hash = hash_from_seed(1);
    geocoin::Height height = 99;
    geocoin::uint256 chain_work;
    geocoin::bytes payload;
    uint64_t pruned_count = 1;
    int32_t file = -1;
    uint32_t pos = 7;

    expect(!storage.save_block(block, 0, false), "closed storage rejects save_block");
    expect(!storage.save_block_with_pos(block, 0, file, pos, false),
           "closed storage rejects save_block_with_pos");
    expect_read_error([&] { (void)storage.get_block(hash, block); }, "closed storage rejects get_block", geocoin::storage::ReadStatus::IO_ERROR);
    expect_read_error([&] { (void)storage.get_block_by_height(0, block); }, "closed storage rejects get_block_by_height", geocoin::storage::ReadStatus::IO_ERROR);
    expect_read_error([&] { (void)storage.get_block_hashes_by_height(0); }, "closed storage has no height hashes", geocoin::storage::ReadStatus::IO_ERROR);
    expect_read_error([&] { (void)storage.get_children(hash); }, "closed storage has no children", geocoin::storage::ReadStatus::IO_ERROR);
    expect(!storage.set_active_block(0, hash), "closed storage rejects set_active_block");
    expect_read_error([&] { (void)storage.get_active_block_hash(0, hash); }, "closed storage rejects get_active_block_hash", geocoin::storage::ReadStatus::IO_ERROR);
    expect(!storage.set_active_tip(hash, 0), "closed storage rejects set_active_tip");
    expect_read_error([&] { (void)storage.get_active_tip(hash, height); }, "closed storage rejects get_active_tip", geocoin::storage::ReadStatus::IO_ERROR);
    expect(!storage.set_active_block_and_tip(0, hash), "closed storage rejects set_active_block_and_tip");
    expect(!storage.truncate_active_chain_and_set_tip(0, hash, 0),
           "closed storage rejects truncate active chain");
    expect(!storage.replace_active_chain({{0, hash}}, hash, 0),
           "closed storage rejects replace active chain");
    std::string ruleset_id;
    expect(!storage.set_consensus_ruleset_id("test-ruleset"),
           "closed storage rejects set consensus ruleset");
    expect_read_error([&] { storage.get_consensus_ruleset_id(ruleset_id); },
                      "closed storage rejects get consensus ruleset", geocoin::storage::ReadStatus::IO_ERROR);
    expect(!storage.set_policy_checkpoint(hash, 0), "closed storage rejects set_policy_checkpoint");
    expect_read_error([&] { (void)storage.get_policy_checkpoint(hash, height); }, "closed storage rejects get_policy_checkpoint", geocoin::storage::ReadStatus::IO_ERROR);
    expect(!storage.clear_active_from_height(0), "closed storage rejects clear_active_from_height");
    expect_read_error([&] { (void)storage.has_block(hash); }, "closed storage has no blocks", geocoin::storage::ReadStatus::IO_ERROR);
    expect_read_error([&] { (void)storage.get_best_block(block); }, "closed storage rejects get_best_block", geocoin::storage::ReadStatus::IO_ERROR);
    expect(!storage.set_best_block(hash, 0), "closed storage rejects set_best_block");
    expect(!storage.delete_block(hash), "closed storage rejects delete_block");
    expect(!storage.prune_block_body_at_height(0), "closed storage rejects prune block body");
    expect(!storage.prune_block_bodies_below_height(1, 0, &pruned_count),
           "closed storage rejects prune block bodies range");
    expect(!storage.prune_block_indexes_below_height(1, hash, block.header, chain_work, &pruned_count),
           "closed storage rejects prune block indexes range");
    geocoin::storage::BlockStorage::PrunedIndexAnchor anchor;
    expect_read_error([&] { (void)storage.get_pruned_index_anchor(anchor); }, "closed storage rejects pruned index anchor", geocoin::storage::ReadStatus::IO_ERROR);
    expect(!storage.prune_witness_at_height(0), "closed storage rejects prune witness");
    expect(!storage.prune_witness_up_to(1), "closed storage rejects witness range maintenance");

    geocoin::storage::BlockStorage::PruningStats stats;
    expect_read_error([&] { (void)storage.get_pruning_stats(stats); }, "closed storage rejects pruning stats", geocoin::storage::ReadStatus::IO_ERROR);
    expect(!storage.set_pruning_stats(stats), "closed storage rejects set pruning stats");
    expect_read_error([&] { (void)storage.get_witness_pruned_height(height); }, "closed storage rejects witness pruned height", geocoin::storage::ReadStatus::IO_ERROR);
    expect(!storage.set_witness_pruned_height(0), "closed storage rejects set witness pruned height");
    expect_read_error([&] { (void)storage.get_witness_payload(hash, payload); }, "closed storage rejects get witness payload", geocoin::storage::ReadStatus::IO_ERROR);
    expect(!storage.set_witness_payload(hash, payload), "closed storage rejects set witness payload");
    expect_read_error([&] { storage.build_witness_payload_from_block(hash, payload); },
                      "closed storage rejects build witness payload", geocoin::storage::ReadStatus::IO_ERROR);
    expect(!storage.restore_witness_from_payload(hash, payload),
           "closed storage rejects restore witness payload");
}

void test_open_empty_metadata_guards()
{
    TempDir dir("geocoin-block-storage-empty");
    geocoin::storage::BlockStorage storage(dir.path().string());
    expect(storage.is_open(), "empty BlockStorage opens");

    geocoin::core::Block block;
    expect(!storage.get_best_block(block), "empty storage has no best block");
    expect(storage.get_best_height() == 0, "empty storage best height defaults to zero");
    expect(storage.get_all_block_hashes().empty(), "empty storage has no block hashes");
    expect(storage.get_all_block_index_records().empty(), "empty storage has no block index records");
    expect(storage.get_block_hashes_by_height(77).empty(), "empty storage height index is empty");
    expect(storage.get_children(hash_from_seed(0x21)).empty(), "empty storage children index is empty");

    geocoin::Hash256 hash{};
    geocoin::Height height = 99;
    expect(!storage.get_active_block_hash(0, hash), "empty storage has no active block");
    expect(!storage.get_active_tip(hash, height), "empty storage has no active tip");
    expect(!storage.get_policy_checkpoint(hash, height), "empty storage has no policy checkpoint");
    std::string ruleset_id;
    expect(!storage.get_consensus_ruleset_id(ruleset_id),
           "empty storage has no consensus ruleset");
    expect(!storage.set_consensus_ruleset_id(""),
           "empty consensus ruleset is rejected");
    expect(!storage.set_consensus_ruleset_id(std::string(129, 'x')),
           "oversized consensus ruleset is rejected");
    expect(storage.set_consensus_ruleset_id("test-consensus-v1"),
           "valid consensus ruleset is stored");
    expect(storage.get_consensus_ruleset_id(ruleset_id) && ruleset_id == "test-consensus-v1",
           "consensus ruleset roundtrips");
    geocoin::storage::BlockStorage::PrunedIndexAnchor anchor;
    expect(!storage.get_pruned_index_anchor(anchor), "empty storage has no pruned index anchor");

    geocoin::uint256 work;
    expect(!storage.get_chainwork(hash_from_seed(0x22), work), "empty storage has no chainwork");
    geocoin::bytes payload;
    expect(!storage.get_witness_payload(hash_from_seed(0x23), payload), "empty storage has no witness payload");
    expect(storage.clear_active_from_height(0), "clear active on empty storage is a no-op");
}

void test_open_indices_and_64bit_height_records()
{
    TempDir dir("geocoin-block-storage-index");
    geocoin::storage::BlockStorage storage;
    expect(storage.open(dir.path().string()), "BlockStorage opens");
    expect(!storage.open(dir.path().string()), "BlockStorage refuses double open");
    expect(storage.is_open(), "BlockStorage reports open");
    expect(!storage.get_chaindata_path().empty(), "chaindata path is populated");
    expect(!storage.get_index_path().empty(), "index path is populated");

    storage.set_ibd_flush_interval(0);
    storage.set_ibd_mode(true);

    const auto genesis = make_block({}, 0, {make_coinbase_tx(0)}, 1);
    const auto fork_a = make_block(genesis.get_hash(), 1, {make_coinbase_tx(1)}, 1);
    const auto fork_b = make_block(genesis.get_hash(), 1, {make_coinbase_tx(1)}, 2);

    save_block(storage, genesis, 0);
    save_block(storage, fork_a, 1);
    save_block(storage, fork_b, 1);
    storage.set_ibd_mode(false);

    expect(storage.has_block(genesis.get_hash()), "saved genesis is present");
    expect(storage.get_block_count() == 3, "block count includes fork block bodies");
    expect(storage.get_all_block_hashes().size() == 3, "all block hashes include saved blocks");

    geocoin::core::Block loaded;
    expect(storage.get_block(fork_a.get_hash(), loaded), "get_block loads fork A");
    expect(loaded.get_hash() == fork_a.get_hash(), "loaded block hash matches");
    expect(!storage.get_block(hash_from_seed(0xee), loaded), "missing block is not loaded");
    expect(!storage.get_block_by_height(1, loaded), "height lookup without active index is absent");

    const auto height_hashes = storage.get_block_hashes_by_height(1);
    expect(height_hashes.size() == 2, "height index keeps both fork hashes");
    expect(contains_hash(height_hashes, fork_a.get_hash()), "height index contains fork A");
    expect(contains_hash(height_hashes, fork_b.get_hash()), "height index contains fork B");

    const auto children = storage.get_children(genesis.get_hash());
    expect(children.size() == 2, "parent-child index keeps both children");

    expect(storage.set_active_block_and_tip(0, genesis.get_hash()), "set active genesis and tip");
    expect(storage.set_active_block_and_tip(1, fork_a.get_hash()), "set active fork A and tip");
    geocoin::Hash256 active_hash{};
    expect(storage.get_active_block_hash(1, active_hash) && active_hash == fork_a.get_hash(),
           "active height resolves fork A");
    expect(storage.get_block_by_height(1, loaded) && loaded.get_hash() == fork_a.get_hash(),
           "get_block_by_height uses active index");

    expect(storage.replace_active_chain({{0, genesis.get_hash()}, {1, fork_b.get_hash()}}, fork_b.get_hash(), 1),
           "replace_active_chain swaps fork");
    expect(storage.get_active_block_hash(1, active_hash) && active_hash == fork_b.get_hash(),
           "active height resolves fork B after replace");
    expect(storage.clear_active_from_height(2), "clear active above tip is a no-op");
    expect(storage.get_active_block_hash(1, active_hash) && active_hash == fork_b.get_hash(),
           "active height survives clear above tip");
    expect(storage.clear_active_from_height(1), "clear active removes entries from requested height");
    expect(!storage.get_active_block_hash(1, active_hash), "active height is removed by clear");
    expect(storage.set_active_block_and_tip(1, fork_b.get_hash()), "restore active fork B after clear");

    expect(storage.truncate_active_chain_and_set_tip(1, genesis.get_hash(), 0),
           "truncate_active_chain_and_set_tip succeeds");
    geocoin::Height active_tip_height = 99;
    expect(storage.get_active_tip(active_hash, active_tip_height) && active_tip_height == 0 &&
               active_hash == genesis.get_hash(),
           "active tip moved back to genesis");
    expect(!storage.get_active_block_hash(1, active_hash), "truncated active height is gone");
    expect(storage.clear_active_from_height(100), "clear_active_from_height above tip is a no-op");

    expect(storage.set_active_block(1000, genesis.get_hash()),
           "set sparse active height without filling 1..999");
    expect(storage.set_active_tip(genesis.get_hash(), 1000), "set sparse active tip");
    expect(storage.replace_active_chain({{0, genesis.get_hash()}}, genesis.get_hash(), 0),
           "replace_active_chain deletes only existing a: keys");
    expect(storage.get_active_block_hash(0, active_hash) && active_hash == genesis.get_hash(),
           "replace_active_chain keeps height 0");
    expect(!storage.get_active_block_hash(1000, active_hash),
           "replace_active_chain removes sparse height 1000 without 0..tip deletes");
    expect(!storage.has_incomplete_split_write(),
           "replace_active_chain dual write clears split pending marker");

    const geocoin::Height big_height = (static_cast<geocoin::Height>(1) << 33) + 17;
    expect(storage.set_active_block(big_height, fork_a.get_hash()), "set active block at 64-bit height");
    expect(storage.set_active_tip(fork_a.get_hash(), big_height), "set active tip at 64-bit height");
    expect(storage.get_active_tip(active_hash, active_tip_height) && active_tip_height == big_height &&
               active_hash == fork_a.get_hash(),
           "active tip preserves 64-bit height");
    expect(storage.get_block_by_height(big_height, loaded) && loaded.get_hash() == fork_a.get_hash(),
           "get_block_by_height works for 64-bit active height");

    expect(storage.set_best_block(fork_b.get_hash(), big_height), "set best block at 64-bit height");
    expect(storage.get_best_height() == big_height, "best height preserves 64-bit value");
    expect(storage.get_best_block(loaded) && loaded.get_hash() == fork_b.get_hash(), "best block loads");

    expect(storage.set_policy_checkpoint(fork_a.get_hash(), big_height), "set policy checkpoint");
    geocoin::Height checkpoint_height = 0;
    expect(storage.get_policy_checkpoint(active_hash, checkpoint_height) &&
               checkpoint_height == big_height && active_hash == fork_a.get_hash(),
           "policy checkpoint preserves 64-bit height");

    expect(storage.get_size() > 0, "storage size is positive after writes");
    storage.close();
    expect(!storage.is_open(), "BlockStorage closes");
}

void test_pruning_and_index_anchor()
{
    TempDir dir("geocoin-block-storage-prune");
    geocoin::storage::BlockStorage storage(dir.path().string());
    expect(storage.is_open(), "prune storage opens");

    const auto block0 = make_block({}, 0, {make_coinbase_tx(0)}, 1);
    const auto block1 = make_block(block0.get_hash(), 1, {make_coinbase_tx(1)}, 1);
    const auto block2 = make_block(block1.get_hash(), 2, {make_coinbase_tx(2)}, 1);
    save_block(storage, block0, 0);
    save_block(storage, block1, 1);
    save_block(storage, block2, 2);
    expect(storage.set_active_block_and_tip(0, block0.get_hash()), "set active block0");
    expect(storage.set_active_block_and_tip(1, block1.get_hash()), "set active block1");
    expect(storage.set_active_block_and_tip(2, block2.get_hash()), "set active block2");

    geocoin::storage::BlockStorage::PruningStats stats;
    expect(storage.get_pruning_stats(stats), "default pruning stats load");
    expect(stats.total_blocks == 0 && stats.pruned_blocks == 0, "default pruning stats are zero");
    stats.total_blocks = 10;
    stats.pruned_blocks = 3;
    stats.body_bytes_reduced = 100;
    stats.bytes_total = 200;
    expect(storage.set_pruning_stats(stats), "set pruning stats");
    geocoin::storage::BlockStorage::PruningStats loaded_stats;
    expect(storage.get_pruning_stats(loaded_stats) && loaded_stats.pruned_blocks == 3,
           "pruning stats roundtrip");

    geocoin::Height witness_pruned_height = 99;
    expect(storage.get_witness_pruned_height(witness_pruned_height) && witness_pruned_height == 0,
           "missing witness pruned height defaults to zero");
    expect(storage.set_witness_pruned_height(123), "set witness pruned height");
    expect(storage.get_witness_pruned_height(witness_pruned_height) && witness_pruned_height == 123,
           "witness pruned height roundtrip");

    geocoin::Hash256 pruned_hash{};
    expect(storage.prune_block_body_at_height(99, &pruned_hash), "pruning missing active height is a no-op");
    expect(storage.set_active_block(99, hash_from_seed(0xfb)), "set active height with missing body");
    expect(storage.prune_block_body_at_height(99, &pruned_hash),
           "pruning active height with missing body is a no-op");
    expect(storage.clear_active_from_height(99), "remove incomplete active fixture before index pruning");
    expect(storage.prune_block_body_at_height(0, &pruned_hash), "prune block body at height 0");
    expect(pruned_hash == block0.get_hash(), "pruned hash is returned");
    expect(!storage.has_block(block0.get_hash()), "pruned block body is removed");
    geocoin::core::Block loaded;
    expect(!storage.get_block(block0.get_hash(), loaded, false), "pruned block body is not readable");

    const auto records = storage.get_all_block_index_records();
    const auto block0_record = std::find_if(records.begin(), records.end(), [&](const auto& record) {
        return record.hash == block0.get_hash();
    });
    expect(block0_record != records.end(), "pruned block keeps index record");
    expect(!block0_record->has_body, "pruned index record marks missing body");

    uint64_t pruned_count = 99;
    expect(storage.prune_block_bodies_below_height(0, 0, &pruned_count) && pruned_count == 0,
           "prune bodies below zero is a no-op");
    expect(storage.prune_block_bodies_below_height(2, 1, &pruned_count) && pruned_count == 1,
           "prune bodies below height honors max_blocks");
    expect(!storage.has_block(block1.get_hash()), "height 1 body was pruned by range pruning");
    expect(storage.has_block(block2.get_hash()), "height 2 body is retained");
    expect(storage.prune_block_bodies_below_height(2, 1, &pruned_count) && pruned_count == 0,
           "range pruning resumes from stored next height");

    geocoin::uint256 chain_work;
    chain_work.data[0] = 1234;
    expect(storage.set_chainwork(block1.get_hash(), chain_work), "set chainwork");
    geocoin::uint256 loaded_work;
    expect(storage.get_chainwork(block1.get_hash(), loaded_work) && loaded_work == chain_work,
           "chainwork roundtrip");

    uint64_t index_pruned = 99;
    expect(storage.prune_block_indexes_below_height(0, block0.get_hash(), block0.header, chain_work, &index_pruned) &&
               index_pruned == 0,
           "index pruning below zero is a no-op");
    expect(!storage.prune_block_indexes_below_height(2, hash_from_seed(0xfa), block1.header, chain_work, &index_pruned),
           "index pruning rejects mismatched anchor hash");
    expect(storage.prune_block_indexes_below_height(2, block1.get_hash(), block1.header, chain_work, &index_pruned),
           "index pruning below height succeeds");
    expect(index_pruned >= 1, "index pruning reports removed records");

    geocoin::storage::BlockStorage::PrunedIndexAnchor anchor;
    expect(storage.get_pruned_index_anchor(anchor), "pruned index anchor loads");
    expect(anchor.height == 1 && anchor.hash == block1.get_hash(), "pruned anchor points to height 1");
    expect(anchor.chain_work == chain_work, "pruned anchor stores chainwork");

    expect(storage.delete_block(block2.get_hash()), "delete_block succeeds");
    expect(!storage.has_block(block2.get_hash()), "delete_block removes body");
}

void test_witness_payload_prune_restore()
{
    TempDir dir("geocoin-block-storage-witness");
    geocoin::storage::BlockStorage storage(dir.path().string());
    expect(storage.is_open(), "witness storage opens");

    const auto witness_block = make_block(hash_from_seed(0x81), 1, {make_witness_tx()}, 1);
    save_block(storage, witness_block, 1);
    expect(storage.set_active_block_and_tip(1, witness_block.get_hash()), "set active witness block");

    geocoin::bytes payload;
    expect(storage.build_witness_payload_from_block(witness_block.get_hash(), payload),
           "build witness payload from material block");
    expect(!payload.empty(), "witness payload is non-empty");
    expect(storage.set_witness_payload(hash_from_seed(0x90), payload), "set detached witness payload");
    geocoin::bytes detached_payload;
    expect(storage.get_witness_payload(hash_from_seed(0x90), detached_payload) && detached_payload == payload,
           "detached witness payload roundtrip");

    expect(!storage.restore_witness_from_payload(witness_block.get_hash(), {}),
           "restore rejects empty witness payload");
    geocoin::bytes malformed{0xff, 0xff, 0xff};
    expect(!storage.restore_witness_from_payload(witness_block.get_hash(), malformed),
           "restore rejects malformed witness payload before hash is pinned");

    expect(storage.prune_witness_at_height(99), "prune witness missing height is no-op");
    expect(storage.prune_witness_at_height(1), "prune witness at active height");
    geocoin::core::Block pruned;
    expect(storage.get_block(witness_block.get_hash(), pruned, false), "read witness-pruned block");
    expect(!block_has_material_witness(pruned), "witness material is removed after prune");
    geocoin::core::Block material;
    expect(storage.get_block(witness_block.get_hash(), material, true) && block_has_material_witness(material),
           "verified reads reconstruct retained witness without rewriting storage");

    geocoin::bytes persisted_payload;
    expect(storage.get_witness_payload(witness_block.get_hash(), persisted_payload),
           "pruned witness payload is persisted");
    geocoin::bytes tampered = persisted_payload;
    tampered.back() ^= 0x01;
    expect(!storage.restore_witness_from_payload(witness_block.get_hash(), tampered),
           "restore rejects payload hash mismatch");
    expect(storage.restore_witness_from_payload(witness_block.get_hash(), persisted_payload),
           "restore accepts original witness payload");
    geocoin::core::Block restored;
    expect(storage.get_block(witness_block.get_hash(), restored, false), "read restored block");
    expect(block_has_material_witness(restored), "restored block has material witness");

    geocoin::bytes rebuilt_payload;
    expect(storage.build_witness_payload_from_block(witness_block.get_hash(), rebuilt_payload),
           "build witness payload after restore");

    const auto no_witness_block = make_block(witness_block.get_hash(), 2, {make_coinbase_tx(2)}, 1);
    save_block(storage, no_witness_block, 2);
    expect(!storage.build_witness_payload_from_block(no_witness_block.get_hash(), rebuilt_payload),
           "build witness payload rejects block without material witness");
}

void test_ibd_disable_wal_sync_transitions()
{
    TempDir dir("geocoin-block-storage-ibd-wal");
    geocoin::storage::StorageOptions options;
    options.profile = geocoin::storage::StorageProfile::FAST_SYNC;
    options.disable_wal_during_ibd = true;

    geocoin::storage::BlockStorage storage(
        dir.path().string(),
        geocoin::storage::StorageBackend::ROCKSDB,
        options
    );
    expect(storage.is_open(), "IBD WAL storage opens");

    storage.set_ibd_flush_interval(2);
    storage.set_ibd_mode(true);

    const auto block0 = make_block({}, 0, {make_coinbase_tx(0)}, 11);
    const auto block1 = make_block(block0.get_hash(), 1, {make_coinbase_tx(1)}, 12);
    const auto block2 = make_block(block1.get_hash(), 2, {make_coinbase_tx(2)}, 13);

    save_block(storage, block0, 0);
    save_block(storage, block1, 1);
    expect(storage.has_block(block0.get_hash()), "IBD disable-WAL first block body is present");
    expect(storage.has_block(block1.get_hash()), "IBD disable-WAL flush-interval block body is present");

    storage.set_ibd_flush_interval(1);
    expect(storage.set_active_block_and_tip(0, block0.get_hash()),
           "IBD disable-WAL active index write succeeds with sync checkpoint");
    expect(storage.set_active_block_and_tip(1, block1.get_hash()),
           "IBD disable-WAL second active index write succeeds with sync checkpoint");

    expect(storage.delete_block(block0.get_hash()),
           "IBD disable-WAL delete succeeds with sync checkpoint");
    expect(!storage.has_block(block0.get_hash()), "IBD disable-WAL delete removes block body");

    storage.set_ibd_flush_interval(100);
    save_block(storage, block2, 2);
    storage.set_ibd_mode(false);

    const auto block3 = make_block(block2.get_hash(), 3, {make_coinbase_tx(3)}, 14);
    save_block(storage, block3, 3);
    expect(storage.has_block(block3.get_hash()), "post-IBD forced sync write succeeds");
}

#ifndef _WIN32
[[noreturn]] void run_split_storage_crash_child(const std::string& path)
{
    auto fail = [](int code) -> void { ::_exit(code); };

    geocoin::storage::StorageOptions options;
    options.profile = geocoin::storage::StorageProfile::FAST_SYNC;
    options.disable_wal_during_ibd = true;
    auto* storage = new geocoin::storage::BlockStorage(
        path,
        geocoin::storage::StorageBackend::ROCKSDB,
        options
    );
    if (!storage->is_open()) {
        fail(11);
    }
    storage->set_ibd_flush_interval(10000);
    storage->set_ibd_mode(true);

    const auto block = make_block({}, 0, {make_coinbase_tx(0)}, 21);
    int32_t file = -1;
    uint32_t pos = 0;
    if (!storage->save_block_with_pos(block, 0, file, pos, false)) {
        fail(12);
    }

    geocoin::storage::ActiveChainTransitionRecord transition;
    transition.kind = geocoin::storage::ActiveChainTransitionKind::CONNECT;
    transition.old_tip_exists = false;
    transition.new_height = 0;
    transition.new_tip_hash = block.get_hash();
    if (!storage->begin_active_chain_transition(transition)) {
        fail(13);
    }
    if (!storage->set_active_block_and_tip(0, block.get_hash())) {
        fail(14);
    }
    if (!storage->clear_active_chain_transition()) {
        fail(15);
    }

    // Simulate power loss: do not run BlockStorage/RocksDB destructors.
    ::_exit(0);
}

void test_split_storage_survives_abrupt_process_exit(const std::string& self_path)
{
    TempDir dir("geocoin-block-storage-crash-durability");
    const pid_t pid = ::fork();
    expect(pid >= 0, "fork for split-storage crash test succeeds");
    if (pid == 0) {
        ::execl(
            self_path.c_str(),
            self_path.c_str(),
            "--split-storage-crash-child",
            dir.path().c_str(),
            static_cast<char*>(nullptr)
        );
        ::_exit(127);
    }

    int status = 0;
    expect(::waitpid(pid, &status, 0) == pid, "split-storage crash child is reaped");
    expect(WIFEXITED(status) && WEXITSTATUS(status) == 0,
           "split-storage crash child completed all writes before abrupt exit");

    geocoin::storage::BlockStorage reopened(dir.path().string());
    expect(reopened.is_open(), "split storage reopens after abrupt process exit");
    const auto expected = make_block({}, 0, {make_coinbase_tx(0)}, 21);
    geocoin::core::Block loaded;
    expect(reopened.get_block(expected.get_hash(), loaded),
           "block body is durable when transition journal is cleared");
    expect(loaded.get_hash() == expected.get_hash(),
           "durable block body has the expected hash");

    geocoin::Hash256 active_hash{};
    geocoin::Height active_height = 1;
    expect(reopened.get_active_tip(active_hash, active_height),
           "active tip is durable after abrupt process exit");
    expect(active_height == 0 && active_hash == expected.get_hash(),
           "durable active tip and block body agree");

    geocoin::storage::ActiveChainTransitionRecord pending;
    expect(reopened.get_active_chain_transition(pending) ==
               geocoin::storage::ActiveChainTransitionReadResult::NOT_FOUND,
           "cleared transition journal remains absent after abrupt process exit");
}
#endif

void test_reopen_persists_split_metadata()
{
    TempDir dir("geocoin-block-storage-reopen");
    const geocoin::Hash256 payload_hash = hash_from_seed(0xa1);
    const auto block0 = make_block({}, 0, {make_coinbase_tx(0)}, 31);
    const auto block1 = make_block(block0.get_hash(), 1, {make_coinbase_tx(1)}, 32);
    geocoin::uint256 chain_work;
    chain_work.data[0] = 777;
    geocoin::storage::BlockStorage::PruningStats stats;
    stats.total_blocks = 2;
    stats.pruned_blocks = 1;
    stats.body_bytes_reduced = 123;
    stats.bytes_total = 456;
    const geocoin::bytes witness_payload{0x01, 0x02, 0x03, 0x04};

    {
        geocoin::storage::BlockStorage storage(dir.path().string());
        expect(storage.is_open(), "reopen source storage opens");
        save_block(storage, block0, 0);
        save_block(storage, block1, 1);
        expect(storage.set_active_block_and_tip(0, block0.get_hash()), "reopen source stores active block0");
        expect(storage.set_active_block_and_tip(1, block1.get_hash()), "reopen source stores active block1");
        expect(storage.set_best_block(block1.get_hash(), 1), "reopen source stores best block");
        expect(storage.set_policy_checkpoint(block0.get_hash(), 0), "reopen source stores policy checkpoint");
        expect(storage.set_consensus_ruleset_id("reopen-consensus-v1"),
               "reopen source stores consensus ruleset");
        expect(storage.set_chainwork(block1.get_hash(), chain_work), "reopen source stores chainwork");
        expect(storage.set_pruning_stats(stats), "reopen source stores pruning stats");
        expect(storage.set_witness_pruned_height(1), "reopen source stores witness prune height");
        expect(storage.set_witness_payload(payload_hash, witness_payload), "reopen source stores witness payload");
        storage.close();
    }

    {
        geocoin::storage::BlockStorage reopened;
        expect(reopened.open(dir.path().string()), "BlockStorage reopens existing split metadata");
        expect(reopened.get_block_count() == 2, "reopened storage sees saved block bodies");

        geocoin::core::Block loaded;
        expect(reopened.get_block(block1.get_hash(), loaded) && loaded.get_hash() == block1.get_hash(),
               "reopened storage loads block body by hash");
        expect(reopened.get_best_block(loaded) && loaded.get_hash() == block1.get_hash(),
               "reopened storage loads best block");
        expect(reopened.get_block_by_height(1, loaded) && loaded.get_hash() == block1.get_hash(),
               "reopened storage loads active height");

        geocoin::Hash256 hash{};
        geocoin::Height height = 0;
        expect(reopened.get_active_tip(hash, height) && hash == block1.get_hash() && height == 1,
               "reopened storage preserves active tip");
        expect(reopened.get_policy_checkpoint(hash, height) && hash == block0.get_hash() && height == 0,
               "reopened storage preserves policy checkpoint");
        std::string ruleset_id;
        expect(reopened.get_consensus_ruleset_id(ruleset_id) && ruleset_id == "reopen-consensus-v1",
               "reopened storage preserves consensus ruleset");

        geocoin::uint256 loaded_work;
        expect(reopened.get_chainwork(block1.get_hash(), loaded_work) && loaded_work == chain_work,
               "reopened storage preserves chainwork");
        geocoin::storage::BlockStorage::PruningStats loaded_stats;
        expect(reopened.get_pruning_stats(loaded_stats) && loaded_stats.body_bytes_reduced == stats.body_bytes_reduced,
               "reopened storage preserves pruning stats");
        geocoin::Height witness_height = 0;
        expect(reopened.get_witness_pruned_height(witness_height) && witness_height == 1,
               "reopened storage preserves witness pruned height");
        geocoin::bytes loaded_payload;
        expect(reopened.get_witness_payload(payload_hash, loaded_payload) && loaded_payload == witness_payload,
               "reopened storage preserves detached witness payload");

        const auto records = reopened.get_all_block_index_records();
        expect(records.size() == 2, "reopened storage preserves block index records");
        expect(contains_hash(reopened.get_all_block_hashes(), block0.get_hash()) &&
                   contains_hash(reopened.get_all_block_hashes(), block1.get_hash()),
               "reopened storage lists all block hashes");
        reopened.close();
    }
}

void test_current_format_read_errors()
{
    using Access = geocoin::storage::BlockStorageTestAccess;
    TempDir dir("geocoin-block-storage-read-errors");
    geocoin::storage::BlockStorage storage(dir.path().string());
    const auto block = make_block({}, 0, {make_coinbase_tx(0)}, 41);
    save_block(storage, block, 0);
    geocoin::Hash256 hash{};
    geocoin::Height height = 0;
    geocoin::core::Block loaded;
    const auto invalid_record = [&](const std::string& key, const std::string& data, auto&& read) {
        expect(Access::put(storage, key, data), "seed malformed record");
        expect_read_error(read, "malformed record reports corruption");
        expect(Access::erase(storage, key), "remove malformed fixture record");
    };
    for (const auto& value : {std::string("short"), legacy_height_hash_record(7, block.get_hash())}) {
        invalid_record(Access::active_tip_key(storage), value, [&] { storage.get_active_tip(hash, height); });
        invalid_record(Access::policy_checkpoint_key(storage), value, [&] { storage.get_policy_checkpoint(hash, height); });
        invalid_record(Access::best_block_key(storage), value, [&] {
            expect_read_error([&] { storage.get_best_height(); }, "bad best height reports corruption");
            storage.get_best_block(loaded);
        });
    }
    expect(!storage.get_active_tip(hash, height), "missing active tip remains distinct");
    invalid_record(Access::active_height_key(storage, 88), "tiny", [&] { storage.get_active_block_hash(88, hash); });
    expect(Access::put(storage, "h:0000000077", hash_bytes(block.get_hash())), "seed unsupported height alias");
    expect(!storage.get_block_by_height(77, loaded), "unsupported height alias is not used");
    const auto bad_hash = hash_from_seed(0xd1);
    invalid_record(Access::block_key(storage, bad_hash), "not-a-block", [&] {
        expect_read_error([&] { storage.get_all_block_index_records(); }, "unreadable body cannot silently disappear from scan");
        storage.get_block(bad_hash, loaded, false);
    });
    invalid_record(Access::block_index_key(storage, bad_hash), "bad-index", [&] { storage.get_all_block_index_records(); });
    invalid_record("hh:0000000007:" + std::string(63, 'a'), "", [&] { storage.get_block_hashes_by_height(7); });
    invalid_record("hh:0000000007:" + std::string(64, 'z'), "", [&] { storage.get_block_hashes_by_height(7); });
    invalid_record("pc:" + hash_to_hex(bad_hash) + ":" + std::string(64, 'y'), "", [&] { storage.get_children(bad_hash); });
    geocoin::storage::BlockStorage::PrunedIndexAnchor anchor;
    invalid_record(Access::pruned_index_anchor_key(storage), "bad-anchor", [&] { storage.get_pruned_index_anchor(anchor); });
    geocoin::uint256 work;
    invalid_record(Access::chainwork_key(storage, block.get_hash()), "short", [&] {
        expect_read_error([&] { storage.get_all_block_index_records(); }, "bad chainwork cannot silently disappear from scan");
        storage.get_chainwork(block.get_hash(), work);
    });
    geocoin::storage::BlockStorage::PruningStats stats;
    invalid_record("prune_stats", "bad", [&] { storage.get_pruning_stats(stats); });
    invalid_record("witness_pruned_height", "bad", [&] { storage.get_witness_pruned_height(height); });
}

void test_stored_block_commitments()
{
    using Access = geocoin::storage::BlockStorageTestAccess;
    TempDir dir("geocoin-stored-block-commitments");
    geocoin::storage::BlockStorage storage(dir.path().string());
    const auto original = make_block({}, 1, {make_coinbase_tx(1), make_witness_tx()}, 61);
    const auto hash = original.get_hash();
    save_block(storage, original, 1);
    expect(storage.set_active_block_and_tip(1, hash), "activate commitment fixture");
    geocoin::core::Block loaded;
    expect(storage.get_block(hash, loaded, true), "matching commitments are readable");

    auto changed = original;
    ++changed.transactions[1].outputs[0].value;
    expect(changed.get_hash() == hash, "changing a transaction preserves the stored header hash");
    const auto changed_bytes = bytes_as_string(changed.serialize());
    expect(Access::put(storage, Access::block_key(storage, hash), changed_bytes), "seed changed transaction body");
    expect_read_error([&] { storage.get_block(hash, loaded, true); },
                      "verified block read must reject a transaction-root mismatch");
    expect(storage.get_block(hash, loaded, false) && loaded.serialize() == changed.serialize(),
           "raw diagnostic read preserves the original stored data");

    size_t archives = 0;
    storage.set_transaction_archiver([&](const geocoin::core::Block&, geocoin::Height) { ++archives; return true; });
    expect_read_error([&] { storage.prune_block_body_at_height(1); },
                      "body pruning must validate commitments before archiving or deleting");
    expect(archives == 0 && storage.has_block(hash), "invalid body remains intact and is not archived");
    geocoin::bytes extracted;
    expect_read_error([&] { storage.build_witness_payload_from_block(hash, extracted); },
                      "witness extraction must not accept mismatched transaction contents");

    changed = original;
    changed.transactions[1].witnesses[0].signature.back() ^= 1;
    expect(Access::put(storage, Access::block_key(storage, hash), bytes_as_string(changed.serialize())),
           "seed changed witness with the original header");
    expect_read_error([&] { storage.get_block(hash, loaded, true); },
                      "verified block read must reject a witness-commitment mismatch");
    expect_read_error([&] { storage.prune_witness_at_height(1); },
                      "witness pruning must reject mismatched witness contents");
    expect(storage.get_block(hash, loaded, false) && loaded.serialize() == changed.serialize(),
           "failed witness pruning leaves stored contents unchanged");
}

void test_restored_witness_commitments()
{
    TempDir dir("geocoin-restored-witness-commitments");
    geocoin::storage::BlockStorage storage(dir.path().string());
    auto payment = make_witness_tx();
    payment.outputs[0].address = geocoin::test::address_from_seed(43);
    payment.sign(geocoin::test::make_key(42));
    expect(payment.verify(), "witness fixture has a valid payment signature");
    const auto original = make_block({}, 1, {make_coinbase_tx(1), payment}, 62);
    const auto hash = original.get_hash();
    save_block(storage, original, 1);
    expect(storage.set_active_block_and_tip(1, hash), "activate witness fixture");
    geocoin::bytes original_payload;
    expect(storage.build_witness_payload_from_block(hash, original_payload), "extract original witness");
    expect(storage.prune_witness_at_height(1), "retain witness separately from body");
    geocoin::core::Block stripped;
    expect(storage.get_block(hash, stripped, false), "read stripped body");
    const auto stripped_bytes = stripped.serialize();

    auto compressed = original;
    compressed.transactions[1].witnesses[0].public_key.clear();
    const auto changed_payload = geocoin::storage::serialize_witness_payload(compressed);
    expect(compressed.transactions[1].get_hash() == payment.get_hash() &&
           compressed.transactions[1].witnesses[0].signature == payment.witnesses[0].signature,
           "alternate witness retains the transaction ID and signature bytes");
    // A matching local payload checksum is insufficient: the block header is
    // the authority for which witness belongs to this transaction sequence.
    expect(storage.set_witness_payload(hash, changed_payload), "seed self-consistent but wrong witness payload");
    geocoin::bytes read_payload;
    expect(storage.get_witness_payload(hash, read_payload) && read_payload == changed_payload,
           "fixture has a matching payload checksum");
    expect(!storage.restore_witness_from_payload(hash, changed_payload),
           "restore must reject witness that does not match the header commitment");
    geocoin::core::Block loaded;
    expect(storage.get_block(hash, loaded, false) && loaded.serialize() == stripped_bytes,
           "rejected witness restore leaves the block body unchanged");
    expect_read_error([&] { storage.get_block(hash, loaded, true); },
                      "rehydration must reject a wrong witness even with a matching local checksum");
    expect(storage.set_witness_payload(hash, original_payload) &&
           storage.restore_witness_from_payload(hash, original_payload), "matching witness still restores");
    expect(storage.get_block(hash, loaded, true) && loaded.serialize() == original.serialize(),
           "restored block matches the original bytes");
}

void test_pruning_stats_preflight(bool witness_only)
{
    using Access = geocoin::storage::BlockStorageTestAccess;
    TempDir dir("geocoin-pruning-stats-preflight");
    geocoin::storage::BlockStorage storage(dir.path().string());
    const auto block = make_block({}, 1, {make_coinbase_tx(1), make_witness_tx()}, 81);
    const auto hash = block.get_hash();
    save_block(storage, block, 1);
    expect(storage.set_active_block_and_tip(1, hash), "activate statistics fixture");
    size_t archived = 0;
    storage.set_transaction_archiver([&](const auto&, auto) { ++archived; return true; });
    expect(Access::put(storage, "prune_stats", "incomplete"), "seed damaged statistics");
    expect_read_error([&] {
        if (witness_only) storage.prune_witness_at_height(1);
        else storage.prune_block_body_at_height(1);
    }, "pruning rejects damaged statistics");
    geocoin::core::Block loaded;
    expect(storage.get_block(hash, loaded, false) && loaded.serialize() == block.serialize(),
           "damaged statistics must be detected before changing the body");
    expect(archived == 0, "statistics preflight runs before archival callbacks");
    std::string value;
    expect(!Access::get(storage, witness_payload_key_for(hash), value),
           "statistics failure creates no witness payload");
    expect(storage.set_pruning_stats({}), "repair statistics fixture");
    expect(witness_only ? storage.prune_witness_at_height(1) : storage.prune_block_body_at_height(1),
           "pruning retries after statistics repair");
    geocoin::storage::BlockStorage::PruningStats stats;
    expect(storage.get_pruning_stats(stats) && stats.total_blocks == 1 && stats.pruned_blocks == 1,
           "successful retry accounts exactly one block");
    storage.close();
    expect(storage.open(dir.path().string()), "reopen after statistics update");
    expect(storage.get_pruning_stats(stats) && stats.pruned_blocks == 1,
           "pruning statistics survive reopening");
}

void test_witness_range_progress()
{
    using Access = geocoin::storage::BlockStorageTestAccess;
    TempDir dir("geocoin-witness-range-progress");
    geocoin::storage::BlockStorage storage(dir.path().string());
    const auto first = make_block({}, 1, {make_coinbase_tx(1), make_witness_tx()}, 82);
    const auto second = make_block(first.get_hash(), 2, {make_coinbase_tx(2), make_witness_tx()}, 83);
    const auto third = make_block(second.get_hash(), 3, {make_coinbase_tx(3), make_witness_tx()}, 84);
    for (const auto& [height, block] : std::vector<std::pair<geocoin::Height, geocoin::core::Block>>{
             {1, first}, {2, second}, {3, third}}) {
        save_block(storage, block, height);
        expect(storage.set_active_block_and_tip(height, block.get_hash()), "publish witness range block");
    }
    const auto second_key = Access::block_key(storage, second.get_hash());
    expect(Access::put(storage, second_key, "incomplete"), "interrupt the witness range at its second body");
    expect_read_error([&] { storage.prune_witness_up_to(3); }, "incomplete body stops witness range");
    geocoin::Height progress = 0;
    geocoin::storage::BlockStorage::PruningStats stats;
    expect(storage.get_witness_pruned_height(progress) && progress == 1 &&
           storage.get_pruning_stats(stats) && stats.pruned_blocks == 1,
           "only the completed prefix is published on failure");
    geocoin::core::Block loaded;
    expect(storage.get_block(third.get_hash(), loaded, false) && loaded.serialize() == third.serialize(),
           "blocks beyond the failed height remain untouched");
    storage.close();
    expect(storage.open(dir.path().string()) && storage.get_witness_pruned_height(progress) && progress == 1,
           "completed prefix survives reopening after interruption");
    expect(Access::put(storage, second_key, bytes_as_string(second.serialize())), "repair second body");
    expect(storage.prune_witness_up_to(3, 1) && storage.get_witness_pruned_height(progress) && progress == 2,
           "retry processes at most the requested number of bodies");
    expect(storage.prune_witness_up_to(3) && storage.get_pruning_stats(stats) && stats.pruned_blocks == 3 &&
           stats.total_blocks == 3, "retry does not recount the completed prefix");
    expect(!storage.prune_witness_up_to(4) && storage.get_witness_pruned_height(progress) && progress == 3,
           "unknown active height is not marked completed");
    expect(storage.get_block(first.get_hash(), loaded, true) && loaded.serialize() == first.serialize(),
           "witness data can still reconstruct the original first block");
}

void test_pruning_read_errors()
{
    using Access = geocoin::storage::BlockStorageTestAccess;
    TempDir dir("geocoin-pruning-read-errors");
    geocoin::storage::BlockStorage storage(dir.path().string());
    const auto block = make_block({}, 1, {make_coinbase_tx(1)}, 63);
    const auto hash = block.get_hash();
    save_block(storage, block, 1);
    expect(storage.set_active_block_and_tip(1, hash), "activate pruning fixture");
    std::string index_record;
    expect(Access::get(storage, Access::block_index_key(storage, hash), index_record), "read original block index");
    expect(Access::put(storage, "block_body_prune_next_height", "short"), "seed malformed pruning cursor");
    expect_read_error([&] { storage.prune_block_bodies_below_height(2); },
                      "malformed pruning cursor must stop before deleting any block");
    expect(storage.has_block(hash), "bad pruning cursor preserves the body");
    expect(Access::put(storage, Access::block_index_key(storage, hash), "short"), "seed malformed block index");
    expect_read_error([&] { storage.is_block_body_pruned(1); },
                      "pruned-status query must surface index corruption");
    expect(Access::put(storage, Access::block_index_key(storage, hash), index_record + 'x'), "seed trailing index data");
    expect_read_error([&] { storage.get_all_block_index_records(); }, "index records reject trailing bytes");
    expect(Access::put(storage, Access::block_index_key(storage, hash), index_record), "restore original block index");
    expect(storage.set_active_block(2, hash), "seed mismatched active height mapping");
    expect_read_error([&] { storage.is_block_body_pruned(2); }, "pruned-status query validates the indexed height");
    storage.close();
    expect_read_error([&] { storage.is_block_body_pruned(1); }, "closed pruned-status query is an I/O error",
                      geocoin::storage::ReadStatus::IO_ERROR);
}

void test_active_range_maintenance()
{
    using Access = geocoin::storage::BlockStorageTestAccess;
    TempDir dir("geocoin-active-range-maintenance");
    geocoin::storage::BlockStorage storage(dir.path().string());
    const auto base = hash_from_seed(71);
    const auto tip = hash_from_seed(72);
    const auto stale = hash_from_seed(73);
    expect(storage.set_active_block(0, base) && storage.set_active_block_and_tip(1, tip), "seed active range");
    expect(storage.set_active_block(21, stale), "seed entry above recorded tip");
    expect(storage.clear_active_from_height(2), "clear range above recorded tip");
    geocoin::Hash256 loaded;
    expect(!storage.get_active_block_hash(21, loaded), "range cleanup removes entries above a stale tip");

    expect(storage.set_active_block(21, stale) && Access::erase(storage, Access::active_tip_key(storage)),
           "seed active range without a tip");
    expect(storage.truncate_active_chain_and_set_tip(2, tip, 1), "truncate range without an old tip");
    expect(!storage.get_active_block_hash(21, loaded), "truncate removes entries even if old tip is absent");
    geocoin::Height height = 0;
    expect(storage.get_active_tip(loaded, height) && height == 1 && loaded == tip, "truncate publishes new tip");

    const geocoin::Height maximum = (std::numeric_limits<geocoin::Height>::max)();
    for (const auto h : {geocoin::Height{9999999999ULL}, geocoin::Height{10000000000ULL}, maximum}) {
        expect(storage.set_active_block(h, stale), "seed sparse 64-bit heights");
    }
    expect(storage.set_active_tip(stale, maximum), "seed maximum tip height");
    expect(storage.clear_active_from_height(10000000000ULL), "clear sparse range across decimal-width boundary");
    expect(storage.get_active_block_hash(9999999999ULL, loaded) && loaded == stale,
           "numeric lower boundary survives lexical height ordering");
    expect(!storage.get_active_block_hash(10000000000ULL, loaded) && !storage.get_active_block_hash(maximum, loaded),
           "sparse cleanup reaches the largest 64-bit height");
    for (const geocoin::Height boundary : {10000000000ULL, 100000000000ULL, 1000000000000000000ULL,
                                          10000000000000000000ULL}) {
        expect(storage.set_active_block(boundary - 2, stale) && storage.set_active_block(boundary - 1, stale) &&
               storage.set_active_block(boundary, stale), "seed adjacent decimal-width heights");
        expect(storage.clear_active_from_height(boundary - 1), "clear range whose wider keys sort before its start");
        expect(storage.get_active_block_hash(boundary - 2, loaded) &&
               !storage.get_active_block_hash(boundary - 1, loaded) && !storage.get_active_block_hash(boundary, loaded),
               "range cleanup preserves numeric ordering across height key widths");
    }
    expect(storage.set_active_block(maximum, stale), "restore maximum-height entry");
    expect(storage.truncate_active_chain_and_set_tip(maximum, tip, 1) &&
           !storage.get_active_block_hash(maximum, loaded), "truncate includes maximum height without overflow");

    expect(storage.set_active_block(50, stale), "seed range before malformed key");
    expect(Access::put(storage, "a:invalid", hash_bytes(stale)), "seed malformed active index key");
    expect_read_error([&] { storage.clear_active_from_height(2); }, "range cleanup rejects malformed active keys");
    expect_read_error([&] { storage.truncate_active_chain_and_set_tip(2, base, 0); },
                      "truncate validates the range before changing its tip");
    expect(storage.get_active_block_hash(50, loaded) && loaded == stale, "failed cleanup preserves active entries");
    expect(storage.get_active_tip(loaded, height) && height == 1 && loaded == tip, "failed truncate preserves old tip");
    expect(Access::erase(storage, "a:invalid") && storage.clear_active_from_height(2), "clean range succeeds after repair");
    expect(storage.get_active_block_hash(0, loaded) && loaded == base, "cleanup keeps lower active entries");
}

void test_index_pruning_preflight()
{
    using Access = geocoin::storage::BlockStorageTestAccess;
    TempDir dir("geocoin-index-pruning-preflight");
    geocoin::storage::BlockStorage storage(dir.path().string());
    const auto block0 = make_block({}, 0, {make_coinbase_tx(0)}, 71);
    const auto block1 = make_block(block0.get_hash(), 1, {make_coinbase_tx(1)}, 72);
    const auto block2 = make_block(block1.get_hash(), 2, {make_coinbase_tx(2)}, 73);
    for (const auto* block : {&block0, &block1, &block2}) {
        const auto height = block == &block0 ? 0 : block == &block1 ? 1 : 2;
        save_block(storage, *block, height);
        expect(storage.set_active_block_and_tip(height, block->get_hash()), "seed pruning index chain");
    }
    expect(storage.set_policy_checkpoint(block0.get_hash(), 0), "seed original checkpoint");
    std::string original_index;
    const auto index_key = Access::block_index_key(storage, block0.get_hash());
    expect(Access::get(storage, index_key, original_index), "read original pruning index");
    geocoin::uint256 work;
    work.data[0] = 100;
    const auto invalid_record = [&](const std::string& key, const std::string& value, const char* message) {
        std::string original_value;
        const bool existed = Access::get(storage, key, original_value);
        expect(Access::put(storage, key, value), "seed invalid pruning input");
        uint64_t count = 99;
        expect_read_error([&] {
            storage.prune_block_indexes_below_height(2, block1.get_hash(), block1.header, work, &count);
        }, message);
        expect(count == 0, "failed index pruning reports no committed deletions");
        std::string loaded_value;
        expect(Access::get(storage, key, loaded_value) && loaded_value == value, "pruning preserves invalid source record");
        geocoin::storage::BlockStorage::PrunedIndexAnchor anchor;
        expect(!storage.get_pruned_index_anchor(anchor), "failed pruning does not publish an anchor");
        geocoin::Hash256 hash;
        geocoin::Height height;
        expect(storage.get_active_tip(hash, height) && height == 2 && hash == block2.get_hash(), "failed pruning preserves tip");
        expect(storage.get_policy_checkpoint(hash, height) && height == 0 && hash == block0.get_hash(),
               "failed pruning preserves checkpoint");
        expect(existed ? Access::put(storage, key, original_value) : Access::erase(storage, key), "restore invalid fixture");
        expect(storage.get_active_block_hash(0, hash) && hash == block0.get_hash(), "failed pruning preserves older active mappings");
        expect(Access::get(storage, index_key, loaded_value) && loaded_value == original_index, "older block index remains intact");
        expect(storage.get_block_hashes_by_height(0).size() == 1 && storage.get_children(block0.get_hash()).size() == 1,
               "failed pruning preserves height and parent-child indexes");
    };
    invalid_record(index_key, "short", "pruning rejects malformed block index records");
    invalid_record(index_key, original_index + 'x', "pruning rejects trailing block index data");
    invalid_record("bi:invalid", original_index, "pruning rejects malformed block index keys");
    invalid_record("hh:0000000000:invalid", "", "pruning validates the hash in height indexes");
    invalid_record("hh:18446744073709551616:" + hash_to_hex(block0.get_hash()), "", "pruning rejects overflowing height keys");
    invalid_record("a:invalid", hash_bytes(block0.get_hash()), "pruning rejects malformed active height keys");
    invalid_record(Access::active_height_key(storage, 0), "short", "pruning rejects malformed active height values");
    invalid_record("pc:invalid:" + hash_to_hex(block1.get_hash()), "", "pruning rejects malformed parent-child keys");
    uint64_t count = 0;
    expect(!storage.prune_block_indexes_below_height(2, block1.get_hash(), block1.header, work, &count) && count == 0,
           "index pruning must not discard unarchived active block bodies");
    expect(storage.has_block(block0.get_hash()), "out-of-order pruning preserves the active source body");
    const auto fork1 = make_block(block0.get_hash(), 1, {make_coinbase_tx(1)}, 74);
    save_block(storage, fork1, 1);
    expect(storage.set_witness_payload(fork1.get_hash(), geocoin::storage::serialize_witness_payload(fork1)),
           "seed discarded branch witness records");
    expect(!storage.prune_block_indexes_below_height(2, fork1.get_hash(), fork1.header, work, &count) && count == 0,
           "pruning anchor must belong to the active chain");
    size_t archived = 0;
    storage.set_transaction_archiver([&](const geocoin::core::Block& body, geocoin::Height height) {
        expect(body.get_hash() == block0.get_hash() && height == 0, "archive the expected active ancestor");
        ++archived;
        return true;
    });
    expect(storage.prune_block_body_at_height(0) && archived == 1, "archive active body before pruning its index");
    uint64_t discarded_bodies = 0;
    expect(storage.prune_block_indexes_below_height(2, block1.get_hash(), block1.header, work, &count, &discarded_bodies) &&
           count == 2 && discarded_bodies == 1, "valid index pruning removes old history and discarded fork data");
    expect(archived == 1 && !storage.has_block(fork1.get_hash()), "only active history is archived before deletion");
    std::string unused;
    expect(!Access::get(storage, witness_payload_key_for(fork1.get_hash()), unused) &&
           !Access::get(storage, witness_payload_hash_key_for(fork1.get_hash()), unused),
           "discarded witness payload and checksum are removed together");
    geocoin::storage::BlockStorage::PruningStats stats;
    expect(storage.get_pruning_stats(stats) && stats.pruned_blocks == 2,
           "pruning statistics include the discarded fork body exactly once");
    geocoin::storage::BlockStorage::PrunedIndexAnchor anchor;
    expect(storage.get_pruned_index_anchor(anchor) && anchor.hash == block1.get_hash() && anchor.height == 1,
           "valid pruning publishes the expected anchor");
    expect(storage.prune_block_indexes_below_height(2, block1.get_hash(), block1.header, work, &count, &discarded_bodies) &&
           count == 0 && discarded_bodies == 0, "repeated index pruning is idempotent");
}

void test_consensus_ruleset_read_errors()
{
    using Access = geocoin::storage::BlockStorageTestAccess;
    TempDir dir("geocoin-ruleset-read-errors");
    geocoin::storage::BlockStorage storage(dir.path().string());
    std::string ruleset = "old output";
    expect(!storage.get_consensus_ruleset_id(ruleset) && ruleset.empty(), "absent ruleset has no stale output");
    for (const auto& value : {std::string{}, std::string(129, 'x')}) {
        expect(Access::put(storage, "consensus_ruleset", value), "seed malformed ruleset record");
        ruleset = "old output";
        expect_read_error([&] { storage.get_consensus_ruleset_id(ruleset); }, "invalid ruleset is distinct from absence");
        expect(ruleset.empty(), "invalid ruleset is not published to the caller");
    }
    expect(storage.set_consensus_ruleset_id("test-ruleset") && storage.get_consensus_ruleset_id(ruleset) &&
           ruleset == "test-ruleset", "valid ruleset still roundtrips");
    storage.close();
    expect_read_error([&] { storage.get_consensus_ruleset_id(ruleset); }, "closed ruleset read is an I/O error",
                      geocoin::storage::ReadStatus::IO_ERROR);
}

void benchmark_active_range_maintenance()
{
    TempDir dir("geocoin-active-range-benchmark");
    geocoin::storage::BlockStorage storage(dir.path().string());
    const auto hash = hash_from_seed(81);
    constexpr geocoin::Height range = 100000;
    for (const bool truncate : {false, true}) {
        expect(storage.set_active_block(0, hash) && storage.set_active_block(range / 2, hash) &&
               storage.set_active_block_and_tip(range, hash), "seed sparse benchmark range");
        const auto start = std::chrono::steady_clock::now();
        expect(truncate ? storage.truncate_active_chain_and_set_tip(1, hash, 0) : storage.clear_active_from_height(1),
               "benchmark range cleanup succeeds");
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
        geocoin::Hash256 loaded;
        expect(storage.get_active_block_hash(0, loaded) && !storage.get_active_block_hash(range / 2, loaded) &&
               !storage.get_active_block_hash(range, loaded), "benchmark removes only requested entries");
        std::cout << (truncate ? "truncate" : "clear") << ": range=" << range
                  << " existing_entries_removed=2 elapsed_us=" << elapsed << '\n';
    }
}

void test_validation_and_witness_guard_paths()
{
    using Access = geocoin::storage::BlockStorageTestAccess;

    TempDir dir("geocoin-block-storage-guards");
    geocoin::storage::BlockStorage storage(dir.path().string());
    expect(storage.is_open(), "guard storage opens");

    auto invalid_block = make_block({}, 0, {make_coinbase_tx(0)}, 51);
    invalid_block.header.merkle_root = hash_from_seed(0xe1);
    expect(!storage.save_block(invalid_block, 0, true),
           "save_block rejects invalid block when validation is enforced");
    int32_t file = -1;
    uint32_t pos = 0;
    expect(!storage.save_block_with_pos(invalid_block, 0, file, pos, true),
           "save_block_with_pos rejects invalid block when validation is enforced");
    expect(storage.save_block(invalid_block, 0, false),
           "save_block can store test fixture block when validation is disabled");

    const auto valid_block = make_block(hash_from_seed(0xe2), 1, {make_coinbase_tx(1)}, 52);
    const geocoin::Hash256 wrong_hash = hash_from_seed(0xe3);
    const auto valid_data = valid_block.serialize();
    expect(Access::put(
               storage,
               Access::block_key(storage, wrong_hash),
               std::string(reinterpret_cast<const char*>(valid_data.data()), valid_data.size())),
           "seed block body under mismatched hash");
    geocoin::core::Block loaded;
    expect_read_error([&] { storage.get_block(wrong_hash, loaded, true); },
                      "get_block reports mismatched hash as corruption");
    expect(storage.get_block(wrong_hash, loaded, false) && loaded.get_hash() == valid_block.get_hash(),
           "get_block can load mismatched fixture when integrity check is disabled");

    expect(storage.set_active_block(77, wrong_hash), "seed active height with corrupted body hash");
    expect(!storage.prune_block_body_at_height(77),
           "prune_block_body_at_height refuses corrupted block body");

    expect(Access::put(storage, "b:short", "ignored"),
           "seed short block body key");
    expect(Access::put(storage, "b:" + std::string(64, 'x'), "ignored"),
           "seed non-hex block body key");
    expect_read_error([&] { storage.get_all_block_hashes(); }, "malformed block body keys fail listing");

    const geocoin::Hash256 payload_hash = hash_from_seed(0xe4);
    expect(Access::put(storage, witness_payload_key_for(payload_hash), "payload"),
           "seed witness payload");
    expect(Access::put(storage, witness_payload_hash_key_for(payload_hash), "short"),
           "seed corrupt witness payload hash size");
    geocoin::bytes payload_out;
    expect_read_error([&] { storage.get_witness_payload(payload_hash, payload_out); }, "get_witness_payload rejects corrupt hash size");
    expect(Access::put(storage, witness_payload_hash_key_for(payload_hash), hash_bytes(hash_from_seed(0xe5))),
           "seed mismatched witness payload hash");
    expect_read_error([&] { storage.get_witness_payload(payload_hash, payload_out); }, "get_witness_payload rejects hash mismatch");
    expect(!storage.restore_witness_from_payload(payload_hash, {0x01, 0x02, 0x03}),
           "restore_witness_from_payload rejects corrupt stored payload hash before block load");

    auto witness_tx = make_witness_tx();
    const auto witness_block = make_block(hash_from_seed(0xe6), 2, {witness_tx}, 53);
    save_block(storage, witness_block, 2);
    const auto witness_hash = witness_block.get_hash();

    geocoin::bytes mismatch_tx_count;
    append_varint(mismatch_tx_count, 0);
    expect(!storage.restore_witness_from_payload(witness_hash, mismatch_tx_count),
           "restore rejects witness payload with wrong transaction count");

    geocoin::bytes bad_version;
    append_varint(bad_version, 1);
    bad_version.push_back(0);
    expect(!storage.restore_witness_from_payload(witness_hash, bad_version),
           "restore rejects witness payload with unsupported witness version");

    geocoin::bytes wrong_witness_count;
    append_varint(wrong_witness_count, 1);
    wrong_witness_count.push_back(geocoin::consensus::params::WITNESS_FORMAT_VERSION_CURRENT);
    append_varint(wrong_witness_count, 2);
    expect(!storage.restore_witness_from_payload(witness_hash, wrong_witness_count),
           "restore rejects witness payload with wrong witness count");

    auto missing_hash = witness_payload_prefix(1);
    expect(!storage.restore_witness_from_payload(witness_hash, missing_hash),
           "restore rejects witness payload missing public key hash");

    auto invalid_flags = witness_payload_prefix(1);
    append_witness_hash_and_flags(invalid_flags, hash_from_seed(0xe7), 0xfe);
    expect(!storage.restore_witness_from_payload(witness_hash, invalid_flags),
           "restore rejects witness payload with unknown flags");

    auto missing_pubkey = witness_payload_prefix(1);
    append_witness_hash_and_flags(missing_pubkey, hash_from_seed(0xe8), 0x01);
    expect(!storage.restore_witness_from_payload(witness_hash, missing_pubkey),
           "restore rejects witness payload missing flagged public key");

    auto pubkey_hash_mismatch = witness_payload_prefix(1);
    append_witness_hash_and_flags(pubkey_hash_mismatch, hash_from_seed(0xe9), 0x01);
    pubkey_hash_mismatch.insert(
        pubkey_hash_mismatch.end(),
        geocoin::consensus::params::WITNESS_PUBKEY_SIZE,
        0x00
    );
    append_varint(pubkey_hash_mismatch, 0);
    expect(!storage.restore_witness_from_payload(witness_hash, pubkey_hash_mismatch),
           "restore rejects witness payload with mismatched public key hash");

    auto oversized_signature = witness_payload_prefix(1);
    append_witness_hash_and_flags(oversized_signature, hash_from_seed(0xea), 0x00);
    append_varint(oversized_signature, geocoin::consensus::params::MAX_WITNESS_SIG_SIZE + 1);
    expect(!storage.restore_witness_from_payload(witness_hash, oversized_signature),
           "restore rejects witness payload with oversized signature length");

    auto truncated_signature = witness_payload_prefix(1);
    append_witness_hash_and_flags(truncated_signature, hash_from_seed(0xeb), 0x00);
    append_varint(truncated_signature, 4);
    truncated_signature.push_back(0x01);
    expect(!storage.restore_witness_from_payload(witness_hash, truncated_signature),
           "restore rejects witness payload with truncated signature bytes");

    auto trailing_bytes = witness_payload_prefix(1);
    append_witness_hash_and_flags(trailing_bytes, hash_from_seed(0xec), 0x00);
    append_varint(trailing_bytes, 0);
    trailing_bytes.push_back(0xff);
    expect(!storage.restore_witness_from_payload(witness_hash, trailing_bytes),
           "restore rejects witness payload with trailing bytes");

    auto signed_witness_tx = make_witness_tx();
    signed_witness_tx.inputs[0].signature_hash = hash_from_seed(0xed);
    const auto signed_witness_block = make_block(hash_from_seed(0xee), 3, {signed_witness_tx}, 54);
    save_block(storage, signed_witness_block, 3);
    auto signature_hash_mismatch = witness_payload_prefix(1);
    append_witness_hash_and_flags(signature_hash_mismatch, hash_from_seed(0xef), 0x00);
    append_varint(signature_hash_mismatch, 1);
    signature_hash_mismatch.push_back(0x01);
    expect(!storage.restore_witness_from_payload(signed_witness_block.get_hash(), signature_hash_mismatch),
           "restore rejects witness signature when stored signature_hash does not match");

    const auto no_witness_block = make_block(witness_hash, 4, {make_coinbase_tx(4)}, 55);
    save_block(storage, no_witness_block, 4);
    expect(storage.set_active_block_and_tip(4, no_witness_block.get_hash()),
           "set active no-witness block for prune guard");
    expect(storage.prune_witness_at_height(4),
           "prune_witness_at_height accepts block without material witness");
    geocoin::storage::BlockStorage::PruningStats stats;
    expect(storage.get_pruning_stats(stats) && stats.total_blocks > 0,
           "no-witness prune path accounts total block bytes");
}

void test_active_chain_transition_journal()
{
    using Access = geocoin::storage::BlockStorageTestAccess;
    using Kind = geocoin::storage::ActiveChainTransitionKind;
    using ReadResult = geocoin::storage::ActiveChainTransitionReadResult;

    TempDir dir("geocoin-block-storage-active-transition");
    geocoin::storage::BlockStorage storage(dir.path().string());
    expect(storage.is_open(), "active transition storage opens");

    geocoin::storage::ActiveChainTransitionRecord loaded;
    expect(storage.get_active_chain_transition(loaded) == ReadResult::NOT_FOUND,
           "fresh storage has no active transition journal");

    geocoin::storage::ActiveChainTransitionRecord invalid;
    invalid.kind = Kind::CONNECT;
    invalid.old_tip_exists = true;
    invalid.old_height = 7;
    invalid.new_height = 9;
    invalid.old_tip_hash = hash_from_seed(0x71);
    invalid.new_tip_hash = hash_from_seed(0x72);
    expect(!storage.begin_active_chain_transition(invalid),
           "non-adjacent connect transition is rejected");

    geocoin::storage::ActiveChainTransitionRecord transition;
    transition.kind = Kind::CONNECT;
    transition.old_tip_exists = true;
    transition.old_height = 7;
    transition.old_tip_hash = hash_from_seed(0x73);
    transition.new_height = 8;
    transition.new_tip_hash = hash_from_seed(0x74);
    expect(storage.begin_active_chain_transition(transition),
           "valid connect transition journal persists");
    expect(storage.get_active_chain_transition(loaded) == ReadResult::VALID,
           "valid connect transition journal loads");
    expect(loaded.kind == transition.kind &&
               loaded.old_tip_exists == transition.old_tip_exists &&
               loaded.old_height == transition.old_height &&
               loaded.old_tip_hash == transition.old_tip_hash &&
               loaded.new_height == transition.new_height &&
               loaded.new_tip_hash == transition.new_tip_hash,
           "active transition journal round-trips exactly");

    expect(storage.clear_active_chain_transition(),
           "active transition journal clears");
    expect(storage.get_active_chain_transition(loaded) == ReadResult::NOT_FOUND,
           "cleared active transition is absent");

    expect(Access::put(storage, Access::active_transition_key(storage), "corrupt"),
           "corrupt active transition fixture writes");
    expect(storage.get_active_chain_transition(loaded) == ReadResult::CORRUPT,
           "corrupt active transition journal is rejected");
    expect(storage.clear_active_chain_transition(),
           "corrupt active transition journal can be cleared explicitly");

    const auto hash0 = hash_from_seed(0x75);
    const auto hash1 = hash_from_seed(0x76);
    expect(storage.set_active_block_and_tip(0, hash0),
           "active transition fixture writes genesis active tip");
    expect(storage.set_active_block_and_tip(1, hash1),
           "active transition fixture writes child active tip");
    transition.old_height = 0;
    transition.old_tip_hash = hash0;
    transition.new_height = 1;
    transition.new_tip_hash = hash1;
    expect(storage.begin_active_chain_transition(transition),
           "active transition fixture journal persists");
    expect(storage.clear_active_chain(), "clear_active_chain succeeds atomically");

    geocoin::Hash256 active_hash{};
    geocoin::Height active_height = 0;
    expect(!storage.get_active_tip(active_hash, active_height),
           "clear_active_chain removes active tip");
    expect(!storage.get_active_block_hash(0, active_hash) &&
               !storage.get_active_block_hash(1, active_hash),
           "clear_active_chain removes active height entries");
    expect(storage.get_active_chain_transition(loaded) == ReadResult::NOT_FOUND,
           "clear_active_chain removes transition journal");
}

void test_split_write_pending_and_merge_iterator()
{
    TempDir dir("geocoin-block-storage-split-pending");
    geocoin::storage::BlockStorage storage(dir.path().string());
    expect(storage.is_open(), "BlockStorage opens for split-write pending test");
    expect(!storage.has_incomplete_split_write(), "fresh storage has no split-write pending");

    const auto genesis = make_block({}, 0, {make_coinbase_tx(0)}, 1);
    const auto child = make_block(genesis.get_hash(), 1, {make_coinbase_tx(1)}, 1);
    save_block(storage, genesis, 0);
    save_block(storage, child, 1);
    expect(!storage.has_incomplete_split_write(),
           "save_block dual write clears split-write pending");

    expect(storage.get_all_block_hashes().size() == 2,
           "merge iterator seek(b:) lists both bodies");
    expect(storage.get_children(genesis.get_hash()).size() == 1,
           "merge iterator seek(pc:) finds the child");
    expect(storage.get_all_block_index_records().size() == 2,
           "merge iterator seek(bi:) lists both index records");

    using Access = geocoin::storage::BlockStorageTestAccess;
    expect(Access::put(storage, "split_write_pending", "1"),
           "seed incomplete split-write marker");
    expect(storage.has_incomplete_split_write(),
           "split-write pending is visible while open");
    storage.close();

    geocoin::storage::BlockStorage reopened(dir.path().string());
    expect(reopened.is_open(), "BlockStorage reopens with split-write pending");
    expect(reopened.has_incomplete_split_write(),
           "split-write pending survives reopen");
}

} // namespace

int main(int argc, char** argv)
{
#ifndef _WIN32
    if (argc == 3 && std::string(argv[1]) == "--split-storage-crash-child") {
        run_split_storage_crash_child(argv[2]);
    }
#endif

    try {
        if (argc == 2) {
            const std::string mode = argv[1];
            if (mode == "--read-integrity") { test_stored_block_commitments(); return 0; }
            if (mode == "--witness-integrity") { test_restored_witness_commitments(); return 0; }
            if (mode == "--prune-errors") { test_pruning_read_errors(); return 0; }
            if (mode == "--body-stats-preflight") { test_pruning_stats_preflight(false); return 0; }
            if (mode == "--witness-stats-preflight") { test_pruning_stats_preflight(true); return 0; }
            if (mode == "--active-range") { test_active_range_maintenance(); return 0; }
            if (mode == "--index-pruning-integrity") { test_index_pruning_preflight(); return 0; }
            if (mode == "--ruleset-errors") { test_consensus_ruleset_read_errors(); return 0; }
            if (mode == "--maintenance-benchmark") { benchmark_active_range_maintenance(); return 0; }
        }
        test_active_range_maintenance();
        test_index_pruning_preflight();
        test_consensus_ruleset_read_errors();
        test_stored_block_commitments();
        test_restored_witness_commitments();
        test_pruning_read_errors();
        test_pruning_stats_preflight(false);
        test_pruning_stats_preflight(true);
        test_witness_range_progress();
        test_closed_storage_guards();
        test_open_empty_metadata_guards();
        test_open_indices_and_64bit_height_records();
        test_pruning_and_index_anchor();
        test_witness_payload_prune_restore();
        test_ibd_disable_wal_sync_transitions();
#ifndef _WIN32
        test_split_storage_survives_abrupt_process_exit(
            std::filesystem::absolute(argv[0]).string()
        );
#endif
        test_reopen_persists_split_metadata();
        test_current_format_read_errors();
        test_active_chain_transition_journal();
        test_split_write_pending_and_merge_iterator();
        test_validation_and_witness_guard_paths();
        std::cout << "Block storage unit tests passed\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "Block storage unit tests failed: " << ex.what() << "\n";
        return 1;
    }
}
