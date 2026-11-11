#include "geocoin/rpc/rpc_protocol.hpp"
#include "geocoin/rpc/rpc_security.hpp"
#include "geocoin/rpc/rpc_dispatch.hpp"
#include "geocoin/utils/base64.hpp"
#include "geocoin/utils/secure_memory.hpp"

#include <algorithm>

namespace geocoin::rpc {

namespace {
constexpr size_t kMaxHttpResponseBodyBytes = 32 * 1024 * 1024; // 32 MB

bool is_base64_auth_payload(const std::string& encoded) {
    bool padding_started = false;
    for (const unsigned char c : encoded) {
        const bool is_base64_char =
            (c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
            c == '+' ||
            c == '/';
        if (c == '=') {
            padding_started = true;
            continue;
        }
        if (padding_started || !is_base64_char) {
            return false;
        }
    }
    return true;
}

}

bool parse_basic_auth(const std::string& auth_header, std::string& user, std::string& password) {
    user.clear();
    utils::cleanse_container(password);

    if (auth_header.rfind("Basic ", 0) != 0) {
        return false;
    }

    std::string encoded = auth_header.substr(6);
    utils::ScopedContainerCleanser encoded_cleanser(encoded);
    const auto begin = encoded.find_first_not_of(" \t\r\n");
    const auto end = encoded.find_last_not_of(" \t\r\n");
    encoded = begin == std::string::npos ? std::string{} : encoded.substr(begin, end - begin + 1);
    if (encoded.empty() ||
        encoded.size() > MAX_RPC_BASIC_AUTH_ENCODED_BYTES ||
        // Beast's decoded_size rounds down to complete quartets, while its
        // decoder also writes bytes from an incomplete final quartet. Reject
        // truncation before allocating/decoding, not only in the canonical check.
        encoded.size() % 4 != 0 ||
        !is_base64_auth_payload(encoded)) {
        return false;
    }

    // Decode base64
    std::string decoded;
    utils::ScopedContainerCleanser decoded_cleanser(decoded);
    const size_t decoded_capacity = utils::base64_decoded_size(encoded.size());
    if (decoded_capacity == 0 || decoded_capacity > MAX_RPC_BASIC_AUTH_DECODED_BYTES) {
        return false;
    }
    decoded.resize(decoded_capacity);

    auto result = utils::base64_decode(
        &decoded[0],
        encoded.data(),
        encoded.size()
    );
    for (size_t i = result.second; i < encoded.size(); ++i) {
        if (encoded[i] != '=') {
            return false;
        }
    }

    decoded.resize(result.first);
    if (decoded.empty() || decoded.size() > MAX_RPC_BASIC_AUTH_DECODED_BYTES) {
        return false;
    }

    // Accept one canonical RFC 4648 representation only. This rejects extra
    // padding, truncated quartets and non-zero unused bits that permissive
    // decoders can otherwise map to the same credential bytes.
    std::string canonical;
    utils::ScopedContainerCleanser canonical_cleanser(canonical);
    canonical.resize(utils::base64_encoded_size(decoded.size()));
    const size_t canonical_size = utils::base64_encode(
        canonical.data(),
        decoded.data(),
        decoded.size()
    );
    canonical.resize(canonical_size);
    if (canonical != encoded) {
        return false;
    }

    // Split user:password
    auto colon_pos = decoded.find(':');
    if (colon_pos == std::string::npos) {
        return false;
    }

    user = decoded.substr(0, colon_pos);
    password = decoded.substr(colon_pos + 1);
    if (!is_valid_rpc_credential_pair(user, password)) {
        user.clear();
        utils::cleanse_container(password);
        return false;
    }

    return true;
}

HTTPResponse create_unauthorized_response() {
    HTTPResponse response;
    response.status_code = 401;
    response.status_text = "Unauthorized";
    response.headers["WWW-Authenticate"] = "Basic realm=\"GeoCoin RPC\"";
    response.headers["Content-Type"] = "text/plain; charset=utf-8";
    response.body = "Unauthorized";
    return response;
}

HTTPResponse create_error_response(const std::string& message) {
    HTTPResponse response;
    response.status_code = 500;
    response.status_text = "Internal Server Error";
    response.headers["Content-Type"] = "application/json";

    response.body = make_json_rpc_error(RPC_INTERNAL_ERROR, message, nullptr).dump();
    return response;
}

HTTPResponse create_json_response(const json& body) {
    HTTPResponse response;
    response.status_code = 200;
    response.status_text = "OK";
    response.headers["Content-Type"] = "application/json";
    response.body = body.dump();
    if (response.body.size() > kMaxHttpResponseBodyBytes) {
        response.status_code = 500;
        response.status_text = "Response Too Large";
        response.body = make_json_rpc_error(
            RPC_MISC_ERROR, "RPC response exceeds the configured size limit",
            body.is_object() ? body.value("id", json(nullptr)) : json(nullptr)).dump();
    }
    return response;
}


} // namespace geocoin::rpc
