#pragma once

#include "geocoin/rpc/rpc_types.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace geocoin::rpc {

using HTTPPreflightHandler =
    std::function<std::optional<HTTPResponse>(const HTTPRequest&)>;

class HTTPRequestGate {
public:
    explicit HTTPRequestGate(size_t max_pending) : max_pending_(max_pending) {}

    bool try_acquire() noexcept;
    void release() noexcept;
    void close() noexcept;

private:
    const size_t max_pending_;
    std::atomic<bool> accepting_{true};
    std::atomic<size_t> pending_{0};
};

/**
 * Called once headers and body are buffered and a handler slot is reserved.
 * Return false to refuse the handoff; the request is then answered with 503
 * and the socket stays with the caller. Return true to take ownership of the
 * socket and the gate slot.
 */
using HTTPDispatchHandler =
    std::function<bool(std::intptr_t socket_fd, HTTPRequest request)>;

/**
 * Read one HTTP/1.1 request and run the header preflight before the body.
 * Returns true when the caller still owns the socket. Returns false when
 * dispatch took the socket and the reserved handler slot.
 */
bool serve_http_client(
    std::intptr_t socket_fd,
    std::string remote_addr,
    std::function<HTTPResponse(const HTTPRequest&)> handler,
    const std::shared_ptr<HTTPRequestGate>& request_gate,
    HTTPPreflightHandler preflight_handler,
    HTTPDispatchHandler dispatch = {});

/** Write the response, then drop credentials from the request. */
void finish_http_exchange(
    std::intptr_t socket_fd,
    HTTPRequest& request,
    HTTPResponse response);

/**
 * @brief HTTP Server
 *
 * Implements Bitcoin-compatible JSON-RPC over HTTP
 */
class HTTPServer {
public:
    HTTPServer(const std::string& host, uint16_t port);
    ~HTTPServer();

    /**
     * @brief Start HTTP server
     */
    bool start();

    /**
     * @brief Stop HTTP server
     */
    void stop();

    /**
     * @brief Check if server is running
     */
    bool is_running() const { return running_; }

    /**
     * @brief Set request handler
     */
    void set_handler(std::function<HTTPResponse(const HTTPRequest&)> handler);

    /** Reject a request from headers alone, before its body is read. */
    void set_preflight_handler(HTTPPreflightHandler handler);

    /** 0 disables the corresponding cap. Must be set before start(). */
    void set_connection_limits(size_t max_connections, size_t max_connections_per_ip);

private:
    struct QueuedClient {
        std::intptr_t fd = -1;
        std::string remote_addr;
        std::string group_key;
    };

    struct HandlerJob {
        std::intptr_t fd = -1;
        HTTPRequest request;
        std::function<HTTPResponse(const HTTPRequest&)> handler;
        std::string group_key;
    };

    void accept_loop();
    void worker_loop(
        std::function<HTTPResponse(const HTTPRequest&)> handler,
        HTTPPreflightHandler preflight_handler);
    void handler_loop();
    bool enqueue_handler(HandlerJob job);
    void release_connection_for_ip(const std::string& remote_ip);
    void cleanup_locked();

    std::string host_;
    uint16_t port_;
    std::atomic<bool> running_{false};
    std::atomic<std::intptr_t> listen_fd_{-1};

    std::thread accept_thread_;
    std::vector<std::thread> worker_threads_;
    std::vector<std::thread> handler_threads_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<QueuedClient> queue_;
    std::mutex handler_mutex_;
    std::condition_variable handler_cv_;
    std::deque<HandlerJob> handler_queue_;
    std::vector<std::intptr_t> active_fds_;
    std::shared_ptr<HTTPRequestGate> request_gate_;
    std::mutex lifecycle_mutex_;
    std::atomic<size_t> active_connections_{0};
    // 0 disables the cap. Production can set these from geocoin.conf.
    size_t max_connections_{0};
    size_t max_connections_per_ip_{0};
    std::mutex connection_counts_mutex_;
    std::unordered_map<std::string, size_t> active_connections_by_ip_;

    std::function<HTTPResponse(const HTTPRequest&)> request_handler_;
    HTTPPreflightHandler preflight_handler_;
};

/**
 * @brief Parse HTTP Basic Auth header
 */
bool parse_basic_auth(const std::string& auth_header, std::string& user, std::string& password);

/**
 * @brief Create HTTP 401 Unauthorized response
 */
HTTPResponse create_unauthorized_response();

/**
 * @brief Create HTTP 500 Internal Server Error response
 */
HTTPResponse create_error_response(const std::string& message);

/**
 * @brief Create HTTP 200 OK response with JSON body
 */
HTTPResponse create_json_response(const json& body);

} // namespace geocoin::rpc
