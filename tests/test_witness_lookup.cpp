#include "chain_test_fixture.hpp"
#include "geocoin/utils/bytes.hpp"
#include "geocoin/state/records.hpp"

#include <algorithm>
#include <cctype>
#include <functional>
#include <iostream>
#include <type_traits>

using namespace geocoin;
using test::expect;

namespace {
struct Scratch {
    std::filesystem::path path = test::make_temp_dir("geocoin-witness-lookup");
    ~Scratch() { std::error_code error; std::filesystem::remove_all(path, error); }
};

bool witness_record(const std::string& key) {
    return key.starts_with("wpk:") || key.starts_with("wpkref:");
}

struct Counts {
    size_t reads = 0, stats_writes = 0, full_scans = 0, largest_page = 0;
    bool fail_stats = false;
};

template<class Backend>
struct TrackedStorage : Backend {
    template<class... Args>
    TrackedStorage(Counts& counts, Args&&... args) : Backend(std::forward<Args>(args)...), counts(counts) {}
    bool get_metadata(const std::string& key, bytes& out) const override {
        if (witness_record(key)) ++counts.reads;
        return Backend::get_metadata(key, out);
    }
    bool has_metadata(const std::string& key) const override {
        // RocksDBStorage::has_metadata delegates to the virtual get_metadata;
        // MemoryStorage checks its map directly. Count either path once.
        if constexpr (std::is_same_v<Backend, state::MemoryStorage>) {
            if (witness_record(key)) ++counts.reads;
        }
        return Backend::has_metadata(key);
    }
    bool put_metadata(const std::string& key, const bytes& value) override {
        if (key == "wpk_stats") {
            ++counts.stats_writes;
            if (counts.fail_stats) return false;
        }
        return Backend::put_metadata(key, value);
    }
    std::vector<std::string> get_keys_with_prefix(const std::string& prefix) const override {
        if (witness_record(prefix)) ++counts.full_scans;
        return Backend::get_keys_with_prefix(prefix);
    }
    std::vector<std::string> get_keys_page(const std::string& prefix, size_t offset, size_t limit,
                                         size_t* total = nullptr, const std::string& after = {}) const override {
        if (witness_record(prefix)) counts.largest_page = std::max(counts.largest_page, limit);
        return Backend::get_keys_page(prefix, offset, limit, total, after);
    }
    Counts& counts;
};

std::unique_ptr<state::Storage> backend(int mode, const Scratch& scratch, Counts& counts) {
    if (mode == 0) return std::make_unique<TrackedStorage<state::MemoryStorage>>(counts);
    auto db = mode == 1
        ? std::make_unique<TrackedStorage<state::RocksDBStorage>>(counts, (scratch.path / "state").string())
        : std::make_unique<TrackedStorage<state::RocksDBStorage>>(counts, (scratch.path / "state").string(), (scratch.path / "addresses").string());
    expect(db->open(), "open witness database");
    return db;
}

bytes pubkey(uint64_t seed) {
    bytes value(consensus::params::WITNESS_PUBKEY_SIZE, 42);
    const auto unique = utils::encode_u64_le(seed);
    std::copy(unique.begin(), unique.end(), value.begin());
    return value;
}

void seed_position(state::Storage& db) {
    const auto tip = test::hash_from_seed(41);
    state::ChainState defaults;
    expect(db.put_metadata("foundation_cfg", state::records::serialize_foundation_config(defaults.get_foundation_config())) &&
           db.put_metadata("interest_pool_balance", utils::encode_u64_le(0)), "seed required foundation state");
    expect(db.put_metadata("idx_epoch", bytes(32, 'a')) &&
           db.put_metadata("chainstate_height", utils::encode_u64_le(0)) &&
           db.put_metadata("chainstate_tip_hash", bytes(tip.begin(), tip.end())) &&
           db.put_metadata("chainstate_utxo_count", utils::encode_u64_le(0)), "seed standalone snapshot position");
}

void seed_key(state::Storage& db, const bytes& pub) {
    const auto hex = utils::hash256_to_hex(crypto::sha256(pub));
    expect(db.put_metadata("wpk:" + hex, pub) && db.put_metadata("wpkref:" + hex, utils::encode_u64_le(1)), "seed witness pair");
}

template<class Operation>
void expect_corrupt(Operation&& operation) {
    bool corrupt = false;
    try { operation(); }
    catch (const storage::ReadError& error) { corrupt = error.status() == storage::ReadStatus::CORRUPT; }
    expect(corrupt, "inconsistent witness pair reports CORRUPT instead of absence or success");
}

void bad_read(const std::string& kind) {
    for (int mode = 0; mode < 3; ++mode) {
        Scratch scratch; Counts counts;
        state::ChainState state(backend(mode, scratch, counts));
        auto* db = state.get_storage();
        auto pub = pubkey(41);
        if (kind == "wrong-size") pub.resize(8);
        const auto hash = kind == "wrong-hash" ? crypto::sha256(pubkey(42)) : crypto::sha256(pub);
        const auto hex = utils::hash256_to_hex(hash);
        if (kind != "missing-payload") expect(db->put_metadata("wpk:" + hex, pub), "seed payload");
        if (kind != "missing-ref") {
            const auto raw = kind == "bad-count" ? bytes{1} : utils::encode_u64_le(kind == "zero-count" ? 0 : 1);
            expect(db->put_metadata("wpkref:" + hex, raw), "seed refcount");
        }
        bytes out{9, 8, 7};
        expect_corrupt([&] { state.get_witness_pubkey(hash, out); });
        expect(out == bytes({9, 8, 7}), "failed witness lookup preserves caller output");
        expect_corrupt([&] { state.has_witness_pubkey(hash); });
        expect(state.get_witness_pubkey_stats().total_keys == 0, "failed lookup does not cache an incomplete pair");
        if (mode != 0) {
            state.set_storage({});
            state.set_storage(backend(mode, scratch, counts));
            db = state.get_storage();
            expect(!state.has_derived_state_snapshot(), "reopened incomplete witness pair invalidates the snapshot");
            expect_corrupt([&] { state.get_witness_pubkey(hash, out); });
            expect(out == bytes({9, 8, 7}), "reopened corrupt lookup preserves caller output");
        }
        seed_key(*db, pubkey(43));
        expect(state.get_witness_pubkey(crypto::sha256(pubkey(43)), out) && out == pubkey(43), "valid cold lookup works after an unrelated bad record");
        expect(!state.get_witness_pubkey(test::hash_from_seed(99), out) && out == pubkey(43), "unknown key remains an ordinary absence with unchanged output");
    }
}

void uppercase_load() {
    for (int mode = 0; mode < 3; ++mode) {
        Scratch scratch; Counts counts;
        auto db = backend(mode, scratch, counts);
        seed_position(*db);
        const auto pub = pubkey(41);
        auto hex = utils::hash256_to_hex(crypto::sha256(pub));
        std::transform(hex.begin(), hex.end(), hex.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        expect(db->put_metadata("wpk:" + hex, pub) && db->put_metadata("wpkref:" + hex, utils::encode_u64_le(1)), "seed noncanonical pair");
        if (mode != 0) { db.reset(); db = backend(mode, scratch, counts); }
        state::ChainState state(std::move(db));
        expect(!state.has_derived_state_snapshot(), "unreachable uppercase witness keys invalidate the snapshot");
        expect(state.get_storage()->has_metadata("wpk:" + hex) && state.get_storage()->has_metadata("wpkref:" + hex), "opening preserves noncanonical records");
    }
}

void clean_stats() {
    for (int mode = 0; mode < 3; ++mode) {
        Scratch scratch; Counts counts;
        state::ChainState state(backend(mode, scratch, counts));
        counts.stats_writes = 0;
        expect(state.get_witness_pubkey_stats().total_keys == 0, "fresh statistics readable");
        state.get_witness_pubkey_stats();
        expect(counts.stats_writes == 0, "unchanged statistics do not write to the database");
        seed_key(*state.get_storage(), pubkey(41));
        bytes out;
        expect(state.get_witness_pubkey(crypto::sha256(pubkey(41)), out), "load witness for changed statistics");
        counts.fail_stats = true;
        expect(state.get_witness_pubkey_stats().cache_misses == 1 && counts.stats_writes == 1, "failed diagnostic write does not affect reading stats");
        counts.fail_stats = false;
        expect(state.get_witness_pubkey_stats().cache_misses == 1 && counts.stats_writes == 2, "dirty statistics retry after failed write");
        bytes raw;
        expect(state.get_storage()->get_metadata("wpk_stats", raw) && raw.size() == 24 && raw[8] == 1, "retried write persists the miss counter");
        state.get_witness_pubkey_stats();
        expect(counts.stats_writes == 2, "successful diagnostic write is not repeated without new events");
    }
}

void paged_load() {
    constexpr size_t key_count = 2050;
    for (int mode = 0; mode < 3; ++mode) {
        Scratch scratch; Counts counts;
        auto db = backend(mode, scratch, counts);
        seed_position(*db);
        db->begin_batch();
        for (size_t i = 0; i < key_count; ++i) seed_key(*db, pubkey(i));
        expect(db->commit_batch(), "commit witness snapshot");
        if (mode != 0) { db.reset(); db = backend(mode, scratch, counts); }
        counts = {};
        state::ChainState state(std::move(db));
        std::cout << "WITNESS_LOAD backend=" << mode << " keys=" << key_count << " reads=" << counts.reads
                  << " full_scans=" << counts.full_scans << " largest_page=" << counts.largest_page << '\n';
        expect(counts.full_scans == 0 && counts.largest_page <= 1024, "witness loading bounds temporary key lists");
        expect(counts.reads == 2 * key_count, "witness loading reads each refcount and payload once");
        expect(state.has_derived_state_snapshot() && state.get_witness_pubkey_stats().total_keys == key_count, "paged loading preserves the complete valid key set");
        for (size_t i = 0; i < key_count; ++i) {
            bytes out;
            expect(state.get_witness_pubkey(crypto::sha256(pubkey(i)), out) && out == pubkey(i), "every loaded key is readable with its exact payload");
        }
        expect(counts.reads == 2 * key_count, "loaded keys are served from cache");
    }
}
}

int main(int argc, char** argv) {
    try {
        consensus::SelectParams(consensus::Network::LOADNET);
        const std::map<std::string, std::function<void()>> cases{
            {"missing-payload", [] { bad_read("missing-payload"); }}, {"missing-ref", [] { bad_read("missing-ref"); }},
            {"bad-count", [] { bad_read("bad-count"); }}, {"zero-count", [] { bad_read("zero-count"); }},
            {"wrong-hash", [] { bad_read("wrong-hash"); }}, {"wrong-size", [] { bad_read("wrong-size"); }},
            {"uppercase-load", uppercase_load}, {"clean-stats", clean_stats}, {"paged-load", paged_load}
        };
        if (argc == 2) cases.at(argv[1])();
        else { expect(argc == 1, "optional argument selects a witness scenario"); for (const auto& [name, run] : cases) run(); }
        std::cout << "Witness lookup regressions passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
