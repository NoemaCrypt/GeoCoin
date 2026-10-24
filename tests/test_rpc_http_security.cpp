#include "geocoin/rpc/rpc_server.hpp"
#include "geocoin/rpc/rpc_protocol.hpp"
#include "geocoin/consensus/consensus.hpp"
#include "rpc_test_utils.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

using json = geocoin::test::json;
using geocoin::test::base64_encode;
using geocoin::test::expect_true;
using geocoin::test::find_free_port;
using geocoin::test::json_rpc_body;
using geocoin::test::make_post_request;
using geocoin::test::send_raw_http;
using geocoin::test::send_raw_rpc;
using geocoin::test::send_rpc_method;
using geocoin::test::TcpStream;

class ScopedEnvVar {
public:
    ScopedEnvVar(const char* name, const char* value)
        : name_(name) {
        const char* old_value = std::getenv(name_.c_str());
        if (old_value != nullptr) {
            had_old_value_ = true;
            old_value_ = old_value;
        }
        set_value(value);
    }

    ~ScopedEnvVar() {
        if (had_old_value_) {
            set_value(old_value_.c_str());
        } else {
            unset_value();
        }
    }

    ScopedEnvVar(const ScopedEnvVar&) = delete;
    ScopedEnvVar& operator=(const ScopedEnvVar&) = delete;

private:
    void set_value(const char* value) const {
#ifdef _WIN32
        _putenv_s(name_.c_str(), value);
#else
        setenv(name_.c_str(), value, 1);
#endif
    }

    void unset_value() const {
#ifdef _WIN32
        _putenv_s(name_.c_str(), "");
#else
        unsetenv(name_.c_str());
#endif
    }

    std::string name_;
    bool had_old_value_ = false;
    std::string old_value_;
};

void test_http_parser_policy(uint16_t port) {
    const auto get_response = send_raw_http(
        port,
        "GET / HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Connection: close\r\n\r\n"
    );
    expect_true(get_response.status == 405, "non-POST HTTP methods must be rejected with 405");

    const std::string body = json_rpc_body("help");
    std::ostringstream malformed_header_req;
    malformed_header_req << "POST / HTTP/1.1\r\n";
    malformed_header_req << "Host 127.0.0.1\r\n";
    malformed_header_req << "Authorization: Basic " << base64_encode("admin:admin-pass") << "\r\n";
    malformed_header_req << "Content-Type: application/json\r\n";
    malformed_header_req << "Content-Length: " << body.size() << "\r\n";
    malformed_header_req << "Connection: close\r\n\r\n";
    malformed_header_req << body;
    const auto malformed_header_response = send_raw_http(port, malformed_header_req.str());
    expect_true(malformed_header_response.status == 400, "malformed HTTP header lines must be rejected");

    std::ostringstream invalid_header_key_req;
    invalid_header_key_req << "POST / HTTP/1.1\r\n";
    invalid_header_key_req << "Host: 127.0.0.1\r\n";
    invalid_header_key_req << "Bad Header: value\r\n";
    invalid_header_key_req << "Authorization: Basic " << base64_encode("admin:admin-pass") << "\r\n";
    invalid_header_key_req << "Content-Type: application/json\r\n";
    invalid_header_key_req << "Content-Length: " << body.size() << "\r\n";
    invalid_header_key_req << "Connection: close\r\n\r\n";
    invalid_header_key_req << body;
    const auto invalid_header_key_response = send_raw_http(port, invalid_header_key_req.str());
    expect_true(invalid_header_key_response.status == 400, "invalid HTTP header names must be rejected");

    std::ostringstream duplicate_length_req;
    duplicate_length_req << "POST / HTTP/1.1\r\n";
    duplicate_length_req << "Host: 127.0.0.1\r\n";
    duplicate_length_req << "Authorization: Basic " << base64_encode("admin:admin-pass") << "\r\n";
    duplicate_length_req << "Content-Type: application/json\r\n";
    duplicate_length_req << "Content-Length: " << body.size() << "\r\n";
    duplicate_length_req << "Content-Length: " << body.size() << "\r\n";
    duplicate_length_req << "Connection: close\r\n\r\n";
    duplicate_length_req << body;
    const auto duplicate_length_response = send_raw_http(port, duplicate_length_req.str());
    expect_true(duplicate_length_response.status == 400, "duplicate Content-Length must be rejected");

    std::ostringstream duplicate_auth_req;
    duplicate_auth_req << "POST / HTTP/1.1\r\n";
    duplicate_auth_req << "Host: 127.0.0.1\r\n";
    duplicate_auth_req << "Authorization: Basic " << base64_encode("admin:admin-pass") << "\r\n";
    duplicate_auth_req << "Authorization: Basic " << base64_encode("admin:admin-pass") << "\r\n";
    duplicate_auth_req << "Content-Type: application/json\r\n";
    duplicate_auth_req << "Content-Length: " << body.size() << "\r\n";
    duplicate_auth_req << "Connection: close\r\n\r\n";
    duplicate_auth_req << body;
    const auto duplicate_auth_response = send_raw_http(port, duplicate_auth_req.str());
    expect_true(duplicate_auth_response.status == 400, "duplicate Authorization must be rejected");

    std::ostringstream malformed_length_req;
    malformed_length_req << "POST / HTTP/1.1\r\n";
    malformed_length_req << "Host: 127.0.0.1\r\n";
    malformed_length_req << "Authorization: Basic " << base64_encode("admin:admin-pass") << "\r\n";
    malformed_length_req << "Content-Type: application/json\r\n";
    malformed_length_req << "Content-Length: 12x\r\n";
    malformed_length_req << "Connection: close\r\n\r\n";
    malformed_length_req << body;
    const auto malformed_length_response = send_raw_http(port, malformed_length_req.str());
    expect_true(malformed_length_response.status == 400, "non-numeric Content-Length must be rejected");

    std::ostringstream chunked_req;
    chunked_req << "POST / HTTP/1.1\r\n";
    chunked_req << "Host: 127.0.0.1\r\n";
    chunked_req << "Authorization: Basic " << base64_encode("admin:admin-pass") << "\r\n";
    chunked_req << "Content-Type: application/json\r\n";
    chunked_req << "Transfer-Encoding: chunked\r\n";
    chunked_req << "Connection: close\r\n\r\n";
    chunked_req << "0\r\n\r\n";
    const auto chunked_response = send_raw_http(port, chunked_req.str());
    expect_true(chunked_response.status == 501, "chunked Transfer-Encoding must be rejected");

    std::ostringstream long_uri_req;
    long_uri_req << "GET /" << std::string(2050, 'x') << " HTTP/1.1\r\n";
    long_uri_req << "Host: 127.0.0.1\r\n";
    long_uri_req << "Connection: close\r\n\r\n";
    const auto long_uri_response = send_raw_http(port, long_uri_req.str());
    expect_true(long_uri_response.status == 414, "overlong URI must be rejected with 414");

    std::ostringstream extra_request_token_req;
    extra_request_token_req << "POST / HTTP/1.1 extra\r\n";
    extra_request_token_req << "Host: 127.0.0.1\r\n";
    extra_request_token_req << "Authorization: Basic " << base64_encode("admin:admin-pass") << "\r\n";
    extra_request_token_req << "Content-Type: application/json\r\n";
    extra_request_token_req << "Content-Length: " << body.size() << "\r\n";
    extra_request_token_req << "Connection: close\r\n\r\n";
    extra_request_token_req << body;
    const auto extra_request_token_response = send_raw_http(port, extra_request_token_req.str());
    expect_true(extra_request_token_response.status == 400, "extra request-line tokens must be rejected");

    std::ostringstream unsupported_version_req;
    unsupported_version_req << "POST / HTTP/2.0\r\n";
    unsupported_version_req << "Host: 127.0.0.1\r\n";
    unsupported_version_req << "Authorization: Basic " << base64_encode("admin:admin-pass") << "\r\n";
    unsupported_version_req << "Content-Type: application/json\r\n";
    unsupported_version_req << "Content-Length: " << body.size() << "\r\n";
    unsupported_version_req << "Connection: close\r\n\r\n";
    unsupported_version_req << body;
    const auto unsupported_version_response = send_raw_http(port, unsupported_version_req.str());
    expect_true(unsupported_version_response.status == 400, "unsupported HTTP versions must be rejected");

    std::ostringstream duplicate_transfer_req;
    duplicate_transfer_req << "POST / HTTP/1.1\r\n";
    duplicate_transfer_req << "Host: 127.0.0.1\r\n";
    duplicate_transfer_req << "Authorization: Basic " << base64_encode("admin:admin-pass") << "\r\n";
    duplicate_transfer_req << "Transfer-Encoding: chunked\r\n";
    duplicate_transfer_req << "Transfer-Encoding: chunked\r\n";
    duplicate_transfer_req << "Connection: close\r\n\r\n";
    duplicate_transfer_req << "0\r\n\r\n";
    const auto duplicate_transfer_response = send_raw_http(port, duplicate_transfer_req.str());
    expect_true(duplicate_transfer_response.status == 400, "duplicate Transfer-Encoding must be rejected");

    std::ostringstream transfer_and_length_req;
    transfer_and_length_req << "POST / HTTP/1.1\r\n";
    transfer_and_length_req << "Host: 127.0.0.1\r\n";
    transfer_and_length_req << "Transfer-Encoding: chunked\r\n";
    transfer_and_length_req << "Content-Length: 0\r\n";
    transfer_and_length_req << "Connection: close\r\n\r\n";
    const auto transfer_and_length_response =
        send_raw_http(port, transfer_and_length_req.str());
    expect_true(transfer_and_length_response.status == 400,
                "Transfer-Encoding plus Content-Length must fail closed");

    const auto missing_host_response = send_raw_http(
        port,
        "GET / HTTP/1.1\r\nConnection: close\r\n\r\n"
    );
    expect_true(missing_host_response.status == 400,
                "HTTP/1.1 without Host must be rejected");

    const auto duplicate_host_response = send_raw_http(
        port,
        "GET / HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Host: localhost\r\n"
        "Connection: close\r\n\r\n"
    );
    expect_true(duplicate_host_response.status == 400,
                "duplicate Host must be rejected instead of using last-wins parsing");

    std::ostringstream duplicate_content_type_req;
    duplicate_content_type_req << "POST / HTTP/1.1\r\n";
    duplicate_content_type_req << "Host: 127.0.0.1\r\n";
    duplicate_content_type_req << "Authorization: Basic "
                               << base64_encode("admin:admin-pass") << "\r\n";
    duplicate_content_type_req << "Content-Type: text/plain\r\n";
    duplicate_content_type_req << "Content-Type: application/json\r\n";
    duplicate_content_type_req << "Content-Length: " << body.size() << "\r\n";
    duplicate_content_type_req << "Connection: close\r\n\r\n" << body;
    expect_true(send_raw_http(port, duplicate_content_type_req.str()).status == 400,
                "duplicate Content-Type must not enable last-wins policy bypass");

    std::ostringstream whitespace_before_colon_req;
    whitespace_before_colon_req << "POST / HTTP/1.1\r\n";
    whitespace_before_colon_req << "Host: 127.0.0.1\r\n";
    whitespace_before_colon_req << "Authorization: Basic "
                                << base64_encode("admin:admin-pass") << "\r\n";
    whitespace_before_colon_req << "Content-Type: application/json\r\n";
    whitespace_before_colon_req << "Content-Length : " << body.size() << "\r\n";
    whitespace_before_colon_req << "Connection: close\r\n\r\n" << body;
    expect_true(send_raw_http(port, whitespace_before_colon_req.str()).status == 400,
                "whitespace before a header colon must be rejected");

    std::ostringstream folded_header_req;
    folded_header_req << "POST / HTTP/1.1\r\n";
    folded_header_req << "Host: 127.0.0.1\r\n";
    folded_header_req << " Authorization: Basic "
                      << base64_encode("admin:admin-pass") << "\r\n";
    folded_header_req << "Content-Type: application/json\r\n";
    folded_header_req << "Content-Length: " << body.size() << "\r\n";
    folded_header_req << "Connection: close\r\n\r\n" << body;
    expect_true(send_raw_http(port, folded_header_req.str()).status == 400,
                "obsolete folded or leading-whitespace headers must be rejected");

    const auto extra_space_response = send_raw_http(
        port,
        "GET  / HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n"
    );
    expect_true(extra_space_response.status == 400,
                "request lines with ambiguous repeated spaces must be rejected");

    const auto tabbed_request_line_response = send_raw_http(
        port,
        "GET\t/ HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n"
    );
    expect_true(tabbed_request_line_response.status == 400,
                "request lines containing tabs must be rejected");

    std::ostringstream mixed_line_endings_req;
    mixed_line_endings_req << "POST / HTTP/1.1\r\n";
    mixed_line_endings_req << "Host: 127.0.0.1\n";
    mixed_line_endings_req << "Authorization: Basic "
                           << base64_encode("admin:admin-pass") << "\r\n";
    mixed_line_endings_req << "Content-Type: application/json\r\n";
    mixed_line_endings_req << "Content-Length: " << body.size() << "\r\n";
    mixed_line_endings_req << "Connection: close\r\n\r\n" << body;
    expect_true(send_raw_http(port, mixed_line_endings_req.str()).status == 400,
                "mixed bare-LF and CRLF headers must be rejected");

    std::ostringstream huge_body_req;
    huge_body_req << "POST / HTTP/1.1\r\n";
    huge_body_req << "Host: 127.0.0.1\r\n";
    huge_body_req << "Authorization: Basic " << base64_encode("admin:admin-pass") << "\r\n";
    huge_body_req << "Content-Type: application/json\r\n";
    huge_body_req << "Content-Length: "
                  << (geocoin::consensus::params::MAX_BLOCK_SIZE * 2 + 64 * 1024 + 1)
                  << "\r\n";
    huge_body_req << "Connection: close\r\n\r\n";
    const auto huge_body_response = send_raw_http(port, huge_body_req.str());
    expect_true(huge_body_response.status == 413, "oversized HTTP body must be rejected before body read");

    std::ostringstream unauthenticated_slow_body_req;
    unauthenticated_slow_body_req << "POST / HTTP/1.1\r\n";
    unauthenticated_slow_body_req << "Host: 127.0.0.1\r\n";
    unauthenticated_slow_body_req << "Authorization: Basic "
                                  << base64_encode("slow-body-attacker:wrong-pass")
                                  << "\r\n";
    unauthenticated_slow_body_req << "Content-Type: application/json\r\n";
    unauthenticated_slow_body_req << "Content-Length: " << (1024 * 1024) << "\r\n";
    unauthenticated_slow_body_req << "Connection: close\r\n\r\n";
    expect_true(send_raw_http(port, unauthenticated_slow_body_req.str()).status == 401,
                "invalid Basic Auth must be rejected from headers before body read");

    std::ostringstream huge_header_req;
    huge_header_req << "POST / HTTP/1.1\r\n";
    huge_header_req << "X-Fill: " << std::string(64 * 1024, 'x') << "\r\n";
    huge_header_req << "Content-Length: 0\r\n";
    huge_header_req << "Connection: close\r\n\r\n";
    const auto huge_header_response = send_raw_http(port, huge_header_req.str());
    expect_true(huge_header_response.status == 413,
                "oversized HTTP headers must be rejected at the documented limit");

    const auto lower_case_response = send_raw_http(port, make_post_request(body, "admin", "admin-pass", true));
    expect_true(lower_case_response.status == 200, "lower-case HTTP headers must be accepted");
    expect_true(!lower_case_response.body.contains("error"), "lower-case HTTP headers should dispatch JSON-RPC");
    expect_true(lower_case_response.raw.find("Cache-Control: no-store") != std::string::npos &&
                    lower_case_response.raw.find("X-Content-Type-Options: nosniff") != std::string::npos &&
                    lower_case_response.raw.find("Connection: close") != std::string::npos,
                "RPC responses must disable caching, MIME sniffing and persistent reuse");

    std::ostringstream rebinding_req;
    rebinding_req << "POST / HTTP/1.1\r\n";
    rebinding_req << "Host: attacker.example\r\n";
    rebinding_req << "Authorization: Basic " << base64_encode("admin:admin-pass") << "\r\n";
    rebinding_req << "Content-Type: application/json\r\n";
    rebinding_req << "Content-Length: " << body.size() << "\r\n";
    rebinding_req << "Connection: close\r\n\r\n";
    rebinding_req << body;
    expect_true(send_raw_http(port, rebinding_req.str()).status == 421,
                "loopback RPC must reject hostile Host values used for DNS rebinding");

    std::string large_valid_payload = json{
        {"jsonrpc", "2.0"},
        {"id", 77},
        {"method", "help"},
        {"params", json::array()}
    }.dump();
    large_valid_payload.append(100 * 1024, ' ');
    const auto large_valid_body = send_raw_rpc(
        port,
        "admin",
        "admin-pass",
        large_valid_payload
    );
    expect_true(large_valid_body.status == 200 && !large_valid_body.body.contains("error"),
                "valid RPC bodies above the header buffer size must reach dispatch");

    std::ostringstream text_plain_req;
    text_plain_req << "POST / HTTP/1.1\r\n";
    text_plain_req << "Host: 127.0.0.1\r\n";
    text_plain_req << "Authorization: Basic " << base64_encode("admin:admin-pass") << "\r\n";
    text_plain_req << "Content-Type: text/plain\r\n";
    text_plain_req << "Content-Length: " << body.size() << "\r\n";
    text_plain_req << "Connection: close\r\n\r\n";
    text_plain_req << body;
    expect_true(send_raw_http(port, text_plain_req.str()).status == 415,
                "browser-simple text/plain RPC requests must be rejected");

    std::ostringstream charset_json_req;
    charset_json_req << "POST / HTTP/1.1\r\n";
    charset_json_req << "Host: 127.0.0.1\r\n";
    charset_json_req << "Authorization: Basic " << base64_encode("admin:admin-pass") << "\r\n";
    charset_json_req << "Content-Type: Application/JSON; charset=utf-8\r\n";
    charset_json_req << "Content-Length: " << body.size() << "\r\n";
    charset_json_req << "Connection: close\r\n\r\n";
    charset_json_req << body;
    expect_true(send_raw_http(port, charset_json_req.str()).status == 200,
                "application/json with a charset parameter remains accepted");

    std::ostringstream wrong_uri_req;
    wrong_uri_req << "POST /admin HTTP/1.1\r\n";
    wrong_uri_req << "Host: 127.0.0.1\r\n";
    wrong_uri_req << "Authorization: Basic " << base64_encode("admin:admin-pass") << "\r\n";
    wrong_uri_req << "Content-Type: application/json\r\n";
    wrong_uri_req << "Content-Length: " << body.size() << "\r\n";
    wrong_uri_req << "Connection: close\r\n\r\n";
    wrong_uri_req << body;
    expect_true(send_raw_http(port, wrong_uri_req.str()).status == 404,
                "RPC requests outside the root endpoint must be rejected");

    const auto duplicate_method = send_raw_rpc(
        port,
        "admin",
        "admin-pass",
        R"({"jsonrpc":"2.0","id":1,"method":"help","method":"stop","params":[]})"
    );
    expect_true(duplicate_method.status == 200 &&
                    duplicate_method.body["error"]["code"].get<int>() ==
                        geocoin::rpc::RPC_INVALID_REQUEST,
                "duplicate JSON method keys must be rejected instead of last-wins dispatch");

    const auto duplicate_nested_key = send_raw_rpc(
        port,
        "admin",
        "admin-pass",
        R"({"jsonrpc":"2.0","id":2,"method":"help","params":{"command":"help","command":"stop"}})"
    );
    expect_true(duplicate_nested_key.body["error"]["code"].get<int>() ==
                    geocoin::rpc::RPC_INVALID_REQUEST,
                "duplicate nested JSON keys must be rejected");

    std::string deeply_nested =
        R"({"jsonrpc":"2.0","id":3,"method":"help","params":)";
    deeply_nested.append(80, '[');
    deeply_nested += "0";
    deeply_nested.append(80, ']');
    deeply_nested += "}";
    const auto deep_response = send_raw_rpc(port, "admin", "admin-pass", deeply_nested);
    expect_true(deep_response.body["error"]["code"].get<int>() ==
                    geocoin::rpc::RPC_INVALID_REQUEST,
                "excessively nested JSON must be rejected before dispatch");

    std::string too_complex =
        R"({"jsonrpc":"2.0","id":4,"method":"help","params":[)";
    for (size_t i = 0; i < 50'100; ++i) {
        if (i > 0) {
            too_complex.push_back(',');
        }
        too_complex.push_back('0');
    }
    too_complex += "]}";
    const auto complex_response = send_raw_rpc(port, "admin", "admin-pass", too_complex);
    expect_true(complex_response.body["error"]["code"].get<int>() ==
                    geocoin::rpc::RPC_INVALID_REQUEST,
                "allocation-amplifying JSON must be rejected by the complexity guard");

    const std::string parse_secret = "RPC_PARSE_SECRET_MUST_NOT_LEAK";
    const auto malformed_json = send_raw_rpc(
        port,
        "admin",
        "admin-pass",
        "{\"jsonrpc\":\"2.0\",\"method\":\"help\",\"params\":[\"" +
            parse_secret + "\""
    );
    expect_true(malformed_json.body["error"]["code"].get<int>() ==
                    geocoin::rpc::RPC_PARSE_ERROR &&
                    malformed_json.body["error"]["message"] == "Parse error" &&
                    malformed_json.raw.find(parse_secret) == std::string::npos,
                "JSON syntax errors must not reflect request fragments or secrets");

    const auto wrong_jsonrpc = send_raw_rpc(
        port,
        "admin",
        "admin-pass",
        R"({"jsonrpc":"1.0","id":5,"method":"help","params":[]})"
    );
    expect_true(wrong_jsonrpc.body["error"]["code"].get<int>() ==
                    geocoin::rpc::RPC_INVALID_REQUEST,
                "an explicit non-2.0 JSON-RPC version must be rejected");

    const auto object_id = send_raw_rpc(
        port,
        "admin",
        "admin-pass",
        R"({"jsonrpc":"2.0","id":{"nested":1},"method":"help","params":[]})"
    );
    expect_true(object_id.body["error"]["code"].get<int>() ==
                    geocoin::rpc::RPC_INVALID_REQUEST && object_id.body["id"].is_null(),
                "object request ids must be rejected without reflection");

    const auto post_attack_health = send_rpc_method(port, "help", "admin", "admin-pass");
    expect_true(post_attack_health.status == 200 && !post_attack_health.body.contains("error"),
                "RPC listener must remain healthy after the malformed-input corpus");

    // Authentication failures deliberately throttle this principal, so keep
    // these terminal checks after authenticated requests for the same user.
    std::ostringstream no_auth_req;
    no_auth_req << "POST / HTTP/1.1\r\n";
    no_auth_req << "Host: 127.0.0.1\r\n";
    no_auth_req << "Content-Type: application/json\r\n";
    no_auth_req << "Content-Length: " << body.size() << "\r\n";
    no_auth_req << "Connection: close\r\n\r\n";
    no_auth_req << body;
    const auto no_auth_response = send_raw_http(port, no_auth_req.str());
    expect_true(no_auth_response.status == 401,
                "valid JSON-RPC request without auth must be rejected with 401");

    std::ostringstream malformed_auth_req;
    malformed_auth_req << "POST / HTTP/1.1\r\n";
    malformed_auth_req << "Host: 127.0.0.1\r\n";
    malformed_auth_req << "Authorization: Basic =\r\n";
    malformed_auth_req << "Content-Type: application/json\r\n";
    malformed_auth_req << "Content-Length: " << body.size() << "\r\n";
    malformed_auth_req << "Connection: close\r\n\r\n";
    malformed_auth_req << body;
    const auto malformed_auth_response = send_raw_http(port, malformed_auth_req.str());
    expect_true(malformed_auth_response.status == 401,
                "zero-sized malformed Basic Auth must be rejected with 401");
}

void test_raw_http_server_helpers() {
    {
        geocoin::rpc::HTTPRequestGate gate(2);
        expect_true(gate.try_acquire() && gate.try_acquire(),
                    "RPC request gate admits work up to its bound");
        expect_true(!gate.try_acquire(), "RPC request gate rejects an overflowing queue");
        gate.release();
        expect_true(gate.try_acquire(), "RPC request gate returns capacity after completion");
        gate.release();
        gate.release();
        gate.close();
        expect_true(!gate.try_acquire(), "closed RPC request gate rejects new work");
    }

    {
        geocoin::rpc::HTTPResponse explicit_response;
        explicit_response.status_code = 202;
        explicit_response.status_text = "Accepted";
        explicit_response.headers["X-Test"] = "ok";
        explicit_response.body = "body";
        const std::string raw = explicit_response.to_string();
        expect_true(raw.find("HTTP/1.1 202 Accepted") != std::string::npos,
                    "HTTPResponse formats status line");
        expect_true(raw.find("Content-Length: 4") != std::string::npos,
                    "HTTPResponse adds Content-Length when absent");
        expect_true(raw.find("Cache-Control: no-store") != std::string::npos &&
                        raw.find("X-Content-Type-Options: nosniff") != std::string::npos,
                    "HTTPResponse adds security headers");

        explicit_response.headers["X-Bad"] = "safe\r\nInjected: yes";
        const std::string injection_safe = explicit_response.to_string();
        expect_true(injection_safe.find("Injected: yes") == std::string::npos,
                    "HTTPResponse drops header values containing response-splitting controls");

        explicit_response.headers["Content-Length"] = "999";
        explicit_response.headers["Connection"] = "keep-alive";
        explicit_response.headers["Cache-Control"] = "public";
        explicit_response.headers["X-Content-Type-Options"] = "off";
        const std::string invariant_headers = explicit_response.to_string();
        expect_true(invariant_headers.find("Content-Length: 999") == std::string::npos &&
                        invariant_headers.find("Connection: keep-alive") == std::string::npos &&
                        invariant_headers.find("Cache-Control: public") == std::string::npos &&
                        invariant_headers.find("X-Content-Type-Options: off") == std::string::npos,
                    "handlers cannot override HTTP framing and browser-security invariants");

        explicit_response.status_text = "Accepted\r\nInjected: yes";
        const std::string safe_status = explicit_response.to_string();
        expect_true(safe_status.find("Injected: yes") == std::string::npos &&
                        safe_status.find("HTTP/1.1 500 Internal Server Error") != std::string::npos,
                    "HTTPResponse rejects status-line response splitting");

        const auto unauthorized = geocoin::rpc::create_unauthorized_response();
        expect_true(unauthorized.status_code == 401 &&
                        unauthorized.headers.count("WWW-Authenticate") == 1,
                    "unauthorized helper builds Basic challenge");

        const auto error = geocoin::rpc::create_error_response("unit failure");
        expect_true(error.status_code == 500 &&
                        geocoin::test::json::parse(error.body)["error"]["message"] == "unit failure",
                    "error helper serializes JSON-RPC error");

        const auto ok = geocoin::rpc::create_json_response(json{{"ok", true}});
        expect_true(ok.status_code == 200 &&
                        geocoin::test::json::parse(ok.body)["ok"].get<bool>(),
                    "json helper serializes OK response");
    }

    std::string parsed_user;
    std::string parsed_password;
    expect_true(!geocoin::rpc::parse_basic_auth("Basic " + base64_encode("useronly"),
                                                parsed_user,
                                                parsed_password),
                "Basic Auth without colon must be rejected");
    expect_true(!geocoin::rpc::parse_basic_auth("Basic " + base64_encode("user:"),
                                                parsed_user,
                                                parsed_password),
                "Basic Auth with empty password must be rejected");
    std::string padded_auth = base64_encode("user:pa");
    expect_true(!geocoin::rpc::parse_basic_auth("Basic " + padded_auth + "=",
                                                parsed_user,
                                                parsed_password),
                "Basic Auth with extra padding must be rejected");
    while (!padded_auth.empty() && padded_auth.back() == '=') {
        padded_auth.pop_back();
    }
    expect_true(!geocoin::rpc::parse_basic_auth("Basic " + padded_auth,
                                                parsed_user,
                                                parsed_password),
                "non-canonical unpadded Basic Auth must be rejected");
    expect_true(!geocoin::rpc::parse_basic_auth(
                    "Basic " + base64_encode(std::string(129, 'u') + ":pass"),
                    parsed_user,
                    parsed_password
                ),
                "oversized Basic Auth usernames must be rejected");
    expect_true(!geocoin::rpc::parse_basic_auth(
                    "Basic " + base64_encode("user:" + std::string(3070, 'p')),
                    parsed_user,
                    parsed_password
                ),
                "oversized decoded Basic Auth credentials must be rejected");

    geocoin::rpc::HTTPServer bad_host_server("not-an-ip-address", find_free_port());
    expect_true(!bad_host_server.start(), "HTTPServer must reject invalid bind host");
    bad_host_server.stop();

    geocoin::rpc::HTTPServer localhost_server("localhost", find_free_port());
    expect_true(localhost_server.start(), "HTTPServer accepts the documented localhost bind");
    localhost_server.stop();

    const uint16_t raw_port = find_free_port();
    geocoin::rpc::HTTPServer server("127.0.0.1", raw_port);
    expect_true(server.start(), "raw HTTPServer starts without a request handler");
    expect_true(server.is_running(), "raw HTTPServer reports running");
    expect_true(!server.start(), "raw HTTPServer refuses double start");
    bool handler_replacement_rejected = false;
    try {
        server.set_handler([](const geocoin::rpc::HTTPRequest&) {
            return geocoin::rpc::HTTPResponse{};
        });
    } catch (const std::logic_error&) {
        handler_replacement_rejected = true;
    }
    expect_true(handler_replacement_rejected,
                "raw HTTPServer rejects request-handler replacement while running");
    bool preflight_replacement_rejected = false;
    try {
        server.set_preflight_handler([](const geocoin::rpc::HTTPRequest&)
                                         -> std::optional<geocoin::rpc::HTTPResponse> {
            return std::nullopt;
        });
    } catch (const std::logic_error&) {
        preflight_replacement_rejected = true;
    }
    expect_true(preflight_replacement_rejected,
                "raw HTTPServer rejects preflight replacement while running");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    const auto no_handler_response = send_raw_http(
        raw_port,
        "GET / HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Connection: close\r\n\r\n"
    );
    expect_true(no_handler_response.status == 500,
                "raw HTTPServer without handler returns internal error");

    server.stop();
    expect_true(!server.is_running(), "raw HTTPServer reports stopped");
    server.stop();
    expect_true(server.start(), "raw HTTPServer can restart after a complete managed shutdown");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const auto restarted_response = send_raw_http(
        raw_port,
        "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n"
    );
    expect_true(restarted_response.status == 500,
                "restarted raw HTTPServer owns a valid request executor");
    server.stop();
}

void test_rate_limit_policy() {
    ScopedEnvVar max_requests("GEOCOIN_RPC_RATE_LIMIT_MAX_REQUESTS_PER_WINDOW", "2");

    geocoin::rpc::RPCServer server;
    const uint16_t port = find_free_port();
    expect_true(server.start("127.0.0.1", port, "rate-admin", "rate-pass"), "failed to start rate-limit RPC server");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    const auto first = send_rpc_method(port, "help", "rate-admin", "rate-pass");
    const auto second = send_rpc_method(port, "help", "rate-admin", "rate-pass");
    const auto third = send_rpc_method(port, "help", "rate-admin", "rate-pass");

    expect_true(first.status == 200, "first RPC request under rate limit should pass");
    expect_true(second.status == 200, "second RPC request under rate limit should pass");
    expect_true(third.status == 429, "third RPC request over configured rate limit must be rejected");
    expect_true(third.raw.find("Retry-After:") != std::string::npos, "rate limited response must include Retry-After");

    server.stop();

    geocoin::rpc::RPCServer body_server;
    const uint16_t body_port = find_free_port();
    expect_true(body_server.start(
                    "127.0.0.1", body_port, "body-admin", "body-pass"),
                "failed to start body-rate RPC server");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const auto large = send_raw_rpc(
        body_port,
        "body-admin",
        "body-pass",
        json{
            {"jsonrpc", "2.0"},
            {"id", 1},
            {"method", "help"},
            {"params", json::array({std::string(256 * 1024, 'x')})}
        }.dump()
    );
    const auto after_large = send_rpc_method(
        body_port, "help", "body-admin", "body-pass");
    expect_true(large.status == 200 && after_large.status == 429,
                "large authenticated bodies consume proportional rate-limit budget");
    body_server.stop();
}

void test_rate_limit_disabled_by_default() {
    ScopedEnvVar max_requests("GEOCOIN_RPC_RATE_LIMIT_MAX_REQUESTS_PER_WINDOW", "");
    ScopedEnvVar legacy("GEOCOIN_RPC_RATE_LIMIT_MAX_REQUESTS", "");

    geocoin::rpc::RPCServer server;
    const uint16_t port = find_free_port();
    expect_true(server.start("127.0.0.1", port, "open-admin", "open-pass"),
                "failed to start unlimited RPC server");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    for (int i = 0; i < 8; ++i) {
        const auto response = send_rpc_method(port, "help", "open-admin", "open-pass");
        expect_true(response.status == 200, "default rpc-rate-limit=0 must not return 429");
    }
    server.stop();
}

void test_rate_limit_from_server_context() {
    ScopedEnvVar max_requests("GEOCOIN_RPC_RATE_LIMIT_MAX_REQUESTS_PER_WINDOW", "");
    ScopedEnvVar legacy("GEOCOIN_RPC_RATE_LIMIT_MAX_REQUESTS", "");

    geocoin::rpc::RPCServer server;
    geocoin::rpc::RPCServerContext context;
    context.rpc_rate_limit_max_requests_per_window = 2;
    expect_true(server.set_context(context), "RPC context installs configured rate limit");
    const uint16_t port = find_free_port();
    expect_true(server.start("127.0.0.1", port, "ctx-admin", "ctx-pass"),
                "failed to start context-limited RPC server");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    expect_true(send_rpc_method(port, "help", "ctx-admin", "ctx-pass").status == 200,
                "first request under context rate limit should pass");
    expect_true(send_rpc_method(port, "help", "ctx-admin", "ctx-pass").status == 200,
                "second request under context rate limit should pass");
    expect_true(send_rpc_method(port, "help", "ctx-admin", "ctx-pass").status == 429,
                "third request over context rate limit must be rejected");
    server.stop();
}

bool connection_was_rejected(uint16_t port) {
    TcpStream socket;
    bool timed_out = false;
    if (!socket.connect_within("127.0.0.1", port, 1000, timed_out)) {
        return true;
    }
    try {
        socket.write_all("GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n");
        return socket.read_until_close().empty();
    } catch (const std::runtime_error&) {
        return true;
    }
}

void test_connection_limits_when_configured() {
    const uint16_t port = find_free_port();
    geocoin::rpc::HTTPServer server("127.0.0.1", port);
    server.set_connection_limits(0, 1);
    expect_true(server.start(), "HTTPServer starts with a per-IP connection cap");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    TcpStream stall;
    stall.connect("127.0.0.1", port);
    stall.write_all("POST / HTTP/1.1\r\nHost: 127.0.0.1\r\nX-Stall: 1");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    expect_true(connection_was_rejected(port),
                "configured rpc-max-connections-per-ip closes extra clients");
    stall.close();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    expect_true(!connection_was_rejected(port),
                "per-IP cap recovers after the stalled client disconnects");
    server.stop();
}

void test_connection_limits_disabled_by_default() {
    const uint16_t port = find_free_port();
    geocoin::rpc::HTTPServer server("127.0.0.1", port);
    expect_true(server.start(), "HTTPServer starts with unlimited connections");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    TcpStream stall;
    stall.connect("127.0.0.1", port);
    stall.write_all("POST / HTTP/1.1\r\nHost: 127.0.0.1\r\nX-Stall: 1");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    expect_true(!connection_was_rejected(port),
                "default rpc-max-connections-per-ip=0 must not drop extra clients");
    server.stop();
}

} // namespace

int main() {
    try {
        geocoin::rpc::RPCServer server;
        const uint16_t port = find_free_port();
        geocoin::rpc::RPCServerContext empty_context;
        expect_true(server.set_context(empty_context),
                    "RPC context can be configured before server start");
        expect_true(server.start("127.0.0.1", port, "admin", "admin-pass"), "failed to start HTTP policy RPC server");
        expect_true(!server.set_context(empty_context),
                    "RPC context replacement must be rejected while requests can run");
        geocoin::rpc::RPCCommand late_command;
        late_command.name = "unit.late";
        late_command.category = "unit";
        late_command.help = "unit.late";
        late_command.required_role = geocoin::rpc::RPCAuthRole::ADMIN;
        late_command.method = [](const json&) { return true; };
        bool late_registration_rejected = false;
        try {
            server.register_method(late_command);
        } catch (const std::logic_error&) {
            late_registration_rejected = true;
        }
        expect_true(late_registration_rejected,
                    "RPC command catalog must remain immutable while requests run");
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        test_http_parser_policy(port);
        server.stop();
        expect_true(server.set_context(empty_context),
                    "RPC context can be reconfigured after server stop");

        test_raw_http_server_helpers();
        test_rate_limit_policy();
        test_rate_limit_disabled_by_default();
        test_rate_limit_from_server_context();
        test_connection_limits_when_configured();
        test_connection_limits_disabled_by_default();

        std::cout << "RPC HTTP security tests passed.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << "\n";
        return 1;
    }
}
