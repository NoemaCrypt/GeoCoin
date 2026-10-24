#include "geocoin/storage/block_storage.hpp"
#include "geocoin/consensus/geo_tag.hpp"
#include "geocoin/storage/block_records.hpp"
#include "rocksdb_store.hpp"
#include "block_storage_keys.hpp"
#include "geocoin/storage/read_status.hpp"
#include "geocoin/storage/witness_payload.hpp"
#include "geocoin/storage/backend_support.hpp"
#include "geocoin/core/merkle.hpp"
#include <filesystem>
#include <fstream>
#include <cstring>
#include <memory>
#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <limits>
#include <utility>
#include "geocoin/utils/varint.hpp"
#include "geocoin/utils/bytes.hpp"
#include "geocoin/consensus/consensus.hpp"
#include "geocoin/crypto/geo_area.hpp"
#include "geocoin/utils/logger.hpp"
#include "geocoin/utils/metrics.hpp"

namespace geocoin::storage {

namespace {
using namespace internal::block_keys;

static const char* backend_name(StorageBackend backend) {
    switch (backend) {
        case StorageBackend::ROCKSDB:
            return "rocksdb";
        default:
            return "unknown";
    }
}

static bool ensure_backend_marker(const std::string& db_path, StorageBackend backend) {
    return geocoin::storage::backend::ensure_backend_marker(db_path, backend_name(backend), "block storage");
}

static bool hash_from_hex_local(const std::string& hex, Hash256& out) {
    return utils::hex_to_hash256_checked(hex, out);
}

static std::string height_digits(Height height) {
    auto value = std::to_string(height);
    if (value.size() < 10) value.insert(0, 10 - value.size(), '0');
    return value;
}

static bool parse_height_digits(const std::string& value, Height& out) {
    if (value.empty()) {
        return false;
    }
    Height parsed = 0;
    for (unsigned char ch : value) {
        if (std::isdigit(ch) == 0) {
            return false;
        }
        const Height digit = static_cast<Height>(ch - '0');
        if (parsed > ((std::numeric_limits<Height>::max)() - digit) / 10) {
            return false;
        }
        parsed = parsed * 10 + digit;
    }
    out = parsed;
    return true;
}

static bool parse_height_hash_index_key(const std::string& key, Height& height, Hash256* hash = nullptr) {
    static constexpr const char* prefix = "hh:";
    if (key.rfind(prefix, 0) != 0) {
        return false;
    }
    const size_t height_begin = std::strlen(prefix);
    const size_t sep = key.find(':', height_begin);
    if (sep == std::string::npos) {
        return false;
    }
    if (!parse_height_digits(key.substr(height_begin, sep - height_begin), height)) {
        return false;
    }
    if (hash) {
        return hash_from_hex_local(key.substr(sep + 1), *hash);
    }
    return true;
}

static bool parse_height_key_with_prefix(const std::string& key, const char* prefix, Height& height) {
    if (key.rfind(prefix, 0) != 0) {
        return false;
    }
    return parse_height_digits(key.substr(std::strlen(prefix)), height);
}

static bool parse_parent_child_index_key(const std::string& key, Hash256& parent_hash, Hash256& child_hash) {
    static constexpr const char* prefix = "pc:";
    if (key.rfind(prefix, 0) != 0) {
        return false;
    }
    const size_t parent_begin = std::strlen(prefix);
    const size_t sep = key.find(':', parent_begin);
    if (sep == std::string::npos) {
        return false;
    }
    return hash_from_hex_local(key.substr(parent_begin, sep - parent_begin), parent_hash) &&
           hash_from_hex_local(key.substr(sep + 1), child_hash);
}

template <typename Visitor>
void for_each_index_key(const RocksDBStore& store, const char* prefix, Visitor&& visit,
                        const std::string& first_key = {}) {
    auto it = store.new_iterator();
    if (!it) throw ReadError(ReadStatus::IO_ERROR, "Cannot create block storage iterator");
    for (it->seek(first_key.empty() ? prefix : first_key); it->valid(); it->next()) {
        const auto key = it->key();
        if (key.rfind(prefix, 0) != 0) break;
        visit(key);
    }
}

void collect_active_deletions(const RocksDBStore& store, Height first_height, std::vector<KVWrite>& writes) {
    auto first_digits = height_digits(first_height);
    // Heights are padded to at least ten digits, not to a fixed width. A wider
    // height can sort before the lower bound, so include the next width bucket
    // and always filter numerically. Ordinary short ranges still start at Seek.
    if (first_digits.size() < height_digits((std::numeric_limits<Height>::max)()).size()) {
        first_digits = std::min(first_digits, "1" + std::string(first_digits.size(), '0'));
    }
    for_each_index_key(store, "a:", [&](const std::string& key) {
        Height height = 0;
        if (!parse_height_key_with_prefix(key, "a:", height) || key != "a:" + height_digits(height)) {
            throw ReadError(ReadStatus::CORRUPT, "Invalid active height key while clearing range");
        }
        if (height >= first_height) writes.push_back({KVWrite::Type::DELETE_KEY, key, {}});
    }, "a:" + first_digits);
}

static std::string witness_payload_key(const Hash256& block_hash) {
    return std::string(kWitnessPayloadPrefix) + utils::hash256_to_hex(block_hash);
}

static std::string witness_payload_hash_key(const Hash256& block_hash) {
    return std::string(kWitnessPayloadHashPrefix) + utils::hash256_to_hex(block_hash);
}

bool block_commitments_match(const core::Block& block) {
    std::vector<Hash256> txids;
    txids.reserve(block.transactions.size());
    for (const auto& tx : block.transactions) txids.push_back(tx.get_hash());
    // Block::calculate_merkle_root changes the header. Verification must retain
    // the stored header and compare against independently computed values.
    return core::compute_merkle_root(txids) == block.header.merkle_root &&
           block.calculate_witness_commitment() == block.header.witness_commitment;
}

void verify_stored_block(const BlockStorage& storage, const Hash256& expected_hash, core::Block& block) {
    if (block.get_hash() != expected_hash) {
        throw ReadError(ReadStatus::CORRUPT, "Stored block hash mismatch");
    }
    if (std::none_of(block.transactions.begin(), block.transactions.end(), has_material_witness)) {
        bytes payload;
        if (storage.get_witness_payload(expected_hash, payload) && !apply_witness_payload(block, payload)) {
            throw ReadError(ReadStatus::CORRUPT, "Invalid retained witness payload");
        }
    }
    if (!block_commitments_match(block)) {
        throw ReadError(ReadStatus::CORRUPT, "Stored block transaction or witness commitment mismatch");
    }
}

static bytes serialize_prune_stats(const BlockStorage::PruningStats& stats) {
    bytes data;
    data.resize(8 * 4);
    utils::write_u64_le(data, 0, stats.total_blocks);
    utils::write_u64_le(data, 8, stats.pruned_blocks);
    utils::write_u64_le(data, 16, stats.body_bytes_reduced);
    utils::write_u64_le(data, 24, stats.bytes_total);
    return data;
}

static KVWrite pruning_stats_write(const BlockStorage::PruningStats& stats) {
    const auto data = serialize_prune_stats(stats);
    return {KVWrite::Type::PUT, kPruneStatsKey,
            std::string(reinterpret_cast<const char*>(data.data()), data.size())};
}

static bool deserialize_prune_stats(const bytes& data, BlockStorage::PruningStats& out) {
    if (data.size() != 32) {
        return false;
    }
    out.total_blocks = utils::read_u64_le_unchecked(data, 0);
    out.pruned_blocks = utils::read_u64_le_unchecked(data, 8);
    out.body_bytes_reduced = utils::read_u64_le_unchecked(data, 16);
    out.bytes_total = utils::read_u64_le_unchecked(data, 24);
    return true;
}

static std::string serialize_height_hash_record(Height height, const Hash256& hash) {
    bytes data(8 + 32);
    for (int i = 0; i < 8; ++i) {
        data[static_cast<size_t>(i)] =
            static_cast<uint8_t>((static_cast<uint64_t>(height) >> (i * 8)) & 0xFF);
    }
    std::memcpy(data.data() + 8, hash.data(), 32);
    return std::string(reinterpret_cast<const char*>(data.data()), data.size());
}

static bool deserialize_height_hash_record(const std::string& value, Height& height, Hash256& hash) {
    if (value.size() == 8 + 32) {
        height = 0;
        for (int i = 0; i < 8; ++i) {
            height |= static_cast<Height>(static_cast<uint8_t>(value[static_cast<size_t>(i)])) << (i * 8);
        }
        std::memcpy(hash.data(), value.data() + 8, 32);
        return true;
    }

    return false;
}

static bool valid_active_transition(const ActiveChainTransitionRecord& transition) {
    if (transition.kind == ActiveChainTransitionKind::CONNECT) {
        if (!transition.old_tip_exists) {
            return transition.new_height == 0;
        }
        return transition.old_height != (std::numeric_limits<Height>::max)() &&
            transition.new_height == transition.old_height + 1;
    }
    if (transition.kind == ActiveChainTransitionKind::DISCONNECT) {
        return transition.old_tip_exists && transition.old_height > 0 &&
            transition.new_height + 1 == transition.old_height;
    }
    return false;
}

static std::string serialize_active_transition(
    const ActiveChainTransitionRecord& transition
) {
    bytes data;
    data.reserve(4 + 2 + 8 + 32 + 8 + 32);
    data.insert(data.end(), kActiveTransitionMagic, kActiveTransitionMagic + 4);
    data.push_back(static_cast<byte>(transition.kind));
    data.push_back(transition.old_tip_exists ? 1 : 0);
    for (int i = 0; i < 8; ++i) {
        data.push_back(static_cast<byte>((transition.old_height >> (i * 8)) & 0xFF));
    }
    data.insert(data.end(), transition.old_tip_hash.begin(), transition.old_tip_hash.end());
    for (int i = 0; i < 8; ++i) {
        data.push_back(static_cast<byte>((transition.new_height >> (i * 8)) & 0xFF));
    }
    data.insert(data.end(), transition.new_tip_hash.begin(), transition.new_tip_hash.end());
    return std::string(reinterpret_cast<const char*>(data.data()), data.size());
}

static bool deserialize_active_transition(
    const std::string& value,
    ActiveChainTransitionRecord& transition
) {
    constexpr size_t kSerializedSize = 4 + 2 + 8 + 32 + 8 + 32;
    if (value.size() != kSerializedSize ||
        std::memcmp(value.data(), kActiveTransitionMagic, 4) != 0) {
        return false;
    }

    const uint8_t kind = static_cast<uint8_t>(value[4]);
    if (kind != static_cast<uint8_t>(ActiveChainTransitionKind::CONNECT) &&
        kind != static_cast<uint8_t>(ActiveChainTransitionKind::DISCONNECT)) {
        return false;
    }
    const uint8_t flags = static_cast<uint8_t>(value[5]);
    if ((flags & 0xFEu) != 0) {
        return false;
    }

    transition = {};
    transition.kind = static_cast<ActiveChainTransitionKind>(kind);
    transition.old_tip_exists = (flags & 0x01u) != 0;
    size_t offset = 6;
    for (int i = 0; i < 8; ++i) {
        transition.old_height |=
            static_cast<Height>(static_cast<uint8_t>(value[offset++])) << (i * 8);
    }
    std::memcpy(transition.old_tip_hash.data(), value.data() + offset, 32);
    offset += 32;
    for (int i = 0; i < 8; ++i) {
        transition.new_height |=
            static_cast<Height>(static_cast<uint8_t>(value[offset++])) << (i * 8);
    }
    std::memcpy(transition.new_tip_hash.data(), value.data() + offset, 32);
    return valid_active_transition(transition);
}

static bool deserialize_block_index_record(
    const Hash256& hash,
    const std::string& value,
    BlockStorage::BlockIndexRecord& out
) {
    Height height = 0;
    core::BlockHeader header;
    if (!decode_block_index_record(hash, value, height, header)) return false;
    out.hash = hash;
    out.header = header;
    out.height = height;
    out.has_body = false;
    return true;
}


}

BlockStorage::BlockStorage()
{
}

BlockStorage::BlockStorage(
    const std::string& path,
    StorageBackend backend,
    StorageOptions options
)
{
    open(path, backend, options);
}

BlockStorage::~BlockStorage() {
    close();
}

bool BlockStorage::open(
    const std::string& path,
    StorageBackend backend,
    StorageOptions options
) {
    if (chain_db_ || index_db_) {
        return false;
    }

    path_ = path;
    backend_ = backend;
    options_ = options;

    chaindata_path_ = (std::filesystem::path(path_) / "chaindata" / "blocks").string();
    index_path_ = (std::filesystem::path(path_) / "index" / "blocks").string();

    std::filesystem::create_directories(chaindata_path_);
    std::filesystem::create_directories(index_path_);

    if (!ensure_backend_marker(chaindata_path_, backend) ||
        !ensure_backend_marker(index_path_, backend)) {
        return false;
    }

    if (backend != StorageBackend::ROCKSDB) return false;
    chain_db_ = std::make_unique<RocksDBStore>(BlockStoreRole::CHAIN_DATA);
    index_db_ = std::make_unique<RocksDBStore>(BlockStoreRole::INDEX);

    if (!chain_db_->open(chaindata_path_, options_) ||
        !index_db_->open(index_path_, options_)) {
        close();
        return false;
    }

    try {
        std::string pending;
        split_write_failed_ = index_db_->get(kSplitWritePendingKey, pending);
    } catch (...) {
        close();
        throw;
    }

    GEO_LOG_INFO(
        "storage",
        "Opened block storage backend=" << backend_name(backend_)
            << " chaindata=" << chaindata_path_
            << " index=" << index_path_
    );
    return true;
}

void BlockStorage::close() {
    if (chain_db_) {
        chain_db_.reset();
    }
    if (index_db_) {
        index_db_.reset();
    }
    split_write_failed_ = false;
}

bool BlockStorage::is_open() const {
    return chain_db_ != nullptr && index_db_ != nullptr;
}

bool BlockStorage::has_incomplete_split_write() const {
    std::lock_guard<std::recursive_mutex> lock(db_write_mutex_);
    if (!is_open()) {
        return false;
    }
    if (split_write_failed_) return true;
    std::string value;
    return db_get(kSplitWritePendingKey, value);
}

bool BlockStorage::next_ibd_sync_write() {
    bool sync_write = true;
    std::lock_guard<std::mutex> lock(write_mode_mutex_);
    if (ibd_mode_) {
        ++ibd_pending_writes_;
        sync_write = (ibd_pending_writes_ >= ibd_flush_interval_) || force_sync_next_write_;
        if (sync_write) {
            ibd_pending_writes_ = 0;
            force_sync_next_write_ = false;
        }
    } else {
        sync_write = true;
        force_sync_next_write_ = false;
        ibd_pending_writes_ = 0;
    }
    return sync_write;
}

bool BlockStorage::is_chaindata_key(const std::string& key) const {
    return key.rfind("b:", 0) == 0 ||
           key.rfind(kWitnessPayloadPrefix, 0) == 0 ||
           key.rfind(kWitnessPayloadHashPrefix, 0) == 0;
}

RocksDBStore* BlockStorage::db_for_key(const std::string& key) const {
    if (is_chaindata_key(key)) {
        return chain_db_.get();
    }
    return index_db_.get();
}

bool BlockStorage::db_get(const std::string& key, std::string& value) const {
    RocksDBStore* store = db_for_key(key);
    if (!store) throw ReadError(ReadStatus::IO_ERROR, "Block storage is closed");
    return store->get(key, value);
}

bool BlockStorage::db_put(const std::string& key, const std::string& value, bool sync) {
    std::lock_guard<std::recursive_mutex> lock(db_write_mutex_);
    if (split_write_failed_) return false;
    const bool disable_wal = disable_wal_for_write();
    RocksDBStore* store = db_for_key(key);
    const bool ok = store && store->put(key, value, sync, disable_wal);
    if (ok && key == kSplitWritePendingKey) split_write_failed_ = true;
    return ok;
}

bool BlockStorage::db_delete(const std::string& key, bool sync) {
    std::lock_guard<std::recursive_mutex> lock(db_write_mutex_);
    if (split_write_failed_) return false;
    const bool disable_wal = disable_wal_for_write();
    RocksDBStore* store = db_for_key(key);
    return store && store->delete_key(key, sync, disable_wal);
}

bool BlockStorage::db_write_batch(std::vector<KVWrite> writes, bool sync) {
    std::lock_guard<std::recursive_mutex> lock(db_write_mutex_);
    if (!chain_db_ || !index_db_ || split_write_failed_) {
        return false;
    }
    const bool disable_wal = disable_wal_for_write();

    std::vector<KVWrite> chain_writes;
    std::vector<KVWrite> index_writes;
    for (auto& write : writes) {
        if (is_chaindata_key(write.key)) {
            chain_writes.push_back(std::move(write));
        } else {
            index_writes.push_back(std::move(write));
        }
    }

    if (chain_writes.empty()) {
        return index_writes.empty() ||
               index_db_->write_batch(index_writes, sync, disable_wal);
    }
    if (index_writes.empty()) {
        return chain_db_->write_batch(chain_writes, sync, disable_wal);
    }

    // Publish durable intent before either payload changes. Keep all writers
    // serialized until the index commit removes that intent atomically.
    // An uncertain failure remains sticky in this instance, even if the
    // attempted marker write itself returned an error.
    index_writes.push_back({KVWrite::Type::DELETE_KEY, kSplitWritePendingKey, {}});
    split_write_failed_ = true;
    if (!index_db_->put(kSplitWritePendingKey, "1", true, false)) {
        return false;
    }
    // A durable marker deletion cannot acknowledge unlogged payloads: each
    // physical store must finish durably, including during initial sync.
    if (!chain_db_->write_batch(chain_writes, true, false)) {
        return false;
    }
    if (!index_db_->write_batch(index_writes, true, false)) {
        return false;
    }
    split_write_failed_ = false;
    return true;
}

bool BlockStorage::disable_wal_for_write() const {
    std::lock_guard<std::mutex> lock(write_mode_mutex_);
    return options_.disable_wal_during_ibd && ibd_mode_;
}

bool BlockStorage::save_block(const core::Block& block, Height height, bool enforce_validation) {
    int32_t file;
    uint32_t pos;
    return save_block_with_pos(block, height, file, pos, enforce_validation);
}

void BlockStorage::set_ibd_mode(bool enabled) {
    std::lock_guard<std::mutex> lock(write_mode_mutex_);
    if (ibd_mode_ == enabled) {
        return;
    }

    ibd_mode_ = enabled;
    if (!ibd_mode_ && ibd_pending_writes_ > 0) {
        // При выходе из IBD гарантируем ближайший sync-запрос.
        force_sync_next_write_ = true;
    }
}

void BlockStorage::set_ibd_flush_interval(size_t blocks) {
    std::lock_guard<std::mutex> lock(write_mode_mutex_);
    ibd_flush_interval_ = (blocks == 0) ? 1 : blocks;
}

bool BlockStorage::save_block_with_pos(
    const core::Block& block,
    Height height,
    int32_t& file,
    uint32_t& pos,
    bool enforce_validation
) {
    if (!is_open()) {
        return false;
    }

    if (enforce_validation && !block.verify()) {
        return false;
    }

    // Сериализуем блок
    bytes data = block.serialize();

    Hash256 block_hash = block.get_hash();

    // Для KV-хранилища возвращаем виртуальную позицию для совместимости с BlockIndex.
    file = 0;
    pos = static_cast<uint32_t>(height);

    // Создаем batch для атомарной записи
    std::vector<KVWrite> batch;

    // Сохраняем блок по хешу
    std::string key = block_key(block_hash);
    batch.push_back({
        KVWrite::Type::PUT,
        key,
        std::string(reinterpret_cast<const char*>(data.data()), data.size())
    });

    const bytes index_record = encode_block_index_record(block.header, height);
    batch.push_back({
        KVWrite::Type::PUT,
        block_index_key(block_hash),
        std::string(reinterpret_cast<const char*>(index_record.data()), index_record.size())
    });

    // Форк-совместимый индекс: добавляем запись "на высоте есть такой блок".
    // Ключ уникален (height + hash), поэтому не перетирается форками.
    batch.push_back({KVWrite::Type::PUT, height_hash_key(height, block_hash), {}});

    // Индекс parent -> child (для быстрого ветвления)
    if (block.header.prev_block_hash != Hash256{}) {
        batch.push_back({KVWrite::Type::PUT, parent_child_key(block.header.prev_block_hash, block_hash), {}});
    }

    // ВАЖНО:
    // - Мы НЕ обновляем здесь индекс активной цепи.
    // - Активная цепь выбирается в Blockchain по chainwork и фиксируется отдельным ключом.

    // Keep the best-height comparison and its write in the same critical
    // section; a concurrent lower block must not overwrite a newer best.
    std::lock_guard<std::recursive_mutex> lock(db_write_mutex_);
    if (split_write_failed_) return false;
    Height current_best = get_best_height();
    if (height > current_best) {
        std::string best_k = best_block_key();

        batch.push_back({
            KVWrite::Type::PUT,
            best_k,
            serialize_height_hash_record(height, block_hash)
        });
    }

    return db_write_batch(std::move(batch), next_ibd_sync_write());
}

bool BlockStorage::get_block(const Hash256& block_hash, core::Block& block, bool verify_integrity) const {
    if (!is_open()) throw ReadError(ReadStatus::IO_ERROR, "Block storage is closed");

    std::string key = block_key(block_hash);
    std::string value;

    if (!db_get(key, value)) {
        return false;
    }

    // Десериализуем блок
    bytes data(value.begin(), value.end());

    try {
        block = core::Block::deserialize(data);
    } catch (const std::exception& error) {
        throw ReadError(ReadStatus::CORRUPT, error.what());
    }

    if (verify_integrity) verify_stored_block(*this, block_hash, block);

    return true;
}

std::vector<Hash256> BlockStorage::get_children(const Hash256& parent_hash) const {
    std::vector<Hash256> result;
    if (!is_open()) throw ReadError(ReadStatus::IO_ERROR, "Block storage is closed");

    const std::string p = "pc:" + utils::hash256_to_hex(parent_hash) + ":";

    auto it = index_db_->new_iterator();
    if (!it) throw ReadError(ReadStatus::IO_ERROR, "Cannot create block storage iterator");
    for (it->seek(p); it->valid(); it->next()) {
        const std::string key = it->key();
        if (key.rfind(p, 0) != 0) {
            break;
        }

        const std::string hash_hex = key.substr(p.size());
        if (hash_hex.size() != 64) {
            throw ReadError(ReadStatus::CORRUPT, "Invalid block index key or record");
        }

        Hash256 hash;
        if (!hash_from_hex_local(hash_hex, hash)) {
            throw ReadError(ReadStatus::CORRUPT, "Invalid block index key or record");
        }
        result.push_back(hash);
    }

    return result;
}

bool BlockStorage::get_block_by_height(Height height, core::Block& block, bool verify_integrity) const {
    if (!is_open()) throw ReadError(ReadStatus::IO_ERROR, "Block storage is closed");

    // Основной путь: читаем индекс активной цепи.
    Hash256 active_hash;
    if (get_active_block_hash(height, active_hash)) {
        return get_block(active_hash, block, verify_integrity);
    }

    return false;
}

std::vector<Hash256> BlockStorage::get_block_hashes_by_height(Height height) const {
    std::vector<Hash256> result;
    if (!is_open()) throw ReadError(ReadStatus::IO_ERROR, "Block storage is closed");

    // Итерируем ключи вида hh:<height>:<hashhex>
    const std::string p = "hh:" + height_digits(height) + ":";

    auto it = index_db_->new_iterator();
    if (!it) throw ReadError(ReadStatus::IO_ERROR, "Cannot create block storage iterator");
    for (it->seek(p); it->valid(); it->next()) {
        const std::string key = it->key();
        if (key.rfind(p, 0) != 0) {
            break;
        }

        const std::string hash_hex = key.substr(p.size());
        if (hash_hex.size() != 64) {
            throw ReadError(ReadStatus::CORRUPT, "Invalid block index key or record");
        }

        Hash256 hash;
        if (!hash_from_hex_local(hash_hex, hash)) {
            throw ReadError(ReadStatus::CORRUPT, "Invalid block index key or record");
        }
        result.push_back(hash);
    }

    return result;
}

bool BlockStorage::set_active_block(Height height, const Hash256& block_hash) {
    if (!is_open()) {
        return false;
    }

    std::string value(reinterpret_cast<const char*>(block_hash.data()), block_hash.size());
    return db_put(active_height_key(height), value, next_ibd_sync_write());
}

bool BlockStorage::get_active_block_hash(Height height, Hash256& block_hash) const {
    if (!is_open()) throw ReadError(ReadStatus::IO_ERROR, "Block storage is closed");

    std::string value;
    if (!db_get(active_height_key(height), value)) return false;
    if (value.size() != 32) throw ReadError(ReadStatus::CORRUPT, "Invalid active height record");

    std::memcpy(block_hash.data(), value.data(), 32);
    return true;
}

bool BlockStorage::get_active_tip(Hash256& block_hash, Height& height) const {
    if (!is_open()) throw ReadError(ReadStatus::IO_ERROR, "Block storage is closed");

    std::string value;
    if (!db_get(active_tip_key(), value)) {
        return false;
    }

    if (!deserialize_height_hash_record(value, height, block_hash))
        throw ReadError(ReadStatus::CORRUPT, "Invalid height/hash record");
    return true;
}

bool BlockStorage::set_active_tip(const Hash256& block_hash, Height height) {
    if (!is_open()) {
        return false;
    }

    return db_put(
        active_tip_key(),
        serialize_height_hash_record(height, block_hash),
        next_ibd_sync_write()
    );
}

bool BlockStorage::set_consensus_ruleset_id(const std::string& ruleset_id) {
    if (!is_open() || ruleset_id.empty() || ruleset_id.size() > 128) {
        return false;
    }
    return db_put(consensus_ruleset_key(), ruleset_id, true);
}

bool BlockStorage::get_consensus_ruleset_id(std::string& ruleset_id) const {
    ruleset_id.clear();
    if (!is_open()) throw ReadError(ReadStatus::IO_ERROR, "Block storage is closed");
    std::string stored;
    if (!db_get(consensus_ruleset_key(), stored)) return false;
    if (stored.empty() || stored.size() > 128) {
        throw ReadError(ReadStatus::CORRUPT, "Invalid consensus ruleset record");
    }
    ruleset_id = std::move(stored);
    return true;
}

bool BlockStorage::set_active_block_and_tip(Height height, const Hash256& block_hash) {
    if (!is_open()) {
        return false;
    }

    std::vector<KVWrite> batch;
    batch.push_back({
        KVWrite::Type::PUT,
        active_height_key(height),
        std::string(reinterpret_cast<const char*>(block_hash.data()), block_hash.size())
    });
    batch.push_back({
        KVWrite::Type::PUT,
        active_tip_key(),
        serialize_height_hash_record(height, block_hash)
    });
    if (!db_write_batch(std::move(batch), true)) {
        return false;
    }
    GEO_METRIC_INC("storage.active_index_entries_written_total", 1);
    return true;
}

bool BlockStorage::begin_active_chain_transition(
    const ActiveChainTransitionRecord& transition
) {
    if (!is_open() || !valid_active_transition(transition)) {
        return false;
    }
    return db_put(
        active_transition_key(),
        serialize_active_transition(transition),
        true
    );
}

ActiveChainTransitionReadResult BlockStorage::get_active_chain_transition(
    ActiveChainTransitionRecord& transition
) const {
    transition = {};
    if (!is_open()) {
        return ActiveChainTransitionReadResult::IO_ERROR;
    }

    std::string value;
    try {
        if (!db_get(active_transition_key(), value)) return ActiveChainTransitionReadResult::NOT_FOUND;
    } catch (const ReadError& error) {
        return error.status() == ReadStatus::CORRUPT ? ActiveChainTransitionReadResult::CORRUPT
                                                    : ActiveChainTransitionReadResult::IO_ERROR;
    }
    if (!deserialize_active_transition(value, transition)) {
        return ActiveChainTransitionReadResult::CORRUPT;
    }
    return ActiveChainTransitionReadResult::VALID;
}

bool BlockStorage::clear_active_chain_transition() {
    if (!is_open() || !chain_db_->sync() || !index_db_->sync()) {
        return false;
    }
    return db_delete(active_transition_key(), true);
}

bool BlockStorage::clear_active_chain() {
    if (!is_open() || !index_db_) {
        return false;
    }

    std::vector<KVWrite> batch;
    auto it = index_db_->new_iterator();
    if (!it) {
        return false;
    }
    for (it->seek("a:"); it->valid(); it->next()) {
        const std::string key = it->key();
        if (key.rfind("a:", 0) != 0) {
            break;
        }
        batch.push_back({KVWrite::Type::DELETE_KEY, key, {}});
    }
    batch.push_back({KVWrite::Type::DELETE_KEY, active_tip_key(), {}});
    batch.push_back({KVWrite::Type::DELETE_KEY, active_transition_key(), {}});
    return db_write_batch(std::move(batch), true);
}

bool BlockStorage::truncate_active_chain_and_set_tip(
    Height first_removed_height,
    const Hash256& tip_hash,
    Height tip_height
) {
    if (!is_open()) {
        return false;
    }

    std::vector<KVWrite> batch;
    collect_active_deletions(*index_db_, first_removed_height, batch);
    batch.push_back({
        KVWrite::Type::PUT,
        active_tip_key(),
        serialize_height_hash_record(tip_height, tip_hash)
    });

    return db_write_batch(std::move(batch), true);
}

bool BlockStorage::replace_active_chain(
    const std::vector<std::pair<Height, Hash256>>& active_blocks,
    const Hash256& tip_hash,
    Height tip_height
) {
    if (!is_open()) {
        return false;
    }

    std::set<Height> keep;
    for (const auto& [height, hash] : active_blocks) {
        (void)hash;
        keep.insert(height);
    }

    auto it = index_db_->new_iterator();
    if (!it) {
        return false;
    }

    std::vector<KVWrite> batch;
    for (it->seek("a:"); it->valid(); it->next()) {
        const std::string key = it->key();
        if (key.rfind("a:", 0) != 0) {
            break;
        }
        Height height = 0;
        if (parse_height_key_with_prefix(key, "a:", height) && keep.count(height) != 0) {
            continue;
        }
        batch.push_back({KVWrite::Type::DELETE_KEY, key, {}});
    }

    for (const auto& [height, hash] : active_blocks) {
        batch.push_back({
            KVWrite::Type::PUT,
            active_height_key(height),
            std::string(reinterpret_cast<const char*>(hash.data()), hash.size())
        });
    }
    batch.push_back({
        KVWrite::Type::PUT,
        active_tip_key(),
        serialize_height_hash_record(tip_height, tip_hash)
    });

    if (!db_write_batch(std::move(batch), true)) {
        return false;
    }
    GEO_METRIC_INC("storage.active_index_entries_written_total", active_blocks.size());
    GEO_METRIC_INC("storage.active_index_rebuilds_total", 1);
    return true;
}

bool BlockStorage::set_policy_checkpoint(const Hash256& block_hash, Height height) {
    if (!is_open()) {
        return false;
    }

    return db_put(
        policy_checkpoint_key(),
        serialize_height_hash_record(height, block_hash),
        true
    );
}

bool BlockStorage::get_policy_checkpoint(Hash256& block_hash, Height& height) const {
    if (!is_open()) throw ReadError(ReadStatus::IO_ERROR, "Block storage is closed");

    std::string value;
    if (!db_get(policy_checkpoint_key(), value)) {
        return false;
    }

    if (!deserialize_height_hash_record(value, height, block_hash))
        throw ReadError(ReadStatus::CORRUPT, "Invalid height/hash record");
    return true;
}

bool BlockStorage::clear_active_from_height(Height start_height) {
    if (!is_open()) {
        return false;
    }

    std::vector<KVWrite> batch;
    collect_active_deletions(*index_db_, start_height, batch);
    if (batch.empty()) return true;
    return db_write_batch(std::move(batch), true);
}

bool BlockStorage::get_block_geography(Height height, BlockGeographyRecord& out) const {
    out = {};
    if (!is_open()) throw ReadError(ReadStatus::IO_ERROR, "Block storage is closed");
    Hash256 tip; Height tip_height = 0;
    if (!get_active_tip(tip, tip_height) || height > tip_height) return false;
    std::string raw;
    Hash256 hash{};
    if (get_active_block_hash(height, hash)) {
        BlockIndexRecord indexed;
        if (!db_get(block_index_key(hash), raw) || !deserialize_block_index_record(hash, raw, indexed) ||
            indexed.height != height || !consensus::geo::decode_geo_tag(indexed.header.geo_tag))
            throw ReadError(ReadStatus::CORRUPT, "Invalid active geography header");
        out = {height, hash, indexed.header.geo_tag, false};
        return true;
    }
    PrunedIndexAnchor anchor;
    if (!get_pruned_index_anchor(anchor)) return false;
    if (height == anchor.height) {
        if (!consensus::geo::decode_geo_tag(anchor.header.geo_tag))
            throw ReadError(ReadStatus::CORRUPT, "Invalid pruning anchor geography");
        out = {height, anchor.hash, anchor.header.geo_tag, false};
        return true;
    }
    if (height > anchor.height) return false;
    if (!db_get("geo:" + height_digits(height), raw))
        throw ReadError(ReadStatus::CORRUPT, "Missing archived block geography");
    uint64_t tag = 0;
    if (height >= anchor.height || !decode_block_geography_record(raw, hash, tag))
        throw ReadError(ReadStatus::CORRUPT, "Invalid archived block geography");
    out = {height, hash, tag, true};
    return true;
}

bool BlockStorage::is_block_body_pruned(Height height) const {
    if (!is_open()) throw ReadError(ReadStatus::IO_ERROR, "Block storage is closed");
    PrunedIndexAnchor anchor;
    if (get_pruned_index_anchor(anchor) && height < anchor.height) return true;
    Hash256 hash{};
    std::string value;
    BlockIndexRecord record;
    if (!get_active_block_hash(height, hash)) {
        if (height == anchor.height && anchor.hash != Hash256{}) return !has_block(anchor.hash);
        return false;
    }
    if (!db_get(block_index_key(hash), value) ||
        !deserialize_block_index_record(hash, value, record) || record.height != height) {
        throw ReadError(ReadStatus::CORRUPT, "Invalid active block index while checking pruning status");
    }
    return !has_block(hash);
}

bool BlockStorage::has_block(const Hash256& block_hash) const {
    if (!is_open()) throw ReadError(ReadStatus::IO_ERROR, "Block storage is closed");

    std::string key = block_key(block_hash);
    std::string value;

    return db_get(key, value);
}

bool BlockStorage::get_best_block(core::Block& block, bool verify_integrity) const {
    if (!is_open()) throw ReadError(ReadStatus::IO_ERROR, "Block storage is closed");

    std::string key = best_block_key();
    std::string value;

    if (!db_get(key, value)) {
        return false;
    }

    Hash256 block_hash;
    Height height = 0;
    if (!deserialize_height_hash_record(value, height, block_hash))
        throw ReadError(ReadStatus::CORRUPT, "Invalid best block record");

    return get_block(block_hash, block, verify_integrity);
}

Height BlockStorage::get_best_height() const {
    if (!is_open()) throw ReadError(ReadStatus::IO_ERROR, "Block storage is closed");

    std::string key = best_block_key();
    std::string value;

    if (!db_get(key, value)) {
        return 0;
    }

    Height height = 0;
    Hash256 block_hash{};
    if (!deserialize_height_hash_record(value, height, block_hash))
        throw ReadError(ReadStatus::CORRUPT, "Invalid best block record");
    return height;
}

bool BlockStorage::set_best_block(const Hash256& block_hash, Height height) {
    if (!is_open()) {
        return false;
    }

    std::string key = best_block_key();

    return db_put(
        key,
        serialize_height_hash_record(height, block_hash),
        next_ibd_sync_write()
    );
}

bool BlockStorage::delete_block(const Hash256& block_hash) {
    if (!is_open()) {
        return false;
    }

    std::string key = block_key(block_hash);
    std::vector<KVWrite> batch;
    batch.push_back({KVWrite::Type::DELETE_KEY, key, {}});
    batch.push_back({KVWrite::Type::DELETE_KEY, witness_payload_key(block_hash), {}});
    batch.push_back({KVWrite::Type::DELETE_KEY, witness_payload_hash_key(block_hash), {}});

    return db_write_batch(std::move(batch), false);
}

bool BlockStorage::prune_block_body_at_height(Height height, Hash256* pruned_hash) {
    if (!is_open()) {
        return false;
    }

    Hash256 active_hash;
    if (!get_active_block_hash(height, active_hash)) {
        PrunedIndexAnchor anchor;
        if (!get_pruned_index_anchor(anchor) || anchor.height != height) return true;
        active_hash = anchor.hash;
    }

    core::Block block;
    if (!get_block(active_hash, block, false)) {
        return true;
    }

    if (block.get_hash() != active_hash) {
        GEO_LOG_ERROR("storage", "Refusing to prune corrupted block body at height " << height);
        return false;
    }

    // Count physical body bytes before witness rehydration. Validate the entire
    // body before invoking an archiver or scheduling any deletion.
    const size_t body_size = block.serialize().size();
    verify_stored_block(*this, active_hash, block);
    PruningStats stats;
    get_pruning_stats(stats);
    if (transaction_archiver_ && !transaction_archiver_(block, height)) return false;

    const bytes index_record = encode_block_index_record(block.header, height);

    std::vector<KVWrite> batch;
    batch.push_back({
        KVWrite::Type::PUT,
        block_index_key(active_hash),
        std::string(reinterpret_cast<const char*>(index_record.data()), index_record.size())
    });
    batch.push_back({KVWrite::Type::DELETE_KEY, block_key(active_hash), {}});
    batch.push_back({KVWrite::Type::DELETE_KEY, witness_payload_key(active_hash), {}});
    batch.push_back({KVWrite::Type::DELETE_KEY, witness_payload_hash_key(active_hash), {}});

    ++stats.total_blocks;
    ++stats.pruned_blocks;
    stats.body_bytes_reduced += body_size;
    stats.bytes_total += body_size;
    batch.push_back(pruning_stats_write(stats));

    const bool ok = db_write_batch(std::move(batch), next_ibd_sync_write());
    if (!ok) {
        return false;
    }

    if (pruned_hash) {
        *pruned_hash = active_hash;
    }

    return true;
}

Height BlockStorage::block_body_prune_next_height() const {
    Height next_height = 0;
    std::string value;
    if (db_get(kBlockBodyPruneNextHeightKey, value) &&
        !utils::decode_height_le_exact(bytes(value.begin(), value.end()), next_height)) {
        throw ReadError(ReadStatus::CORRUPT, "Invalid block-body pruning cursor");
    }
    return next_height;
}

bool BlockStorage::prune_block_bodies_below_height(
    Height keep_from_height,
    size_t max_blocks,
    uint64_t* pruned_count
) {
    if (pruned_count) {
        *pruned_count = 0;
    }
    if (!is_open() || keep_from_height == 0) {
        return is_open();
    }

    const Height next_height = block_body_prune_next_height();

    if (next_height >= keep_from_height) {
        return true;
    }

    uint64_t pruned = 0;
    Height h = next_height;
    for (; h < keep_from_height; ++h) {
        Hash256 active_hash;
        if (get_active_block_hash(h, active_hash) && has_block(active_hash)) {
            if (!prune_block_body_at_height(h, nullptr)) {
                return false;
            }
            ++pruned;
            if (max_blocks > 0 && pruned >= max_blocks) {
                ++h;
                break;
            }
        }
    }

    const bytes next_data = utils::encode_height_le(h);
    if (!db_put(
            kBlockBodyPruneNextHeightKey,
            std::string(reinterpret_cast<const char*>(next_data.data()), next_data.size()),
            false
        )) {
        return false;
    }

    if (pruned_count) {
        *pruned_count = pruned;
    }
    return true;
}

bool BlockStorage::prune_block_indexes_below_height(
    Height keep_from_height,
    const Hash256& anchor_hash,
    const core::BlockHeader& anchor_header,
    const uint256& anchor_chain_work,
    uint64_t* pruned_count,
    uint64_t* discarded_body_count
) {
    if (pruned_count) {
        *pruned_count = 0;
    }
    if (discarded_body_count) *discarded_body_count = 0;
    if (!is_open()) {
        return false;
    }
    if (keep_from_height == 0) {
        return true;
    }

    const Height anchor_height = keep_from_height - 1;
    if (anchor_header.get_hash() != anchor_hash) {
        GEO_LOG_ERROR("storage", "Refusing to prune indexes with mismatched pruned anchor hash");
        return false;
    }

    const bytes anchor_record = encode_block_index_record(anchor_header, anchor_height);
    const bytes anchor_chain_work_data = encode_chain_work_record(anchor_chain_work);
    const bytes anchor_payload = encode_pruned_index_anchor_record(anchor_height, anchor_hash, anchor_chain_work);

    std::vector<KVWrite> batch;
    batch.push_back({
        KVWrite::Type::PUT,
        block_index_key(anchor_hash),
        std::string(reinterpret_cast<const char*>(anchor_record.data()), anchor_record.size())
    });
    batch.push_back({
        KVWrite::Type::PUT,
        chainwork_key(anchor_hash),
        std::string(reinterpret_cast<const char*>(anchor_chain_work_data.data()), anchor_chain_work_data.size())
    });
    batch.push_back({
        KVWrite::Type::PUT,
        pruned_index_anchor_key(),
        std::string(reinterpret_cast<const char*>(anchor_payload.data()), anchor_payload.size())
    });

    std::map<Hash256, BlockIndexRecord> indexed;

    for_each_index_key(*index_db_, kBlockIndexPrefix, [&](const std::string& key) {
        Hash256 hash;
        if (!hash_from_hex_local(key.substr(std::strlen(kBlockIndexPrefix)), hash)) {
            throw ReadError(ReadStatus::CORRUPT, "Invalid block index key before pruning");
        }

        std::string value;
        if (!db_get(key, value)) {
            throw ReadError(ReadStatus::CORRUPT, "Missing block index record before pruning");
        }

        BlockIndexRecord record;
        if (!deserialize_block_index_record(hash, value, record)) {
            throw ReadError(ReadStatus::CORRUPT, "Invalid block index record before pruning");
        }

        indexed.emplace(hash, std::move(record));
    });

    const auto anchor_it = indexed.find(anchor_hash);
    if (anchor_it == indexed.end() || anchor_it->second.height != anchor_height) {
        throw ReadError(ReadStatus::CORRUPT, "Missing or mismatched pruning anchor index");
    }
    PrunedIndexAnchor previous_anchor;
    const bool had_anchor = get_pruned_index_anchor(previous_anchor);
    if (had_anchor && previous_anchor.height > anchor_height) return false;
    Hash256 active_anchor;
    if (get_active_block_hash(anchor_height, active_anchor)) {
        if (active_anchor != anchor_hash) return false;
    } else if (!had_anchor || previous_anchor.height != anchor_height || previous_anchor.hash != anchor_hash) {
        return false;
    }

    std::set<Hash256> active_ancestors;
    const auto* ancestor = &anchor_it->second;
    while (ancestor->header.prev_block_hash != Hash256{}) {
        const auto parent = indexed.find(ancestor->header.prev_block_hash);
        if (parent == indexed.end()) {
            if (had_anchor && ancestor->hash == previous_anchor.hash) break;
            // A parent body/header may still be in flight. Do not compact an
            // incomplete graph or guess which active history can be dropped.
            return false;
        }
        if (ancestor->height == 0 || parent->second.height != ancestor->height - 1) {
            throw ReadError(ReadStatus::CORRUPT, "Invalid ancestor height before index pruning");
        }
        active_ancestors.insert(parent->first);
        ancestor = &parent->second;
    }
    if (ancestor->header.prev_block_hash == Hash256{} && ancestor->height != 0) {
        throw ReadError(ReadStatus::CORRUPT, "Invalid root height before index pruning");
    }

    std::vector<const BlockIndexRecord*> ordered;
    ordered.reserve(indexed.size());
    for (const auto& [hash, record] : indexed) ordered.push_back(&record);
    std::sort(ordered.begin(), ordered.end(), [](const auto* left, const auto* right) {
        return left->height < right->height;
    });
    std::set<Hash256> retained{anchor_hash};
    for (const auto* record : ordered) {
        if (record->height <= anchor_height) continue;
        const auto parent = indexed.find(record->header.prev_block_hash);
        if (parent == indexed.end()) return false;
        if (parent->second.height != record->height - 1) {
            throw ReadError(ReadStatus::CORRUPT, "Invalid descendant height before index pruning");
        }
        if (retained.contains(parent->first)) retained.insert(record->hash);
    }
    // Replace discarded active headers with a minimal permanent geography
    // catalogue in the same atomic batch. Fork headers never enter it.
    for (const auto& hash : active_ancestors) {
        const auto& record = indexed.at(hash);
        if (retained.contains(hash)) continue;
        if (!consensus::geo::decode_geo_tag(record.header.geo_tag))
            throw ReadError(ReadStatus::CORRUPT, "Invalid active geography before pruning");
        const auto payload = encode_block_geography_record(hash, record.header.geo_tag);
        const std::string value(payload.begin(), payload.end());
        const std::string key = "geo:" + height_digits(record.height);
        std::string previous;
        if (db_get(key, previous) && previous != value)
            throw ReadError(ReadStatus::CORRUPT, "Archived geography disagrees with the pruning branch");
        batch.push_back({KVWrite::Type::PUT, key, value});
    }
    std::set<Hash256> removed_hashes;
    for (const auto& [hash, record] : indexed) {
        if (retained.contains(hash)) continue;
        removed_hashes.insert(hash);
        batch.push_back({KVWrite::Type::DELETE_KEY, block_index_key(hash), {}});
        batch.push_back({KVWrite::Type::DELETE_KEY, chainwork_key(hash), {}});
    }

    for_each_index_key(*index_db_, "hh:", [&](const std::string& key) {
        Height height = 0;
        Hash256 hash;
        if (!parse_height_hash_index_key(key, height, &hash)) {
            throw ReadError(ReadStatus::CORRUPT, "Invalid height/hash index key before pruning");
        }
        if (height < keep_from_height || removed_hashes.contains(hash)) {
            batch.push_back({KVWrite::Type::DELETE_KEY, key, {}});
        }
    });

    for_each_index_key(*index_db_, "a:", [&](const std::string& key) {
        Height height = 0;
        if (!parse_height_key_with_prefix(key, "a:", height)) {
            throw ReadError(ReadStatus::CORRUPT, "Invalid height index key before pruning");
        }
        std::string value;
        if (!db_get(key, value) || value.size() != 32) {
            throw ReadError(ReadStatus::CORRUPT, "Invalid height index value before pruning");
        }
        Hash256 hash;
        std::memcpy(hash.data(), value.data(), hash.size());
        if (height >= keep_from_height &&
            (!retained.contains(hash) || indexed.at(hash).height != height)) {
            throw ReadError(ReadStatus::CORRUPT, "Active height does not match the retained pruning branch");
        }
        if (height < keep_from_height || removed_hashes.contains(hash)) {
            batch.push_back({KVWrite::Type::DELETE_KEY, key, {}});
        }
    });

    for_each_index_key(*index_db_, "pc:", [&](const std::string& key) {
        Hash256 parent_hash{};
        Hash256 child_hash{};
        if (!parse_parent_child_index_key(key, parent_hash, child_hash)) {
            throw ReadError(ReadStatus::CORRUPT, "Invalid parent-child index key before pruning");
        }
        const bool old_parent_removed = removed_hashes.count(parent_hash) != 0;
        const bool old_child_removed = removed_hashes.count(child_hash) != 0;
        if (old_parent_removed || old_child_removed) batch.push_back({KVWrite::Type::DELETE_KEY, key, {}});
    });

    Hash256 checkpoint_hash;
    Height checkpoint_height = 0;
    if (!get_policy_checkpoint(checkpoint_hash, checkpoint_height) || checkpoint_height < anchor_height) {
        batch.push_back({
            KVWrite::Type::PUT,
            policy_checkpoint_key(),
            serialize_height_hash_record(anchor_height, anchor_hash)
        });
    }

    std::string best_value;
    if (db_get(best_block_key(), best_value)) {
        Height best_height;
        Hash256 best_hash;
        if (!deserialize_height_hash_record(best_value, best_height, best_hash)) {
            throw ReadError(ReadStatus::CORRUPT, "Invalid best block record before index pruning");
        }
        if (removed_hashes.contains(best_hash)) {
            Hash256 tip_hash;
            Height tip_height;
            if (!get_active_tip(tip_hash, tip_height) || !retained.contains(tip_hash) ||
                indexed.at(tip_hash).height != tip_height) {
                throw ReadError(ReadStatus::CORRUPT, "Cannot replace discarded best block with active tip");
            }
            batch.push_back({KVWrite::Type::PUT, best_block_key(), serialize_height_hash_record(tip_height, tip_hash)});
        }
    }

    uint64_t discarded_bodies = 0;
    uint64_t discarded_bytes = 0;
    for (const auto& hash : removed_hashes) {
        core::Block body;
        const bool has_body = get_block(hash, body, false);
        if (has_body) {
            if (active_ancestors.contains(hash)) {
                GEO_LOG_WARN("storage", "Old active block bodies must be archived before pruning their indexes");
                return false;
            }
            discarded_bytes += body.serialize().size();
            verify_stored_block(*this, hash, body);
            ++discarded_bodies;
        }
        std::string unused;
        if (has_body || db_get(witness_payload_hash_key(hash), unused) || db_get(witness_payload_key(hash), unused)) {
            batch.push_back({KVWrite::Type::DELETE_KEY, block_key(hash), {}});
            batch.push_back({KVWrite::Type::DELETE_KEY, witness_payload_key(hash), {}});
            batch.push_back({KVWrite::Type::DELETE_KEY, witness_payload_hash_key(hash), {}});
        }
    }
    if (discarded_bodies != 0) {
        PruningStats stats;
        get_pruning_stats(stats);
        stats.total_blocks += discarded_bodies;
        stats.pruned_blocks += discarded_bodies;
        stats.body_bytes_reduced += discarded_bytes;
        stats.bytes_total += discarded_bytes;
        batch.push_back(pruning_stats_write(stats));
    }

    if (!db_write_batch(std::move(batch), next_ibd_sync_write())) {
        return false;
    }

    if (pruned_count) {
        *pruned_count = removed_hashes.size();
    }
    if (discarded_body_count) *discarded_body_count = discarded_bodies;
    return true;
}

bool BlockStorage::get_pruned_index_anchor(PrunedIndexAnchor& out) const {
    if (!is_open()) throw ReadError(ReadStatus::IO_ERROR, "Block storage is closed");

    std::string value;
    if (!db_get(pruned_index_anchor_key(), value)) {
        return false;
    }

    Height height = 0;
    Hash256 hash{};
    uint256 chain_work{};
    if (!decode_pruned_index_anchor_record(value, height, hash, chain_work)) {
        throw ReadError(ReadStatus::CORRUPT, "Corrupted pruned index anchor record");
    }

    std::string index_value;
    BlockIndexRecord record;
    if (!db_get(block_index_key(hash), index_value) ||
        !deserialize_block_index_record(hash, index_value, record) ||
        record.height != height) {
        throw ReadError(ReadStatus::CORRUPT, "Pruned index anchor points to missing or invalid block index record");
    }

    out.hash = hash;
    out.header = record.header;
    out.height = height;
    out.chain_work = chain_work;
    return true;
}

bool BlockStorage::prune_witness_at_height(Height height) {
    return prune_witness_at_height_impl(height, false);
}

bool BlockStorage::prune_witness_up_to(Height target_height, size_t max_blocks) {
    if (!is_open()) return false;
    Height completed = 0;
    get_witness_pruned_height(completed);
    if (target_height <= completed) return true;

    // Body pruning has already archived and removed every witness below this
    // cursor. Skip that completed range without one lookup per old height.
    const Height body_next = block_body_prune_next_height();
    if (body_next > 0) {
        const Height skip_to = std::min(target_height, body_next - 1);
        if (completed < skip_to) {
            if (!set_witness_pruned_height(skip_to)) return false;
            completed = skip_to;
        }
    }
    for (size_t processed = 0; completed < target_height; ++processed) {
        if (max_blocks > 0 && processed >= max_blocks) break;
        // completed < target_height guarantees that increment cannot wrap.
        const Height next = completed + 1;
        if (!prune_witness_at_height_impl(next, true)) return false;
        completed = next;
    }
    return true;
}

bool BlockStorage::prune_witness_at_height_impl(Height height, bool advance_progress) {
    if (!is_open()) {
        return false;
    }

    Hash256 active_hash;
    if (!get_active_block_hash(height, active_hash)) {
        return !advance_progress;
    }

    core::Block block;
    if (!get_block(active_hash, block, false)) {
        return false;
    }

    bool has_witness = false;
    for (const auto& tx : block.transactions) {
        if (has_material_witness(tx)) {
            has_witness = true;
            break;
        }
    }
    const size_t size_before = block.serialize().size();
    verify_stored_block(*this, active_hash, block);
    PruningStats stats;
    get_pruning_stats(stats);
    ++stats.total_blocks;
    stats.bytes_total += size_before;
    std::vector<KVWrite> batch;
    if (advance_progress) {
        const auto cursor = utils::encode_height_le(height);
        batch.push_back({KVWrite::Type::PUT, kWitnessPrunedHeightKey,
                         std::string(reinterpret_cast<const char*>(cursor.data()), cursor.size())});
    }
    if (!has_witness) {
        batch.push_back(pruning_stats_write(stats));
        return db_write_batch(std::move(batch), true);
    }

    const bytes witness_payload = serialize_witness_payload(block);
    const Hash256 witness_payload_hash = crypto::sha256(witness_payload);
    for (auto& tx : block.transactions) {
        tx.witnesses.clear();
    }
    bytes data = block.serialize();
    const size_t size_after = data.size();
    std::string key = block_key(active_hash);
    const std::string payload_key = witness_payload_key(active_hash);
    const std::string payload_hash_key = witness_payload_hash_key(active_hash);

    batch.push_back({
        KVWrite::Type::PUT,
        key,
        std::string(reinterpret_cast<const char*>(data.data()), data.size())
    });
    batch.push_back({
        KVWrite::Type::PUT,
        payload_key,
        std::string(reinterpret_cast<const char*>(witness_payload.data()), witness_payload.size())
    });
    batch.push_back({
        KVWrite::Type::PUT,
        payload_hash_key,
        std::string(reinterpret_cast<const char*>(witness_payload_hash.data()), witness_payload_hash.size())
    });

    ++stats.pruned_blocks;
    stats.body_bytes_reduced += size_before - size_after;
    batch.push_back(pruning_stats_write(stats));
    return db_write_batch(std::move(batch), true);
}

bool BlockStorage::get_pruning_stats(PruningStats& out) const {
    if (!is_open()) throw ReadError(ReadStatus::IO_ERROR, "Block storage is closed");
    std::string value;
    if (!db_get(kPruneStatsKey, value)) {
        out = PruningStats{};
        return true;
    }
    bytes data(value.begin(), value.end());
    if (!deserialize_prune_stats(data, out)) {
        out = PruningStats{};
        throw ReadError(ReadStatus::CORRUPT, "Invalid pruning statistics");
    }
    return true;
}

bool BlockStorage::set_pruning_stats(const PruningStats& stats) {
    if (!is_open()) {
        return false;
    }
    bytes data = serialize_prune_stats(stats);
    return db_put(
        kPruneStatsKey,
        std::string(reinterpret_cast<const char*>(data.data()), data.size()),
        false
    );
}

bool BlockStorage::get_witness_pruned_height(Height& out) const {
    if (!is_open()) throw ReadError(ReadStatus::IO_ERROR, "Block storage is closed");
    std::string value;
    if (!db_get(kWitnessPrunedHeightKey, value)) {
        out = 0;
        return true;
    }
    bytes data(value.begin(), value.end());
    if (!utils::decode_height_le_exact(data, out)) throw ReadError(ReadStatus::CORRUPT, "Invalid witness pruned height");
    return true;
}

bool BlockStorage::set_witness_pruned_height(Height height) {
    if (!is_open()) {
        return false;
    }
    bytes data = utils::encode_height_le(height);
    return db_put(
        kWitnessPrunedHeightKey,
        std::string(reinterpret_cast<const char*>(data.data()), data.size()),
        false
    );
}

bool BlockStorage::get_witness_payload(const Hash256& block_hash, bytes& out) const {
    if (!is_open()) throw ReadError(ReadStatus::IO_ERROR, "Block storage is closed");
    const std::string key = witness_payload_key(block_hash);
    std::string value;
    if (!db_get(key, value)) {
        return false;
    }
    bytes payload(value.begin(), value.end());

    std::string hash_value;
    const std::string hash_key = witness_payload_hash_key(block_hash);
    if (!db_get(hash_key, hash_value)) throw ReadError(ReadStatus::CORRUPT, "Missing witness payload hash");
    {
        if (hash_value.size() != 32) {
            throw ReadError(ReadStatus::CORRUPT, "Corrupted witness payload hash size");
        }
        Hash256 expected_hash{};
        std::memcpy(expected_hash.data(), hash_value.data(), 32);
        if (crypto::sha256(payload) != expected_hash) {
            throw ReadError(ReadStatus::CORRUPT, "Witness payload hash mismatch");
        }
    }

    out = std::move(payload);
    return true;
}

bool BlockStorage::set_witness_payload(const Hash256& block_hash, const bytes& payload) {
    if (!is_open()) {
        return false;
    }
    const std::string key = witness_payload_key(block_hash);
    const std::string hash_key = witness_payload_hash_key(block_hash);
    const Hash256 payload_hash = crypto::sha256(payload);

    std::vector<KVWrite> batch;
    batch.push_back({
        KVWrite::Type::PUT,
        key,
        std::string(reinterpret_cast<const char*>(payload.data()), payload.size())
    });
    batch.push_back({
        KVWrite::Type::PUT,
        hash_key,
        std::string(reinterpret_cast<const char*>(payload_hash.data()), payload_hash.size())
    });

    return db_write_batch(std::move(batch), false);
}

bool BlockStorage::build_witness_payload_from_block(const Hash256& block_hash, bytes& out) const {
    if (!is_open()) {
        throw ReadError(ReadStatus::IO_ERROR, "Block storage is closed");
    }
    core::Block block;
    if (!get_block(block_hash, block, false)) {
        return false;
    }
    bool has_witness = false;
    for (const auto& tx : block.transactions) {
        if (has_material_witness(tx)) {
            has_witness = true;
            break;
        }
    }
    if (!has_witness) {
        return false;
    }
    verify_stored_block(*this, block_hash, block);
    out = serialize_witness_payload(block);
    return true;
}

bool BlockStorage::restore_witness_from_payload(const Hash256& block_hash, const bytes& payload) {
    if (!is_open()) {
        return false;
    }
    if (payload.empty()) {
        return false;
    }

    std::string hash_value;
    const std::string hash_key = witness_payload_hash_key(block_hash);
    if (db_get(hash_key, hash_value)) {
        if (hash_value.size() != 32) {
            GEO_LOG_WARN("storage", "Corrupted witness hash record for block: " << utils::hash256_to_hex(block_hash));
            return false;
        }
        Hash256 expected_hash{};
        std::memcpy(expected_hash.data(), hash_value.data(), 32);
        if (crypto::sha256(payload) != expected_hash) {
            GEO_LOG_WARN("storage", "Rejected restore: witness payload hash mismatch for block " << utils::hash256_to_hex(block_hash));
            return false;
        }
    }

    core::Block block;
    if (!get_block(block_hash, block, false)) {
        return false;
    }
    if (!apply_witness_payload(block, payload)) {
        GEO_LOG_WARN("storage", "Rejected restore: invalid witness payload format for block " << utils::hash256_to_hex(block_hash));
        return false;
    }
    if (block.get_hash() != block_hash || !block_commitments_match(block)) {
        GEO_LOG_WARN("storage", "Rejected restore: witness does not match block commitments");
        return false;
    }
    bytes data = block.serialize();
    std::string key = block_key(block_hash);
    std::vector<KVWrite> batch;
    batch.push_back({
        KVWrite::Type::PUT,
        key,
        std::string(reinterpret_cast<const char*>(data.data()), data.size())
    });
    const std::string payload_key = witness_payload_key(block_hash);
    const Hash256 payload_hash = crypto::sha256(payload);
    batch.push_back({
        KVWrite::Type::PUT,
        payload_key,
        std::string(reinterpret_cast<const char*>(payload.data()), payload.size())
    });
    batch.push_back({
        KVWrite::Type::PUT,
        hash_key,
        std::string(reinterpret_cast<const char*>(payload_hash.data()), payload_hash.size())
    });

    if (!db_write_batch(std::move(batch), true)) {
        return false;
    }
    return true;
}

bool BlockStorage::mark_block_failed(const Hash256& block_hash) {
    if (!is_open()) {
        return false;
    }
    std::string index_value;
    if (!db_get(block_index_key(block_hash), index_value)) {
        return false;
    }
    return db_put(failed_block_key(block_hash), std::string(1, '\x01'), true);
}

bool BlockStorage::set_chainwork(const Hash256& block_hash, const uint256& chain_work) {
    if (!is_open()) {
        return false;
    }

    bytes data = encode_chain_work_record(chain_work);

    return db_put(
        chainwork_key(block_hash),
        std::string(reinterpret_cast<const char*>(data.data()), data.size()),
        false
    );
}

bool BlockStorage::get_chainwork(const Hash256& block_hash, uint256& chain_work) const {
    if (!is_open()) throw ReadError(ReadStatus::IO_ERROR, "Block storage is closed");

    std::string value;
    if (!db_get(chainwork_key(block_hash), value)) {
        return false;
    }

    if (!decode_chain_work_record(value, chain_work)) throw ReadError(ReadStatus::CORRUPT, "Invalid chainwork record");
    return true;
}

uint64_t BlockStorage::get_size() const {
    if (!is_open()) {
        return 0;
    }

    if (path_.empty()) {
        return 0;
    }

    {
        std::lock_guard<std::mutex> lock(size_cache_mutex_);
        const auto now = std::chrono::steady_clock::now();
        constexpr auto kCacheTtl = std::chrono::seconds(5);
        if (size_cache_time_.time_since_epoch().count() != 0 &&
            now - size_cache_time_ < kCacheTtl) {
            return size_cache_;
        }
    }

    uint64_t total = 0;
    for (const auto& directory : {get_chaindata_path(), get_index_path()}) {
        std::error_code ec;
        if (!std::filesystem::exists(directory, ec)) {
            if (ec) throw ReadError(ReadStatus::IO_ERROR, ec.message());
            continue;
        }
        std::filesystem::recursive_directory_iterator it(directory, ec), end;
        for (; !ec && it != end; it.increment(ec)) {
            if (it->is_regular_file(ec)) total += it->file_size(ec);
        }
        if (ec) throw ReadError(ReadStatus::IO_ERROR, "Storage size scan: " + ec.message());
    }

    {
        std::lock_guard<std::mutex> lock(size_cache_mutex_);
        size_cache_ = total;
        size_cache_time_ = std::chrono::steady_clock::now();
    }

    return total;
}

namespace {
size_t scan_block_hashes(const RocksDBStore* store, std::vector<Hash256>* hashes) {
    size_t count = 0;

    if (!store) throw ReadError(ReadStatus::IO_ERROR, "Block storage is closed");

    // Итерируемся по всем ключам с префиксом "b:"
    auto it = store->new_iterator();
    if (!it) throw ReadError(ReadStatus::IO_ERROR, "Cannot create block storage iterator");

    std::string prefix = "b:";
    for (it->seek(prefix); it->valid() && it->key().substr(0, 2) == prefix; it->next()) {
        std::string key = it->key();

        // Извлекаем хеш из ключа (после "b:")
        std::string hash_hex = key.substr(2);

        if (hash_hex.size() != 64) {
            throw ReadError(ReadStatus::CORRUPT, "Invalid block index key or record");
        }

        // Конвертируем hex в Hash256
        Hash256 hash;
        if (!hash_from_hex_local(hash_hex, hash)) {
            throw ReadError(ReadStatus::CORRUPT, "Invalid block index key or record");
        }

        if (hashes) hashes->push_back(hash);
        ++count;
    }

    return count;
}
} // namespace

std::vector<Hash256> BlockStorage::get_all_block_hashes() const {
    std::vector<Hash256> hashes;
    scan_block_hashes(is_open() ? chain_db_.get() : nullptr, &hashes);
    return hashes;
}

std::vector<BlockStorage::BlockIndexRecord> BlockStorage::get_all_block_index_records() const {
    std::vector<BlockIndexRecord> records;
    if (!is_open()) throw ReadError(ReadStatus::IO_ERROR, "Block storage is closed");

    std::map<Hash256, BlockIndexRecord> by_hash;
    auto it = index_db_->new_iterator();
    if (!it) throw ReadError(ReadStatus::IO_ERROR, "Cannot create block storage iterator");

    for (it->seek(kBlockIndexPrefix); it->valid(); it->next()) {
        const std::string key = it->key();
        if (key.rfind(kBlockIndexPrefix, 0) != 0) {
            break;
        }

        Hash256 hash;
        if (!hash_from_hex_local(key.substr(std::strlen(kBlockIndexPrefix)), hash)) {
            throw ReadError(ReadStatus::CORRUPT, "Invalid block index key or record");
        }

        std::string value;
        if (!db_get(key, value)) {
            throw ReadError(ReadStatus::CORRUPT, "Invalid block index key or record");
        }

        BlockIndexRecord record;
        if (!deserialize_block_index_record(hash, value, record)) {
            GEO_LOG_WARN("storage", "Corrupted block index record: " << utils::hash256_to_hex(hash));
            throw ReadError(ReadStatus::CORRUPT, "Invalid block index key or record");
        }
        if (get_chainwork(hash, record.chain_work)) {
            record.has_chain_work = true;
        }
        if (!read_failed_marker(hash, record.failed)) {
            throw ReadError(ReadStatus::CORRUPT, "Invalid permanent block failure marker");
        }
        by_hash[hash] = record;
    }

    const std::string block_prefix = "b:";
    auto block_it = chain_db_->new_iterator();
    if (!block_it) throw ReadError(ReadStatus::IO_ERROR, "Cannot create block storage iterator");
    for (block_it->seek(block_prefix); block_it->valid(); block_it->next()) {
        const std::string key = block_it->key();
        if (key.rfind(block_prefix, 0) != 0) {
            break;
        }

        Hash256 hash;
        if (!hash_from_hex_local(key.substr(block_prefix.size()), hash)) {
            throw ReadError(ReadStatus::CORRUPT, "Invalid block index key or record");
        }

        auto existing = by_hash.find(hash);
        if (existing != by_hash.end()) {
            existing->second.has_body = true;
            continue;
        }

        core::Block block;
        if (!get_block(hash, block, false)) {
            GEO_LOG_WARN("storage", "Unreadable block body: " << utils::hash256_to_hex(hash));
            throw ReadError(ReadStatus::CORRUPT, "Invalid block index key or record");
        }
        if (block.get_hash() != hash) {
            GEO_LOG_WARN("storage", "Corrupted block body: " << utils::hash256_to_hex(hash));
            throw ReadError(ReadStatus::CORRUPT, "Invalid block index key or record");
        }

        BlockIndexRecord record;
        record.hash = hash;
        record.header = block.header;
        record.height = std::numeric_limits<Height>::max();
        record.has_body = true;
        if (get_chainwork(hash, record.chain_work)) {
            record.has_chain_work = true;
        }
        if (!read_failed_marker(hash, record.failed)) {
            throw ReadError(ReadStatus::CORRUPT, "Invalid permanent block failure marker");
        }
        by_hash[hash] = record;
    }

    records.reserve(by_hash.size());
    for (auto& [hash, record] : by_hash) {
        (void)hash;
        records.push_back(std::move(record));
    }
    return records;
}

size_t BlockStorage::get_block_count() const {
    return scan_block_hashes(is_open() ? chain_db_.get() : nullptr, nullptr);
}

std::string BlockStorage::block_key(const Hash256& hash) const {
    return "b:" + utils::hash256_to_hex(hash);
}

std::string BlockStorage::block_index_key(const Hash256& hash) const {
    return std::string(kBlockIndexPrefix) + utils::hash256_to_hex(hash);
}

std::string BlockStorage::failed_block_key(const Hash256& hash) const {
    return std::string(kFailedBlockPrefix) + utils::hash256_to_hex(hash);
}

bool BlockStorage::read_failed_marker(const Hash256& hash, bool& failed) const {
    failed = false;
    std::string value;
    if (!db_get(failed_block_key(hash), value)) {
        return true;
    }
    if (value.size() != 1 || value[0] != '\x01') {
        return false;
    }
    failed = true;
    return true;
}

std::string BlockStorage::height_hash_key(Height height, const Hash256& hash) const {
    return "hh:" + height_digits(height) + ":" + utils::hash256_to_hex(hash);
}

std::string BlockStorage::active_height_key(Height height) const {
    return "a:" + height_digits(height);
}

std::string BlockStorage::active_tip_key() const {
    return "active_tip";
}

std::string BlockStorage::active_transition_key() const {
    return "active_transition";
}

std::string BlockStorage::consensus_ruleset_key() const {
    return kConsensusRulesetKey;
}

std::string BlockStorage::policy_checkpoint_key() const {
    return "policy_checkpoint";
}

std::string BlockStorage::pruned_index_anchor_key() const {
    return kPrunedIndexAnchorKey;
}

std::string BlockStorage::best_block_key() const {
    return "best";
}

std::string BlockStorage::parent_child_key(const Hash256& parent_hash, const Hash256& child_hash) const {
    return "pc:" + utils::hash256_to_hex(parent_hash) + ":" + utils::hash256_to_hex(child_hash);
}

std::string BlockStorage::chainwork_key(const Hash256& hash) const {
    return "bw:" + utils::hash256_to_hex(hash);
}

} // namespace geocoin::storage
