#include "geocoin/storage/backend_support.hpp"
#include "geocoin/storage/block_records.hpp"
#include "geocoin/storage/state_records.hpp"
#include "geocoin/storage/schema.hpp"
#include "geocoin/storage/witness_payload.hpp"
#include "geocoin/core/merkle.hpp"
#include "geocoin/crypto/geo_area.hpp"
#include "geocoin/state/state.hpp"
#include "geocoin/state/records.hpp"
#include "geocoin/crypto/addresses.hpp"
#include "geocoin/consensus/consensus.hpp"
#include "geocoin/utils/bytes.hpp"
#include "nlohmann_json.hpp"
#include <rocksdb/db.h>
#include <rocksdb/env.h>
#include <rocksdb/utilities/checkpoint.h>
#include <rocksdb/version.h>
#include <openssl/evp.h>
#include <array>
#include <algorithm>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <map>
#include <optional>
#include <set>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#endif

namespace fs = std::filesystem;
using json = nlohmann::json;
using namespace geocoin;

namespace {
const auto& stores = storage::schema::paths;
void require(bool ok, const std::string& message) { if (!ok) throw std::runtime_error(message); }
void check(const rocksdb::Status& status) { require(status.ok(), status.ToString()); }

struct TemporaryDirectory {
    fs::path path;
    explicit TemporaryDirectory(fs::path value) : path(std::move(value)) {
        fs::create_directories(path.parent_path());
        require(fs::create_directory(path), "Temporary directory already exists");
    }
    ~TemporaryDirectory() { std::error_code error; fs::remove_all(path, error); }
};

struct Database {
    struct ReadOnlyLogger final : rocksdb::Logger {
        void Logv(const char*, va_list) override {}
    };
    std::unique_ptr<rocksdb::DB> db;
    std::vector<rocksdb::ColumnFamilyHandle*> handles;
    Database(const fs::path& path, bool readonly) {
        rocksdb::Options options;
        options.create_if_missing = false;
        if (readonly) options.info_log = std::make_shared<ReadOnlyLogger>();
        std::vector<std::string> names;
        check(rocksdb::DB::ListColumnFamilies(options, path.string(), &names));
        std::vector<rocksdb::ColumnFamilyDescriptor> columns;
        for (const auto& name : names) columns.emplace_back(name, rocksdb::ColumnFamilyOptions{});
#if ROCKSDB_MAJOR >= 11
        const auto status = readonly
            ? rocksdb::DB::OpenForReadOnly(options, path.string(), columns, &handles, &db)
            : rocksdb::DB::Open(options, path.string(), columns, &handles, &db);
#else
        rocksdb::DB* raw = nullptr;
        const auto status = readonly
            ? rocksdb::DB::OpenForReadOnly(options, path.string(), columns, &handles, &raw)
            : rocksdb::DB::Open(options, path.string(), columns, &handles, &raw);
        db.reset(raw);
#endif
        check(status);
    }
    ~Database() { if (db) for (auto* handle : handles) db->DestroyColumnFamilyHandle(handle); }
    bool get(const std::string& key, std::string& value) const {
        bool found = false;
        for (auto* cf : handles) {
            std::string current;
            const auto status = db->Get(rocksdb::ReadOptions(), cf, key, &current);
            if (status.IsNotFound()) continue;
            check(status);
            require(!found, "Duplicate key across column families: " + key);
            value = std::move(current); found = true;
        }
        return found;
    }
    std::string required(const std::string& key) const {
        std::string value;
        require(get(key, value), "Missing required snapshot record: " + key);
        return value;
    }
    void checkpoint(const fs::path& path) const {
        rocksdb::Checkpoint* raw = nullptr;
        check(rocksdb::Checkpoint::Create(db.get(), &raw));
        std::unique_ptr<rocksdb::Checkpoint> checkpoint(raw);
        fs::create_directories(path.parent_path());
        check(checkpoint->CreateCheckpoint(path.string()));
    }
};

using Databases = std::vector<std::unique_ptr<Database>>;
Databases open_stores(const fs::path& root, bool readonly) {
    Databases result;
    // Acquire every exclusive RocksDB lock before taking the first checkpoint.
    // A running node or another maintenance writer makes this operation fail.
    for (const auto& name : stores) result.push_back(std::make_unique<Database>(root / name, readonly));
    return result;
}

struct HistoryAnchor {
    Height height = 0;
    Hash256 hash{};
};

std::optional<HistoryAnchor> read_history_anchor(const Database& indexes, Height tip_height) {
    std::string raw;
    if (!indexes.get("pruned_index_anchor", raw)) return std::nullopt;
    HistoryAnchor anchor;
    uint256 work;
    require(storage::decode_pruned_index_anchor_record(raw, anchor.height, anchor.hash, work) &&
            anchor.height <= tip_height, "Invalid pruning anchor");
    Height indexed_height = 0;
    core::BlockHeader header;
    require(storage::decode_block_index_record(anchor.hash,
                indexes.required("bi:" + utils::hash256_to_hex(anchor.hash)), indexed_height, header) &&
            indexed_height == anchor.height, "Pruning anchor and block index disagree");
    return anchor;
}

std::optional<core::BlockHeader> active_header(const Database& indexes, const std::optional<HistoryAnchor>& anchor,
                                             Height height, const Hash256& hash) {
    require(!anchor || height != anchor->height || hash == anchor->hash,
            "Pruning anchor is absent from active transaction history");
    auto digits = std::to_string(height);
    if (digits.size() < 10) digits.insert(0, 10 - digits.size(), '0');
    std::string active;
    if (indexes.get("a:" + digits, active)) {
        require(active == std::string(hash.begin(), hash.end()), "Active height and transaction membership disagree");
    } else {
        require(anchor && height <= anchor->height && (height < anchor->height || hash == anchor->hash),
                "Missing active height outside pruned history");
    }
    std::string raw;
    if (!indexes.get("bi:" + utils::hash256_to_hex(hash), raw)) {
        require(anchor && height < anchor->height, "Missing retained block index");
        return std::nullopt;
    }
    Height indexed_height = 0;
    core::BlockHeader header;
    require(storage::decode_block_index_record(hash, raw, indexed_height, header) && indexed_height == height,
            "Invalid active block index");
    return header;
}

std::vector<Hash256> validated_body_txids(const Database& blocks, const std::string& hash,
                                        const std::string& raw, uint64_t expected) {
    auto block = core::Block::deserialize(bytes(raw.begin(), raw.end()));
    require(utils::hash256_to_hex(block.get_hash()) == hash && block.transactions.size() == expected,
            "Invalid active block body");
    std::vector<Hash256> txids;
    txids.reserve(block.transactions.size());
    for (const auto& tx : block.transactions) txids.push_back(tx.get_hash());
    // calculate_merkle_root mutates the header; validate against a pure result.
    require(core::compute_merkle_root(txids) == block.header.merkle_root, "Active block Merkle root mismatch");

    std::string payload, payload_hash;
    const bool has_payload = blocks.get("witness:" + hash, payload);
    require(blocks.get("witnessh:" + hash, payload_hash) == has_payload, "Incomplete retained witness pair");
    const bool material = std::any_of(block.transactions.begin(), block.transactions.end(), storage::has_material_witness);
    if (material || !has_payload) {
        require(block.calculate_witness_commitment() == block.header.witness_commitment,
                "Active block witness commitment mismatch");
    }
    if (has_payload) {
        const bytes data(payload.begin(), payload.end());
        const auto actual_hash = crypto::sha256(data);
        require(payload_hash == std::string(actual_hash.begin(), actual_hash.end()), "Retained witness checksum mismatch");
        require(storage::apply_witness_payload(block, data) &&
                block.calculate_witness_commitment() == block.header.witness_commitment,
                "Retained witness does not match block commitment");
    }
    return txids;
}

struct QueryReference {
    Height height = 0;
    std::string txid;
};

QueryReference parse_query_reference(std::string_view key) {
    const size_t prefix = key.starts_with("qaddr:") ? 6 : key.starts_with("qnft:") ? 5 : 0;
    require(prefix != 0, "Invalid query index kind");
    const auto separator = key.find(':', prefix);
    require(separator != std::string_view::npos && separator > prefix, "Missing query index identifier");
    const auto id = key.substr(prefix, separator - prefix);
    bytes decoded;
    require(utils::hex_to_bytes_checked(id, decoded) && utils::bytes_to_hex(decoded) == id,
            "Invalid query index identifier encoding");
    if (prefix == 5) {
        const std::string nft(decoded.begin(), decoded.end());
        Hash256 hash{};
        require(utils::hex_to_hash256_checked(nft, hash) && utils::hash256_to_hex(hash) == nft,
                "Invalid NFT query identifier");
    }
    const auto suffix = key.substr(separator + 1);
    require(suffix.size() == 85 && suffix[20] == ':', "Invalid query index suffix");
    QueryReference ref;
    const auto parsed = std::from_chars(suffix.data(), suffix.data() + 20, ref.height);
    require(parsed.ec == std::errc{} && parsed.ptr == suffix.data() + 20, "Invalid query index height");
    Hash256 hash{};
    ref.txid = suffix.substr(21);
    require(utils::hex_to_hash256_checked(ref.txid, hash) && utils::hash256_to_hex(hash) == ref.txid,
            "Invalid query transaction hash");
    return ref;
}

Hash256 canonical_hash(std::string_view text) {
    Hash256 hash{};
    require(text.size() == 64 && utils::hex_to_hash256_checked(std::string(text), hash) &&
            utils::hash256_to_hex(hash) == text, "Noncanonical database identity");
    return hash;
}

bytes raw_bytes(const std::string& raw) { return bytes(raw.begin(), raw.end()); }

std::string padded_height(Height height) {
    auto text = std::to_string(height);
    if (text.size() < 10) text.insert(0, 10 - text.size(), '0');
    return text;
}

json validate_records(const Databases& databases, Height tip_height) {
    const auto anchor = read_history_anchor(*databases[1], tip_height);
    const auto& state_db = *databases[2];
    const auto epoch = state_db.required("meta:idx_epoch");
    require(epoch.size() == 32 && std::all_of(epoch.begin(), epoch.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }), "Invalid state index epoch");
    state::FoundationConfig foundation;
    require(state::records::deserialize_foundation_config(raw_bytes(state_db.required("meta:foundation_cfg")), foundation),
            "Invalid or missing foundation configuration");
    const auto pool = raw_bytes(state_db.required("meta:interest_pool_balance"));
    uint64_t pool_value = 0;
    require(utils::decode_u64_le_exact(pool, pool_value) && pool_value <= static_cast<uint64_t>(std::numeric_limits<Amount>::max()),
            "Invalid or missing interest pool balance");
    std::set<std::pair<std::string, uint32_t>> nonces;
    std::set<Hash256> card_keys;
    std::set<std::string> accounting_namespaces;
    json inventory = json::object();
    uint64_t geographic_records = 0;
    for (size_t store = 0; store < databases.size(); ++store) {
        const auto& db = *databases[store];
        std::set<std::string> columns;
        for (auto* cf : db.handles) columns.insert(cf->GetName());
        require(columns == storage::schema::columns(static_cast<storage::schema::Store>(store)), "Unexpected database column families");
        for (auto* cf : db.handles) {
            std::unique_ptr<rocksdb::Iterator> it(db.db->NewIterator(rocksdb::ReadOptions(), cf));
            for (it->SeekToFirst(); it->Valid(); it->Next()) {
                const auto key = it->key().ToString();
                const auto raw = it->value().ToString();
                if (key == storage::backend::kFormatKey) {
                    require(cf->GetName() == "default", "Misplaced database format marker");
                    continue;
                }
                const auto* spec = storage::schema::find(key);
                require(spec && static_cast<size_t>(spec->store) == store && cf->GetName() == spec->column,
                        "Unknown, duplicated or misplaced database record: " + key);
                require(spec->bytes < 0 || raw.size() == static_cast<size_t>(spec->bytes), "Invalid record length: " + key);
                auto& count = inventory[std::string(stores[store])][std::string(spec->key)];
                count = count.is_null() ? 1 : count.get<uint64_t>() + 1;
                const auto suffix = std::string_view(key).substr(spec->key.size());
                if (!spec->prefix) continue;
                if (spec->key == "meta:utxo:" || spec->key == "meta:addr:utxo:" ||
                    spec->key == "meta:qaddr:" || spec->key == "meta:qnft:" || spec->key == "meta:qrefs:") continue;
                if (spec->key == "meta:idx:") {
                    require(suffix.starts_with(epoch + ":"), "Stale uniqueness index epoch");
                    const auto rest = suffix.substr(epoch.size() + 1);
                    const auto separator = rest.find(':');
                    require(separator != std::string_view::npos, "Malformed uniqueness index");
                    const auto kind = rest.substr(0, separator);
                    const auto id = canonical_hash(rest.substr(separator + 1));
                    if (kind == "dp") {
                        require(raw.size() == 32, "Delegation index must reference its state record");
                        state::DelegationState entry;
                        const auto txid = utils::bytes_to_hex(reinterpret_cast<const byte*>(raw.data()), raw.size());
                        require(state::records::deserialize_delegation(raw_bytes(state_db.required("meta:delegation:" + txid)), entry) &&
                                entry.proof_key == id, "Missing or inconsistent delegation state");
                    } else {
                        require((kind == "ac" || kind == "ms" || kind == "hs" || kind == "rd" || kind == "nft") && raw == std::string(1, '\1'),
                                "Invalid uniqueness index kind or value");
                    }
                    continue;
                }
                if (spec->key == "geo:") {
                    Height height = 0;
                    const auto parsed = std::from_chars(suffix.data(), suffix.data() + suffix.size(), height);
                    Hash256 hash{}; uint64_t tag = 0;
                    require(parsed.ec == std::errc{} && parsed.ptr == suffix.data() + suffix.size() &&
                        suffix == padded_height(height) && anchor && height < anchor->height &&
                        storage::decode_block_geography_record(raw, hash, tag), "Invalid archived block geography");
                    ++geographic_records;
                    continue;
                }
                if (spec->key == "hh:" || spec->key == "a:") {
                    const auto separator = suffix.find(':');
                    const auto digits = spec->key == "a:" ? suffix : suffix.substr(0, separator);
                    Height height = 0;
                    const auto result = std::from_chars(digits.data(), digits.data() + digits.size(), height);
                    require(result.ec == std::errc{} && result.ptr == digits.data() + digits.size() && digits == padded_height(height),
                            "Noncanonical block height index");
                    const auto hash = spec->key == "a:"
                        ? canonical_hash(utils::bytes_to_hex(reinterpret_cast<const byte*>(raw.data()), raw.size()))
                        : canonical_hash(separator == std::string_view::npos ? "" : suffix.substr(separator + 1));
                    Height recorded_height = 0; core::BlockHeader header;
                    require(storage::decode_block_index_record(hash, databases[1]->required("bi:" + utils::hash256_to_hex(hash)), recorded_height, header) &&
                            height == recorded_height, "Height index disagrees with block record");
                    continue;
                }
                if (spec->key == "pc:") {
                    require(suffix.size() == 129 && suffix[64] == ':', "Malformed parent-child index");
                    const auto parent = canonical_hash(suffix.substr(0, 64));
                    const auto child = canonical_hash(suffix.substr(65));
                    Height height = 0; core::BlockHeader header;
                    require(storage::decode_block_index_record(child, databases[1]->required("bi:" + utils::hash256_to_hex(child)), height, header) &&
                            header.prev_block_hash == parent, "Parent-child index disagrees with header");
                    continue;
                }
                const auto hash = canonical_hash(suffix);
                const auto id = std::string(suffix);
                const auto data = raw_bytes(raw);
                if (spec->key == "b:") {
                    const auto block = core::Block::deserialize(data);
                    (void)validated_body_txids(db, id, raw, block.transactions.size());
                } else if (spec->key == "bi:") {
                    Height height = 0; core::BlockHeader header;
                    require(storage::decode_block_index_record(hash, raw, height, header), "Invalid block index record");
                    if (!anchor || hash != anchor->hash) {
                        require(databases[1]->required("hh:" + padded_height(height) + ":" + id).empty(), "Missing reverse height index");
                    }
                } else if (spec->key == "witness:" || spec->key == "witnessh:") {
                    const auto payload = databases[0]->required("witness:" + id);
                    const auto digest = crypto::sha256(raw_bytes(payload));
                    require(databases[0]->required("witnessh:" + id) == std::string(digest.begin(), digest.end()), "Incomplete witness payload pair");
                    require(!databases[0]->required("b:" + id).empty(), "Witness payload has no retained block");
                } else if (spec->key == "tx:") {
                    require(core::Transaction::deserialize_canonical(data).get_hash() == hash, "Invalid archived transaction identity");
                } else if (spec->key == "meta:undo:") {
                    const auto undo = state::BlockUndo::deserialize(data);
                    require(undo.block_hash == hash && undo.height <= tip_height && undo.serialize() == data, "Invalid undo record");
                } else if (spec->key == "meta:scd:") {
                    state::SidechainDeposit entry;
                    require(!utils::is_null_hash256(hash) && state::records::deserialize_sidechain_deposit(data, entry) &&
                            state::records::validate_sidechain_deposit(entry, tip_height), "Invalid sidechain deposit fields");
                    if (entry.last_checkpoint_height > 0) {
                        std::string registry;
                        require(state_db.get("meta:sc:reg:" + id, registry), "Checkpointed deposit has no sidechain registry");
                    }
                } else if (spec->key == "meta:sc:reg:") {
                    state::SidechainRegistryEntry entry;
                    state::SidechainDeposit deposit;
                    require(!utils::is_null_hash256(hash) && state::records::deserialize_sidechain_registry(data, entry) &&
                            state::records::validate_sidechain_registry(entry, tip_height), "Invalid sidechain registry fields");
                    require(state::records::deserialize_sidechain_deposit(raw_bytes(state_db.required("meta:scd:" + id)), deposit) &&
                            state::records::validate_sidechain_deposit(deposit, tip_height) && deposit.owner == entry.owner,
                            "Sidechain has no valid matching deposit");
                    require(entry.accounting_namespace.empty() || accounting_namespaces.insert(entry.accounting_namespace).second,
                            "Duplicate accounting namespace");
                } else if (spec->key == "meta:card:prog:") {
                    state::CardProgramEntry entry;
                    require(!utils::is_null_hash256(hash) && state::records::deserialize_card_program(data, entry) &&
                            state::records::validate_card_program(entry, tip_height), "Invalid card program fields");
                } else if (spec->key == "meta:card:uid:") {
                    state::CardInstanceEntry entry; state::CardProgramEntry program;
                    require(state::records::deserialize_card_instance(data, entry) && entry.uid_hash == hash &&
                            card_keys.insert(entry.pubkey_hash).second, "Invalid or duplicated card fields");
                    require(state::records::deserialize_card_program(raw_bytes(state_db.required("meta:card:prog:" + utils::hash256_to_hex(entry.program_id))), program) &&
                            state::records::validate_card_program(program, tip_height) &&
                            state::records::validate_card_instance(entry, program, tip_height), "Card has no valid matching program or fields");
                } else if (spec->key == "meta:nft:meta:" || spec->key == "meta:nft:owner:") {
                    const auto metadata = nft::NFTMetadata::deserialize(raw_bytes(state_db.required("meta:nft:meta:" + id)));
                    require(metadata.calculate_hash() == hash && crypto::verify_address(Address(state_db.required("meta:nft:owner:" + id))) &&
                            state_db.required("meta:idx:" + epoch + ":nft:" + id) == std::string(1, '\1'), "Incomplete or invalid NFT record");
                } else if (spec->key == "meta:wpk:" || spec->key == "meta:wpkref:") {
                    uint64_t references = 0;
                    const auto pubkey = raw_bytes(state_db.required("meta:wpk:" + id));
                    require(pubkey.size() == consensus::params::WITNESS_PUBKEY_SIZE && crypto::sha256(pubkey) == hash &&
                            utils::decode_u64_le_exact(raw_bytes(state_db.required("meta:wpkref:" + id)), references) && references > 0,
                            "Incomplete or invalid witness public key pair");
                } else if (spec->key == "meta:delegation:") {
                    state::DelegationState entry;
                    require(state::records::deserialize_delegation(data, entry) && crypto::verify_address(entry.primary_address) &&
                            nonces.emplace(entry.primary_address.str(), entry.nonce).second &&
                            state_db.required("meta:idx:" + epoch + ":dp:" + utils::hash256_to_hex(entry.proof_key)) == std::string(hash.begin(), hash.end()),
                            "Invalid or duplicated delegation state");
                    require(state_db.required("meta:atx:" + id).size() == 48, "Delegation is absent from active transaction history");
                }
            }
            check(it->status());
        }
    }
    require(geographic_records == (anchor ? anchor->height : 0), "Incomplete archived block geography");
    return inventory;
}

json schema_catalog() {
    json result = json::array();
    for (const auto& record : storage::schema::records) {
        result.push_back({{"key", record.key}, {"prefix", record.prefix},
            {"database", stores[static_cast<size_t>(record.store)]}, {"column_family", record.column},
            {"value_bytes", record.bytes}, {"fields", record.fields}});
    }
    result.push_back({{"key", storage::backend::kFormatKey}, {"database", "each database"}, {"column_family", "default"},
        {"fields", "storage format; network; genesis identity"}});
    result.push_back({{"key", "meta:split_commit_pending"}, {"database", "chaindata/state and addresses/state"},
        {"column_family", "default"}, {"fields", "temporary marker while committing both state databases"}});
    return result;
}

json physical_inventory(const Databases& databases) {
    json result = json::object();
    std::map<std::string, std::pair<size_t, rocksdb::ColumnFamilyHandle*>> owners;
    for (size_t index = 0; index < databases.size(); ++index) {
        const auto& database = *databases[index];
        check(database.db->VerifyChecksum());
        json item{{"checksum", "ok"}, {"records", 0}, {"key_bytes", 0}, {"value_bytes", 0},
                  {"duplicate_keys", 0}, {"cross_database_keys", 0}, {"equal_cross_database_values", 0},
                  {"format", nullptr}, {"families", json::object()}};
        std::set<std::string> seen;
        for (auto* cf : database.handles) {
            std::unique_ptr<rocksdb::Iterator> it(database.db->NewIterator(rocksdb::ReadOptions(), cf));
            for (it->SeekToFirst(); it->Valid(); it->Next()) {
                const auto key = it->key().ToString();
                const auto* spec = storage::schema::find(key);
                const auto prefix_end = key.find(':', key.starts_with("meta:") ? 5 : 0);
                const std::string family = spec ? std::string(spec->key) : key.substr(0, prefix_end == std::string::npos ? 64 : prefix_end + 1);
                if (key == storage::backend::kFormatKey) item["format"] = it->value().ToString();
                item["records"] = item["records"].get<uint64_t>() + 1;
                item["key_bytes"] = item["key_bytes"].get<uint64_t>() + key.size();
                item["value_bytes"] = item["value_bytes"].get<uint64_t>() + it->value().size();
                if (!seen.insert(key).second) item["duplicate_keys"] = item["duplicate_keys"].get<uint64_t>() + 1;
                const auto [owner, inserted] = owners.emplace(key, std::pair{index, cf});
                if (!inserted && owner->second.first != index && key != storage::backend::kFormatKey &&
                    key != "meta:split_commit_pending") {
                    std::string previous;
                    check(databases[owner->second.first]->db->Get(rocksdb::ReadOptions(), owner->second.second, key, &previous));
                    item["cross_database_keys"] = item["cross_database_keys"].get<uint64_t>() + 1;
                    if (previous == it->value().ToString()) {
                        item["equal_cross_database_values"] = item["equal_cross_database_values"].get<uint64_t>() + 1;
                    }
                }
                auto& count = item["families"][cf->GetName()][family];
                count = count.is_null() ? 1 : count.get<uint64_t>() + 1;
            }
            check(it->status());
        }
        result[std::string(stores[index])] = std::move(item);
    }
    return result;
}

json validate(const Databases& databases) {
    const auto format = databases[0]->required(storage::backend::kFormatKey);
    require(format.starts_with(storage::backend::kFormatPrefix), "Unsupported snapshot format");
    for (const auto& db : databases) {
        require(db->required(storage::backend::kFormatKey) == format, "Databases belong to different formats or networks");
        check(db->db->VerifyChecksum());
        for (auto* cf : db->handles) {
            std::unique_ptr<rocksdb::Iterator> it(db->db->NewIterator(rocksdb::ReadOptions(), cf));
            for (it->SeekToFirst(); it->Valid(); it->Next()) {}
            check(it->status());
        }
        std::string value;
        for (const auto* key : {"split_write_pending", "active_transition", "meta:split_commit_pending",
                               "meta:chainstate_journal", "meta:chainstate_rebuild_pending"}) {
            require(!db->get(key, value), std::string("Snapshot requires recovery: ") + key);
        }
    }
    const auto height = databases[2]->required("meta:chainstate_height");
    const auto tip = databases[2]->required("meta:chainstate_tip_hash");
    require(height.size() == 8 && tip.size() == 32, "Invalid state position");
    require(databases[1]->required("active_tip") == height + tip, "Active chain and state positions differ");
    require(databases[2]->required("meta:query_index_tip") == height + tip, "Query index belongs to another position");
    const auto count = databases[2]->required("meta:chainstate_utxo_count");
    require(count.size() == 8, "Invalid UTXO count");
    uint64_t actual = 0, address_entries = 0;
    for (auto* cf : databases[3]->handles) {
        std::unique_ptr<rocksdb::Iterator> it(databases[3]->db->NewIterator(rocksdb::ReadOptions(), cf));
        for (it->Seek("meta:utxo:"); it->Valid() && it->key().starts_with("meta:utxo:"); it->Next()) {
            const auto raw = it->value().ToString();
            const auto utxo = state::UTXO::deserialize(bytes(raw.begin(), raw.end()));
            require(it->key().ToString() == "meta:" + utxo.get_key(), "UTXO key/value mismatch");
            std::string value;
            require(databases[3]->get("meta:" + utxo.get_address_index_key(), value) && value.empty(), "Invalid address UTXO index");
            ++actual;
        }
        check(it->status());
        for (it->Seek("meta:addr:utxo:"); it->Valid() && it->key().starts_with("meta:addr:utxo:"); it->Next()) ++address_entries;
        check(it->status());
    }
    const bytes count_bytes(count.begin(), count.end()), height_bytes(height.begin(), height.end());
    require(actual == utils::read_u64_le_unchecked(count_bytes, 0) && actual == address_entries, "Snapshot UTXO indexes disagree");
    const auto tip_height = utils::read_u64_le_unchecked(height_bytes, 0);
    const auto anchor = read_history_anchor(*databases[1], tip_height);
    struct Membership { uint64_t height, expected, seen; bool body; std::optional<Hash256> previous; };
    std::map<std::string, Membership> membership;
    uint64_t transaction_count = 0;
    for (auto* cf : databases[2]->handles) {
        std::unique_ptr<rocksdb::Iterator> it(databases[2]->db->NewIterator(rocksdb::ReadOptions(), cf));
        for (it->Seek("meta:atx:"); it->Valid() && it->key().starts_with("meta:atx:"); it->Next()) {
            const auto txid = it->key().ToString().substr(9);
            Hash256 tx_hash{};
            require(utils::hex_to_hash256_checked(txid, tx_hash), "Invalid transaction membership key");
            const auto raw = it->value().ToString();
            storage::ActiveTransactionRecord record;
            require(storage::ActiveTransactionRecord::decode(
                std::span<const byte>(reinterpret_cast<const byte*>(raw.data()), raw.size()), record),
                "Invalid transaction membership record");
            const auto h = record.height;
            const auto expected = record.transaction_count;
            require(h <= tip_height, "Invalid transaction membership position");
            const auto block_hash = utils::hash256_to_hex(record.block_hash);
            auto group = membership.find(block_hash);
            if (group == membership.end()) {
                const auto header = active_header(*databases[1], anchor, h, record.block_hash);
                std::string body;
                const bool has_body = databases[0]->get("b:" + block_hash, body);
                if (has_body) {
                    for (const auto& txid : validated_body_txids(*databases[0], block_hash, body, expected)) {
                        const auto ref = databases[2]->required("meta:atx:" + utils::hash256_to_hex(txid));
                        require(ref == raw, "Block and transaction membership disagree");
                    }
                }
                const auto previous = header ? std::optional<Hash256>(header->prev_block_hash) : std::nullopt;
                group = membership.emplace(block_hash, Membership{h, expected, 0, has_body, previous}).first;
            }
            require(group->second.height == h && group->second.expected == expected, "Inconsistent transaction membership group");
            ++group->second.seen; ++transaction_count;
            if (!group->second.body) {
                const auto tx_raw = databases[2]->required("tx:" + txid);
                size_t offset = 0;
                const auto tx = core::Transaction::deserialize(bytes(tx_raw.begin(), tx_raw.end()), offset);
                require(offset == tx_raw.size() && tx.get_hash() == tx_hash, "Invalid archived transaction");
            }
        }
        check(it->status());
        for (const auto* prefix : {"meta:qaddr:", "meta:qnft:"}) {
            for (it->Seek(prefix); it->Valid() && it->key().starts_with(prefix); it->Next()) {
                const auto key = it->key().ToString();
                const auto ref = parse_query_reference(std::string_view(key).substr(5));
                require(it->value().empty(), "Unexpected query index value");
                const auto raw = databases[2]->required("meta:atx:" + ref.txid);
                storage::ActiveTransactionRecord record;
                require(storage::ActiveTransactionRecord::decode(
                            std::span<const byte>(reinterpret_cast<const byte*>(raw.data()), raw.size()), record) &&
                        record.height == ref.height, "Query height and active transaction disagree");
                const auto reverse = raw_bytes(databases[2]->required("meta:qrefs:" + ref.txid));
                std::vector<std::string> references;
                require(storage::QueryReferenceList::decode(reverse, references) &&
                        std::binary_search(references.begin(), references.end(), key.substr(5)),
                        "Missing or invalid query reference list");
            }
            check(it->status());
        }
        for (it->Seek("meta:qrefs:"); it->Valid() && it->key().starts_with("meta:qrefs:"); it->Next()) {
            const auto key = it->key().ToString();
            const auto txid = key.substr(11);
            (void)canonical_hash(txid);
            std::vector<std::string> references;
            require(storage::QueryReferenceList::decode(raw_bytes(it->value().ToString()), references),
                    "Invalid query reference list");
            const auto marker = raw_bytes(databases[2]->required("meta:atx:" + txid));
            storage::ActiveTransactionRecord record;
            require(storage::ActiveTransactionRecord::decode(marker, record), "Invalid query reference membership");
            for (const auto& forward : references) {
                const auto ref = parse_query_reference(forward);
                require(ref.txid == txid && ref.height == record.height, "Query list refers to another transaction or height");
                require(databases[2]->required("meta:" + forward).empty(), "Missing or invalid forward query index");
            }
        }
        check(it->status());
    }
    std::set<uint64_t> heights;
    for (const auto& [hash, group] : membership) {
        require(group.seen == group.expected && heights.insert(group.height).second, "Incomplete or ambiguous transaction membership group");
        if (group.previous && group.height > 0) {
            const auto previous = membership.find(utils::hash256_to_hex(*group.previous));
            require(previous != membership.end() && previous->second.height == group.height - 1,
                    "Active header does not extend the preceding history group");
        }
    }
    require(!heights.empty() && heights.size() - 1 == tip_height, "Missing active transaction history height");
    const auto last = membership.find(utils::bytes_to_hex(reinterpret_cast<const byte*>(tip.data()), tip.size()));
    require(last != membership.end() && last->second.height == tip_height, "State tip is absent from active transaction history");
    const auto inventory = validate_records(databases, tip_height);
    return {{"format_identity", format}, {"height", utils::read_u64_le_unchecked(height_bytes, 0)},
            {"tip", utils::bytes_to_hex(reinterpret_cast<const byte*>(tip.data()), tip.size())},
            {"utxos", actual}, {"transactions", transaction_count}, {"records", inventory}};
}

std::string sha256_file(const fs::path& path) {
    std::ifstream input(path, std::ios::binary); require(input.good(), "Cannot read " + path.string());
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    require(ctx && EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) == 1, "Digest initialization failed");
    std::array<char, 65536> buffer;
    while (input.read(buffer.data(), buffer.size()) || input.gcount() != 0) {
        require(EVP_DigestUpdate(ctx.get(), buffer.data(), input.gcount()) == 1, "Digest update failed");
    }
    require(input.eof(), "Snapshot file read failed");
    std::array<byte, 32> hash{}; unsigned length = 0;
    require(EVP_DigestFinal_ex(ctx.get(), hash.data(), &length) == 1 && length == hash.size(), "Digest finalization failed");
    return utils::bytes_to_hex(hash.data(), hash.size());
}

json inventory(const fs::path& root) {
    std::map<std::string, json> files;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        require(!entry.is_symlink(), "Snapshot must not contain symbolic links");
        if (!entry.is_regular_file()) continue;
        const auto name = entry.path().filename().string();
        const auto relative = fs::relative(entry.path(), root).generic_string();
        if (relative == "snapshot.json" || name == "LOCK" || name == "LOG" || name.rfind("LOG.old.", 0) == 0) continue;
        files[relative] = {{"bytes", entry.file_size()}, {"sha256", sha256_file(entry.path())}};
    }
    return files;
}

void sync_path(const fs::path& path) {
#ifndef _WIN32
    const int fd = ::open(path.c_str(), O_RDONLY | (fs::is_directory(path) ? O_DIRECTORY : 0));
    require(fd >= 0, "Cannot open snapshot path for synchronization");
    const bool ok = ::fsync(fd) == 0; ::close(fd);
    require(ok, "Snapshot synchronization failed");
#else
    if (fs::is_directory(path)) return; // Directory publication uses MoveFileExW below.
    const auto handle = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    require(handle != INVALID_HANDLE_VALUE, "Cannot open snapshot file for synchronization");
    const bool ok = FlushFileBuffers(handle) != 0;
    CloseHandle(handle);
    require(ok, "Snapshot synchronization failed");
#endif
}

json verify(const fs::path& root) {
    std::ifstream input(root / "snapshot.json"); require(input.good(), "Missing snapshot.json");
    const auto manifest = json::parse(input);
    require(manifest.at("version") == 1, "Unsupported snapshot manifest");
    require(manifest.at("files") == inventory(root), "Snapshot file inventory or checksum mismatch");
    auto databases = open_stores(root, true);
    require(manifest.at("state") == validate(databases), "Snapshot state differs from manifest");
    return manifest;
}

void publish(const fs::path& temporary, const fs::path& destination) {
    for (const auto& entry : fs::recursive_directory_iterator(temporary)) if (entry.is_regular_file()) sync_path(entry.path());
    for (const auto& entry : fs::recursive_directory_iterator(temporary)) if (entry.is_directory()) sync_path(entry.path());
    sync_path(temporary);
    require(!fs::exists(destination), "Destination already exists");
#ifdef _WIN32
    require(MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH) != 0,
            "Cannot publish snapshot directory");
#else
    fs::rename(temporary, destination);
#endif
    sync_path(destination.parent_path());
}
}

int main(int argc, char** argv) {
#ifndef _WIN32
    ::umask(0077);
#endif
    try {
        if (argc == 2 && std::string_view(argv[1]) == "schema") {
            std::cout << schema_catalog().dump(2) << '\n'; return 0;
        }
        require(argc >= 3, "Usage: geocoin-db schema | inspect <network-datadir> | check <stopped-network-datadir> | backup <stopped-network-datadir> <new-snapshot-dir> | verify <snapshot-dir> | restore <snapshot-dir> <new-network-datadir>");
        const std::string command = argv[1];
        const auto source = fs::canonical(argv[2]);
        if (command == "inspect") { require(argc == 3, "inspect expects one path"); auto databases = open_stores(source, true); std::cout << physical_inventory(databases).dump(2) << '\n'; return 0; }
        if (command == "check") { require(argc == 3, "check expects one path"); auto databases = open_stores(source, false); std::cout << validate(databases).dump(2) << '\n'; return 0; }
        if (command == "verify") { require(argc == 3, "verify expects one path"); std::cout << verify(source).at("state").dump(2) << '\n'; return 0; }
        require((command == "backup" || command == "restore") && argc == 4, "Invalid command or arguments");
        const auto destination = fs::weakly_canonical(fs::absolute(argv[3]));
        require(!fs::exists(destination), "Destination must be new; existing data is never overwritten");
        const auto relative = destination.lexically_relative(source);
        require(destination.root_path() != source.root_path() || (!relative.empty() && *relative.begin() == ".."),
                "Destination must be outside source");
        TemporaryDirectory scratch(fs::path(destination.string() + ".tmp-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())));
        const auto& temporary = scratch.path;
        if (command == "backup") {
            auto databases = open_stores(source, false);
            const auto state = validate(databases);
            for (size_t i = 0; i < stores.size(); ++i) {
                databases[i]->checkpoint(temporary / stores[i]);
                std::ofstream marker(fs::path(temporary / stores[i]).replace_extension(".backend")); marker << "rocksdb\n";
                require(marker.good(), "Cannot write backend marker");
            }
            for (const auto* name : {"geocoin.conf", "activation-manifest.json"}) {
                if (fs::exists(source / name)) { require(!fs::is_symlink(source / name), "Config symlink is unsupported"); fs::copy_file(source / name, temporary / name); }
            }
            json manifest{{"version", 1}, {"state", state}, {"files", inventory(temporary)}};
            std::ofstream output(temporary / "snapshot.json"); output << manifest.dump(2) << '\n'; output.close(); require(output.good(), "Cannot write snapshot manifest");
        } else {
            (void)verify(source);
            fs::copy(source, temporary, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
        }
        (void)verify(temporary);
        publish(temporary, destination);
        std::cout << command << " completed: " << destination << '\n';
        return 0;
    } catch (const std::exception& error) { std::cerr << "geocoin-db: " << error.what() << '\n'; return 1; }
}
