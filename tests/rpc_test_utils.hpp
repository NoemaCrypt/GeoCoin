#pragma once

#include "geocoin/core/transaction.hpp"
#include "geocoin/rpc/rpc_encoding.hpp"
#include "geocoin/utils/base64.hpp"
#include "nlohmann_json.hpp"
#include "tcp_test_client.hpp"

#include <cstdint>
#include <sstream>
#include <stdexcept>
#include <string>

namespace geocoin::test {

using json = nlohmann::json;

struct RpcHttpResponse {
    int status = 0;
    std::string raw;
    json body;
};

inline void expect_true(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

inline uint16_t find_free_port() {
    return bind_free_loopback_port();
}

inline std::string base64_encode(const std::string& input) {
    return utils::base64_encode(input);
}

inline std::string json_rpc_body(const std::string& method, const json& params = json::array()) {
    return json{
        {"jsonrpc", "2.0"},
        {"id", 1},
        {"method", method},
        {"params", params}
    }.dump();
}

inline RpcHttpResponse parse_raw_response(const std::string& raw) {
    RpcHttpResponse response;
    response.raw = raw;

    std::istringstream status_stream(raw);
    std::string http_version;
    status_stream >> http_version >> response.status;

    const auto body_pos = raw.find("\r\n\r\n");
    if (body_pos != std::string::npos &&
        body_pos + 4 < raw.size() &&
        (response.status == 200 || raw.find("application/json") != std::string::npos)) {
        response.body = json::parse(raw.substr(body_pos + 4));
    }

    return response;
}

inline RpcHttpResponse send_raw_http(uint16_t port, const std::string& request) {
    TcpStream socket;
    socket.connect("127.0.0.1", port);
    socket.write_all(request);
    return parse_raw_response(socket.read_until_close());
}

inline std::string make_post_request(
    const std::string& body,
    const std::string& user = "admin",
    const std::string& pass = "admin-pass",
    bool lower_case_headers = false
) {
    std::ostringstream req;
    req << "POST / HTTP/1.1\r\n";
    req << (lower_case_headers ? "host" : "Host") << ": 127.0.0.1\r\n";
    req << (lower_case_headers ? "authorization" : "Authorization")
        << ": Basic " << base64_encode(user + ":" + pass) << "\r\n";
    req << (lower_case_headers ? "content-type" : "Content-Type") << ": application/json\r\n";
    req << (lower_case_headers ? "content-length" : "Content-Length") << ": " << body.size() << "\r\n";
    req << (lower_case_headers ? "connection" : "Connection") << ": close\r\n\r\n";
    req << body;
    return req.str();
}

inline RpcHttpResponse send_raw_rpc(
    uint16_t port,
    const std::string& user,
    const std::string& pass,
    const std::string& body
) {
    return send_raw_http(port, make_post_request(body, user, pass));
}

inline RpcHttpResponse send_rpc(
    uint16_t port,
    const std::string& user,
    const std::string& pass,
    const std::string& method,
    const json& params = json::array()
) {
    return send_raw_rpc(port, user, pass, json_rpc_body(method, params));
}

inline RpcHttpResponse send_rpc_method(
    uint16_t port,
    const std::string& method,
    const std::string& user = "admin",
    const std::string& pass = "admin-pass"
) {
    return send_rpc(port, user, pass, method);
}

inline core::Transaction decode_raw_tx_or_throw(const std::string& hex) {
    bytes data;
    expect_true(rpc::hex_to_bytes_checked(hex, data), "raw tx should be valid hex");
    size_t offset = 0;
    core::Transaction tx = core::Transaction::deserialize(data, offset);
    expect_true(offset == data.size(), "raw tx should deserialize exactly");
    return tx;
}

} // namespace geocoin::test
