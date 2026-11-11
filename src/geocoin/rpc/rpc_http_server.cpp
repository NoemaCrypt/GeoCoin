#include "geocoin/rpc/rpc_protocol.hpp"
#include "geocoin/rpc/rpc_security.hpp"
#include "geocoin/utils/logger.hpp"
#include "rpc_socket.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace geocoin::rpc {

namespace {
constexpr size_t kMaxPendingRpcRequests = 32;
constexpr int kHttpWorkerCount = 4;
}

HTTPServer::HTTPServer(const std::string& host, uint16_t port)
    : host_(host), port_(port) {
}

HTTPServer::~HTTPServer() {
    stop();
}

bool HTTPServer::start() {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    if (running_.load(std::memory_order_acquire)) {
        return false;
    }

    cleanup_locked();
    try {
        std::string address_text = host_;
        if (address_text == "localhost") {
            address_text = "127.0.0.1";
        } else if (address_text.size() > 2 &&
                   address_text.front() == '[' && address_text.back() == ']') {
            address_text = address_text.substr(1, address_text.size() - 2);
        }

        request_gate_ = std::make_shared<HTTPRequestGate>(kMaxPendingRpcRequests);
        listen_fd_.store(socket_detail::listen_tcp(address_text, port_), std::memory_order_release);
        running_.store(true, std::memory_order_release);

        const auto handler = request_handler_;
        const auto preflight = preflight_handler_;
        for (int i = 0; i < kHttpWorkerCount; ++i) {
            handler_threads_.emplace_back([this]() { handler_loop(); });
        }
        for (int i = 0; i < kHttpWorkerCount; ++i) {
            worker_threads_.emplace_back([this, handler, preflight]() {
                worker_loop(handler, preflight);
            });
        }
        accept_thread_ = std::thread([this]() { accept_loop(); });

        GEO_LOG_INFO("rpc", "RPC server started on " << host_ << ":" << port_);
        return true;
    } catch (const std::exception& e) {
        GEO_LOG_ERROR("rpc", "Failed to start RPC server: " << e.what());
        running_.store(false, std::memory_order_release);
        cleanup_locked();
        return false;
    }
}

void HTTPServer::stop() {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    const bool was_running = running_.exchange(false, std::memory_order_acq_rel);
    cleanup_locked();
    if (was_running) {
        GEO_LOG_INFO("rpc", "RPC server stopped");
    }
}

void HTTPServer::cleanup_locked() {
    if (request_gate_) {
        request_gate_->close();
    }
    running_.store(false, std::memory_order_release);

    const auto listen_fd = listen_fd_.exchange(-1, std::memory_order_acq_rel);
    if (listen_fd >= 0) {
        const auto socket = socket_detail::from_fd(listen_fd);
        socket_detail::shutdown_both(socket);
        socket_detail::close_socket(socket);
    }

    std::deque<QueuedClient> discarded;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        discarded.swap(queue_);
        for (const std::intptr_t fd : active_fds_) {
            socket_detail::shutdown_both(socket_detail::from_fd(fd));
        }
    }
    queue_cv_.notify_all();
    handler_cv_.notify_all();

    for (const auto& client : discarded) {
        const auto socket = socket_detail::from_fd(client.fd);
        socket_detail::shutdown_both(socket);
        socket_detail::close_socket(socket);
        release_connection_for_ip(client.group_key);
    }

    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }
    for (auto& thread : worker_threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    worker_threads_.clear();

    std::deque<HandlerJob> abandoned_handlers;
    {
        std::lock_guard<std::mutex> lock(handler_mutex_);
        abandoned_handlers.swap(handler_queue_);
    }
    handler_cv_.notify_all();
    for (auto& thread : handler_threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    handler_threads_.clear();
    for (auto& job : abandoned_handlers) {
        const auto socket = socket_detail::from_fd(job.fd);
        socket_detail::shutdown_both(socket);
        socket_detail::close_socket(socket);
        if (request_gate_) {
            request_gate_->release();
        }
        release_connection_for_ip(job.group_key);
        std::lock_guard<std::mutex> lock(queue_mutex_);
        const auto it = std::find(active_fds_.begin(), active_fds_.end(), job.fd);
        if (it != active_fds_.end()) {
            active_fds_.erase(it);
        }
    }

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        for (const std::intptr_t fd : active_fds_) {
            socket_detail::close_socket(socket_detail::from_fd(fd));
        }
        active_fds_.clear();
        for (const auto& client : queue_) {
            socket_detail::close_socket(socket_detail::from_fd(client.fd));
            release_connection_for_ip(client.group_key);
        }
        queue_.clear();
    }

    request_gate_.reset();
    active_connections_.store(0, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> counts_lock(connection_counts_mutex_);
        active_connections_by_ip_.clear();
    }
}

void HTTPServer::set_handler(std::function<HTTPResponse(const HTTPRequest&)> handler) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    if (running_.load(std::memory_order_acquire)) {
        throw std::logic_error("HTTP request handler cannot change while the server is running");
    }
    request_handler_ = std::move(handler);
}

void HTTPServer::set_preflight_handler(HTTPPreflightHandler handler) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    if (running_.load(std::memory_order_acquire)) {
        throw std::logic_error("HTTP preflight handler cannot change while the server is running");
    }
    preflight_handler_ = std::move(handler);
}

void HTTPServer::set_connection_limits(size_t max_connections, size_t max_connections_per_ip) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    if (running_.load(std::memory_order_acquire)) {
        throw std::logic_error("HTTP connection limits cannot change while the server is running");
    }
    max_connections_ = max_connections;
    max_connections_per_ip_ = max_connections_per_ip;
}

void HTTPServer::release_connection_for_ip(const std::string& remote_ip) {
    size_t active = active_connections_.load(std::memory_order_relaxed);
    while (active > 0 &&
           !active_connections_.compare_exchange_weak(
               active,
               active - 1,
               std::memory_order_relaxed,
               std::memory_order_relaxed)) {
    }
    std::lock_guard<std::mutex> lock(connection_counts_mutex_);
    auto it = active_connections_by_ip_.find(remote_ip);
    if (it == active_connections_by_ip_.end()) {
        return;
    }
    if (it->second <= 1) {
        active_connections_by_ip_.erase(it);
    } else {
        --it->second;
    }
}

void HTTPServer::worker_loop(
    std::function<HTTPResponse(const HTTPRequest&)> handler,
    HTTPPreflightHandler preflight_handler) {
    while (true) {
        QueuedClient client;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait(lock, [this]() {
                return !running_.load(std::memory_order_acquire) || !queue_.empty();
            });
            if (queue_.empty()) {
                return;
            }
            client = std::move(queue_.front());
            queue_.pop_front();
            active_fds_.push_back(client.fd);
        }

        bool caller_owns_socket = true;
        try {
            caller_owns_socket = serve_http_client(
                client.fd,
                client.remote_addr,
                handler,
                request_gate_,
                preflight_handler,
                [this, handler, group_key = client.group_key](std::intptr_t fd, HTTPRequest request) {
                    return enqueue_handler(HandlerJob{
                        fd,
                        std::move(request),
                        handler,
                        group_key
                    });
                });
        } catch (const std::exception& error) {
            GEO_LOG_ERROR(
                "rpc",
                "HTTP connection failed: " << sanitize_rpc_log_value(error.what(), 256));
        } catch (...) {
            GEO_LOG_ERROR("rpc", "HTTP connection failed");
        }

        if (!caller_owns_socket) {
            continue;
        }
        socket_detail::close_socket(socket_detail::from_fd(client.fd));
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            const auto it = std::find(active_fds_.begin(), active_fds_.end(), client.fd);
            if (it != active_fds_.end()) {
                active_fds_.erase(it);
            }
        }
        release_connection_for_ip(client.group_key);
    }
}

bool HTTPServer::enqueue_handler(HandlerJob job) {
    {
        std::lock_guard<std::mutex> lock(handler_mutex_);
        if (!running_.load(std::memory_order_acquire)) {
            return false;
        }
        handler_queue_.push_back(std::move(job));
    }
    handler_cv_.notify_one();
    return true;
}

void HTTPServer::handler_loop() {
    while (true) {
        HandlerJob job;
        {
            std::unique_lock<std::mutex> lock(handler_mutex_);
            handler_cv_.wait(lock, [this]() {
                return !running_.load(std::memory_order_acquire) || !handler_queue_.empty();
            });
            if (handler_queue_.empty()) {
                return;
            }
            job = std::move(handler_queue_.front());
            handler_queue_.pop_front();
        }

        HTTPResponse response;
        try {
            if (job.handler) {
                response = job.handler(job.request);
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

        finish_http_exchange(job.fd, job.request, std::move(response));
        socket_detail::close_socket(socket_detail::from_fd(job.fd));
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            const auto it = std::find(active_fds_.begin(), active_fds_.end(), job.fd);
            if (it != active_fds_.end()) {
                active_fds_.erase(it);
            }
        }
        if (request_gate_) {
            request_gate_->release();
        }
        release_connection_for_ip(job.group_key);
    }
}

void HTTPServer::accept_loop() {
    const auto listen_fd = socket_detail::from_fd(listen_fd_.load(std::memory_order_acquire));
    while (running_.load(std::memory_order_acquire)) {
        sockaddr_storage address{};
        socklen_t length = sizeof(address);
        const auto client = ::accept(
            listen_fd,
            reinterpret_cast<sockaddr*>(&address),
            &length);
        if (!socket_detail::valid(client)) {
            if (!running_.load(std::memory_order_acquire)) {
                break;
            }
            if (socket_detail::interrupted(socket_detail::last_error())) {
                continue;
            }
            break;
        }
        if (!running_.load(std::memory_order_acquire)) {
            socket_detail::close_socket(client);
            break;
        }

        const std::string remote_addr = socket_detail::peer_address(client);
        const std::string group_key = rpc_client_network_key(remote_addr);
        bool accepted = false;
        {
            std::lock_guard<std::mutex> lock(connection_counts_mutex_);
            const auto it = active_connections_by_ip_.find(group_key);
            const size_t per_ip = (it == active_connections_by_ip_.end()) ? 0 : it->second;
            const size_t active = active_connections_.load(std::memory_order_relaxed);
            const bool under_global = max_connections_ == 0 || active < max_connections_;
            const bool under_ip = max_connections_per_ip_ == 0 || per_ip < max_connections_per_ip_;
            if (under_global && under_ip) {
                active_connections_by_ip_[group_key] = per_ip + 1;
                active_connections_.fetch_add(1, std::memory_order_relaxed);
                accepted = true;
            }
        }

        if (!accepted) {
            socket_detail::shutdown_both(client);
            socket_detail::close_socket(client);
            continue;
        }
        if (!socket_detail::set_nonblocking(client)) {
            socket_detail::close_socket(client);
            release_connection_for_ip(group_key);
            continue;
        }

        bool queued = false;
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            if (running_.load(std::memory_order_acquire)) {
                queue_.push_back(QueuedClient{
                    socket_detail::to_fd(client),
                    remote_addr,
                    group_key
                });
                queued = true;
            }
        }
        if (!queued) {
            socket_detail::close_socket(client);
            release_connection_for_ip(group_key);
            break;
        }
        queue_cv_.notify_one();
    }
}

} // namespace geocoin::rpc
