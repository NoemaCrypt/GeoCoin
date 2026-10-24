#include "geocoin/rpc/rpc_dispatch.hpp"
#include "geocoin/rpc/rpc_params.hpp"
#include "geocoin/rpc/rpc_server.hpp"
#include "geocoin/rpc/rpc_security.hpp"
#include "geocoin/state/state.hpp"
#include "geocoin/network/mempool.hpp"
#include "geocoin/wallet/wallet.hpp"
#include "geocoin/core/blockchain.hpp"
#include "geocoin/rpc/rpc_blockchain_format.hpp"
#include "geocoin/rpc/rpc_nft_metadata.hpp"
#include "rpc_test_utils.hpp"

#include <fstream>
#include <iostream>
#include <limits>

namespace geocoin::rpc {
struct RPCServerTestAccess {
    static json call(RPCServer& server, const json& request, RPCAuthRole role) {
        return server.handle_rpc_call(request, role);
    }
};
}

namespace geocoin::core {
struct BlockchainTestAccess {
    static chain::BlockIndex* insert(Blockchain& chain, const Block& block, chain::BlockIndex* prev) {
        auto index = std::make_unique<chain::BlockIndex>(block.header);
        index->prev = prev;
        index->height = prev ? prev->height + 1 : 0;
        auto* result = index.get();
        chain.block_index_.emplace(block.get_hash(), std::move(index));
        return result;
    }
    static void activate(Blockchain& chain, chain::BlockIndex* index) {
        chain.chain_active_.set_tip(index);
    }
};
}

namespace {
using namespace geocoin;
using namespace geocoin::rpc;
json checks = json::array();

void check(bool ok, const std::string& name) {
    checks.push_back({{"name", name}, {"passed", ok}});
}

bool is_error(const json& response, int code) {
    return response.contains("error") && response["error"].is_object() &&
        response["error"].value("code", 0) == code;
}

json call(RPCServer& server, const std::string& method, const json& params = json::array(),
          RPCAuthRole role = RPCAuthRole::ADMIN) {
    return RPCServerTestAccess::call(server,
        {{"jsonrpc", "2.0"}, {"id", "contract"}, {"method", method}, {"params", params}}, role);
}

void test_protocol(RPCServer& server) {
    const auto success = call(server, "getrpcinfo");
    check(success.value("jsonrpc", "") == "2.0" && success.contains("result") &&
              !success.contains("error") && success["id"] == "contract", "JSON-RPC success envelope");
    const auto error = call(server, "unknown.contract.method");
    check(error.value("jsonrpc", "") == "2.0" && error.contains("error") &&
              !error.contains("result") && error["id"] == "contract", "JSON-RPC error envelope");
    check(call(server, "getrpcinfo", json::object()).contains("result"),
          "empty named params for a method without arguments");
    check(call(server, "getrawmempool", {{"limit", 1}}).contains("result"),
          "named optional params may omit earlier defaults");

    const uint16_t port = test::find_free_port();
    test::expect_true(server.start("127.0.0.1", port, "admin", "admin-pass"), "RPC starts");
    const auto send = [&](const json& payload) {
        return test::send_raw_rpc(port, "admin", "admin-pass", payload.dump());
    };
    const json notification{{"jsonrpc", "2.0"}, {"method", "getrpcinfo"}};
    const auto single = send(notification);
    check(single.status == 204 && single.body.is_null(), "notification has no JSON response");
    const auto all = send(json::array({notification, notification}));
    check(all.status == 204 && all.body.is_null(), "notification batch has no JSON response");
    const auto mixed = send(json::array({notification,
        json{{"jsonrpc", "2.0"}, {"method", "getrpcinfo"}, {"id", 7}}, 5}));
    check(mixed.status == 200 && mixed.body.is_array() && mixed.body.size() == 2 &&
          mixed.body[0].value("id", 0) == 7 && is_error(mixed.body[1], RPC_INVALID_REQUEST),
          "mixed batch omits notifications and keeps invalid items");
    const auto null_id = send({{"jsonrpc", "2.0"}, {"method", "getrpcinfo"}, {"id", nullptr}});
    check(null_id.status == 200 && null_id.body.contains("result") && null_id.body["id"].is_null(),
          "explicit null id receives a response");
    server.stop();
}

void test_integer_parameters(RPCServer& server) {
    const std::string txid(64, 'a');
    for (const auto& value : std::vector<json>{-1, 0.5, uint64_t{1} << 32, true}) {
        check(is_error(call(server, "gettxout", json::array({txid, value})), RPC_INVALID_PARAMETER),
              "gettxout rejects invalid vout " + value.dump());
    }
    for (const auto& value : std::vector<json>{0.5, (uint64_t{1} << 32) + 1, true}) {
        check(is_error(call(server, "listunspent", json::array({value})), RPC_INVALID_PARAMETER),
              "listunspent rejects invalid minconf " + value.dump());
        check(is_error(call(server, "listtransactions", json::array({"*", value})), RPC_INVALID_PARAMETER),
              "listtransactions rejects invalid count " + value.dump());
        check(is_error(call(server, "setgenerate", json::array({false, value})), RPC_INVALID_PARAMETER),
              "setgenerate rejects invalid genproclimit " + value.dump());
    }
    check(!call(server, "listunspent", json::array({0, 1})).contains("error"),
          "valid integer confirmation range still works");
}

json nft_request(const std::string& creator) {
    return {{"creator_address", creator}, {"royalty_percent", 500}, {"metadata", {
        {"name", "contract parcel"}, {"content_hash", std::string(64, 'b')},
        {"created_at", 456}, {"block_height", 7}, {"block_hash", std::string(64, 'c')},
        {"geometry", {{"parts", json::array({json{{"exterior", json::array({json{{"lat_ndeg", 1000000}, {"lon_ndeg", 2000000}}, json{{"lat_ndeg", 1000000}, {"lon_ndeg", 4000000}}, json{{"lat_ndeg", 5000000}, {"lon_ndeg", 2000000}}})}, {"holes", json::array()}}})}}}
    }}};
}

void test_nft_parameters(RPCServer& server, const std::string& creator) {
    const auto valid = nft_request(creator);
    const auto good = call(server, "createnftcreatepayload", json::array({valid}));
    check(good.contains("result") && good["result"].is_object(), "valid NFT signing payload");
    const auto reject = [&](const json& request, const std::string& name) {
        check(is_error(call(server, "createnftcreatepayload", json::array({request})), RPC_INVALID_PARAMETER), name);
    };
    for (const auto& value : std::vector<json>{0.5, 90'000'000'001LL,
                                             (std::numeric_limits<uint64_t>::max)()}) {
        auto request = valid;
        request["metadata"]["geometry"]["parts"][0]["exterior"][0]["lat_ndeg"] = value;
        reject(request, "NFT rejects invalid coordinate " + value.dump());
    }
    for (const auto& field : {"block_hash", "content_hash"}) {
        auto request = valid;
        request["metadata"][field] = "invalid-hex";
        reject(request, std::string("NFT rejects invalid ") + field);
    }
    for (const auto& field : {"tags", "cadastral", "geometry"}) {
        auto request = valid;
        request["metadata"][field] = "invalid-type";
        reject(request, std::string("NFT rejects invalid ") + field);
    }
    for (const auto& field : {"ownership_document_hash", "technical_plan_hash"}) {
        auto request = valid;
        request["metadata"]["cadastral"] = {{field, "invalid-hex"}};
        reject(request, std::string("NFT rejects invalid ") + field);
    }
    auto request = valid;
    request["metadata"]["cadastral"] = {{"photo_hashes", json::array({"invalid-hex"})}};
    reject(request, "NFT rejects invalid photo hash instead of dropping it");
    request = valid;
    request["metadata"]["geometry"]["parts"][0]["exterior"][0] = json{{"lat_ndeg", 1}};
    reject(request, "NFT rejects missing coordinate instead of substituting zero");
    request = valid;
    request["royalty_percent"] = 10001;
    reject(request, "NFT signing payload rejects excessive royalty");
    request = valid;
    request["creator_address"] = "invalid-address";
    reject(request, "NFT signing payload requires a valid creator address");

    auto metadata_json = valid["metadata"];
    metadata_json["cadastral"] = {{"market_value_angles", 123}, {"cadastral_value_angles", 0}};
    const auto metadata = parse_nft_metadata_from_rpc(metadata_json, nft::NFTMetadata{});
    const auto formatted = nft_metadata_to_json(metadata);
    const auto restored = parse_nft_metadata_from_rpc(formatted, nft::NFTMetadata{});
    check(restored.serialize() == metadata.serialize() && restored.calculate_hash() == metadata.calculate_hash(),
          "NFT metadata round trip preserves every serialized field and identifier");
    check(formatted["content_hash"] == valid["metadata"]["content_hash"] &&
          formatted["cadastral"]["market_value_angles"] == 123,
          "NFT read format includes content hash and explicit amount units");
}

void test_sidechain_parameters(RPCServer& server, const std::string& owner) {
    const json valid{{"sidechain_id", std::string(64, 'a')}, {"owner_address", owner},
        {"amount", 1}, {"unlock_height", 2}, {"validate", false},
        {"inputs", json::array({json{{"txid", std::string(64, 'b')}, {"vout", 0}}})},
        {"outputs", json::array({json{{"address", owner}, {"value", 1}}})}};
    check(!call(server, "createsidechainlockpayload", json::array({valid})).contains("error"),
          "valid sidechain signing payload remains available without state validation");
    for (const auto& field : {"sidechain_id", "genesis_hash", "checkpoint"}) {
        auto request = valid;
        request[field] = 7;
        check(is_error(call(server, "createsidechainlockpayload", json::array({request})), RPC_INVALID_PARAMETER),
              std::string("sidechain rejects mistyped ") + field);
    }
    for (const auto& field : {"script_sig_hex", "signature_hex"}) {
        auto request = valid;
        request["inputs"][0][field] = 7;
        check(is_error(call(server, "createsidechainlockpayload", json::array({request})), RPC_INVALID_PARAMETER),
              std::string("sidechain rejects mistyped input ") + field);
    }
    check(is_error(call(server, "createbatchsidechaincheckpointpayload",
        json::array({json{{"items", json::array()}, {"validate", false}}})), RPC_INVALID_PARAMETER),
        "empty sidechain checkpoint batch rejected before serialization");
}

void test_block_branches(const std::shared_ptr<state::ChainState>& state) {
    auto chain = std::make_shared<core::Blockchain>(state, nullptr);
    const auto block = [](uint64_t nonce, const Hash256& parent) {
        core::Block out;
        out.header.nonce = nonce;
        out.header.prev_block_hash = parent;
        return out;
    };
    const auto genesis = block(1, {});
    auto* root = core::BlockchainTestAccess::insert(*chain, genesis, nullptr);
    const auto active = block(2, genesis.get_hash());
    auto* tip = core::BlockchainTestAccess::insert(*chain, active, root);
    core::BlockchainTestAccess::activate(*chain, tip);
    auto fork = block(3, genesis.get_hash());
    auto* fork_tip = core::BlockchainTestAccess::insert(*chain, fork, root);
    for (int height = 1; height <= 3; ++height) {
        const auto formatted = block_to_verbose_json(fork, fork.get_hash(),
            hash256_to_hex_string(fork.get_hash()), *chain);
        check(formatted["confirmations"] == -1 && !formatted.contains("nextblockhash") &&
              !formatted["in_active_chain"].get<bool>(),
              "alternative block has no active confirmations at height " + std::to_string(height));
        fork = block(3 + height, fork.get_hash());
        fork_tip = core::BlockchainTestAccess::insert(*chain, fork, fork_tip);
    }
    const auto formatted = block_to_verbose_json(genesis, genesis.get_hash(),
        hash256_to_hex_string(genesis.get_hash()), *chain);
    check(formatted["confirmations"] == 2 && formatted["nextblockhash"] == hash256_to_hex_string(active.get_hash()),
          "active block confirmations and successor use one chain snapshot");
    RPCServer server;
    test::expect_true(server.set_context(RPCServerContext{.blockchain = chain, .chain_state = state}), "chain context");
    const auto available = call(server, "getblockavailability", json::array({hash256_to_hex_string(fork.get_hash())}));
    check(!available["result"]["found"].get<bool>() && !available["result"]["in_active_chain"].get<bool>(),
          "alternative block is not reported as part of the active chain");
    check(call(server, "getblockhash", json::array({0}))["result"] == hash256_to_hex_string(genesis.get_hash()),
          "getblockhash uses the active index without reading a block body");
    for (const auto& value : std::vector<json>{-1, 0.5, true}) {
        check(is_error(call(server, "getblockhash", json::array({value})), RPC_INVALID_PARAMETER),
              "getblockhash rejects invalid height " + value.dump());
    }
}
}

int main(int argc, char** argv) {
    try {
        RPCServer server;
        auto state = std::make_shared<state::ChainState>(std::make_unique<state::MemoryStorage>());
        auto wallet = std::make_shared<wallet::Wallet>();
        test::expect_true(wallet->initialize(bytes(32, 42)), "wallet initializes");
        auto mempool = std::make_shared<network::Mempool>(state);
        test::expect_true(server.set_context(RPCServerContext{
            .wallet = wallet, .chain_state = state, .mempool = mempool}), "context accepted");
        const json inventory = call(server, "getrpcinfo")["result"];
        test_protocol(server);
        test_integer_parameters(server);
        test_nft_parameters(server, wallet->get_address().str());
        test_sidechain_parameters(server, wallet->get_address().str());
        test_block_branches(state);
        size_t failures = 0;
        for (const auto& item : checks) if (!item["passed"].get<bool>()) ++failures;
        json report{{"checks", checks}, {"passed", checks.size() - failures},
                    {"failed", failures}, {"registry", inventory}};
        if (argc > 1) std::ofstream(argv[1]) << report.dump(2) << '\n';
        std::cout << "RPC contract checks: " << checks.size() - failures << "/" << checks.size() << '\n';
        for (const auto& item : checks) if (!item["passed"].get<bool>()) std::cout << "FAIL " << item["name"] << '\n';
        return failures == 0 ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "test_rpc_contract: " << error.what() << '\n';
        return 1;
    }
}
