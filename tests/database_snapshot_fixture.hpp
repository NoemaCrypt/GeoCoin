#pragma once

#include "chain_test_fixture.hpp"
#include "card_registry_fixture.hpp"
#include "sidechain_registry_fixture.hpp"
#include "geocoin/storage/backend_support.hpp"
#include "geocoin/storage/state_records.hpp"
#include "geocoin/utils/bytes.hpp"
#include <rocksdb/db.h>
#include <rocksdb/version.h>
#include <map>

namespace geocoin::test {

// Alter temporary snapshots through real RocksDB writes, so checksum validation
// succeeds and the offline checker must examine the logical relationships.
class SnapshotDatabase {
public:
    explicit SnapshotDatabase(const std::filesystem::path& path) {
        rocksdb::Options options;
        std::vector<std::string> names;
        expect(rocksdb::DB::ListColumnFamilies(options, path.string(), &names).ok(), "list fixture columns");
        std::vector<rocksdb::ColumnFamilyDescriptor> columns;
        for (const auto& name : names) columns.emplace_back(name, rocksdb::ColumnFamilyOptions{});
#if ROCKSDB_MAJOR >= 11
        const auto status = rocksdb::DB::Open(options, path.string(), columns, &handles_, &db_);
#else
        rocksdb::DB* raw = nullptr;
        const auto status = rocksdb::DB::Open(options, path.string(), columns, &handles_, &raw);
        db_.reset(raw);
#endif
        expect(status.ok(), "open fixture database");
    }
    ~SnapshotDatabase() {
        if (db_) for (auto* handle : handles_) db_->DestroyColumnFamilyHandle(handle);
    }
    std::map<std::string, std::string> records(const std::string& prefix) const {
        std::map<std::string, std::string> result;
        for (auto* handle : handles_) {
            std::unique_ptr<rocksdb::Iterator> it(db_->NewIterator(rocksdb::ReadOptions(), handle));
            for (it->Seek(prefix); it->Valid() && it->key().starts_with(prefix); it->Next()) {
                expect(result.emplace(it->key().ToString(), it->value().ToString()).second, "unique fixture keys");
            }
            expect(it->status().ok(), "iterate fixture database");
        }
        return result;
    }
    std::string get(const std::string& key) const {
        std::string value;
        expect(find(key, value) != nullptr, "fixture record exists: " + key);
        return value;
    }
    void put(const std::string& key, const std::string& value) {
        std::string old;
        auto* handle = find(key, old);
        rocksdb::WriteOptions options;
        options.sync = true;
        expect(db_->Put(options, handle ? handle : db_->DefaultColumnFamily(), key, value).ok(), "write fixture record");
    }
    void erase(const std::string& key) {
        std::string old;
        auto* handle = find(key, old);
        expect(handle != nullptr, "fixture deletion exists");
        rocksdb::WriteOptions options;
        options.sync = true;
        expect(db_->Delete(options, handle, key).ok(), "delete fixture record");
    }
    void put_column(const std::string& key, const std::string& value, const std::string& column) {
        rocksdb::WriteOptions options; options.sync = true;
        for (auto* handle : handles_) {
            if (handle->GetName() == column) {
                expect(db_->Put(options, handle, key, value).ok(), "write fixture in explicit column");
                return;
            }
        }
        throw std::runtime_error("Unknown fixture column");
    }
private:
    rocksdb::ColumnFamilyHandle* find(const std::string& key, std::string& value) const {
        for (auto* handle : handles_) {
            const auto status = db_->Get(rocksdb::ReadOptions(), handle, key, &value);
            if (status.ok()) return handle;
            expect(status.IsNotFound(), "read fixture database");
        }
        return nullptr;
    }
    std::unique_ptr<rocksdb::DB> db_;
    std::vector<rocksdb::ColumnFamilyHandle*> handles_;
};

inline std::string snapshot_raw(const bytes& data) {
    return std::string(data.begin(), data.end());
}

inline void alter_snapshot(const std::filesystem::path& root, const std::string& kind) {
    if (kind == "prune-bodies" || kind == "prune-indexes" || kind == "prune-witness") {
        auto db = std::make_unique<state::RocksDBStorage>((root / "chaindata/state").string(),
                                                       (root / "addresses/state").string());
        expect(db->open(), "open state for fixture maintenance");
        auto state = std::make_shared<state::ChainState>(std::move(db));
        auto blocks = std::make_shared<storage::BlockStorage>(root.string());
        state->set_block_storage(blocks);
        core::Blockchain chain(state, blocks);
        expect(chain.initialize(), "initialize fixture before maintenance");
        expect(kind == "prune-witness" ? state->prune_witness_up_to(3)
                                      : chain.prune_history(2, kind == "prune-indexes"), "prune fixture");
        return;
    }

    SnapshotDatabase indexes(root / "index/blocks");
    SnapshotDatabase blocks(root / "chaindata/blocks");
    SnapshotDatabase state(root / "chaindata/state");
    if (kind.starts_with("geography-")) {
        const auto records = indexes.records("geo:");
        expect(!records.empty(), "fixture contains pruned geography");
        const auto& [key, value] = *records.begin();
        if (kind == "geography-missing") indexes.erase(key);
        else if (kind == "geography-duplicate") indexes.put_column("geo:0000000001", value, "geography_archive");
        else {
            auto damaged = value;
            if (kind == "geography-length") damaged.pop_back();
            else if (kind == "geography-tag") std::fill(damaged.begin() + 32, damaged.end(), '\xff');
            else if (kind == "geography-null-hash") std::fill(damaged.begin(), damaged.begin() + 32, '\0');
            else throw std::runtime_error("Unknown geography mutation");
            indexes.put(key, damaged);
        }
    } else if (kind.starts_with("sidechain-")) {
        const auto mode = kind.substr(10);
        expect(std::find(sidechain_damage_cases.begin(), sidechain_damage_cases.end(), mode) != sidechain_damage_cases.end() ||
               std::find(sidechain_valid_cases.begin(), sidechain_valid_cases.end(), mode) != sidechain_valid_cases.end(), "known sidechain snapshot case");
        for (const auto& [key, value] : SidechainRegistryFixture{}.records(mode)) state.put("meta:" + key, snapshot_raw(value));
    } else if (kind.starts_with("card-")) {
        const auto damage = kind.substr(5);
        expect(damage == "valid" || std::find(card_damage_cases.begin(), card_damage_cases.end(), damage) != card_damage_cases.end(),
               "known card snapshot mutation");
        for (const auto& [key, value] : CardRegistryFixture{}.records(damage == "valid" ? "" : damage))
            state.put("meta:" + key, snapshot_raw(value));
    } else if (kind == "duplicate-column" || kind == "misplaced-column") {
        const auto entries = indexes.records("bi:");
        const auto& [key, value] = *entries.begin();
        if (kind == "misplaced-column") indexes.erase(key);
        indexes.put_column(key, value, "default");
    } else if (kind == "misplaced-state") {
        SnapshotDatabase addresses(root / "addresses/state");
        addresses.put("meta:foundation_cfg", state.get("meta:foundation_cfg"));
    } else if (kind == "unknown-record") {
        state.put("meta:obsolete_record", "unused copy");
    } else if (kind == "missing-foundation" || kind == "missing-interest-pool") {
        state.erase(kind == "missing-foundation" ? "meta:foundation_cfg" : "meta:interest_pool_balance");
    } else if (kind == "malformed-deposit" || kind == "malformed-card" || kind == "malformed-undo") {
        const std::string prefix = kind == "malformed-deposit" ? "meta:scd:" :
            kind == "malformed-card" ? "meta:card:uid:" : "meta:undo:";
        state.put(prefix + utils::hash256_to_hex(hash_from_seed(98)), "truncated");
    } else if (kind == "orphan-nft") {
        state.put("meta:nft:owner:" + utils::hash256_to_hex(hash_from_seed(98)), address_from_seed(98).str());
    } else if (kind == "orphan-witness") {
        state.put("meta:wpkref:" + utils::hash256_to_hex(hash_from_seed(98)), snapshot_raw(utils::encode_u64_le(1)));
    } else if (kind == "active-index") {
        indexes.put("a:0000000001", indexes.get("a:0000000002"));
    } else if (kind == "membership-height") {
        for (const auto& [key, raw] : state.records("meta:atx:")) {
            storage::ActiveTransactionRecord record;
            expect(storage::ActiveTransactionRecord::decode(bytes(raw.begin(), raw.end()), record), "decode fixture membership");
            if (record.height == 1 || record.height == 2) {
                record.height = 3 - record.height;
                state.put(key, snapshot_raw(record.serialize()));
            }
        }
    } else if (kind == "merkle" || kind == "witness") {
        const auto hash = indexes.get("a:0000000001");
        const auto key = "b:" + utils::bytes_to_hex(reinterpret_cast<const byte*>(hash.data()), hash.size());
        const auto raw = blocks.get(key);
        auto block = core::Block::deserialize(bytes(raw.begin(), raw.end()));
        auto& tx = block.transactions.front();
        const auto old_id = utils::hash256_to_hex(tx.get_hash());
        if (kind == "merkle") ++tx.timestamp;
        else { expect(!tx.witnesses.empty(), "fixture witness slot exists"); tx.witnesses.front().public_key_hash[0] ^= 1; }
        const auto new_id = utils::hash256_to_hex(tx.get_hash());
        expect((old_id != new_id) == (kind == "merkle"), "mutation changes only the selected commitment");
        blocks.put(key, snapshot_raw(block.serialize()));
        if (kind == "merkle") {
            state.put("meta:atx:" + new_id, state.get("meta:atx:" + old_id));
            state.erase("meta:atx:" + old_id);
            for (const auto* prefix : {"meta:qaddr:", "meta:qnft:", "meta:qrefs:"}) {
                for (const auto& [old_key, value] : state.records(prefix)) {
                    auto changed = old_key;
                    for (auto pos = changed.find(old_id); pos != std::string::npos; pos = changed.find(old_id, pos + new_id.size()))
                        changed.replace(pos, old_id.size(), new_id);
                    auto changed_value = value;
                    if (std::string_view(prefix) == "meta:qrefs:") {
                        for (auto pos = changed_value.find(old_id); pos != std::string::npos; pos = changed_value.find(old_id, pos + new_id.size()))
                            changed_value.replace(pos, old_id.size(), new_id);
                    }
                    if (changed != old_key) state.erase(old_key);
                    if (changed != old_key || changed_value != value) state.put(changed, changed_value);
                }
            }
        }
    } else if (kind == "query-height" || kind == "query-syntax" || kind == "query-value") {
        const auto entries = state.records("meta:qaddr:");
        expect(!entries.empty(), "fixture query index exists");
        const auto& [old_key, value] = *entries.begin();
        if (kind == "query-value") { state.put(old_key, "unexpected"); return; }
        auto key = old_key;
        const size_t height_start = key.size() - 85;
        key.replace(height_start, 20, kind == "query-height" ? "00000000000000000099" : "xxxxxxxxxxxxxxxxxxxx");
        const auto reverse = "meta:qrefs:" + old_key.substr(old_key.size() - 64);
        auto listed = state.get(reverse);
        const auto old_forward = old_key.substr(5);
        const auto position = listed.find(old_forward);
        expect(position != std::string::npos, "forward key exists in its packed reverse index");
        listed.replace(position, old_forward.size(), key.substr(5));
        state.erase(old_key); state.put(key, value); state.put(reverse, listed);
    } else if (kind == "orphan-reverse") {
        const auto entries = state.records("meta:qaddr:");
        const auto& first = entries.begin()->first;
        const auto first_id = first.substr(first.size() - 64);
        const auto reverse = "meta:qrefs:" + first_id;
        std::vector<std::string> references;
        const auto raw = state.get(reverse);
        expect(storage::QueryReferenceList::decode(bytes(raw.begin(), raw.end()), references),
               "decode packed query references");
        for (const auto& [key, value] : entries) {
            if (key.substr(key.size() - 64) != first_id) {
                references.push_back(key.substr(5));
                std::sort(references.begin(), references.end());
                state.put(reverse, snapshot_raw(storage::QueryReferenceList::serialize(references)));
                return;
            }
        }
        expect(false, "fixture needs two different transactions");
    } else if (kind == "payload-hash" || kind == "payload-data") {
        const auto entries = blocks.records("witness:");
        expect(!entries.empty(), "fixture retained witness exists");
        const auto& [key, raw] = *entries.begin();
        const auto hash_key = "witnessh:" + key.substr(8);
        if (kind == "payload-hash") {
            auto hash = blocks.get(hash_key); hash.front() ^= 1; blocks.put(hash_key, hash);
        } else {
            auto changed = raw; changed.back() ^= 1; blocks.put(key, changed);
            const auto hash = crypto::sha256(bytes(changed.begin(), changed.end()));
            blocks.put(hash_key, std::string(hash.begin(), hash.end()));
        }
    } else { expect(false, "unknown snapshot mutation: " + kind); }
}

} // namespace geocoin::test
