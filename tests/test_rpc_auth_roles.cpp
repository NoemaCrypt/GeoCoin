#include "geocoin/core/transaction.hpp"
#include "geocoin/node/node.hpp"
#include "geocoin/rpc/rpc_protocol.hpp"
#include "geocoin/rpc/rpc_server.hpp"
#include "geocoin/rpc/rpc_wallet_helpers.hpp"
#include "rpc_test_utils.hpp"

#include <chrono>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

using json = geocoin::test::json;
using geocoin::test::base64_encode;
using geocoin::test::decode_raw_tx_or_throw;
using geocoin::test::expect_true;
using geocoin::test::find_free_port;
using geocoin::test::send_raw_http;
using geocoin::test::send_raw_rpc;
using geocoin::test::send_rpc;

const json* find_rpc_command(const json& commands, const std::string& name) {
    for (const auto& command : commands) {
        if (command.value("name", "") == name) {
            return &command;
        }
    }
    return nullptr;
}

} // namespace

int main() {
    try {
        std::string parsed_user;
        std::string parsed_pass;
        expect_true(!geocoin::rpc::parse_basic_auth("Basic ", parsed_user, parsed_pass),
            "empty Basic Auth payload must be rejected");
        expect_true(!geocoin::rpc::parse_basic_auth("Basic !!!!", parsed_user, parsed_pass),
            "malformed Basic Auth payload must be rejected");
        expect_true(!geocoin::rpc::parse_basic_auth("Basic =", parsed_user, parsed_pass),
            "zero-sized malformed Basic Auth payload must be rejected safely");
        expect_true(!geocoin::rpc::parse_basic_auth(
                "Basic jpBaTswYwYXNlcjpwYwYXdX", parsed_user, parsed_pass),
            "libFuzzer truncated-quartet stack overflow regression");
        for (size_t length = 1; length <= 128; ++length) {
            if (length % 4 == 0) continue;
            parsed_user = "stale-user";
            parsed_pass = "stale-password";
            expect_true(!geocoin::rpc::parse_basic_auth(
                    "Basic " + std::string(length, 'A'), parsed_user, parsed_pass),
                "incomplete Base64 quartets must be rejected before decoding");
            expect_true(parsed_user.empty() && parsed_pass.empty(),
                "invalid Basic Auth must clear previous credentials");
        }
        expect_true(!geocoin::rpc::parse_basic_auth(
                "Basic " + base64_encode("user:\npass"),
                parsed_user,
                parsed_pass
            ),
            "Basic Auth payload with control characters must be rejected");
        expect_true(!geocoin::rpc::parse_basic_auth(
                "Basic " + base64_encode(":pass"),
                parsed_user,
                parsed_pass
            ),
            "Basic Auth payload with empty user must be rejected");
        expect_true(geocoin::rpc::parse_basic_auth(
                "Basic " + base64_encode("user:pass"),
                parsed_user,
                parsed_pass
            ),
            "valid Basic Auth payload must parse");
        expect_true(parsed_user == "user" && parsed_pass == "pass",
            "valid Basic Auth payload should split user and password");
        expect_true(
            geocoin::node::clamp_cpu_mining_threads(geocoin::node::MAX_CPU_MINING_THREADS + 1024) <=
                geocoin::node::cpu_mining_thread_limit(),
            "CPU mining thread clamp must enforce the safe thread limit"
        );

        {
            geocoin::rpc::RPCServer invalid_server;
            const uint16_t port = find_free_port();
            expect_true(!invalid_server.start("127.0.0.1", port, "same", "secret", "same", "secret"),
                "RPC server must reject identical admin and miner credentials");
        }

        {
            geocoin::rpc::RPCServer private_gpu_rpc(
                geocoin::rpc::RPCCommandProfile::MINING_ONLY
            );
            const uint16_t port = find_free_port();
            expect_true(private_gpu_rpc.start(
                    "127.0.0.1",
                    port,
                    "geocoin-gpu-admin",
                    "geocoin-gpu-admin-pass",
                    "geocoin-gpu",
                    "geocoin-gpu-pass"
                ),
                "private GUI GPU RPC must start with distinct hidden admin and miner credentials");
            std::this_thread::sleep_for(std::chrono::milliseconds(50));

            const auto gpu_admin_template = send_rpc(
                port,
                "geocoin-gpu-admin",
                "geocoin-gpu-admin-pass",
                "getblocktemplate"
            );
            expect_true(gpu_admin_template.status == 200, "hidden GUI admin getblocktemplate should get JSON-RPC response");
            expect_true(gpu_admin_template.body["error"]["code"].get<int>() == geocoin::rpc::RPC_FORBIDDEN_BY_SAFE_MODE,
                "hidden GUI admin credential must not access external mining methods");

            const auto gpu_miner_template = send_rpc(
                port,
                "geocoin-gpu",
                "geocoin-gpu-pass",
                "getblocktemplate"
            );
            expect_true(gpu_miner_template.status == 200, "GUI GPU miner getblocktemplate should get JSON-RPC response");
            expect_true(gpu_miner_template.body["error"]["code"].get<int>() == geocoin::rpc::RPC_INTERNAL_ERROR,
                "GUI GPU miner credential should pass authz and reach getblocktemplate handler");

            const auto gpu_miner_help = send_rpc(
                port,
                "geocoin-gpu",
                "geocoin-gpu-pass",
                "help"
            );
            expect_true(gpu_miner_help.status == 200, "GUI GPU miner help request should get JSON-RPC response");
            expect_true(gpu_miner_help.body["error"]["code"].get<int>() == geocoin::rpc::RPC_METHOD_NOT_FOUND,
                "mining-only RPC must not retain admin/control methods");

            const auto hidden_wallet_method = send_rpc(
                port,
                "geocoin-gpu-admin",
                "geocoin-gpu-admin-pass",
                "getnewaddress"
            );
            expect_true(hidden_wallet_method.status == 200,
                "mining-only wallet probe should get JSON-RPC response");
            expect_true(
                hidden_wallet_method.body["error"]["code"].get<int>() ==
                    geocoin::rpc::RPC_METHOD_NOT_FOUND,
                "mining-only RPC must not retain wallet handlers even for admin credentials"
            );
            private_gpu_rpc.stop();
        }

        geocoin::rpc::RPCServer server;
        const uint16_t port = find_free_port();
        expect_true(server.start("127.0.0.1", port, "admin", "admin-pass", "miner", "miner-pass"),
            "failed to start test RPC server");
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        const std::string valid_body = json{
            {"jsonrpc", "2.0"},
            {"id", 7},
            {"method", "help"},
            {"params", json::array()}
        }.dump();
        std::ostringstream lower_header_req;
        lower_header_req << "POST / HTTP/1.1\r\n";
        lower_header_req << "host: 127.0.0.1\r\n";
        lower_header_req << "authorization: Basic " << base64_encode("admin:admin-pass") << "\r\n";
        lower_header_req << "content-type: application/json\r\n";
        lower_header_req << "content-length: " << valid_body.size() << "\r\n";
        lower_header_req << "connection: close\r\n\r\n";
        lower_header_req << valid_body;
        const auto lower_header_response = send_raw_http(port, lower_header_req.str());
        expect_true(lower_header_response.status == 200, "lower-case HTTP auth/content-length headers must be accepted");
        expect_true(!lower_header_response.body.contains("error"), "lower-case header request should dispatch JSON-RPC");

        std::ostringstream bad_length_req;
        bad_length_req << "POST / HTTP/1.1\r\n";
        bad_length_req << "Host: 127.0.0.1\r\n";
        bad_length_req << "Authorization: Basic " << base64_encode("admin:admin-pass") << "\r\n";
        bad_length_req << "Content-Length: not-a-number\r\n";
        bad_length_req << "Connection: close\r\n\r\n";
        const auto bad_length_response = send_raw_http(port, bad_length_req.str());
        expect_true(bad_length_response.status == 400, "malformed Content-Length must be rejected with HTTP 400");

        std::ostringstream chunked_req;
        chunked_req << "POST / HTTP/1.1\r\n";
        chunked_req << "Host: 127.0.0.1\r\n";
        chunked_req << "Authorization: Basic " << base64_encode("admin:admin-pass") << "\r\n";
        chunked_req << "Transfer-Encoding: chunked\r\n";
        chunked_req << "Connection: close\r\n\r\n";
        chunked_req << "0\r\n\r\n";
        const auto chunked_response = send_raw_http(port, chunked_req.str());
        expect_true(chunked_response.status == 501, "unsupported Transfer-Encoding must be rejected before dispatch");

        std::ostringstream missing_length_req;
        missing_length_req << "POST / HTTP/1.1\r\n";
        missing_length_req << "Host: 127.0.0.1\r\n";
        missing_length_req << "Authorization: Basic " << base64_encode("admin:admin-pass") << "\r\n";
        missing_length_req << "Connection: close\r\n\r\n";
        const auto missing_length_response = send_raw_http(port, missing_length_req.str());
        expect_true(missing_length_response.status == 411, "POST without Content-Length must be rejected with HTTP 411");

        const auto miner_help = send_rpc(port, "miner", "miner-pass", "help");
        expect_true(miner_help.status == 200, "miner help request should get JSON-RPC response");
        expect_true(miner_help.body["error"]["code"].get<int>() == geocoin::rpc::RPC_FORBIDDEN_BY_SAFE_MODE,
            "miner credential must not access control/admin methods");

        const auto admin_template = send_rpc(port, "admin", "admin-pass", "getblocktemplate");
        expect_true(admin_template.status == 200, "admin getblocktemplate should get JSON-RPC response");
        expect_true(admin_template.body["error"]["code"].get<int>() == geocoin::rpc::RPC_FORBIDDEN_BY_SAFE_MODE,
            "admin credential must not access external mining methods");

        const auto miner_template = send_rpc(port, "miner", "miner-pass", "getblocktemplate");
        expect_true(miner_template.status == 200, "miner getblocktemplate should get JSON-RPC response");
        expect_true(miner_template.body["error"]["code"].get<int>() == geocoin::rpc::RPC_INTERNAL_ERROR,
            "miner credential should pass authz and reach getblocktemplate handler");

        const auto admin_help = send_rpc(port, "admin", "admin-pass", "help");
        expect_true(admin_help.status == 200, "admin help request should get JSON-RPC response");
        expect_true(!admin_help.body.contains("error"), "admin credential must access admin/control methods");

        json maximum_batch = json::array();
        for (int i = 0; i < 10; ++i) {
            maximum_batch.push_back(json{
                {"jsonrpc", "2.0"},
                {"id", i},
                {"method", "help"},
                {"params", json::array()}
            });
        }
        const auto maximum_batch_response = send_raw_rpc(
            port,
            "admin",
            "admin-pass",
            maximum_batch.dump()
        );
        expect_true(maximum_batch_response.status == 200,
            "maximum-size batch should get JSON-RPC response");
        expect_true(maximum_batch_response.body.is_array() &&
                        maximum_batch_response.body.size() == maximum_batch.size(),
            "a batch at the exact limit must be dispatched completely");

        json oversized_batch = json::array();
        for (int i = 0; i < 11; ++i) {
            oversized_batch.push_back(json{
                {"jsonrpc", "2.0"},
                {"id", i},
                {"method", "help"},
                {"params", json::array()}
            });
        }
        const auto batch_response = send_raw_rpc(
            port,
            "admin",
            "admin-pass",
            oversized_batch.dump()
        );
        expect_true(batch_response.status == 200, "oversized batch should get JSON-RPC response");
        expect_true(batch_response.body.is_object(), "oversized batch should return a single JSON-RPC error");
        expect_true(batch_response.body["error"]["code"].get<int>() == geocoin::rpc::RPC_INVALID_REQUEST,
            "oversized batch must be rejected before dispatch");

        const auto empty_batch_response = send_raw_rpc(
            port,
            "admin",
            "admin-pass",
            json::array().dump()
        );
        expect_true(empty_batch_response.status == 200, "empty batch should get JSON-RPC response");
        expect_true(empty_batch_response.body["error"]["code"].get<int>() == geocoin::rpc::RPC_INVALID_REQUEST,
            "empty batch must be rejected before dispatch");

        const auto scalar_request_response = send_raw_rpc(
            port,
            "admin",
            "admin-pass",
            json(42).dump()
        );
        expect_true(scalar_request_response.status == 200, "scalar JSON-RPC request should get JSON-RPC response");
        expect_true(scalar_request_response.body["error"]["code"].get<int>() == geocoin::rpc::RPC_INVALID_REQUEST,
            "scalar JSON-RPC request must be rejected as invalid request");

        const auto long_method = send_rpc(port, "admin", "admin-pass", std::string(129, 'a'));
        expect_true(long_method.status == 200, "long method request should get JSON-RPC response");
        expect_true(long_method.body["error"]["code"].get<int>() == geocoin::rpc::RPC_INVALID_REQUEST,
            "overlong method name must be rejected before lookup");

        const auto scalar_params = send_raw_rpc(
            port,
            "admin",
            "admin-pass",
            json{
                {"jsonrpc", "2.0"},
                {"id", 8},
                {"method", "help"},
                {"params", 42}
            }.dump()
        );
        expect_true(scalar_params.status == 200, "scalar params request should get JSON-RPC response");
        expect_true(scalar_params.body["error"]["code"].get<int>() == geocoin::rpc::RPC_INVALID_PARAMS,
            "scalar params must be rejected before method dispatch");

        const std::string zero_txid(64, '0');
        const std::string test_address = "GEO1KrNQJ6C6qYNH4N2goZPU2eDAfiPvknY2x";

        {
            std::vector<geocoin::core::TxOutput> outputs;
            const json overflow_outputs = json::array({
                json{{"address", test_address}, {"value", (std::numeric_limits<geocoin::Amount>::max)()}},
                json{{"address", test_address}, {"value", 1}}
            });

            bool rejected = false;
            try {
                (void)geocoin::rpc::append_transaction_outputs_from_rpc(overflow_outputs, outputs);
            } catch (const geocoin::rpc::RPCException& e) {
                rejected = e.code() == geocoin::rpc::RPC_INVALID_PARAMETER;
            }
            expect_true(rejected, "sidechain output total overflow must be rejected");
        }

        {
            bool rejected = false;
            try {
                (void)geocoin::rpc::calc_sidechain_fee_rpc(
                    (std::numeric_limits<geocoin::Amount>::max)(),
                    (std::numeric_limits<uint32_t>::max)()
                );
            } catch (const geocoin::rpc::RPCException& e) {
                rejected = e.code() == geocoin::rpc::RPC_INVALID_PARAMETER;
            }
            expect_true(rejected, "sidechain fee overflow must be rejected");
        }

        const auto raw_integer_gc = send_rpc(
            port,
            "admin",
            "admin-pass",
            "createrawtransaction",
            json::array({
                json::array({json{{"txid", zero_txid}, {"vout", 0}}}),
                json{{test_address, 1}}
            })
        );
        expect_true(raw_integer_gc.status == 200, "createrawtransaction integer GC request should get JSON-RPC response");
        expect_true(!raw_integer_gc.body.contains("error"), "createrawtransaction integer GC should succeed");
        const auto tx_integer_gc = decode_raw_tx_or_throw(raw_integer_gc.body["result"].get<std::string>());
        expect_true(tx_integer_gc.outputs.size() == 1, "createrawtransaction should create one output");
        expect_true(tx_integer_gc.outputs[0].value == geocoin::COIN, "integer amount should be interpreted as GC");

        const auto raw_named_params = send_rpc(
            port,
            "admin",
            "admin-pass",
            "createrawtransaction",
            json::object({
                {"inputs", json::array({json{{"txid", zero_txid}, {"vout", 0}}})},
                {"outputs", json{{test_address, 1}}}
            })
        );
        expect_true(raw_named_params.status == 200, "createrawtransaction named params should get JSON-RPC response");
        expect_true(!raw_named_params.body.contains("error"), "createrawtransaction named params should succeed");
        const auto tx_named_params = decode_raw_tx_or_throw(raw_named_params.body["result"].get<std::string>());
        expect_true(tx_named_params.outputs.size() == 1, "createrawtransaction named params should create one output");
        expect_true(tx_named_params.outputs[0].value == geocoin::COIN,
            "createrawtransaction named params should preserve GC amount semantics");

        const auto skipped_named_params = send_rpc(
            port,
            "admin",
            "admin-pass",
            "createrawtransaction",
            json::object({
                {"outputs", json{{test_address, 1}}}
            })
        );
        expect_true(skipped_named_params.status == 200, "skipped named params should get JSON-RPC response");
        expect_true(skipped_named_params.body["error"]["code"].get<int>() == geocoin::rpc::RPC_INVALID_PARAMETER,
            "omitted required inputs are rejected by the transaction builder");

        const auto raw_one_angle = send_rpc(
            port,
            "admin",
            "admin-pass",
            "createrawtransaction",
            json::array({
                json::array({json{{"txid", zero_txid}, {"vout", 0}}}),
                json::array({json{{"address", test_address}, {"value_angles", 1}}})
            })
        );
        expect_true(raw_one_angle.status == 200, "createrawtransaction value_angles request should get JSON-RPC response");
        expect_true(!raw_one_angle.body.contains("error"), "createrawtransaction value_angles should succeed");
        const auto tx_one_angle = decode_raw_tx_or_throw(raw_one_angle.body["result"].get<std::string>());
        expect_true(tx_one_angle.outputs.size() == 1, "createrawtransaction value_angles should create one output");
        expect_true(tx_one_angle.outputs[0].value == 1, "value_angles should be interpreted as angles");

        const auto raw_overflow_angle = send_rpc(
            port,
            "admin",
            "admin-pass",
            "createrawtransaction",
            json::array({
                json::array({json{{"txid", zero_txid}, {"vout", 0}}}),
                json::array({json{{"address", test_address}, {"value_angles", (std::numeric_limits<uint64_t>::max)()}}})
            })
        );
        expect_true(raw_overflow_angle.status == 200, "createrawtransaction overflow value_angles should get JSON-RPC response");
        expect_true(raw_overflow_angle.body["error"]["code"].get<int>() == geocoin::rpc::RPC_INVALID_PARAMETER,
            "createrawtransaction must reject value_angles above Amount max");

        const auto raw_huge_gc = send_rpc(
            port,
            "admin",
            "admin-pass",
            "createrawtransaction",
            json::array({
                json::array({json{{"txid", zero_txid}, {"vout", 0}}}),
                json{{test_address, 1.0e100}}
            })
        );
        expect_true(raw_huge_gc.status == 200, "createrawtransaction huge GC amount should get JSON-RPC response");
        expect_true(raw_huge_gc.body["error"]["code"].get<int>() == geocoin::rpc::RPC_INVALID_PARAMETER,
            "createrawtransaction must reject GC amounts above Amount max");

        const auto rpc_info = send_rpc(port, "admin", "admin-pass", "getrpcinfo");
        expect_true(rpc_info.status == 200, "admin getrpcinfo request should get JSON-RPC response");
        expect_true(!rpc_info.body.contains("error"), "getrpcinfo should succeed for admin");

        const auto p2p_diag = send_rpc(port, "admin", "admin-pass", "getconnectiondiagnostics");
        expect_true(p2p_diag.status == 200, "getconnectiondiagnostics should get JSON-RPC response");
        expect_true(!p2p_diag.body.contains("error"), "getconnectiondiagnostics should succeed without peer manager");
        expect_true(p2p_diag.body["result"]["peers"].is_array(), "getconnectiondiagnostics peers should be an array");
        expect_true(p2p_diag.body["result"]["outbound_attempts"].is_array(),
            "getconnectiondiagnostics outbound_attempts should be an array");
        expect_true(p2p_diag.body["result"]["disconnect_reasons"].is_object(),
            "getconnectiondiagnostics disconnect_reasons should be an object");

        const auto bad_addnode = send_rpc(
            port,
            "admin",
            "admin-pass",
            "addnode",
            json::array({"bad host", "add"})
        );
        expect_true(bad_addnode.status == 200, "bad addnode target should get JSON-RPC response");
        expect_true(bad_addnode.body["error"]["code"].get<int>() == geocoin::rpc::RPC_INVALID_PARAMETER,
            "addnode must reject malformed peer addresses before mutating peer state");

        const auto& commands = rpc_info.body["result"]["commands"];
        expect_true(commands.is_array(), "getrpcinfo commands should be an array");
        expect_true(rpc_info.body["result"]["api_version"].get<std::string>() == "1",
            "getrpcinfo should expose api_version");
        expect_true(rpc_info.body["result"]["role_capabilities"]["readonly"].is_array(),
            "getrpcinfo should expose role capabilities");

        const json* getaddressutxos = find_rpc_command(commands, "getaddressutxos");
        expect_true(getaddressutxos != nullptr, "getaddressutxos must be registered");
        expect_true((*getaddressutxos)["role"].get<std::string>() == "readonly", "getaddressutxos should be readonly");
        expect_true((*getaddressutxos)["category"].get<std::string>() == "blockchain", "getaddressutxos category should be blockchain");
        expect_true((*getaddressutxos)["mutates"].get<bool>() == false, "getaddressutxos must be side-effect free");

        const json* sendrawtransaction = find_rpc_command(commands, "sendrawtransaction");
        expect_true(sendrawtransaction != nullptr, "sendrawtransaction must be registered");
        expect_true((*sendrawtransaction)["category"].get<std::string>() == "mempool", "sendrawtransaction category should be mempool");
        expect_true((*sendrawtransaction)["mutates"].get<bool>() == true, "sendrawtransaction must be marked mutating");
        expect_true((*sendrawtransaction)["capabilities"].is_array(), "sendrawtransaction should expose capabilities");

        const json* submitblock = find_rpc_command(commands, "submitblock");
        expect_true(submitblock != nullptr, "submitblock must be registered");
        expect_true((*submitblock)["role"].get<std::string>() == "miner", "submitblock should require miner credentials");
        expect_true((*submitblock)["mutates"].get<bool>() == true, "submitblock must be marked mutating");

        const json* getutxos = find_rpc_command(commands, "getutxos");
        expect_true(getutxos != nullptr, "getutxos must be registered");
        expect_true((*getutxos)["deprecated"].get<bool>() == true, "getutxos should be marked deprecated");

        const json* getrawmempool = find_rpc_command(commands, "getrawmempool");
        expect_true(getrawmempool != nullptr, "getrawmempool must be registered");
        expect_true((*getrawmempool)["category"].get<std::string>() == "mempool", "getrawmempool category should be mempool");
        expect_true((*getrawmempool)["readonly"].get<bool>() == true, "getrawmempool should be readonly");

        const json* getmempoolinfo = find_rpc_command(commands, "getmempoolinfo");
        expect_true(getmempoolinfo != nullptr, "getmempoolinfo must be registered");
        expect_true((*getmempoolinfo)["category"].get<std::string>() == "mempool", "getmempoolinfo category should be mempool");
        expect_true((*getmempoolinfo)["readonly"].get<bool>() == true, "getmempoolinfo should be callable by readonly capability");

        const json* getconnectiondiagnostics = find_rpc_command(commands, "getconnectiondiagnostics");
        expect_true(getconnectiondiagnostics != nullptr, "getconnectiondiagnostics must be registered");
        expect_true((*getconnectiondiagnostics)["category"].get<std::string>() == "network",
            "getconnectiondiagnostics category should be network");
        expect_true((*getconnectiondiagnostics)["readonly"].get<bool>() == false &&
                        (*getconnectiondiagnostics)["role"].get<std::string>() == "admin",
            "getconnectiondiagnostics should require admin diagnostics capability");

        const json* getblockbyheight = find_rpc_command(commands, "getblockbyheight");
        expect_true(getblockbyheight != nullptr, "getblockbyheight must be registered");
        expect_true((*getblockbyheight)["category"].get<std::string>() == "blockchain", "getblockbyheight category should be blockchain");

        const json* signraw = find_rpc_command(commands, "signrawtransactionwithwallet");
        expect_true(signraw != nullptr, "signrawtransactionwithwallet must be registered");
        expect_true((*signraw)["category"].get<std::string>() == "txbuilder", "signrawtransactionwithwallet category should be txbuilder");
        expect_true((*signraw)["requires_wallet"].get<bool>() == true, "signrawtransactionwithwallet should require wallet");

        server.stop();

        std::cout << "test_rpc_auth_roles: ok\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "test_rpc_auth_roles failed: " << e.what() << "\n";
        return 1;
    }
}
