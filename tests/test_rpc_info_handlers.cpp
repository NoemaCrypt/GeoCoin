#include "geocoin/consensus/consensus.hpp"
#include "geocoin/core/transaction.hpp"
#include "geocoin/core/blockchain.hpp"
#include "geocoin/core/genesis.hpp"
#include "geocoin/crypto/geo_area.hpp"
#include "geocoin/consensus/proof_of_work.hpp"
#include "geocoin/network/mempool.hpp"
#include "geocoin/nft/geodesy.hpp"
#include "geocoin/nft/nft_types.hpp"
#include "geocoin/node/node.hpp"
#include "geocoin/rpc/rpc_encoding.hpp"
#include "geocoin/rpc/rpc_server.hpp"
#include "geocoin/storage/block_storage.hpp"
#include "geocoin/utils/activation_manifest.hpp"
#include "geocoin/wallet/multisig.hpp"
#include "geocoin/wallet/wallet.hpp"
#include "rpc_test_utils.hpp"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using geocoin::test::expect_true;
using geocoin::test::find_free_port;
using geocoin::test::json;
using geocoin::test::send_rpc;

std::filesystem::path unique_test_dir()
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
        ("geocoin_rpc_info_handlers_" + std::to_string(now));
}

geocoin::bytes make_key(uint8_t seed)
{
    geocoin::bytes key(32);
    for (size_t i = 0; i < key.size(); ++i) {
        key[i] = static_cast<uint8_t>(seed + i);
    }
    return key;
}

std::string hash_hex(char fill)
{
    return std::string(64, fill);
}

std::string key_hex(uint8_t seed)
{
    return geocoin::rpc::bytes_to_hex_string(make_key(seed));
}

geocoin::Hash256 hash_from_seed(uint8_t seed)
{
    geocoin::Hash256 out{};
    for (size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<uint8_t>(seed + i);
    }
    return out;
}

geocoin::core::Block make_rpc_hashrate_block(
    geocoin::Height height,
    const geocoin::Hash256& prev_hash,
    const geocoin::Address& miner,
    const geocoin::Address& foundation
)
{
    geocoin::core::Block block;
    block.header.version = 1;
    block.header.prev_block_hash = prev_hash;
    block.header.timestamp = static_cast<uint32_t>(2000000000 + height * 60);
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

uint64_t projected_area_sqcm(const geocoin::nft::NFTMetadata& metadata)
{
    const double area = geocoin::nft::coordinates::measure_polygon(metadata.geometry.polygons.front().exterior)->area_sqm();
    return static_cast<uint64_t>(std::llround(area * 10000.0));
}

json call_ok(
    uint16_t port,
    const std::string& method,
    const json& params = json::array(),
    const std::string& user = "admin",
    const std::string& pass = "admin-pass"
)
{
    const auto response = send_rpc(port, user, pass, method, params);
    expect_true(response.status == 200, "RPC handler should return JSON-RPC HTTP 200");
    if (response.body.contains("error")) {
        throw std::runtime_error(method + " unexpectedly failed: " + response.body["error"].dump());
    }
    return response.body["result"];
}

void call_error(
    uint16_t port,
    const std::string& method,
    const json& params,
    int expected_code,
    const std::string& user = "admin",
    const std::string& pass = "admin-pass"
)
{
    const auto response = send_rpc(port, user, pass, method, params);
    expect_true(response.status == 200, "RPC handler error should return JSON-RPC HTTP 200");
    expect_true(response.body.contains("error"), "RPC handler should return JSON-RPC error");
    if (response.body["error"]["code"].get<int>() != expected_code) {
        throw std::runtime_error("RPC error code should match for " + method + ": expected " +
                                 std::to_string(expected_code) + ", got " + response.body["error"].dump());
    }
}

void test_misc_diagnostics(uint16_t port, const std::filesystem::path& data_dir)
{
    const json pow = call_ok(port, "powdeterminism", json::array(), "readonly", "readonly-pass");
    expect_true(pow["pow_hash"].get<std::string>().size() == 64, "powdeterminism returns pow hash");
    expect_true(pow["header"]["geo_tag"] == pow["geo_tag"], "powdeterminism header echoes geo tag");

    const json metrics = call_ok(port, "getmetrics", json::array(), "readonly", "readonly-pass");
    expect_true(metrics["gauges"].is_object(), "getmetrics returns gauges object");
    expect_true(metrics["counters"].is_object(), "getmetrics returns counters object");

    call_error(port, "getstorageinfo", json::array(),
               geocoin::rpc::RPC_FORBIDDEN_BY_SAFE_MODE, "readonly", "readonly-pass");
    const json storage = call_ok(port, "getstorageinfo", json::array(), "admin", "admin-pass");
    expect_true(storage["backend"] == "rocksdb", "getstorageinfo returns backend");
    expect_true(storage["profile"] == "safe", "getstorageinfo returns profile");
    expect_true(storage["directories"]["data"]["exists"], "getstorageinfo reports data directory");
    expect_true(storage["data_dir"].get<std::string>().find(data_dir.filename().string()) != std::string::npos,
                "getstorageinfo returns node data dir");
    expect_true(storage["block_storage"]["open"], "getstorageinfo reports open block storage");
    expect_true(storage["block_storage"]["format_version"] == 2,
                "getstorageinfo reports the current geocoin-storage/2 format");

    call_error(port, "getdbstats", json::array(),
               geocoin::rpc::RPC_FORBIDDEN_BY_SAFE_MODE, "readonly", "readonly-pass");
    const json dbstats = call_ok(port, "getdbstats", json::array(), "admin", "admin-pass");
    expect_true(dbstats["rocksdb"]["enabled"], "getdbstats reports rocksdb enabled");
    expect_true(dbstats["sizes"]["data"]["bytes"].is_number_unsigned(), "getdbstats returns data size");
    expect_true(dbstats["block_storage"]["size_mb"].is_number(), "getdbstats returns block storage size_mb");

    const json pruning = call_ok(port, "getpruninginfo", json::array(), "readonly", "readonly-pass");
    expect_true(pruning["pruned"], "getpruninginfo reports pruned node");
    expect_true(pruning["retention_blocks"] == 10, "getpruninginfo returns configured retention");
    expect_true(!pruning["delete_old_blocks"], "getpruninginfo returns delete-old-blocks flag");
    expect_true(pruning["archive"].is_boolean(), "getpruninginfo returns archive flag");
}

void test_blockchain_info(uint16_t port)
{
    expect_true(call_ok(port, "getblockcount", json::array(), "readonly", "readonly-pass").get<int64_t>() == 0,
                "getblockcount returns empty chain height");

    const json rejects = call_ok(port, "getblockrejectstats", json::array(), "readonly", "readonly-pass");
    expect_true(rejects["consensus"].is_object(), "getblockrejectstats returns consensus object");
    expect_true(rejects["policy"].is_object(), "getblockrejectstats returns policy object");

    const json witness = call_ok(port, "getwitnesspubkeystats", json::array(), "readonly", "readonly-pass");
    expect_true(witness["total_keys"].is_number_unsigned(), "getwitnesspubkeystats returns counters");

    const json interest = call_ok(port, "getinterestpool", json::array(), "readonly", "readonly-pass");
    expect_true(interest["balance_angles"].is_number_integer(), "getinterestpool returns balance");
    expect_true(interest["activation_height"].is_number_unsigned(), "getinterestpool returns activation height");

    const json chain = call_ok(port, "getblockchaininfo", json::array(), "readonly", "readonly-pass");
    expect_true(chain["chain"] == "degree", "getblockchaininfo returns selected chain");
    expect_true(chain["consensus_ruleset"] == "degree-lwma-maturity100-sidechain-v4-genesis20260926",
                "getblockchaininfo returns persistent consensus ruleset");
    expect_true(chain["blocks"] == 0, "getblockchaininfo returns height");
    expect_true(chain["pruned"], "getblockchaininfo returns pruned status");
    expect_true(chain["verificationprogress"] == 1.0, "getblockchaininfo returns verification progress");
    expect_true(chain["reorg_policy"]["policy_checkpoint"]["enabled"].is_boolean(),
                "getblockchaininfo returns reorg policy");

    const json genesis_hash = call_ok(port, "getblockhash", json::array({0}), "readonly", "readonly-pass");
    expect_true(genesis_hash.get<std::string>().size() == 64, "getblockhash returns genesis hash");
}

void test_block_query_info(uint16_t port)
{
    const std::string genesis_hash =
        call_ok(port, "getblockhash", json::array({0}), "readonly", "readonly-pass").get<std::string>();

    const json block = call_ok(port, "getblock", json::array({genesis_hash}), "readonly", "readonly-pass");
    expect_true(block["hash"] == genesis_hash, "getblock returns requested hash");
    expect_true(block["height"] == 0, "getblock returns genesis height");

    const json raw_block = call_ok(port, "getblock", json::array({genesis_hash, false}),
                                   "readonly", "readonly-pass");
    expect_true(raw_block.is_string(), "getblock verbose=false returns hex");
    expect_true(raw_block.get<std::string>().size() > 10, "getblock raw hex is non-empty");

    const json by_height = call_ok(port, "getblockbyheight", json::array({0}), "readonly", "readonly-pass");
    expect_true(by_height["hash"] == genesis_hash, "getblockbyheight returns genesis");
    const json by_height_raw = call_ok(port, "getblockbyheight", json::array({0, false}),
                                       "readonly", "readonly-pass");
    expect_true(by_height_raw.is_string(), "getblockbyheight verbose=false returns hex");

    const json children = call_ok(port, "getblockchildren", json::array({genesis_hash}),
                                  "readonly", "readonly-pass");
    expect_true(children.is_array(), "getblockchildren returns array");

    const json tree = call_ok(port, "getblocktree", json::array({genesis_hash, 1, 10}),
                              "readonly", "readonly-pass");
    expect_true(tree["root"]["hash"] == genesis_hash, "getblocktree returns root");
    expect_true(tree["nodes_returned"] == 1, "getblocktree returns visited count");

    call_error(port, "getblock", json::array({"not-a-hash"}), geocoin::rpc::RPC_INVALID_PARAMETER,
               "readonly", "readonly-pass");
    call_error(port, "getblockbyheight", json::array({999}), geocoin::rpc::RPC_INVALID_PARAMETER,
               "readonly", "readonly-pass");
    call_error(port, "getblockchildren", json::array({"bad"}), geocoin::rpc::RPC_INVALID_PARAMETER,
               "readonly", "readonly-pass");
    call_error(port, "getblocktree", json::array({genesis_hash, -1}), geocoin::rpc::RPC_INVALID_PARAMETER,
               "readonly", "readonly-pass");
    call_error(port, "getblocktree", json::array({genesis_hash, 1, 0}), geocoin::rpc::RPC_INVALID_PARAMETER,
               "readonly", "readonly-pass");
    call_error(port, "getblocktree", json::array({genesis_hash, 33}), geocoin::rpc::RPC_INVALID_PARAMETER,
               "readonly", "readonly-pass");
    call_error(port, "getblocktree", json::array({genesis_hash, 1, 5001}), geocoin::rpc::RPC_INVALID_PARAMETER,
               "readonly", "readonly-pass");
}

void test_availability_info(uint16_t port)
{
    const json range = call_ok(port, "getprunedrange", json::array(), "readonly", "readonly-pass");
    expect_true(range["node_type"] == "pruned", "getprunedrange returns node type");
    expect_true(range["first_available_height"] == 0, "getprunedrange returns first available height");

    const json by_height = call_ok(port, "getblockavailability", json::array({0}), "readonly", "readonly-pass");
    expect_true(by_height["found"], "getblockavailability reports initialized genesis height");
    expect_true(by_height["available"], "getblockavailability reports available genesis height");
    expect_true(by_height["reason"] == "available", "getblockavailability returns available reason");

    call_error(port, "getblockavailability", json::array({"bad-hash"}), geocoin::rpc::RPC_INVALID_PARAMETER,
               "readonly", "readonly-pass");

    const json listed = call_ok(port, "getavailableblocks", json::array({0, 1}), "readonly", "readonly-pass");
    expect_true(listed["count"] == 1, "getavailableblocks returns requested range size");
    expect_true(listed["blocks"][0]["height"] == 0, "getavailableblocks returns block height entry");
    expect_true(listed["blocks"][0]["available"], "getavailableblocks marks unpruned height available");
    expect_true(listed["blocks"][0]["hash"].get<std::string>().size() == 64,
                "getavailableblocks returns active hash");

    const json empty = call_ok(port, "getavailableblocks", json::array({0, 0}), "readonly", "readonly-pass");
    expect_true(empty["count"] == 0, "getavailableblocks supports zero count");

    call_error(port, "getavailableblocks", json::array({0, 5001}), geocoin::rpc::RPC_INVALID_PARAMETER,
               "readonly", "readonly-pass");
}

void test_pruning_and_sidechain_info(uint16_t port)
{
    const json prune = call_ok(port, "prunenow", json::array(), "admin", "admin-pass");
    expect_true(prune["result"] == "nothing_to_prune", "prunenow reports nothing on shallow chain");

    const json unprune = call_ok(port, "unprune", json::array({0}), "admin", "admin-pass");
    expect_true(unprune["result"] == "no_peers", "unprune reports no peers in isolated node");
    expect_true(unprune["blockhash"].get<std::string>().size() == 64, "unprune returns block hash");
    call_error(port, "unprune", json::array({"bad"}), geocoin::rpc::RPC_INVALID_PARAMETER,
               "admin", "admin-pass");
    call_error(port, "unprune", json::array({999}), geocoin::rpc::RPC_INVALID_PARAMETER,
               "admin", "admin-pass");

    const json genesis = call_ok(port, "createsidechaingenesis", json::array({json{
        {"chain_name", "test-sidechain"},
        {"meta", "metadata"},
        {"timestamp", 1},
        {"nonce", 2},
        {"version", 3},
    }}), "readonly", "readonly-pass");
    expect_true(genesis["genesis_hash"].get<std::string>().size() == 64,
                "createsidechaingenesis returns genesis hash");
    expect_true(genesis["payload_hex"].get<std::string>().size() > 10,
                "createsidechaingenesis returns generated payload");

    const json accounting_genesis = call_ok(port, "createsidechaingenesis", json::array({json{
        {"chain_name", "accounting-sidechain"},
        {"sidechain_kind", "accounting"},
        {"schema_hash", hash_hex('b')},
        {"timestamp", 3},
        {"nonce", 4},
    }}), "readonly", "readonly-pass");
    expect_true(accounting_genesis["sidechain_kind"] == "accounting" &&
                    accounting_genesis["schema_hash"] == hash_hex('b'),
                "createsidechaingenesis binds accounting kind and schema");
    call_error(port, "createsidechaingenesis", json::array({json{
                   {"sidechain_kind", "accounting"}
               }}), geocoin::rpc::RPC_INVALID_PARAMETER, "readonly", "readonly-pass");

    const json from_payload = call_ok(port, "createsidechaingenesis", json::array({json{
        {"payload_hex", "534347454e0100000000000000000000000000000000000000"},
    }}), "readonly", "readonly-pass");
    expect_true(from_payload["genesis_hash"].get<std::string>().size() == 64,
                "createsidechaingenesis accepts payload hex");
    call_error(port, "createsidechaingenesis", json::array({json{{"payload_hex", "bad"}}}),
               geocoin::rpc::RPC_INVALID_PARAMETER, "readonly", "readonly-pass");
    call_error(port, "createsidechaingenesis", json::array({json{{"timestamp", -1}}}),
               geocoin::rpc::RPC_INVALID_PARAMETER, "readonly", "readonly-pass");
    call_error(port, "createsidechaingenesis", json::array({json{{"version", 0x1'0000'0000ULL}}}),
               geocoin::rpc::RPC_INVALID_PARAMETER, "readonly", "readonly-pass");
    call_error(port, "createsidechaingenesis", json::array({json{
                   {"payload_hex", "00"}, {"nonce", 1}
               }}), geocoin::rpc::RPC_INVALID_PARAMETER, "readonly", "readonly-pass");
    call_error(port, "createsidechaingenesis", json::array({json::object(), 1}),
               geocoin::rpc::RPC_INVALID_PARAMS, "readonly", "readonly-pass");
    call_error(port, "getsidechaingenesis", json::array({hash_hex('a')}),
               geocoin::rpc::RPC_INVALID_PARAMETER, "readonly", "readonly-pass");
}

void test_control_info(uint16_t port)
{
    const json help = call_ok(port, "help", json::array(), "readonly", "readonly-pass");
    expect_true(help.is_string(), "help returns text");
    expect_true(help.get<std::string>().find("getblockcount") != std::string::npos,
                "help lists registered commands");

    const json getinfo_help = call_ok(port, "help", json::array({"getinfo"}), "readonly", "readonly-pass");
    expect_true(getinfo_help.is_string(), "help command returns text");
    expect_true(getinfo_help.get<std::string>().find("getinfo") != std::string::npos,
                "help command includes requested command");

    const json rpcinfo = call_ok(port, "getrpcinfo", json::array(), "readonly", "readonly-pass");
    expect_true(rpcinfo["command_count"].get<size_t>() > 0, "getrpcinfo returns commands");
    expect_true(rpcinfo["categories"].is_object(), "getrpcinfo returns categories");
    expect_true(rpcinfo["role_capabilities"]["admin"].is_array(), "getrpcinfo returns role capabilities");
    bool dumpprivkey_visible = false;
    bool getinfo_visible = false;
    for (const auto& command : rpcinfo["commands"]) {
        const auto name = command["name"].get<std::string>();
        if (name == "dumpprivkey") {
            dumpprivkey_visible = true;
        }
        if (name == "getinfo") {
            getinfo_visible = true;
        }
    }
    expect_true(getinfo_visible, "readonly getrpcinfo lists getinfo");
    expect_true(!dumpprivkey_visible, "readonly getrpcinfo must not list dumpprivkey");

    const json dump_help = call_ok(port, "help", json::array({"dumpprivkey"}), "readonly", "readonly-pass");
    expect_true(dump_help.get<std::string>().find("Method not found") != std::string::npos,
                "readonly help hides dumpprivkey text");

    const json info = call_ok(port, "getinfo", json::array(), "readonly", "readonly-pass");
    expect_true(info["wallet_loaded"], "getinfo reports loaded wallet");
    expect_true(info["blocks"] == 0, "getinfo reports block height");

    const json node = call_ok(port, "getnodeinfo", json::array(), "readonly", "readonly-pass");
    expect_true(node["running"], "getnodeinfo reports running node");
    expect_true(!node["p2p_listen"], "getnodeinfo reports disabled P2P listener");
    expect_true(node["limits"]["mempool_max_size"] == 4096, "getnodeinfo returns limits");

    const json sync = call_ok(port, "getsyncstatus", json::array(), "readonly", "readonly-pass");
    expect_true(sync["current_height"] == 0, "getsyncstatus returns current height");
    expect_true(sync["verificationprogress"].is_number(), "getsyncstatus returns progress");

    const json health = call_ok(port, "gethealth", json::array(), "readonly", "readonly-pass");
    expect_true(health["ok"], "gethealth reports ok isolated node");
    expect_true(health["components"]["mempool"] == "ok", "gethealth reports mempool component");
}

void test_health_degrades_for_missing_derived_state(
    uint16_t port,
    const std::shared_ptr<geocoin::state::ChainState>& chain_state
)
{
    chain_state->clear();
    chain_state->set_height(1);
    const json health = call_ok(port, "gethealth", json::array(), "readonly", "readonly-pass");
    expect_true(!health["ok"].get<bool>() && health["live"].get<bool>() &&
                    !health["ready"].get<bool>(),
                "gethealth is live but not ready when derived state requires rebuild");
    expect_true(health["status"] == "degraded" &&
                    health["chain_state"] == "rebuild-required" &&
                    health["components"]["chain_state"] == "rebuild-required",
                "gethealth exposes a precise derived-state recovery reason");
}

void test_network_rpc_info(uint16_t port)
{
    call_error(port, "getpeerinfo", json::array(),
               geocoin::rpc::RPC_FORBIDDEN_BY_SAFE_MODE, "readonly", "readonly-pass");
    const json peers = call_ok(port, "getpeerinfo", json::array(), "admin", "admin-pass");
    expect_true(peers.is_array() && peers.empty(), "getpeerinfo returns empty array for isolated node");

    const json connection_count = call_ok(port, "getconnectioncount", json::array(),
                                          "readonly", "readonly-pass");
    expect_true(connection_count == 0, "getconnectioncount reports no peers");

    call_error(port, "getaddednodeinfo", json::array(),
               geocoin::rpc::RPC_FORBIDDEN_BY_SAFE_MODE, "readonly", "readonly-pass");
    const json added_empty = call_ok(port, "getaddednodeinfo", json::array(),
                                     "admin", "admin-pass");
    expect_true(added_empty.is_array() && added_empty.empty(), "getaddednodeinfo starts empty");

    call_error(port, "addnode", json::array({"bad peer", "add"}), geocoin::rpc::RPC_INVALID_PARAMETER,
               "admin", "admin-pass");
    call_error(port, "addnode", json::array({"127.0.0.1:39999", "bad"}), geocoin::rpc::RPC_INVALID_PARAMETER,
               "admin", "admin-pass");

    const json add_result = call_ok(port, "addnode", json::array({"127.0.0.1:39999", "add"}),
                                    "admin", "admin-pass");
    expect_true(add_result.is_null(), "addnode add returns null on success");
    call_error(port, "addnode", json::array({"127.0.0.1:39999", "add"}),
               geocoin::rpc::RPC_CLIENT_NODE_ALREADY_ADDED, "admin", "admin-pass");

    const json added = call_ok(port, "getaddednodeinfo", json::array(), "admin", "admin-pass");
    expect_true(added.is_array() && added.size() == 1, "getaddednodeinfo returns added node");
    expect_true(added[0]["addr"] == "127.0.0.1:39999", "getaddednodeinfo normalizes endpoint");
    expect_true(!added[0]["connected"], "unreachable added node is disconnected");

    call_error(port, "getconnectiondiagnostics", json::array(),
               geocoin::rpc::RPC_FORBIDDEN_BY_SAFE_MODE, "readonly", "readonly-pass");
    const json diagnostics = call_ok(port, "getconnectiondiagnostics", json::array(),
                                     "admin", "admin-pass");
    expect_true(diagnostics["stats"]["total_peers"].is_number_unsigned(),
                "getconnectiondiagnostics returns stats object");
    expect_true(diagnostics["stats"]["encrypted_peers"] == 0,
                "getconnectiondiagnostics reports no encrypted peers for an isolated node");
    expect_true(diagnostics["outbound_attempts"].is_array(),
                "getconnectiondiagnostics returns outbound attempts");

    const json policy_stats = call_ok(port, "getp2ppolicystats", json::array(),
                                      "readonly", "readonly-pass");
    expect_true(policy_stats["reject_reasons"].is_object(), "getp2ppolicystats returns reject reasons");

    call_error(port, "disconnectnode", json::array({"127.0.0.1:39999"}),
               geocoin::rpc::RPC_CLIENT_NODE_NOT_CONNECTED, "admin", "admin-pass");

    const json remove_result = call_ok(port, "addnode", json::array({"127.0.0.1:39999", "remove"}),
                                       "admin", "admin-pass");
    expect_true(remove_result.is_null(), "addnode remove returns null on success");
    call_error(port, "addnode", json::array({"127.0.0.1:39999", "remove"}),
               geocoin::rpc::RPC_CLIENT_NODE_NOT_ADDED, "admin", "admin-pass");

    call_error(port, "setban", json::array({"203.0.113.7:3030", "bad"}),
               geocoin::rpc::RPC_INVALID_PARAMETER, "admin", "admin-pass");
    const json ban_added = call_ok(port, "setban", json::array({"203.0.113.7:3030", "add", 60}),
                                   "admin", "admin-pass");
    expect_true(ban_added.get<bool>(), "setban add returns true");
    call_error(port, "listbans", json::array(),
               geocoin::rpc::RPC_FORBIDDEN_BY_SAFE_MODE, "readonly", "readonly-pass");
    const json bans = call_ok(port, "listbans", json::array(), "admin", "admin-pass");
    expect_true(bans.is_array() && bans.size() == 1, "listbans returns active manual ban");
    expect_true(bans[0]["address"] == "203.0.113.7" && bans[0]["manual"].get<bool>(),
                "listbans normalizes banned endpoint to IP");

    const json ban_removed = call_ok(port, "setban", json::array({"203.0.113.7", "remove"}),
                                     "admin", "admin-pass");
    expect_true(ban_removed.get<bool>(), "setban remove returns true");
    call_error(port, "setban", json::array({"203.0.113.7", "remove"}),
               geocoin::rpc::RPC_INVALID_PARAMETER, "admin", "admin-pass");

    const json ban_added_again = call_ok(port, "setban", json::array({"203.0.113.8", "add"}),
                                         "admin", "admin-pass");
    expect_true(ban_added_again.get<bool>(), "setban add uses default bantime");
    const json clear_bans = call_ok(port, "clearbans", json::array(), "admin", "admin-pass");
    expect_true(clear_bans.get<bool>(), "clearbans returns true");
    const json bans_after_clear = call_ok(port, "listbans", json::array(), "admin", "admin-pass");
    expect_true(bans_after_clear.empty(), "clearbans removes active bans");
}

void test_nft_query_info(uint16_t port, const std::string& address)
{
    const json missing = call_ok(port, "getnft", json::array({hash_hex('b')}),
                                 "readonly", "readonly-pass");
    expect_true(missing["nft_id"] == hash_hex('b'), "getnft echoes requested id");
    expect_true(!missing["exists"], "getnft reports missing NFT");
    call_error(port, "getnft", json::array({"bad"}), geocoin::rpc::RPC_INVALID_PARAMETER,
               "readonly", "readonly-pass");

    const json by_owner = call_ok(port, "listnftsbyowner", json::array({address}),
                                  "readonly", "readonly-pass");
    expect_true(by_owner.is_array(), "listnftsbyowner returns array");
    expect_true(by_owner.empty(), "listnftsbyowner returns empty result for fresh address");
    call_error(port, "listnftsbyowner", json::array({address, 0, 1001}),
               geocoin::rpc::RPC_INVALID_PARAMETER, "readonly", "readonly-pass");

    const json utxos = call_ok(port, "getutxos", json::array({address}), "readonly", "readonly-pass");
    expect_true(utxos.is_array(), "getutxos returns array");
    call_error(port, "getutxos", json::array({address, 0, 5001}),
               geocoin::rpc::RPC_INVALID_PARAMETER, "readonly", "readonly-pass");
    call_error(port, "getaddressutxos", json::array({address, 0, 5001}),
               geocoin::rpc::RPC_INVALID_PARAMETER, "readonly", "readonly-pass");
}

void test_legacy_wallet_and_transaction_queries(
    uint16_t port,
    const std::string& address,
    const std::string& second_address
)
{
    const json transactions = call_ok(port, "listtransactions", json::array({"*", 10, 0}),
                                      "admin", "admin-pass");
    expect_true(transactions.is_array(), "listtransactions returns array");
    const json empty_count = call_ok(port, "listtransactions", json::array({"*", 0, 0}),
                                     "admin", "admin-pass");
    expect_true(empty_count.is_array() && empty_count.empty(),
                "listtransactions count=0 returns empty array");
    call_error(port, "listtransactions", json::array({"*", 10, 100001}),
               geocoin::rpc::RPC_INVALID_PARAMETER, "admin", "admin-pass");

    const json addresses = call_ok(port, "getaddressesbyaccount", json::array({"*"}),
                                   "admin", "admin-pass");
    expect_true(addresses.is_array(), "getaddressesbyaccount returns array");
    expect_true(!addresses.empty(), "getaddressesbyaccount returns wallet address");
    call_error(port, "getaddressesbyaccount", json::array({"*", 0, 5001}),
               geocoin::rpc::RPC_INVALID_PARAMETER, "admin", "admin-pass");

    const json fund = call_ok(port, "getfundbalance", json::array(), "readonly", "readonly-pass");
    expect_true(fund["address"].is_string(), "getfundbalance returns foundation address");
    expect_true(fund["expected_balance_angles"].is_number_integer(), "getfundbalance returns expected balance");

    const std::vector<geocoin::Address> multisig_participants = {
        geocoin::Address(address),
        geocoin::Address(second_address)
    };
    const auto multisig = geocoin::wallet::create_multisig_address(
        1, 2, multisig_participants);
    const json registered = call_ok(port, "registermultisig",
                                    json::array({multisig.address, 1, 2, address, second_address}),
                                    "admin", "admin-pass");
    expect_true(registered["registered"], "registermultisig stores config");
    expect_true(registered["m"] == 1, "registermultisig returns m");

    call_error(port, "registermultisig", json::array({multisig.address, 2, 1}),
               geocoin::rpc::RPC_INVALID_PARAMETER, "admin", "admin-pass");
    call_error(port, "registermultisig", json::array({multisig.address, 1, 2, address}),
               geocoin::rpc::RPC_INVALID_PARAMS, "admin", "admin-pass");

    call_error(port, "getrawtransaction", json::array({"bad"}), geocoin::rpc::RPC_INVALID_PARAMETER,
               "readonly", "readonly-pass");
    call_error(port, "getrawtransaction", json::array({hash_hex('c')}),
               geocoin::rpc::RPC_INVALID_ADDRESS_OR_KEY, "readonly", "readonly-pass");
    call_error(port, "getdelegationproof", json::array({hash_hex('d')}),
               geocoin::rpc::RPC_INVALID_ADDRESS_OR_KEY, "readonly", "readonly-pass");

    const json txout = call_ok(port, "gettxout", json::array({hash_hex('e'), 0}),
                               "readonly", "readonly-pass");
    expect_true(txout.is_null(), "gettxout returns null for unknown output");
    call_error(port, "gettxout", json::array({"bad", 0}), geocoin::rpc::RPC_INVALID_PARAMETER,
               "readonly", "readonly-pass");

    const json balance = call_ok(port, "getaddressbalance", json::array({address}),
                                 "readonly", "readonly-pass");
    expect_true(balance["address"] == address, "getaddressbalance echoes address");
    expect_true(balance["balance_angles"].is_number_integer(), "getaddressbalance returns amount");

    const json txids = call_ok(port, "getaddresstxids", json::array({address}),
                               "readonly", "readonly-pass");
    expect_true(txids.is_array(), "getaddresstxids returns array");
    const json history = call_ok(port, "getaddresshistory", json::array({address, true}),
                                 "readonly", "readonly-pass");
    expect_true(history.is_array(), "getaddresshistory returns array");

    const json nft_history = call_ok(port, "getnftshistory", json::array({hash_hex('f'), false}),
                                     "readonly", "readonly-pass");
    expect_true(nft_history.is_array(), "getnftshistory returns array");
    expect_true(call_ok(port, "getaddresstxids", json::array({address, 1, 1}),
                        "readonly", "readonly-pass").is_array(),
                "getaddresstxids accepts bounded pagination");
    call_error(port, "getaddresshistory", json::array({address, true, 0, 251}),
               geocoin::rpc::RPC_INVALID_PARAMETER, "readonly", "readonly-pass");
    call_error(port, "getnftshistory", json::array({hash_hex('f'), false, -1, 1}),
               geocoin::rpc::RPC_INVALID_PARAMETER, "readonly", "readonly-pass");
    call_error(port, "getnftshistory", json::array({"bad"}), geocoin::rpc::RPC_INVALID_PARAMETER,
               "readonly", "readonly-pass");
}

void test_signing_info(uint16_t port, const std::string& address)
{
    const json valid = call_ok(port, "validateaddress", json::array({address}),
                               "readonly", "readonly-pass");
    expect_true(valid["isvalid"], "validateaddress accepts wallet address");
    expect_true(!valid.contains("ismine"),
                "read-only validateaddress does not reveal wallet ownership");

    const json wallet_info = call_ok(port, "getwalletaddressinfo", json::array({address}),
                                     "admin", "admin-pass");
    expect_true(wallet_info["isvalid"] && wallet_info["ismine"],
                "admin wallet address info detects wallet-owned address");

    const json invalid = call_ok(port, "validateaddress", json::array({"bad-address"}),
                                 "readonly", "readonly-pass");
    expect_true(!invalid["isvalid"], "validateaddress rejects invalid address");

    const json falcon = call_ok(port, "getfalconinfo", json::array(), "readonly", "readonly-pass");
    expect_true(falcon["name"] == "FALCON-512", "getfalconinfo returns signature backend name");

    const json signed_message = call_ok(port, "signmessage", json::array({address, "hello"}),
                                        "admin", "admin-pass");
    expect_true(signed_message["signature"].get<std::string>().size() > 10,
                "signmessage returns signature");
    expect_true(signed_message["crypto"] == "FALCON-512", "signmessage returns crypto marker");

    const json verified = call_ok(port, "verifysignature",
                                  json::array({address, signed_message["signature"], "hello"}),
                                  "readonly", "readonly-pass");
    expect_true(verified.is_boolean(), "verifysignature returns boolean");
    expect_true(verified.get<bool>(), "verifysignature accepts matching signature");

    call_error(port, "verifysignature", json::array({"bad-address", "abcd", "hello"}),
               geocoin::rpc::RPC_INVALID_ADDRESS_OR_KEY, "readonly", "readonly-pass");
    call_error(port, "verifysignature", json::array({address, "zz", "hello"}),
               geocoin::rpc::RPC_INVALID_PARAMETER, "readonly", "readonly-pass");
}

void test_mempool_rpc_info(uint16_t port)
{
    const json txids = call_ok(port, "getrawmempool", json::array(), "readonly", "readonly-pass");
    expect_true(txids.is_array(), "getrawmempool returns txid array");
    expect_true(txids.empty(), "fresh mempool is empty");

    const json verbose = call_ok(port, "getrawmempool", json::array({true}), "readonly", "readonly-pass");
    expect_true(verbose.is_object(), "getrawmempool verbose returns object");
    expect_true(verbose.empty(), "fresh verbose mempool is empty");
    expect_true(call_ok(port, "getrawmempool", json::array({false, 10, 5}),
                        "readonly", "readonly-pass").empty(),
                "getrawmempool accepts bounded pagination");
    call_error(port, "getrawmempool", json::array({false, -1, 1}),
               geocoin::rpc::RPC_INVALID_PARAMETER, "readonly", "readonly-pass");
    call_error(port, "getrawmempool", json::array({false, 0, 5001}),
               geocoin::rpc::RPC_INVALID_PARAMETER, "readonly", "readonly-pass");
    call_error(port, "getrawmempool", json::array({true, 0, 251}),
               geocoin::rpc::RPC_INVALID_PARAMETER, "readonly", "readonly-pass");

    call_error(port, "getmempoolentry", json::array({"bad"}), geocoin::rpc::RPC_INVALID_PARAMETER,
               "readonly", "readonly-pass");
    call_error(port, "getmempoolentry", json::array({hash_hex('a')}),
               geocoin::rpc::RPC_INVALID_ADDRESS_OR_KEY, "readonly", "readonly-pass");

    call_error(port, "testmempoolaccept", json::array({json::array()}),
               geocoin::rpc::RPC_INVALID_PARAMETER, "readonly", "readonly-pass");
    json too_many = json::array();
    for (int i = 0; i < 26; ++i) {
        too_many.push_back("00");
    }
    call_error(port, "testmempoolaccept", json::array({too_many}),
               geocoin::rpc::RPC_INVALID_PARAMETER, "readonly", "readonly-pass");
    call_error(port, "testmempoolaccept", json::array({json::array({1})}),
               geocoin::rpc::RPC_INVALID_PARAMETER, "readonly", "readonly-pass");
    call_error(port, "testmempoolaccept", json::array({"bad"}),
               geocoin::rpc::RPC_INVALID_PARAMETER, "readonly", "readonly-pass");
}

void test_mining_and_mempool_info(uint16_t port, const std::string& wallet_address)
{
    const json mining = call_ok(port, "getmininginfo", json::array(), "miner", "miner-pass");
    expect_true(mining["blocks"] == 0, "getmininginfo returns chain height");
    expect_true(!mining["generate"], "getmininginfo reports mining stopped");
    expect_true(mining["pooledtx"] == 0, "getmininginfo reports empty mempool");
    expect_true(mining["genproclimit"] == 0, "getmininginfo reports no mining threads");

    const json mempool = call_ok(port, "getmempoolinfo", json::array(), "miner", "miner-pass");
    expect_true(mempool["size"] == 0, "getmempoolinfo returns empty mempool size");
    expect_true(mempool["bytes"] == 0, "getmempoolinfo returns empty mempool bytes");
    expect_true(mempool["maxmempool"] == 4096, "getmempoolinfo returns configured max size");

    const json hashrate = call_ok(port, "getnetworkhashps", json::array({120, -1}), "miner", "miner-pass");
    expect_true(hashrate.is_number(), "getnetworkhashps returns numeric result");
    expect_true(hashrate.get<double>() == 0.0, "getnetworkhashps returns zero for empty chain");

    const json zero_hashrate = call_ok(port, "getnetworkhashps", json::array({0, 0}), "miner", "miner-pass");
    expect_true(zero_hashrate.get<double>() == 0.0, "getnetworkhashps handles zero block window");

    geocoin::Address explicit_miner;
    expect_true(geocoin::crypto::compute_address_from_private_key(make_key(90), explicit_miner),
                "derive explicit mining address");
    const std::string miner_address = explicit_miner.str();
    const auto address_hex = [](const std::string& address) {
        std::string hex;
        hex.reserve(address.size() * 2);
        static constexpr char kDigits[] = "0123456789abcdef";
        for (unsigned char byte : address) {
            hex.push_back(kDigits[byte >> 4]);
            hex.push_back(kDigits[byte & 0x0f]);
        }
        return hex;
    };

    call_error(port, "getblocktemplate", json::array(),
               geocoin::rpc::RPC_INVALID_PARAMS, "miner", "miner-pass");
    call_error(port, "getblocktemplate", json::array({json{{"timestamp", 1}}}),
               geocoin::rpc::RPC_INVALID_PARAMS, "miner", "miner-pass");

    const json templ = call_ok(
        port,
        "getblocktemplate",
        json::array({json{{"address", miner_address}}}),
        "miner",
        "miner-pass"
    );
    expect_true(templ["height"] == 1, "getblocktemplate returns next block height");
    const std::string block_hex = templ["blockhex"].get<std::string>();
    expect_true(block_hex.find(address_hex(wallet_address)) == std::string::npos,
                "getblocktemplate does not embed the node wallet address");
    expect_true(block_hex.find(address_hex(miner_address)) != std::string::npos,
                "getblocktemplate pays the explicit mining address");
    expect_true(templ["previousblockhash"].get<std::string>().size() == 64,
                "getblocktemplate returns previous block hash");
    expect_true(templ["target"].get<std::string>().size() == 64, "getblocktemplate returns target");
    expect_true(templ["blockhex"].get<std::string>().size() > 100, "getblocktemplate returns serialized block");
    expect_true(templ["coinbasevalue"].get<int64_t>() > 0, "getblocktemplate returns positive coinbase value");
    expect_true(templ["transactions"].is_array() && templ["transactions"].empty(),
                "empty mempool template contains no non-coinbase transactions");

    const auto requested_timestamp =
        geocoin::core::GENESIS_TIMESTAMP + geocoin::consensus::params::TARGET_BLOCK_TIME;
    const json timestamped_template = call_ok(
        port,
        "getblocktemplate",
        json::array({json{{"address", miner_address}, {"timestamp", requested_timestamp}}}),
        "miner",
        "miner-pass"
    );
    expect_true(timestamped_template["curtime"] == requested_timestamp,
                "getblocktemplate honors a consensus-valid requested timestamp");
    call_error(
        port,
        "getblocktemplate",
        json::array({json{{"address", miner_address}, {"timestamp", -1}}}),
        geocoin::rpc::RPC_INVALID_PARAMETER,
        "miner",
        "miner-pass"
    );
    call_error(
        port,
        "getblocktemplate",
        json::array({json{{"address", miner_address}, {"timestamp", uint64_t{1} << 32}}}),
        geocoin::rpc::RPC_INVALID_PARAMETER,
        "miner",
        "miner-pass"
    );

    const json string_address_template = call_ok(
        port,
        "getblocktemplate",
        json::array({[] {
            geocoin::Address address;
            expect_true(geocoin::crypto::compute_address_from_private_key(make_key(90), address),
                        "derive canonical mining address");
            return address.str();
        }()}),
        "miner",
        "miner-pass"
    );
    expect_true(string_address_template["height"] == 1, "getblocktemplate accepts address string parameter");
    call_error(port, "getblocktemplate", json::array({"geo1-miner-template-address"}),
               geocoin::rpc::RPC_INVALID_ADDRESS_OR_KEY, "miner", "miner-pass");
    call_error(port, "getblocktemplate", json::array({17}),
               geocoin::rpc::RPC_INVALID_PARAMETER, "miner", "miner-pass");
    call_error(port, "getblocktemplate", json::array({json{{"mode", "proposal"}}}),
               geocoin::rpc::RPC_INVALID_PARAMETER, "miner", "miner-pass");

    const std::string genesis_hash =
        call_ok(port, "getblockhash", json::array({0}), "readonly", "readonly-pass").get<std::string>();
    const std::string genesis_hex =
        call_ok(port, "getblock", json::array({genesis_hash, false}), "readonly", "readonly-pass").get<std::string>();
    const json duplicate_submit = call_ok(port, "submitblock", json::array({genesis_hex}), "miner", "miner-pass");
    expect_true(duplicate_submit.is_null() || duplicate_submit.is_string(),
                "submitblock handles a valid serialized block without RPC decode error");

    call_error(port, "submitblock", json::array({"zz"}), geocoin::rpc::RPC_DESERIALIZATION_ERROR,
               "miner", "miner-pass");
    call_error(port, "submitblock", json::array({}), geocoin::rpc::RPC_INVALID_PARAMS,
               "miner", "miner-pass");
    call_error(port, "setgenerate", json::array({true, 1000000}), geocoin::rpc::RPC_INVALID_PARAMETER,
               "admin", "admin-pass");

    const json stopped = call_ok(port, "setgenerate", json::array({false}), "admin", "admin-pass");
    expect_true(stopped.is_null(), "setgenerate false succeeds without starting CPU mining");
}

void test_mining_hashrate_rpc_on_synthetic_chain()
{
    const std::filesystem::path root = unique_test_dir() / "hashrate-chain";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);

    auto storage = std::make_shared<geocoin::storage::BlockStorage>(root.string());
    expect_true(storage->is_open(), "synthetic hashrate block storage opens");

    auto chain_state = std::make_shared<geocoin::state::ChainState>(
        std::make_unique<geocoin::state::MemoryStorage>());
    chain_state->set_block_storage(storage);

    auto blockchain = std::make_shared<geocoin::core::Blockchain>(chain_state, storage);
    expect_true(blockchain->initialize(), "synthetic hashrate blockchain initializes");
    auto mempool = std::make_shared<geocoin::network::Mempool>(chain_state);

    geocoin::Hash256 prev_hash{};
    expect_true(blockchain->get_active_block_hash_by_height(0, prev_hash),
                "synthetic hashrate genesis hash resolves");

    std::vector<std::unique_ptr<geocoin::chain::BlockIndex>> indexes;
    indexes.reserve(12);

    geocoin::chain::BlockIndex* prev_index = blockchain->get_tip();
    for (geocoin::Height height = 1; height <= 12; ++height) {
        const auto block = make_rpc_hashrate_block(
            height,
            prev_hash,
            geocoin::Address("GEOminer-rpc-hashrate"),
            chain_state->get_foundation_address()
        );

        int32_t file = 0;
        uint32_t pos = 0;
        expect_true(storage->save_block_with_pos(block, height, file, pos, false),
                    "synthetic hashrate block saves");

        auto index = std::make_unique<geocoin::chain::BlockIndex>(block.header);
        index->hash = block.get_hash();
        index->prev = prev_index;
        index->height = height;
        index->block_pos.file = file;
        index->block_pos.pos = pos;
        index->status = geocoin::chain::BlockStatus::HAVE_DATA;
        index->chain_work = geocoin::consensus::calculate_chainwork(prev_index, block.header.bits);

        geocoin::consensus::ValidationState state;
        expect_true(blockchain->connect_tip(state, index.get()),
                    "synthetic hashrate block connects");

        prev_hash = block.get_hash();
        prev_index = index.get();
        indexes.push_back(std::move(index));
    }

    geocoin::rpc::RPCServer server;
    expect_true(server.set_context(geocoin::rpc::RPCServerContext{
        .blockchain = blockchain,
        .chain_state = chain_state,
        .mempool = mempool,
    }), "synthetic hashrate RPC context installs atomically");

    const uint16_t port = find_free_port();
    expect_true(server.start(
        "127.0.0.1",
        port,
        "admin",
        "admin-pass",
        "miner",
        "miner-pass",
        "readonly",
        "readonly-pass"
    ), "synthetic hashrate RPC server starts");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    const json mining = call_ok(port, "getmininginfo", json::array(), "miner", "miner-pass");
    expect_true(mining["blocks"] == 12, "getmininginfo sees synthetic chain height");
    expect_true(mining["networkhashps"].get<double>() > 0.0,
                "getmininginfo estimates network hashrate on non-empty chain");
    expect_true(!mining["generate"], "getmininginfo reports stopped mining without node");

    const json recent = call_ok(port, "getnetworkhashps", json::array({5, 12}), "miner", "miner-pass");
    expect_true(recent.get<double>() > 0.0, "getnetworkhashps returns positive recent window");

    const json expand_same_height_window =
        call_ok(port, "getnetworkhashps", json::array({1, 2}), "miner", "miner-pass");
    expect_true(expand_same_height_window.get<double>() > 0.0,
                "getnetworkhashps expands one-block window to find elapsed time");

    const json clamped = call_ok(port, "getnetworkhashps", json::array({50000, 12}), "miner", "miner-pass");
    expect_true(clamped.get<double>() > 0.0, "getnetworkhashps clamps oversized window to height");

    const json missing_height = call_ok(port, "getnetworkhashps", json::array({5, 100}), "miner", "miner-pass");
    expect_true(missing_height.get<double>() == 0.0, "getnetworkhashps returns zero for missing height");

    const json zero_window = call_ok(port, "getnetworkhashps", json::array({0, 12}), "miner", "miner-pass");
    expect_true(zero_window.get<double>() == 0.0, "getnetworkhashps returns zero for zero window");

    const json first_height = call_ok(port, "getnetworkhashps", json::array({1, 1}), "miner", "miner-pass");
    expect_true(first_height.get<double>() == 0.0, "getnetworkhashps returns zero without elapsed time");

    const std::string genesis_hash =
        call_ok(port, "getblockhash", json::array({0}), "readonly", "readonly-pass").get<std::string>();
    const json children = call_ok(port, "getblockchildren", json::array({genesis_hash}),
                                  "readonly", "readonly-pass");
    expect_true(children.is_array() && children.size() == 1,
                "synthetic chain exposes genesis child");
    expect_true(children[0].get<std::string>().size() == 64,
                "getblockchildren returns child hash");

    const json depth_limited_tree = call_ok(
        port,
        "getblocktree",
        json::array({genesis_hash, 1, 100}),
        "readonly",
        "readonly-pass"
    );
    expect_true(depth_limited_tree["root"]["children"].is_array() &&
                    depth_limited_tree["root"]["children"].size() == 1,
                "getblocktree includes direct child");
    expect_true(depth_limited_tree["root"]["children"][0]["children"].is_array() &&
                    depth_limited_tree["root"]["children"][0]["children"].empty(),
                "getblocktree stops at max depth");

    const json node_limited_tree = call_ok(
        port,
        "getblocktree",
        json::array({genesis_hash, 12, 2}),
        "readonly",
        "readonly-pass"
    );
    expect_true(node_limited_tree["nodes_returned"] == 2,
                "getblocktree stops at max node limit");

    const std::string tip_hash =
        call_ok(port, "getblockhash", json::array({12}), "readonly", "readonly-pass").get<std::string>();
    call_error(port, "switchtotip", json::array({tip_hash}),
               geocoin::rpc::RPC_INTERNAL_ERROR, "admin", "admin-pass");

    const json miner_txids = call_ok(
        port,
        "getaddresstxids",
        json::array({"GEOminer-rpc-hashrate"}),
        "readonly",
        "readonly-pass"
    );
    expect_true(miner_txids.is_array() && !miner_txids.empty(),
                "synthetic chain indexes miner address txids");
    const json miner_history = call_ok(
        port,
        "getaddresshistory",
        json::array({"GEOminer-rpc-hashrate", true}),
        "readonly",
        "readonly-pass"
    );
    expect_true(miner_history.is_array() && !miner_history.empty(),
                "verbose getaddresshistory returns indexed miner refs");
    expect_true(miner_history[0].contains("tx"),
                "verbose getaddresshistory includes transaction details");

    chain_state->set_prune_depth(5);
    const json pruned = call_ok(port, "prunenow", json::array({7, 2}), "admin", "admin-pass");
    expect_true(pruned["result"] == "ok", "prunenow prunes synthetic chain witness range");
    expect_true(pruned["target_height"] <= 7, "prunenow reports bounded target height");

    server.stop();
    storage->close();
    std::filesystem::remove_all(root, ec);
}

void fund_rpc_wallet_chainstate(geocoin::node::Node& node, const geocoin::wallet::Wallet& wallet)
{
    auto chain_state = node.get_chain_state();
    expect_true(chain_state != nullptr, "node chainstate is available");

    const auto coinbase = geocoin::core::Transaction::create_coinbase(
        1,
        wallet.get_address(),
        25 * geocoin::COIN
    );
    expect_true(chain_state->apply_transaction(coinbase, 1, true), "test coinbase applies to chainstate");
    chain_state->set_height(geocoin::consensus::params::COINBASE_MATURITY + 3);
}

struct RpcCreatedNft {
    geocoin::Hash256 nft_id{};
    geocoin::Hash256 txid{};
};

RpcCreatedNft create_rpc_test_nft(geocoin::node::Node& node, geocoin::wallet::Wallet& wallet)
{
    auto chain_state = node.get_chain_state();
    expect_true(chain_state != nullptr, "node chainstate is available for NFT test");
    chain_state->set_activation_manifest(geocoin::utils::ActivationManifest::fully_enabled_for_tests("degree"));

    geocoin::nft::NFTMetadata metadata;
    metadata.name = "RPC NFT";
    metadata.description = "RPC query coverage asset";
    metadata.image_uri = "ipfs://rpc-nft";
    metadata.content_hash = std::string(64, 'b');
    metadata.geometry.polygons.front().exterior = {
        {56000000000LL, 37000000000LL},
        {56000000000LL, 37000100000LL},
        {56000100000LL, 37000000000LL},
    };
    metadata.created_at = 1700000100;
    metadata.block_height = chain_state->get_height() + 1;
    metadata.block_hash = hash_from_seed(0x51);
    metadata.creator = wallet.get_address().str();
    metadata.location_name = "RPC plot";
    metadata.tags = {"rpc", "coverage"};

    geocoin::nft::NFTMetadata::CadastralData cadastral;
    cadastral.cadastral_number = "77:01:0001001:4242";
    cadastral.owner_name = "RPC Owner";
    cadastral.land_category = "Testing";
    cadastral.permitted_use = "Coverage";
    cadastral.area_sqcm_official = projected_area_sqcm(metadata);
    cadastral.ownership_document_hash = hash_from_seed(0x52);
    cadastral.technical_plan_hash = hash_from_seed(0x53);
    cadastral.photo_hashes = {hash_from_seed(0x54), hash_from_seed(0x55)};
    cadastral.market_value = geocoin::COIN;
    cadastral.cadastral_value = geocoin::COIN / 2;
    cadastral.last_valuation_date = 1700000200;
    cadastral.restrictions = {"none"};
    cadastral.has_buildings = true;
    cadastral.building_count = 2;
    metadata.cadastral = cadastral;

    geocoin::core::Transaction tx;
    expect_true(wallet.create_nft_create(
                    metadata,
                    250,
                    geocoin::consensus::params::MIN_TX_FEE,
                    chain_state,
                    tx
                ),
                "wallet creates RPC NFT_CREATE transaction");
    expect_true(chain_state->validate_transaction(tx), "RPC NFT_CREATE validates");
    const geocoin::Height nft_height = chain_state->get_height() + 1;
    expect_true(chain_state->apply_transaction(tx, nft_height, false), "RPC NFT_CREATE applies");
    chain_state->set_height(nft_height);
    expect_true(tx.nft_create.has_value(), "RPC NFT_CREATE has payload");
    return RpcCreatedNft{tx.nft_create->nft_id, tx.get_hash()};
}

void test_blockchain_transaction_queries_existing(
    uint16_t port,
    const std::string& owner_address,
    const RpcCreatedNft& created
)
{
    const std::string genesis_hash =
        call_ok(port, "getblockhash", json::array({0}), "readonly", "readonly-pass").get<std::string>();
    const json genesis = call_ok(port, "getblock", json::array({genesis_hash}),
                                 "readonly", "readonly-pass");
    expect_true(genesis["tx"].is_array() && !genesis["tx"].empty(),
                "genesis block exposes indexed coinbase txid");
    const std::string indexed_txid_hex = genesis["tx"][0].get<std::string>();

    const json raw_tx = call_ok(port, "getrawtransaction", json::array({indexed_txid_hex}),
                                "readonly", "readonly-pass");
    expect_true(raw_tx.is_string() && raw_tx.get<std::string>().size() > 20,
                "getrawtransaction returns raw tx hex for indexed transaction");

    const json verbose_tx = call_ok(port, "getrawtransaction", json::array({indexed_txid_hex, 1}),
                                    "readonly", "readonly-pass");
    expect_true(verbose_tx["txid"] == indexed_txid_hex, "verbose getrawtransaction echoes txid");
    expect_true(verbose_tx["outputs"].is_array() && !verbose_tx["outputs"].empty(),
                "verbose getrawtransaction returns outputs");

    call_error(port, "getdelegationproof", json::array({indexed_txid_hex}),
               geocoin::rpc::RPC_INVALID_PARAMETER, "readonly", "readonly-pass");
    call_error(port, "getdelegationproof", json::array({"bad"}),
               geocoin::rpc::RPC_INVALID_PARAMETER, "readonly", "readonly-pass");

    const json address_utxos = call_ok(port, "getaddressutxos", json::array({owner_address}),
                                       "readonly", "readonly-pass");
    expect_true(address_utxos.is_array() && !address_utxos.empty(),
                "existing tx query test requires address UTXO");
    const json first_utxo = address_utxos[0];
    const json txout = call_ok(
        port,
        "gettxout",
        json::array({first_utxo["txid"], first_utxo["vout"]}),
        "readonly",
        "readonly-pass"
    );
    expect_true(!txout.is_null(), "gettxout returns existing UTXO");
    expect_true(txout["address"] == owner_address, "gettxout returns UTXO address");
    expect_true(txout["value_angles"].is_number_integer(), "gettxout returns UTXO value");

    const json owner_txids = call_ok(port, "getaddresstxids", json::array({owner_address}),
                                     "readonly", "readonly-pass");
    expect_true(owner_txids.is_array(), "getaddresstxids returns an array for owner address");

    const json owner_history = call_ok(port, "getaddresshistory", json::array({owner_address, true}),
                                       "readonly", "readonly-pass");
    expect_true(owner_history.is_array(), "verbose getaddresshistory returns an array for owner address");

    const std::string nft_id_hex = geocoin::rpc::hash256_to_hex_string(created.nft_id);
    const json nft_history = call_ok(port, "getnftshistory", json::array({nft_id_hex, true}),
                                     "readonly", "readonly-pass");
    expect_true(nft_history.is_array(), "verbose getnftshistory returns an array for existing NFT id");
}

void test_nft_query_existing(uint16_t port, const std::string& owner_address, const geocoin::Hash256& nft_id)
{
    const std::string nft_id_hex = geocoin::rpc::hash256_to_hex_string(nft_id);
    const json nft = call_ok(port, "getnft", json::array({nft_id_hex}), "readonly", "readonly-pass");
    expect_true(nft["exists"].get<bool>(), "getnft reports existing NFT");
    const auto feature = call_ok(port, "getnftgeojson", json::array({nft_id_hex, 0.1}), "readonly", "readonly-pass");
    expect_true(feature["type"] == "Feature" && feature["id"] == nft_id_hex &&
        feature["properties"]["native_geometry"] == nft["metadata"]["geometry"] &&
        feature["properties"]["display_tolerance_m"] == 0.1,
        "GeoJSON export includes native geometry for exact NFT recovery");
    const auto geometry_analysis = call_ok(port, "analyzegeometry", json::array({feature["properties"]["native_geometry"]}),
        "readonly", "readonly-pass");
    expect_true(geometry_analysis["valid"] == true && geometry_analysis["area_sqcm"] == nft["geography"]["area_sqcm"],
        "stateless geometry analysis and stored NFT report the same precise area");
    std::vector<geocoin::nft::NFTMetadata::GeoPoint> polygon;
    for (const auto& p : nft["metadata"]["geometry"]["parts"][0]["exterior"])
        polygon.push_back({p["lat_ndeg"].get<int64_t>(), p["lon_ndeg"].get<int64_t>()});
    const auto measured = *geocoin::nft::coordinates::measure_polygon(polygon);
    expect_true(nft["geography"]["library"] == "GeographicLib" &&
                nft["geography"]["library_version"] == "2.3.0" &&
                nft["geography"]["rules_version"] == 1 &&
                nft["geography"]["edge_model"] == "shortest_wgs84_geodesic" &&
                std::abs(nft["geography"]["area_sqm"].get<double>() - measured.area_sqm()) < 1e-6 &&
                std::abs(nft["geography"]["perimeter_m"].get<double>() - measured.perimeter_m) < 1e-6,
                "getnft exposes pinned WGS84 geodesic measurements");
    expect_true(nft["owner"] == owner_address, "getnft returns current owner");
    expect_true(nft["metadata"]["name"] == "RPC NFT", "getnft returns metadata name");
    expect_true(nft["metadata"]["geometry"]["parts"][0]["exterior"].is_array() && nft["metadata"]["geometry"]["parts"][0]["exterior"].size() == 3,
                "getnft returns polygon points");
    expect_true(nft["metadata"]["tags"].is_array() && nft["metadata"]["tags"].size() == 2,
                "getnft returns tags");
    expect_true(nft["metadata"]["cadastral"]["cadastral_number"] == "77:01:0001001:4242",
                "getnft returns cadastral data");
    expect_true(nft["metadata"]["cadastral"]["photo_hashes"].size() == 2,
                "getnft returns cadastral photo hashes");
    expect_true(nft["metadata"]["cadastral"]["building_count"] == 2,
                "getnft returns cadastral building count");

    const json by_owner = call_ok(port, "listnftsbyowner", json::array({owner_address}),
                                  "readonly", "readonly-pass");
    expect_true(by_owner.is_array() && !by_owner.empty(), "listnftsbyowner returns owned NFT");
    expect_true(by_owner[0]["nft_id"] == nft_id_hex, "listnftsbyowner returns NFT id");
    expect_true(by_owner[0]["name"] == "RPC NFT", "listnftsbyowner returns NFT name");
    expect_true(by_owner[0]["image_uri"] == "ipfs://rpc-nft", "listnftsbyowner returns image URI");

    const json legacy_utxos = call_ok(port, "getutxos", json::array({owner_address}),
                                      "readonly", "readonly-pass");
    expect_true(legacy_utxos.is_array() && !legacy_utxos.empty(), "getutxos returns owner UTXOs");
    expect_true(legacy_utxos[0]["txid"].get<std::string>().size() == 64, "getutxos returns txid");
    expect_true(legacy_utxos[0]["address"] == owner_address, "getutxos returns UTXO address");
    expect_true(legacy_utxos[0]["height"].is_number_unsigned(), "getutxos returns UTXO height");
}

void test_wallet_txbuilder_rpc(
    uint16_t port,
    const std::string& wallet_address,
    const std::string& recipient_address
)
{
    const json utxos = call_ok(port, "listunspent", json::array(), "admin", "admin-pass");
    expect_true(utxos.is_array() && !utxos.empty(), "txbuilder test requires wallet UTXO");
    const std::string txid = utxos[0]["txid"].get<std::string>();
    const uint32_t vout = utxos[0]["vout"].get<uint32_t>();

    const json raw = call_ok(port, "createrawtransaction", json::array({
        json::array({json{{"txid", txid}, {"vout", vout}, {"sequence", 12345}}}),
        json::array({
            json{{"address", recipient_address}, {"amount", 0.25}},
            json{{"address", wallet_address}, {"value", 0.50}},
            json{{"address", wallet_address}, {"value_angles", 100}},
        }),
        7,
    }), "admin", "admin-pass");
    expect_true(raw.is_string() && raw.get<std::string>().size() > 20,
                "createrawtransaction returns raw transaction hex");

    const json decoded = call_ok(port, "decoderawtransaction", json::array({raw}),
                                 "readonly", "readonly-pass");
    expect_true(decoded["inputs"].is_array() && decoded["inputs"].size() == 1,
                "decoderawtransaction returns input list");
    expect_true(decoded["outputs"].is_array() && decoded["outputs"].size() == 3,
                "decoderawtransaction returns output list");
    expect_true(decoded["outputs"][0]["address"] == recipient_address,
                "decoderawtransaction returns output address");
    expect_true(decoded["lock_time"] == 7 && decoded["inputs"][0]["sequence"] == 12345,
                "decoderawtransaction exposes lock and sequence before signing");

    const json signed_tx = call_ok(port, "signrawtransactionwithwallet", json::array({raw}),
                                   "admin", "admin-pass");
    expect_true(signed_tx["complete"].get<bool>(), "signrawtransactionwithwallet signs wallet UTXO");
    expect_true(signed_tx["errors"].empty(), "signrawtransactionwithwallet returns no errors for wallet UTXO");
    expect_true(signed_tx["hex"].get<std::string>().size() >= raw.get<std::string>().size(),
                "signrawtransactionwithwallet returns signed hex");
    const auto signed_decoded = call_ok(port, "decoderawtransaction", json::array({signed_tx["hex"]}),
                                      "readonly", "readonly-pass");
    expect_true(signed_decoded["lock_time"] == 7 && signed_decoded["inputs"][0]["sequence"] == 12345,
                "wallet signing preserves the complete finality intent");

    const json raw_missing = call_ok(port, "createrawtransaction", json::array({
        json::array({json{{"txid", hash_hex('9')}, {"vout", 0}}}),
        json{{recipient_address, 0.1}},
    }), "admin", "admin-pass");
    const json missing_signed = call_ok(port, "signrawtransactionwithwallet", json::array({raw_missing}),
                                        "admin", "admin-pass");
    expect_true(!missing_signed["complete"].get<bool>(), "signrawtransactionwithwallet reports missing UTXO");
    expect_true(missing_signed["missing_inputs"] == 1, "signrawtransactionwithwallet counts missing inputs");

    geocoin::core::Transaction non_standard;
    non_standard.type = geocoin::core::TransactionType::CADASTRAL_DOC;
    non_standard.timestamp = 1700000300;
    const std::string non_standard_hex = geocoin::rpc::bytes_to_hex_string(non_standard.serialize());
    const json non_standard_signed = call_ok(port, "signrawtransactionwithwallet",
                                             json::array({non_standard_hex}), "admin", "admin-pass");
    expect_true(!non_standard_signed["complete"].get<bool>(),
                "signrawtransactionwithwallet rejects unsupported transaction type");
    expect_true(non_standard_signed["errors"].size() >= 2,
                "signrawtransactionwithwallet reports no-input and type errors");

    call_error(port, "createrawtransaction", json::array({json::array(), json{{recipient_address, 1}}}),
               geocoin::rpc::RPC_INVALID_PARAMETER, "admin", "admin-pass");
    call_error(port, "createrawtransaction", json::array({json::array({1}), json{{recipient_address, 1}}}),
               geocoin::rpc::RPC_INVALID_PARAMETER, "admin", "admin-pass");
    call_error(port, "createrawtransaction",
               json::array({json::array({json{{"txid", "bad"}, {"vout", 0}}}), json{{recipient_address, 1}}}),
               geocoin::rpc::RPC_INVALID_PARAMETER, "admin", "admin-pass");
    call_error(port, "createrawtransaction",
               json::array({json::array({json{{"txid", txid}, {"vout", vout}}}), json::array()}),
               geocoin::rpc::RPC_INVALID_PARAMETER, "admin", "admin-pass");
    call_error(port, "createrawtransaction",
               json::array({json::array({json{{"txid", txid}, {"vout", vout}}}), json::array({1})}),
               geocoin::rpc::RPC_INVALID_PARAMETER, "admin", "admin-pass");
    call_error(port, "createrawtransaction",
               json::array({json::array({json{{"txid", txid}, {"vout", vout}}}),
                            json::array({json{{"address", recipient_address}}})}),
               geocoin::rpc::RPC_INVALID_PARAMETER, "admin", "admin-pass");
    call_error(port, "createrawtransaction",
               json::array({json::array({json{{"txid", txid}, {"vout", vout}}}), 1}),
               geocoin::rpc::RPC_INVALID_PARAMETER, "admin", "admin-pass");
    call_error(port, "decoderawtransaction", json::array({"00"}),
               geocoin::rpc::RPC_INVALID_PARAMETER, "readonly", "readonly-pass");
    call_error(port, "signrawtransactionwithwallet", json::array({"00"}),
               geocoin::rpc::RPC_INVALID_PARAMETER, "admin", "admin-pass");
}

void test_wallet_basic_rpc(uint16_t port, const std::string& wallet_address, const std::string& recipient_address)
{
    const json balance = call_ok(port, "getbalance", json::array(), "admin", "admin-pass");
    expect_true(balance.is_number(), "getbalance returns numeric balance");
    expect_true(balance.get<double>() > 0.0, "getbalance sees funded wallet");

    const json balances = call_ok(port, "listaddressbalances", json::array(), "admin", "admin-pass");
    expect_true(balances.is_object(), "listaddressbalances returns object");
    expect_true(balances.contains(wallet_address), "listaddressbalances includes funded wallet address");
    call_error(port, "listaddressbalances", json::array({0, 5001}),
               geocoin::rpc::RPC_INVALID_PARAMETER, "admin", "admin-pass");

    const json utxos = call_ok(port, "listunspent", json::array(), "admin", "admin-pass");
    expect_true(utxos.is_array() && !utxos.empty(), "listunspent returns funded UTXO");
    expect_true(utxos[0]["address"] == wallet_address, "listunspent returns wallet address");
    expect_true(utxos[0]["spendable"].get<bool>(), "mature coinbase is spendable");

    const json fresh_only = call_ok(port, "listunspent", json::array({1, 1}), "admin", "admin-pass");
    expect_true(fresh_only.is_array() && !fresh_only.empty(),
                "listunspent maxconf filter returns fresh NFT change UTXO");
    for (const auto& utxo : fresh_only) {
        expect_true(utxo["confirmations"] == 1, "listunspent maxconf filter keeps only one-conf UTXOs");
    }

    const json mature_only = call_ok(port, "listunspent", json::array({2, 9999999}), "admin", "admin-pass");
    expect_true(mature_only.empty(), "listunspent minconf filter excludes fresh NFT change UTXO");

    const json address_utxos = call_ok(
        port,
        "getaddressutxos",
        json::array({wallet_address}),
        "readonly",
        "readonly-pass"
    );
    expect_true(address_utxos.is_array() && !address_utxos.empty(), "getaddressutxos returns funded address UTXO");
    expect_true(!address_utxos[0]["coinbase"].get<bool>(),
                "getaddressutxos reports NFT change output as non-coinbase");

    const json empty_address_utxos = call_ok(
        port,
        "getaddressutxos",
        json::array({recipient_address}),
        "readonly",
        "readonly-pass"
    );
    expect_true(empty_address_utxos.is_array() && empty_address_utxos.empty(),
                "getaddressutxos returns empty array for unfunded address");

    const json fee_rate = call_ok(port, "getfeerate", json::array(), "readonly", "readonly-pass");
    expect_true(fee_rate["recommended_fee_rate_per_kb"].is_number_unsigned(), "getfeerate returns recommended fee");
    expect_true(fee_rate["recommended_fee_rate_per_kb_angles"] ==
                    fee_rate["recommended_fee_rate_per_kb"],
                "getfeerate labels its legacy rate as angles");
    expect_true(fee_rate["recommended_fee_rate_per_kb_gc"].is_number_float(),
                "getfeerate exposes the rate in GC");
    expect_true(fee_rate["min_tx_fee_angles"] == geocoin::consensus::params::MIN_TX_FEE,
                "getfeerate exposes the flat minimum in angles");
    expect_true(fee_rate["max_congestion_multiplier"] == 16,
                "getfeerate reports the bounded congestion multiplier");
    expect_true(fee_rate["fee_rate_unit"] == "angles_per_kb",
                "getfeerate reports explicit rate units");
    expect_true(fee_rate["mempool_txs"] == 0, "getfeerate reports empty mempool before send");

    // Consensus rejects outputs below the dust limit; sendtoaddress must not
    // build, sign and record such a payment.
    call_error(port, "sendtoaddress", json::array({recipient_address, 0.000005}),
               geocoin::rpc::RPC_INVALID_PARAMETER, "admin", "admin-pass");

    const json txid = call_ok(
        port,
        "sendtoaddress",
        json::array({recipient_address, 1.0, 0.00000001}),
        "admin",
        "admin-pass"
    );
    expect_true(txid.is_string() && txid.get<std::string>().size() == 64, "sendtoaddress returns txid");

    const json mempool = call_ok(port, "getrawmempool", json::array(), "readonly", "readonly-pass");
    expect_true(mempool.is_array() && mempool.size() == 1, "sendtoaddress submits tx to mempool");
    expect_true(mempool[0] == txid, "mempool contains sendtoaddress txid");

    const json verbose_mempool = call_ok(port, "getrawmempool", json::array({true}), "readonly", "readonly-pass");
    expect_true(verbose_mempool.is_object() && verbose_mempool.contains(txid.get<std::string>()),
                "verbose mempool exposes submitted tx entry");
    const json verbose_entry = verbose_mempool[txid.get<std::string>()];
    expect_true(verbose_entry["txid"] == txid, "verbose mempool entry echoes txid");
    expect_true(verbose_entry["hex"].get<std::string>().size() > 20, "verbose mempool entry includes raw tx hex");
    expect_true(verbose_entry["depends"].is_array() && !verbose_entry["depends"].empty(),
                "verbose mempool entry lists non-coinbase input dependency");

    const json mempool_entry = call_ok(
        port,
        "getmempoolentry",
        json::array({txid}),
        "readonly",
        "readonly-pass"
    );
    expect_true(mempool_entry["txid"] == txid, "getmempoolentry returns submitted tx");
    expect_true(mempool_entry["fee"].is_number_integer(), "getmempoolentry returns fee");
    const auto pending_raw = call_ok(port, "getrawtransaction", json::array({txid, 0}), "readonly", "readonly-pass");
    expect_true(pending_raw == mempool_entry["hex"], "getrawtransaction finds the accepted mempool transaction");
    const auto pending_verbose = call_ok(port, "getrawtransaction", json::array({txid, 1}), "readonly", "readonly-pass");
    expect_true(pending_verbose["txid"] == txid, "verbose getrawtransaction finds the accepted mempool transaction");

    {
        // Regression: listunspent reported outputs already spent by a pending
        // mempool transaction as spendable.
        const auto& spent_input = pending_verbose["inputs"].at(0);
        const json after_send = call_ok(port, "listunspent", json::array({0, 9999999}), "admin", "admin-pass");
        bool spent_input_listed = false;
        for (const auto& utxo : after_send) {
            if (utxo["txid"] == spent_input["prev_tx_hash"] && utxo["vout"] == spent_input["vout"]) {
                spent_input_listed = true;
                expect_true(!utxo["spendable"].get<bool>(),
                            "listunspent marks an output spent in the mempool as not spendable");
            }
        }
        expect_true(spent_input_listed, "listunspent still lists the confirmed output");

        // Regression: sendtoaddress ignored mempool spends and re-selected the
        // same output, so the second send failed as a mempool conflict and
        // left an unconfirmed entry in wallet history.
        bool has_free_output = false;
        for (const auto& utxo : after_send) {
            has_free_output = has_free_output || utxo["spendable"].get<bool>();
        }
        expect_true(!has_free_output, "the fixture's only spendable output is pending in the mempool");
        call_error(port, "sendtoaddress", json::array({recipient_address, 1.0}),
                   geocoin::rpc::RPC_WALLET_ERROR, "admin", "admin-pass");
        const json mempool_after_retry = call_ok(port, "getrawmempool", json::array(), "readonly", "readonly-pass");
        expect_true(mempool_after_retry.size() == 1, "a send without free outputs adds nothing to the mempool");

        // Regression: listtransactions paged from the oldest entry.
        const json newest = call_ok(port, "listtransactions", json::array({"*", 1, 0}), "admin", "admin-pass");
        expect_true(newest.is_array() && newest.size() == 1 && newest[0]["txid"] == txid,
                    "listtransactions returns the most recent wallet transaction first");
    }

    const json mempool_accept = call_ok(
        port,
        "testmempoolaccept",
        json::array({json::array({verbose_entry["hex"]})}),
        "readonly",
        "readonly-pass"
    );
    expect_true(mempool_accept.is_array() && mempool_accept.size() == 1,
                "testmempoolaccept returns one result for real raw tx");
    expect_true(mempool_accept[0]["txid"] == txid, "testmempoolaccept result includes txid");
    expect_true(mempool_accept[0].contains("allowed"), "testmempoolaccept result includes allowed flag");
    expect_true(!mempool_accept[0]["allowed"].get<bool>(),
                "testmempoolaccept rejects a transaction already present in mempool");
    expect_true(mempool_accept[0].contains("reject_reason") &&
                    !mempool_accept[0]["reject_reason"].get<std::string>().empty(),
                "testmempoolaccept reports a duplicate mempool reason");

    const json raw_verify = call_ok(
        port,
        "verifyrawtransaction",
        json::array({verbose_entry["hex"]}),
        "readonly",
        "readonly-pass"
    );
    expect_true(raw_verify["txid"] == txid, "verifyrawtransaction returns txid");
    expect_true(raw_verify["verified"].is_boolean(), "verifyrawtransaction returns verified flag");
    expect_true(raw_verify["valid"].is_boolean(), "verifyrawtransaction returns chainstate validation flag");
    expect_true(raw_verify["contextual"].is_boolean(), "verifyrawtransaction returns contextual flag");

    call_error(port, "verifyrawtransaction", json::array({"not-hex"}),
               geocoin::rpc::RPC_INVALID_PARAMETER, "readonly", "readonly-pass");
    call_error(port, "sendrawtransaction", json::array({"not-hex"}),
               geocoin::rpc::RPC_INVALID_PARAMETER, "admin", "admin-pass");
    call_error(port, "sendrawtransaction", json::array({verbose_entry["hex"]}),
               geocoin::rpc::RPC_VERIFY_ERROR, "admin", "admin-pass");

    const json mempool_info_after = call_ok(port, "getmempoolinfo", json::array(), "miner", "miner-pass");
    expect_true(mempool_info_after["size"] == 1, "getmempoolinfo sees submitted tx");
    expect_true(mempool_info_after["bytes"].get<uint64_t>() > 0, "getmempoolinfo reports non-zero bytes");

    const json fee_rate_after = call_ok(port, "getfeerate", json::array(), "readonly", "readonly-pass");
    expect_true(fee_rate_after["mempool_txs"] == 1, "getfeerate reports non-empty mempool after send");

    const json mining_template = call_ok(
        port,
        "getblocktemplate",
        json::array({json{{"address", wallet_address}}}),
        "miner",
        "miner-pass"
    );
    expect_true(mining_template["transactions"].is_array() && mining_template["transactions"].size() == 1,
                "getblocktemplate includes mempool transaction after sendtoaddress");
    expect_true(mining_template["transactions"][0]["txid"] == txid,
                "getblocktemplate returns mempool txid");
    expect_true(mining_template["transactions"][0]["fee"].get<int64_t>() >= 1,
                "getblocktemplate returns mempool transaction fee");
    expect_true(mining_template["transactions"][0]["data"].get<std::string>().size() > 20,
                "getblocktemplate returns serialized mempool transaction");

    call_error(port, "sendtoaddress", json::array({recipient_address}),
               geocoin::rpc::RPC_INVALID_PARAMS, "admin", "admin-pass");
    call_error(port, "getaddressutxos", json::array(),
               geocoin::rpc::RPC_INVALID_PARAMS, "readonly", "readonly-pass");
}

void test_wallet_account_rpc(
    uint16_t port,
    geocoin::wallet::Wallet& wallet,
    const std::string& unknown_wallet_address,
    const std::filesystem::path& root
)
{
    struct DefaultWalletPathGuard {
        std::string old_path;
        ~DefaultWalletPathGuard()
        {
            geocoin::wallet::Wallet::set_default_wallet_path(old_path);
        }
    } default_path_guard{geocoin::wallet::Wallet::get_default_wallet_path()};

    const auto rpc_wallet_path = root / "rpc-wallet-account.dat";
    geocoin::wallet::Wallet::set_default_wallet_path(rpc_wallet_path.string());

    call_error(port, "createwallet", json::array(), geocoin::rpc::RPC_INVALID_PARAMETER,
               "admin", "admin-pass");
#ifdef _WIN32
    _putenv_s("GEOCOIN_ALLOW_PLAINTEXT_WALLET", "1");
#else
    setenv("GEOCOIN_ALLOW_PLAINTEXT_WALLET", "1", 1);
#endif
    const json created = call_ok(port, "createwallet", json::array(), "admin", "admin-pass");
    expect_true(created["address"].get<std::string>().size() > 10, "createwallet returns new address");
    expect_true(!created["encrypted"].get<bool>(), "createwallet without password reports unencrypted");
    expect_true(std::filesystem::exists(rpc_wallet_path), "createwallet persists wallet to default path");

    expect_true(wallet.load_from_file(rpc_wallet_path.string(), ""), "load wallet with its own path");
    const auto unrelated_default = root / "unrelated-wallet.dat";
    geocoin::wallet::Wallet::set_default_wallet_path(unrelated_default.string());
    const json new_address = call_ok(port, "getnewaddress", json::array({"rpc-label"}), "admin", "admin-pass");
    geocoin::wallet::Wallet reopened_wallet;
    expect_true(reopened_wallet.load_from_file(rpc_wallet_path.string(), "") &&
                reopened_wallet.has_key(geocoin::Address(new_address.get<std::string>())),
                "RPC saves new keys to the loaded wallet path");
    expect_true(!std::filesystem::exists(unrelated_default), "RPC does not save into another default path");

    expect_true(new_address.is_string() && new_address.get<std::string>().size() > 10,
                "getnewaddress returns a new wallet address");

    call_error(port, "importprivkey", json::array({"abcd"}), geocoin::rpc::RPC_INVALID_PARAMETER,
               "admin", "admin-pass");
    const std::string imported_key = key_hex(201);
    const json imported = call_ok(port, "importprivkey", json::array({imported_key, "imported-label"}),
                                  "admin", "admin-pass");
    expect_true(imported["label"] == "imported-label", "importprivkey returns requested label");
    expect_true(imported["address"].get<std::string>().size() > 10, "importprivkey returns imported address");
    expect_true(imported["rescan_requested"].get<bool>() &&
                    !imported["rescan_complete"].get<bool>() &&
                    imported["rescan_status"] == "read-error",
                "importprivkey attempts history recovery by default and reports an incomplete synthetic chain");
    expect_true(imported["next_height"].is_number_unsigned(),
                "importprivkey returns a durable rescan continuation height");

    const json resumed_import = call_ok(
        port,
        "importprivkey",
        json::array({imported_key, "ignored-on-retry", "", true, imported["next_height"]}),
        "admin",
        "admin-pass"
    );
    expect_true(resumed_import["address"] == imported["address"] &&
                    resumed_import["label"] == "imported-label" &&
                    resumed_import["rescan_requested"].get<bool>(),
                "importprivkey is idempotent so an incomplete scan can resume from next_height");

    const json imported_without_scan = call_ok(
        port,
        "importprivkey",
        json::array({key_hex(203), "no-rescan", "", false}),
        "admin",
        "admin-pass"
    );
    expect_true(!imported_without_scan["rescan_requested"].get<bool>() &&
                    imported_without_scan["rescan_complete"].get<bool>() &&
                    imported_without_scan["rescan_status"] == "disabled",
                "importprivkey only skips history when rescan is explicitly disabled");

    const json dumped = call_ok(port, "dumpprivkey", json::array({imported["address"]}),
                                "admin", "admin-pass");
    expect_true(dumped == imported_key, "dumpprivkey returns imported private key");
    call_error(port, "dumpprivkey", json::array({unknown_wallet_address}), geocoin::rpc::RPC_WALLET_ERROR,
               "admin", "admin-pass");

    wallet.remember_password("rpc-secret");
    call_error(port, "getnewaddress", json::array({"missing-password"}), geocoin::rpc::RPC_WALLET_ERROR,
               "admin", "admin-pass");
    call_error(port, "getnewaddress", json::array({"bad-password", "wrong"}), geocoin::rpc::RPC_WALLET_ERROR,
               "admin", "admin-pass");

    const json protected_address = call_ok(
        port,
        "getnewaddress",
        json::array({"protected-label", "rpc-secret"}),
        "admin",
        "admin-pass"
    );
    expect_true(protected_address.is_string() && protected_address.get<std::string>().size() > 10,
                "getnewaddress accepts correct wallet password");

    call_error(port, "importprivkey", json::array({key_hex(202), "protected-import"}),
               geocoin::rpc::RPC_WALLET_ERROR, "admin", "admin-pass");
    call_error(port, "dumpprivkey", json::array({protected_address}), geocoin::rpc::RPC_WALLET_ERROR,
               "admin", "admin-pass");
    call_error(port, "signmessage", json::array({protected_address, "hello"}),
               geocoin::rpc::RPC_WALLET_ERROR, "admin", "admin-pass");
    const json protected_signed = call_ok(
        port,
        "signmessage",
        json::array({protected_address, "hello", "rpc-secret"}),
        "admin",
        "admin-pass"
    );
    expect_true(protected_signed["crypto"] == "FALCON-512",
                "signmessage accepts correct wallet password");
    call_error(port, "sendtoaddress", json::array({unknown_wallet_address, 0.00000001}),
               geocoin::rpc::RPC_WALLET_ERROR, "admin", "admin-pass");
    call_error(
        port,
        "signrawtransactionwithwallet",
        json::array({"00"}),
        geocoin::rpc::RPC_WALLET_ERROR,
        "admin",
        "admin-pass"
    );
    call_error(
        port,
        "createnft",
        json::array({json{{"metadata", json{{"name", "locked"}}}}}),
        geocoin::rpc::RPC_WALLET_ERROR,
        "admin",
        "admin-pass"
    );
    const json protected_key = call_ok(port, "dumpprivkey", json::array({protected_address, "rpc-secret"}),
                                       "admin", "admin-pass");
    expect_true(protected_key.is_string() && protected_key.get<std::string>().size() == 64,
                "dumpprivkey accepts correct wallet password");
    wallet.clear_password_cache();
#ifdef _WIN32
    _putenv_s("GEOCOIN_ALLOW_PLAINTEXT_WALLET", "");
#else
    unsetenv("GEOCOIN_ALLOW_PLAINTEXT_WALLET");
#endif
}

} // namespace

int main()
{
    setenv("GEOCOIN_RPC_RATE_LIMIT_MAX_REQUESTS_PER_WINDOW", "1000", 1);

    const std::filesystem::path root = unique_test_dir();
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);

    try {
        geocoin::node::NodeConfig config;
        config.data_dir = root.string();
        config.enable_auto_connect = false;
        config.enable_p2p_listen = false;
        config.mempool_max_size = 4096;
        config.node_type = geocoin::node::NodeType::PRUNED;
        config.pruned_block_retention = 10;
        config.pruned_delete_old_blocks = false;
        config.pruned_delete_old_indexes = false;

        auto node = std::make_shared<geocoin::node::Node>(config);
        expect_true(node->start(make_key(1)), "failed to start isolated test node");
        node->get_peer_manager()->set_outbound_connect_timeout(std::chrono::milliseconds(1));

        auto wallet = std::make_shared<geocoin::wallet::Wallet>();
        expect_true(wallet->initialize(make_key(42)), "failed to initialize test wallet");
        auto second_wallet = std::make_shared<geocoin::wallet::Wallet>();
        expect_true(second_wallet->initialize(make_key(43)), "failed to initialize second test wallet");

        geocoin::rpc::RPCServer server;
        const auto components = node->component_handle();
        expect_true(server.set_context(geocoin::rpc::RPCServerContext{
            .blockchain = components.blockchain,
            .wallet = wallet,
            .chain_state = components.chain_state,
            .mempool = components.mempool,
            .peer_manager = components.peer_manager,
            .chain_mutations = components.services.mutations,
            .node_services = components.services,
        }), "full RPC context installs atomically");

        const uint16_t port = find_free_port();
        expect_true(server.start(
            "127.0.0.1",
            port,
            "admin",
            "admin-pass",
            "miner",
            "miner-pass",
            "readonly",
            "readonly-pass"
        ), "failed to start RPC server");
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        test_control_info(port);
        test_network_rpc_info(port);
        test_misc_diagnostics(port, root);
        test_blockchain_info(port);
        test_block_query_info(port);
        test_availability_info(port);
        test_pruning_and_sidechain_info(port);
        test_nft_query_info(port, wallet->get_address().str());
        test_legacy_wallet_and_transaction_queries(
            port,
            wallet->get_address().str(),
            second_wallet->get_address().str()
        );
        test_signing_info(port, wallet->get_address().str());
        test_mempool_rpc_info(port);
        test_mining_and_mempool_info(port, wallet->get_address().str());
        test_mining_hashrate_rpc_on_synthetic_chain();
        fund_rpc_wallet_chainstate(*node, *wallet);
        const RpcCreatedNft rpc_nft = create_rpc_test_nft(*node, *wallet);
        test_blockchain_transaction_queries_existing(port, wallet->get_address().str(), rpc_nft);
        test_nft_query_existing(port, wallet->get_address().str(), rpc_nft.nft_id);
        test_wallet_txbuilder_rpc(port, wallet->get_address().str(), second_wallet->get_address().str());
        test_wallet_basic_rpc(port, wallet->get_address().str(), second_wallet->get_address().str());
        test_wallet_account_rpc(port, *wallet, second_wallet->get_address().str(), root);
        test_health_degrades_for_missing_derived_state(port, components.chain_state);

        server.stop();
        node->stop();
        std::filesystem::remove_all(root, ec);
        std::cout << "test_rpc_info_handlers: ok\n";
        return 0;
    } catch (const std::exception& e) {
        std::filesystem::remove_all(root, ec);
        std::cerr << "test_rpc_info_handlers failed: " << e.what() << "\n";
        return 1;
    }
}
