#include "geocoin/consensus/chainparams.hpp"
#include "geocoin/consensus/geo_tag.hpp"
#include "geocoin/consensus/optimized_miner.hpp"
#include "geocoin/consensus/proof_of_work.hpp"
#include "geocoin/utils/bytes.hpp"
#include "geocoin/core/blockchain.hpp"
#include "geocoin/crypto/geo_area.hpp"
#include "geocoin/geography/wgs84.hpp"
#include "geocoin/network/mempool.hpp"
#include "geocoin/nft/geometry_codec.hpp"
#include "geocoin/rpc/rpc_encoding.hpp"
#include "geocoin/rpc/rpc_geography.hpp"
#include "geocoin/rpc/rpc_server.hpp"
#include "geocoin/storage/read_status.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace geocoin::core {
struct BlockchainTestAccess {
    static chain::BlockIndex* insert(Blockchain& chain, const BlockHeader& header,
                                    chain::BlockIndex* prev, Height height) {
        auto index = std::make_unique<chain::BlockIndex>(header);
        index->prev = prev;
        index->height = height;
        auto* result = index.get();
        chain.block_index_.emplace(result->hash, std::move(index));
        return result;
    }
    static void activate(Blockchain& chain, chain::BlockIndex* tip) { chain.chain_active_.set_tip(tip); }
};
}
namespace geocoin::storage {
struct BlockStorageTestAccess {
    static bool put(BlockStorage& store, const std::string& key, const std::string& value) {
        return store.db_put(key, value, true);
    }
    static bool erase(BlockStorage& store, const std::string& key) { return store.db_delete(key, true); }
    static bool get(const BlockStorage& store, const std::string& key, std::string& value) {
        return store.db_get(key, value);
    }
};
}
namespace geocoin::rpc {
struct RPCServerTestAccess {
    static json call(RPCServer& server, const std::string& method, const json& params, RPCAuthRole role) {
        return server.handle_rpc_call({{"jsonrpc", "2.0"}, {"id", 1}, {"method", method}, {"params", params}}, role);
    }
};
}

namespace {
using namespace geocoin;
using rpc::json;
namespace geopow = consensus::geo;
size_t checks = 0;
void check(bool passed, const char* message) {
    ++checks;
    if (!passed) throw std::runtime_error(message);
}
void near(double actual, double expected, const char* message) {
    check(std::abs(actual - expected) < 1e-12, message);
}
struct Scratch {
    std::filesystem::path directory = std::filesystem::temp_directory_path() /
        ("geocoin_geography_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ~Scratch() { std::error_code ec; std::filesystem::remove_all(directory, ec); }
};
json call(rpc::RPCServer& server, const char* method, const json& params,
          rpc::RPCAuthRole role = rpc::RPCAuthRole::READONLY) {
    auto response = rpc::RPCServerTestAccess::call(server, method, params, role);
    if (response.contains("error")) throw std::runtime_error(response["error"].dump());
    return response.at("result");
}

// A real, independently mined header from the established degree chain.
// Its proof and geographic commitment must remain valid after upgrading.
void activation_contract() {
    const auto& main = consensus::Params(consensus::Network::MAIN);
    const Height activation = main.geopow_activation_height();
    check(activation == 2500, "mainnet activation is a fixed consensus height");
    check(consensus::Params(consensus::Network::TESTNET).geopow_activation_height() == 1 &&
          consensus::Params(consensus::Network::LOADNET).geopow_activation_height() == 1,
          "isolated networks use WGS84 from their first child");
    const auto header = core::BlockHeader::deserialize(utils::hex_to_bytes("010000000d1ac26a00000000e2c70b6a2e23dabee76868effa1d10aaf7a965cab05b888bb43a28b71a2a041d201f83712c7462471716e2e53956b62b598e1e67582b162d81abf296ffff001db9080000bd040000b47bd7313278642bccc5933fb2b42ce2ea779b6054816444d9e4e6b5bd1a3ce70b75040000000000"));
    const auto& genesis = main.get_genesis_block();
    check(utils::hash256_to_hex(header.get_hash()) ==
          "000000002a9532ecdf93474dbdf0775b9eac21003ecc003997d4e45b0c76bee7",
          "historical fixture has its independent block hash");
    const auto legacy = geopow::compute_block_geo_tag(genesis.header.timestamp, genesis.get_hash(), 1, activation);
    const auto modern = geopow::compute_block_geo_tag(genesis.header.timestamp, genesis.get_hash(), activation, activation);
    check(legacy.geo_tag == 5209795332281ULL && modern.geo_tag == 3397319134584ULL,
          "both selection algorithms match frozen vectors");
    check(legacy.lat_ndeg % 1000 == 0 && legacy.lon_ndeg % 1000 == 0,
          "historical point is converted to nanodegrees without rounding");
    check(consensus::ProofOfWork::verify_proof_of_work(header, header.bits,
          genesis.header.timestamp, genesis.get_hash(), 1, main), "real historical proof remains valid");
    check(!consensus::ProofOfWork::verify_proof_of_work(header, header.bits,
          genesis.header.timestamp, genesis.get_hash(), activation, main), "historical proof is forbidden after activation");
    check(consensus::ProofOfWork::compute_pow_hash(header, genesis.header.timestamp,
          genesis.get_hash(), 1) == header.get_hash(), "historical mining and block ID use the same commitment");
    check(consensus::ProofOfWork::verify_proof_of_work(genesis.header, genesis.header.bits, 0, {}, 0, main),
          "genesis proof is unchanged");
    struct EasyMainParams : consensus::MainNetParams {
        EasyMainParams() { pow_limit_bits_ = 0x207fffff; }
    } easy;
    std::array<core::Block, 3> candidates;
    for (size_t i = 0; i < candidates.size(); ++i) {
        const Height height = activation - 1 + i;
        auto& candidate = candidates[i];
        candidate.header = header;
        candidate.header.bits = easy.pow_limit_bits();
        candidate.header.nonce = 0;
        std::atomic<bool> running{true};
        std::atomic<uint64_t> hashes{0};
        consensus::OptimizedMiner miner;
        check(miner.mine_block_optimized(candidate, candidate.header.bits, genesis.header.timestamp,
              genesis.get_hash(), height, 1024, &running, -1, &hashes), "boundary template is mineable");
        check(consensus::ProofOfWork::verify_proof_of_work(candidate.header, candidate.header.bits,
              genesis.header.timestamp, genesis.get_hash(), height, easy), "boundary miner and verifier agree");
        check(candidate.header.geo_tag == (height < activation ? legacy.geo_tag : modern.geo_tag),
              "mining switches at the exact activation boundary");
    }
    check(!consensus::ProofOfWork::verify_proof_of_work(candidates[0].header, candidates[0].header.bits,
          genesis.header.timestamp, genesis.get_hash(), activation, easy), "old rule has no post-activation fallback");
    check(!consensus::ProofOfWork::verify_proof_of_work(candidates[1].header, candidates[1].header.bits,
          genesis.header.timestamp, genesis.get_hash(), activation - 1, easy), "new rule cannot be used before activation");
    consensus::SelectParams(consensus::Network::LOADNET);
    check(consensus::ProofOfWork::verify_proof_of_work(header, header.bits,
          genesis.header.timestamp, genesis.get_hash(), 1, main), "explicit-network validation ignores global selection");
    consensus::SelectParams(consensus::Network::MAIN);
    check(rpc::geopow_rules_to_json(activation - 1)["selection_rule"] == "historical_solar" &&
          rpc::geopow_rules_to_json(activation)["selection_rule"] == "wgs84_area" &&
          rpc::geopow_rules_to_json(activation)["algorithm_version"] == 1,
          "RPC publishes the same version-one activation schedule");
}

void coordinate_contract() {
    static_assert(geography::Wgs84Point{90'000'000'000, 180'000'000'000}.is_valid());
    static_assert(!geography::Wgs84Point{90'000'000'001, 0}.is_valid());
    check(std::string(geography::kGeographicCrs) == "EPSG:4326", "named axes use EPSG:4326");
    check(std::string(geography::kGeoJsonCrs) == "OGC:CRS84", "GeoJSON uses CRS84 order");
    check(geography::Wgs84Point{0, 100'000'000'000}.geojson_position() == std::array<double, 2>{100, 0},
          "GeoJSON point is longitude then latitude");
    near(geography::Wgs84Point{-33'868'800'000, 151'209'300'000}.latitude_degrees(), -33.8688, "signed latitude");
    near(geography::Wgs84Point{1, -1}.longitude_degrees(), -1e-9, "nanodegree storage precision");
    for (const auto p : std::array<geography::Wgs84Point, 4>{{{-90'000'000'000, -180'000'000'000},
             {90'000'000'000, 180'000'000'000}, {0, -180'000'000'000}, {0, 180'000'000'000}}})
        check(p.is_valid(), "poles and both dateline representations are valid");
    for (const auto p : std::array<geography::Wgs84Point, 4>{{{-90'000'000'001, 0}, {90'000'000'001, 0},
             {0, -180'000'000'001}, {(std::numeric_limits<int64_t>::max)(), 0}}})
        check(!p.is_valid(), "invalid coordinates are rejected without wrapping");
    const auto first = geopow::decode_geo_tag(0);
    const auto last = geopow::decode_geo_tag((uint64_t{1799} << 32) | 3599);
    check(first->geojson_bbox() == std::array<double, 4>{-180, -90, -179.9, -89.9}, "southwestern bbox");
    check(last->geojson_bbox() == std::array<double, 4>{179.9, 89.9, 180, 90}, "northeastern bbox");
    check(first->contains({-90'000'000'000, -180'000'000'000}), "lower boundaries belong to cell");
    check(!first->contains({-89'900'000'000, -180'000'000'000}), "upper boundary belongs to next cell");
    check(!geopow::decode_geo_tag(uint64_t{1800} << 32) && !geopow::decode_geo_tag(3600), "invalid cell indices");
    const auto genesis = geopow::compute_geo_tag(0, Hash256{});
    check(genesis.lat_ndeg == 48'851'583'000 && genesis.lon_ndeg == 8'897'278'000 &&
          genesis.geo_tag == 5'961'414'608'736ULL, "fixed genesis location and tag are unchanged");
    for (uint32_t t : {1U, 951782400U, 1700000000U, 4294967295U}) {
        Hash256 hash{}; hash[31] = 9;
        const auto computed = geopow::compute_geo_tag(t, hash);
        const auto restored = geopow::point_from_geo_tag(computed.geo_tag);
        check(restored->lat_ndeg == computed.lat_ndeg && restored->lon_ndeg == computed.lon_ndeg,
              "committed tag restores the exact canonical point without old timestamps");
    }
    const auto exact = rpc::geo_tag_to_json(genesis.geo_tag, genesis);
    check(exact["status"] == "exact" && exact["integer_unit"] == "nanodegree", "explicit integer contract");
    near(exact["point"]["coordinates"][0], 8.897278, "genesis longitude first");
    near(exact["point"]["coordinates"][1], 48.851583, "genesis latitude second");
    check(json::parse(exact.dump())["latitude_ndeg"] == genesis.lat_ndeg, "JSON preserves full integer precision");
    check(rpc::geo_tag_to_json(genesis.geo_tag)["status"] == "exact", "cell centre is defined without chain context");
    check(rpc::geo_tag_to_json(uint64_t{1800} << 32)["status"] == "invalid_tag", "invalid tag is diagnosed");
    core::Blockchain no_history(nullptr, nullptr);
    core::BlockHeader header; header.prev_block_hash[0] = 11; header.geo_tag = 123;
    check(rpc::block_geography_to_json(header, no_history)["status"] == "exact", "pruned MTP never removes point");
}

void rpc_activation_boundary() {
    Scratch scratch;
    auto store = std::make_shared<storage::BlockStorage>(scratch.directory.string());
    auto state = std::make_shared<state::ChainState>(std::make_unique<state::MemoryStorage>());
    auto chain = std::make_shared<core::Blockchain>(state, store);
    auto mempool = std::make_shared<network::Mempool>(state);
    check(chain->initialize(), "RPC activation fixture initializes genesis");
    rpc::RPCServer server;
    check(server.set_context({.blockchain = chain, .chain_state = state, .mempool = mempool}),
          "RPC activation fixture has chain context");
    Address reward;
    check(crypto::compute_address_from_private_key(bytes(32, 42), reward), "derive boundary reward address");
    const auto& genesis = consensus::Params().get_genesis_block();
    auto* root = chain->get_tip();
    const Height activation = consensus::Params().geopow_activation_height();
    for (Height height : {activation - 1, activation, activation + 1}) {
        auto parent_header = genesis.header;
        parent_header.prev_block_hash = genesis.get_hash();
        parent_header.nonce = height;
        auto* parent = core::BlockchainTestAccess::insert(*chain, parent_header, root, height - 1);
        core::BlockchainTestAccess::activate(*chain, parent);
        const auto templ = call(server, "getblocktemplate", json::array({json{{"address", reward.str()}}}),
                                rpc::RPCAuthRole::MINER);
        const auto expected = geopow::compute_block_geo_tag(static_cast<uint32_t>(parent->get_median_time_past()),
            parent->hash, height, activation);
        check(templ["height"] == height && templ["geo_tag"] == expected.geo_tag,
              "RPC template switches using the candidate height");
        check(templ["geography"]["latitude_ndeg"] == expected.lat_ndeg &&
              templ["geography"]["longitude_ndeg"] == expected.lon_ndeg,
              "RPC geography agrees with the mined commitment at the boundary");
        const auto info = call(server, "getblockchaininfo", json::array());
        check(info["geopow"]["activation_height"] == activation &&
              info["geopow"]["next_selection_rule"] == templ["geopow"]["selection_rule"],
              "chain status and external mining expose the same schedule");
    }
}

void rpc_and_pruning() {
    Scratch scratch;
    auto store = std::make_shared<storage::BlockStorage>(scratch.directory.string());
    check(store->is_open(), "temporary RocksDB opens");
    auto state = std::make_shared<state::ChainState>(std::make_unique<state::MemoryStorage>());
    auto chain = std::make_shared<core::Blockchain>(state, store);
    auto mempool = std::make_shared<network::Mempool>(state);
    const auto genesis = consensus::Params().get_genesis_block();
    check(store->save_block(genesis, 0) && store->set_active_block_and_tip(0, genesis.get_hash()), "persist genesis");
    auto* root = core::BlockchainTestAccess::insert(*chain, genesis.header, nullptr, 0);
    core::BlockchainTestAccess::activate(*chain, root);
    rpc::RPCServer server;
    check(server.set_context({.blockchain = chain, .chain_state = state, .mempool = mempool}), "install RPC context");
    const auto hash = rpc::hash256_to_hex_string(genesis.get_hash());
    const auto block = call(server, "getblock", json::array({hash}));
    check(block["geography"] == call(server, "getblockbyheight", json::array({0}))["geography"], "block geography endpoints agree");
    check(call(server, "getblock", json::array({hash, false})) == rpc::bytes_to_hex_string(genesis.serialize()), "raw block preserves consensus bytes");
    Address reward;
    check(crypto::compute_address_from_private_key(bytes(32, 42), reward), "derive reward address");
    const auto templ = call(server, "getblocktemplate", json::array({json{{"address", reward.str()}}}), rpc::RPCAuthRole::MINER);
    const auto expected = geopow::compute_block_geo_tag(genesis.header.timestamp, genesis.get_hash(), 1,
        consensus::Params().geopow_activation_height());
    check(templ["geography"]["latitude_ndeg"] == expected.lat_ndeg && templ["geography"]["longitude_ndeg"] == expected.lon_ndeg,
          "template uses parent-derived global selection");
    check(call(server, "powdeterminism", json::array())["geography"] == block["geography"], "diagnostics use same coordinates");
    const json shape{{"parts", json::array({json{{"exterior", json::array({
        json{{"lat_ndeg", 0}, {"lon_ndeg", 0}}, json{{"lat_ndeg", 0}, {"lon_ndeg", 1'000'000}},
        json{{"lat_ndeg", 1'000'000}, {"lon_ndeg", 0}}})}, {"holes", json::array()}}})}};
    const auto analysis = call(server, "analyzegeometry", json::array({shape, 1}));
    check(analysis["valid"] == true && analysis["geojson"]["type"] == "Polygon", "read-only geometry analysis exports display geometry");
    auto broken = shape; broken["parts"][0]["exterior"][1] = broken["parts"][0]["exterior"][0];
    const auto diagnostic = call(server, "analyzegeometry", json::array({broken}));
    check(diagnostic["valid"] == false && diagnostic["issue"]["code"] == "duplicate_position" &&
          diagnostic["issue"].contains("vertex"), "geometry diagnostics locate the input error");

    std::vector<core::Block> blocks{genesis};
    auto* previous = root;
    for (Height h = 1; h <= 8; ++h) {
        auto next = genesis;
        next.header.prev_block_hash = blocks.back().get_hash();
        next.header.timestamp += h * 600;
        next.header.nonce = h;
        next.header.geo_tag = geopow::compute_block_geo_tag(previous->get_median_time_past(), previous->hash, h,
            consensus::Params().geopow_activation_height()).geo_tag;
        check(store->save_block(next, h, false) && store->set_active_block_and_tip(h, next.get_hash()), "persist active header and height");
        previous = core::BlockchainTestAccess::insert(*chain, next.header, previous, h);
        blocks.push_back(std::move(next));
    }
    core::BlockchainTestAccess::activate(*chain, previous);
    const auto before = call(server, "listblockgeography", json::array({0, 100}));
    check(before.is_array() && before.size() == 9, "catalogue contains every active height");
    auto fork = blocks[1]; fork.header.nonce = 99; fork.header.geo_tag = 123;
    check(store->save_block(fork, 1, false), "persist alternative branch");
    uint256 work{}; work.data[0] = 100;
    store->set_transaction_archiver([](const core::Block&, Height) { return true; });
    const auto prune = [&](Height keep) {
        for (Height h = 0; h + 1 < keep; ++h) {
            if (store->has_block(blocks[h].get_hash())) check(store->prune_block_body_at_height(h), "prune body with archive callback");
        }
        uint64_t removed = 0;
        check(store->prune_block_indexes_below_height(keep, blocks[keep-1].get_hash(), blocks[keep-1].header, work, &removed),
              "compact header indexes and catalogue atomically");
    };
    prune(4);
    prune(7);
    const auto after = call(server, "listblockgeography", json::array({0, 100}));
    check(after.size() == before.size(), "repeated pruning preserves complete catalogue");
    for (size_t h = 0; h < blocks.size(); ++h) {
        check(before[h]["hash"] == after[h]["hash"] && before[h]["geography"] == after[h]["geography"],
              "exact location and active hash survive header deletion");
        check(after[h]["index_pruned"] == (h < 6), "archive contains only discarded headers, never retained duplicates");
    }
    check(!store->has_block(fork.get_hash()), "discarded fork never replaces the active catalogue");
    const auto page = call(server, "listblockgeography", json::array({3, 2}));
    check(page.size() == 2 && page[0]["height"] == 3 && page[1]["height"] == 4, "catalogue pagination crosses archived heights");
    check(call(server, "listblockgeography", json::array({9, 1})).empty(), "page beyond tip is empty");
    for (const auto params : {json::array({0, 0}), json::array({0, 1001}), json::array({-1}), json::array({1.5})})
        check(rpc::RPCServerTestAccess::call(server, "listblockgeography", params, rpc::RPCAuthRole::READONLY).contains("error"),
              "catalogue validates pagination bounds");
    // The retained suffix follows a reorganization without writing stale archive records.
    auto alternate = blocks.back(); alternate.header.nonce += 100;
    check(store->save_block(alternate, 8, false) && store->set_active_block_and_tip(8, alternate.get_hash()), "change retained active tip");
    check(call(server, "listblockgeography", json::array({8, 1}))[0]["hash"] == rpc::hash256_to_hex_string(alternate.get_hash()),
          "catalogue follows reorganization of retained history");
    store->close();
    check(store->open(scratch.directory.string()), "reopen pruned RocksDB");
    check(call(server, "listblockgeography", json::array({0, 8})).size() == 8, "persistent catalogue survives restart");
    using Access = storage::BlockStorageTestAccess;
    std::string original;
    check(Access::get(*store, "geo:0000000000", original) && original.size() == 40, "archive stores only hash and committed tag");
    for (const auto value : {std::string("short"), original + 'x', std::string(40, '\xff')}) {
        check(Access::put(*store, "geo:0000000000", value), "seed corrupt catalogue record");
        bool corrupt = false;
        try { storage::BlockStorage::BlockGeographyRecord record; store->get_block_geography(0, record); }
        catch (const storage::ReadError& error) { corrupt = error.status() == storage::ReadStatus::CORRUPT; }
        check(corrupt, "catalogue corruption is distinct from missing requested height");
    }
    check(Access::erase(*store, "geo:0000000000"), "remove expected archive record");
    bool missing_is_corrupt = false;
    try { storage::BlockStorage::BlockGeographyRecord record; store->get_block_geography(0, record); }
    catch (const storage::ReadError&) { missing_is_corrupt = true; }
    check(missing_is_corrupt, "missing historical catalogue entry is reported as corruption");
    check(Access::put(*store, "geo:0000000000", original), "restore catalogue fixture");
}
}

int main() {
    try {
        consensus::SelectParams(consensus::Network::MAIN);
        activation_contract();
        coordinate_contract();
        rpc_activation_boundary();
        rpc_and_pruning();
        std::cout << "Geography checks passed: " << checks << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "test_geography: " << error.what() << '\n';
        return 1;
    }
}
