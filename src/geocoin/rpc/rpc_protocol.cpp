#include "geocoin/rpc/rpc_protocol.hpp"
#include "geocoin/rpc/rpc_security.hpp"
#include "geocoin/consensus/consensus.hpp"
#include "geocoin/utils/logger.hpp"
#include "geocoin/utils/secure_memory.hpp"
#include "rpc_socket.hpp"
#include <sstream>
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <utility>

namespace geocoin::rpc {

namespace {
constexpr size_t kMaxHttpHeaderBytes = 64 * 1024;     // 64 KB
constexpr size_t kMaxHttpBodyBytes =
    consensus::params::MAX_BLOCK_SIZE * 2 + 64 * 1024; // max block hex + JSON framing
constexpr size_t kMaxHttpUriBytes = 2048;
constexpr std::chrono::seconds kHttpReadTimeout{15};
constexpr std::chrono::seconds kHttpWriteTimeout{15};

struct HTTPParseException : public std::runtime_error {
    HTTPParseException(int status, std::string status_text, const std::string& message)
        : std::runtime_error(message)
        , status_code(status)
        , status_text(std::move(status_text)) {}

    int status_code;
    std::string status_text;
};

HTTPResponse create_plain_response(int status_code, const std::string& status_text, const std::string& body) {
    HTTPResponse response;
    response.status_code = status_code;
    response.status_text = status_text;
    response.headers["Content-Type"] = "text/plain";
    response.body = body;
    return response;
}

std::string canonical_header_key(std::string key) {
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return key;
}

std::string trim_http_ows(std::string value) {
    const auto is_ows = [](char c) { return c == ' ' || c == '\t'; };
    while (!value.empty() && is_ows(value.front())) {
        value.erase(value.begin());
    }
    while (!value.empty() && is_ows(value.back())) {
        value.pop_back();
    }
    return value;
}

bool is_http_token_char(unsigned char c) {
    return std::isalnum(c) ||
           c == '!' || c == '#' || c == '$' || c == '%' || c == '&' ||
           c == '\'' || c == '*' || c == '+' || c == '-' || c == '.' ||
           c == '^' || c == '_' || c == '`' || c == '|' || c == '~';
}

bool is_valid_header_key(const std::string& key) {
    return !key.empty() && std::all_of(key.begin(), key.end(), [](unsigned char c) {
        return is_http_token_char(c);
    });
}

bool has_invalid_header_value_chars(const std::string& value) {
    return std::any_of(value.begin(), value.end(), [](unsigned char c) {
        return (c < 0x20 && c != '\t') || c == 0x7f;
    });
}

bool has_invalid_request_line_chars(const std::string& value) {
    return std::any_of(value.begin(), value.end(), [](unsigned char c) {
        return c < 0x20 || c == 0x7f;
    });
}

bool is_reserved_response_header(const std::string& key) {
    const std::string canonical = canonical_header_key(key);
    return canonical == "content-length" ||
           canonical == "connection" ||
           canonical == "cache-control" ||
           canonical == "x-content-type-options";
}

bool is_valid_status_text(const std::string& value) {
    return !value.empty() &&
        std::none_of(value.begin(), value.end(), [](unsigned char c) {
            return c < 0x20 || c == 0x7f;
        });
}

void cleanse_http_request_secrets(HTTPRequest& request) noexcept {
    utils::cleanse_container(request.auth_password);
    utils::cleanse_container(request.body);
    const auto auth = request.headers.find("authorization");
    if (auth != request.headers.end()) {
        utils::cleanse_container(auth->second);
        request.headers.erase(auth);
    }
}
}

bool HTTPRequestGate::try_acquire() noexcept {
    if (!accepting_.load(std::memory_order_acquire)) {
        return false;
    }

    size_t current = pending_.load(std::memory_order_relaxed);
    while (current < max_pending_) {
        if (pending_.compare_exchange_weak(
                current,
                current + 1,
                std::memory_order_acq_rel,
                std::memory_order_relaxed)) {
            if (accepting_.load(std::memory_order_acquire)) {
                return true;
            }
            release();
            return false;
        }
    }
    return false;
}

void HTTPRequestGate::release() noexcept {
    size_t current = pending_.load(std::memory_order_relaxed);
    while (current > 0 &&
           !pending_.compare_exchange_weak(
               current,
               current - 1,
               std::memory_order_acq_rel,
               std::memory_order_relaxed)) {
    }
}

void HTTPRequestGate::close() noexcept {
    accepting_.store(false, std::memory_order_release);
}

// HTTPResponse implementation
std::string HTTPResponse::to_string() const {
    std::ostringstream oss;

    const bool valid_status = status_code >= 100 && status_code <= 599 &&
        is_valid_status_text(status_text);
    const int serialized_status = valid_status ? status_code : 500;
    const std::string serialized_text = valid_status
        ? status_text
        : "Internal Server Error";
    oss << "HTTP/1.1 " << serialized_status << " " << serialized_text << "\r\n";

    for (const auto& [key, value] : headers) {
        if (!is_reserved_response_header(key) &&
            is_valid_header_key(key) &&
            !has_invalid_header_value_chars(value)) {
            oss << key << ": " << value << "\r\n";
        }
    }

    // These framing and browser-security headers are invariants of the
    // connection, not handler-controlled metadata.
    oss << "Content-Length: " << body.size() << "\r\n";
    oss << "Connection: close\r\n";
    oss << "Cache-Control: no-store\r\n";
    oss << "X-Content-Type-Options: nosniff\r\n";

    oss << "\r\n";
    oss << body;

    return oss.str();
}

HTTPRequest parse_http_request(const std::string& data);

namespace {

using socket_detail::IoStatus;

bool send_response(std::intptr_t socket_fd, const HTTPResponse& response) {
    std::string bytes = response.to_string();
    utils::ScopedContainerCleanser<std::string> cleanser(bytes);
    const auto deadline = std::chrono::steady_clock::now() + kHttpWriteTimeout;
    return socket_detail::write_all(
               socket_detail::from_fd(socket_fd),
               bytes.data(),
               bytes.size(),
               deadline) == IoStatus::Ok;
}

} // namespace

bool serve_http_client(
    std::intptr_t socket_fd,
    std::string remote_addr,
    std::function<HTTPResponse(const HTTPRequest&)> handler,
    const std::shared_ptr<HTTPRequestGate>& request_gate,
    HTTPPreflightHandler preflight_handler,
    HTTPDispatchHandler dispatch) {
    HTTPRequest request;
    HTTPResponse response;
    std::array<char, 64 * 1024> body_chunk{};
    std::string raw;
    struct ConnectionSecrets {
        HTTPRequest& request;
        HTTPResponse& response;
        std::array<char, 64 * 1024>& body_chunk;
        std::string& raw;
        ~ConnectionSecrets() {
            cleanse_http_request_secrets(request);
            utils::cleanse_container(response.body);
            utils::cleanse_container(raw);
            OPENSSL_cleanse(body_chunk.data(), body_chunk.size());
        }
    } secrets{request, response, body_chunk, raw};

    const auto fd = socket_detail::from_fd(socket_fd);
    const auto header_deadline = std::chrono::steady_clock::now() + kHttpReadTimeout;
    raw.reserve(4096);
    while (raw.find("\r\n\r\n") == std::string::npos) {
        if (raw.size() >= kMaxHttpHeaderBytes) {
            response = create_plain_response(413, "Payload Too Large", "HTTP header too large");
            send_response(socket_fd, response);
            socket_detail::shutdown_both(fd);
            return true;
        }
        size_t got = 0;
        const IoStatus status = socket_detail::read_some(
            fd,
            body_chunk.data(),
            body_chunk.size(),
            got,
            header_deadline);
        if (status != IoStatus::Ok || got == 0) {
            return true;
        }
        raw.append(body_chunk.data(), got);
    }

    const auto marker = raw.find("\r\n\r\n");
    if (marker + 4 > kMaxHttpHeaderBytes) {
        response = create_plain_response(413, "Payload Too Large", "HTTP header too large");
        send_response(socket_fd, response);
        socket_detail::shutdown_both(fd);
        return true;
    }

    std::string headers_data;
    utils::ScopedContainerCleanser<std::string> headers_cleanser(headers_data);
    {
        std::istringstream header_stream(raw);
        std::string line;
        while (std::getline(header_stream, line) && line != "\r") {
            headers_data += line + "\n";
        }
    }
    std::string leftover = raw.substr(marker + 4);
    utils::ScopedContainerCleanser<std::string> leftover_cleanser(leftover);

    try {
        request = parse_http_request(headers_data);
        request.remote_addr = std::move(remote_addr);

        if (request.headers.find("transfer-encoding") != request.headers.end()) {
            const bool has_content_length =
                request.headers.find("content-length") != request.headers.end();
            response = create_plain_response(
                has_content_length ? 400 : 501,
                has_content_length ? "Bad Request" : "Not Implemented",
                has_content_length
                    ? "Transfer-Encoding and Content-Length cannot be combined"
                    : "Transfer-Encoding is not supported");
            send_response(socket_fd, response);
            socket_detail::shutdown_both(fd);
            return true;
        }

        size_t content_length = 0;
        const auto length_header = request.headers.find("content-length");
        const bool has_length = length_header != request.headers.end();
        if (has_length) {
            if (!parse_rpc_content_length(length_header->second, content_length)) {
                response = create_plain_response(400, "Bad Request", "Invalid Content-Length");
                send_response(socket_fd, response);
                socket_detail::shutdown_both(fd);
                return true;
            }
            if (content_length > kMaxHttpBodyBytes) {
                response = create_plain_response(413, "Payload Too Large", "Request body too large");
                send_response(socket_fd, response);
                socket_detail::shutdown_both(fd);
                return true;
            }
        } else if (request.method == "POST") {
            response = create_plain_response(411, "Length Required", "Content-Length is required");
            send_response(socket_fd, response);
            socket_detail::shutdown_both(fd);
            return true;
        }

        if (preflight_handler) {
            std::optional<HTTPResponse> rejection;
            try {
                rejection = preflight_handler(request);
            } catch (const std::exception& error) {
                GEO_LOG_ERROR(
                    "rpc",
                    "RPC header preflight exception: " <<
                        sanitize_rpc_log_value(error.what(), 256));
                response = create_error_response("Internal RPC error");
                send_response(socket_fd, response);
                socket_detail::shutdown_both(fd);
                return true;
            } catch (...) {
                GEO_LOG_ERROR("rpc", "RPC header preflight non-standard exception");
                response = create_error_response("Internal RPC error");
                send_response(socket_fd, response);
                socket_detail::shutdown_both(fd);
                return true;
            }
            if (rejection) {
                response = std::move(*rejection);
                send_response(socket_fd, response);
                socket_detail::shutdown_both(fd);
                return true;
            }
        }

        if (has_length) {
            const size_t buffered = std::min(leftover.size(), content_length);
            request.body.assign(leftover.data(), buffered);
            size_t remaining = content_length - buffered;
            const auto body_deadline = std::chrono::steady_clock::now() + kHttpReadTimeout;
            while (remaining > 0) {
                const size_t chunk_size = std::min(remaining, body_chunk.size());
                size_t got = 0;
                const IoStatus status = socket_detail::read_some(
                    fd,
                    body_chunk.data(),
                    chunk_size,
                    got,
                    body_deadline);
                if (status != IoStatus::Ok || got == 0 || got > remaining) {
                    response = create_plain_response(
                        400,
                        "Bad Request",
                        "Incomplete HTTP request body");
                    send_response(socket_fd, response);
                    socket_detail::shutdown_both(fd);
                    return true;
                }
                request.body.append(body_chunk.data(), got);
                remaining -= got;
            }
        }

        if (!request_gate || !request_gate->try_acquire()) {
            response = create_plain_response(503, "Service Unavailable", "RPC worker queue is full");
            response.headers["Retry-After"] = "1";
            send_response(socket_fd, response);
            socket_detail::shutdown_both(fd);
            return true;
        }

        if (dispatch) {
            if (!dispatch(socket_fd, std::move(request))) {
                request_gate->release();
                response = create_plain_response(503, "Service Unavailable", "RPC server is stopping");
                response.headers["Retry-After"] = "1";
                send_response(socket_fd, response);
                socket_detail::shutdown_both(fd);
                return true;
            }
            return false;
        }

        struct GateRelease {
            HTTPRequestGate* gate = nullptr;
            ~GateRelease() {
                if (gate != nullptr) {
                    gate->release();
                }
            }
        } gate_release{request_gate.get()};

        try {
            if (handler) {
                response = handler(request);
            } else {
                response = create_error_response("No request handler set");
            }
        } catch (const std::exception& error) {
            GEO_LOG_ERROR(
                "rpc",
                "Unhandled RPC handler exception: " <<
                    sanitize_rpc_log_value(error.what(), 256));
            response = create_error_response("Internal RPC error");
        } catch (...) {
            GEO_LOG_ERROR("rpc", "Unhandled non-standard RPC handler exception");
            response = create_error_response("Internal RPC error");
        }
        gate_release.gate = nullptr;
        request_gate->release();
    } catch (const HTTPParseException& error) {
        response = create_plain_response(error.status_code, error.status_text, error.what());
    } catch (const std::exception&) {
        GEO_LOG_WARN("rpc", "Unexpected HTTP request parsing failure");
        response = create_plain_response(400, "Bad Request", "Malformed HTTP request");
    }

    send_response(socket_fd, response);
    socket_detail::shutdown_both(fd);
    return true;
}

void finish_http_exchange(
    std::intptr_t socket_fd,
    HTTPRequest& request,
    HTTPResponse response) {
    cleanse_http_request_secrets(request);
    send_response(socket_fd, response);
    socket_detail::shutdown_both(socket_detail::from_fd(socket_fd));
    utils::cleanse_container(response.body);
}

HTTPRequest parse_http_request(const std::string& data) {
    HTTPRequest request;

    std::istringstream iss(data);
    std::string line;

    // Parse request line
    if (std::getline(iss, line)) {
        if (line.empty() || line.back() != '\r') {
            throw HTTPParseException(400, "Bad Request", "Non-canonical HTTP line ending");
        }
        line.pop_back();

        const size_t first_space = line.find(' ');
        const size_t second_space = first_space == std::string::npos
            ? std::string::npos
            : line.find(' ', first_space + 1);
        if (first_space == std::string::npos || second_space == std::string::npos ||
            first_space == 0 || second_space == first_space + 1 ||
            second_space + 1 >= line.size() ||
            line.find(' ', second_space + 1) != std::string::npos ||
            has_invalid_request_line_chars(line)) {
            throw HTTPParseException(400, "Bad Request", "Malformed request line");
        }
        request.method = line.substr(0, first_space);
        request.uri = line.substr(first_space + 1, second_space - first_space - 1);
        request.version = line.substr(second_space + 1);
        if (!std::all_of(request.method.begin(), request.method.end(), [](unsigned char c) {
                return is_http_token_char(c);
            }) ||
            has_invalid_header_value_chars(request.uri)) {
            throw HTTPParseException(400, "Bad Request", "Malformed request line");
        }
        if (request.uri.size() > kMaxHttpUriBytes) {
            throw HTTPParseException(414, "URI Too Long", "Request URI too long");
        }
        if (request.version != "HTTP/1.0" && request.version != "HTTP/1.1") {
            throw HTTPParseException(400, "Bad Request", "Unsupported HTTP version");
        }
    } else {
        throw HTTPParseException(400, "Bad Request", "Missing request line");
    }

    // Parse headers
    while (std::getline(iss, line)) {
        if (line.empty() || line.back() != '\r') {
            throw HTTPParseException(400, "Bad Request", "Non-canonical HTTP line ending");
        }
        line.pop_back();
        if (line.empty()) {
            break;
        }
        if (line.front() == ' ' || line.front() == '\t') {
            throw HTTPParseException(400, "Bad Request", "Obsolete folded header is not allowed");
        }

        auto colon_pos = line.find(':');
        if (colon_pos == std::string::npos) {
            throw HTTPParseException(400, "Bad Request", "Malformed header");
        }

        const std::string raw_key = line.substr(0, colon_pos);
        const std::string raw_value = line.substr(colon_pos + 1);
        if (!is_valid_header_key(raw_key) || has_invalid_header_value_chars(raw_value)) {
            throw HTTPParseException(400, "Bad Request", "Malformed header");
        }

        std::string key = canonical_header_key(raw_key);
        std::string value = trim_http_ows(raw_value);
        if (request.headers.find(key) != request.headers.end()) {
            throw HTTPParseException(400, "Bad Request", "Duplicate HTTP header");
        }
        request.headers[key] = value;
    }

    if (request.version == "HTTP/1.1") {
        const auto host = request.headers.find("host");
        if (host == request.headers.end() || host->second.empty()) {
            throw HTTPParseException(400, "Bad Request", "HTTP/1.1 Host header is required");
        }
    }

    // Parse Authorization header
    auto auth_it = request.headers.find("authorization");
    if (auth_it != request.headers.end()) {
        parse_basic_auth(auth_it->second, request.auth_user, request.auth_password);
        utils::cleanse_container(auth_it->second);
        request.headers.erase(auth_it);
    }

    // Read body
    std::ostringstream body_stream;
    body_stream << iss.rdbuf();
    request.body = body_stream.str();

    return request;
}

} // namespace geocoin::rpc
