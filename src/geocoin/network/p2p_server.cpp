#include "geocoin/network/p2p_server.hpp"
#include "geocoin/network/socket_owner.hpp"
#include "geocoin/network/network_utils.hpp"
#include "geocoin/utils/logger.hpp"
#include "geocoin/utils/secure_memory.hpp"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#endif
#include <cstring>
#include <cerrno>
#include <chrono>

namespace geocoin::network {

namespace {
std::string extract_ip(const std::string& peer_address) {
    std::string host;
    uint16_t port = 0;
    if (NetworkUtils::split_host_port(peer_address, host, port)) {
        return host;
    }
    return NetworkUtils::normalize_ip_address(peer_address);
}

#ifdef _WIN32
void ensure_winsock_initialized() {
    static const bool initialized = []() {
        WSADATA wsa_data;
        return WSAStartup(MAKEWORD(2, 2), &wsa_data) == 0;
    }();
    (void)initialized;
}

int last_socket_error() {
    return WSAGetLastError();
}

void close_socket_portable(SocketHandle socket_fd) {
    if (socket_fd >= 0) {
        closesocket(static_cast<SOCKET>(socket_fd));
    }
}

constexpr int kShutdownBoth = SD_BOTH;
using socket_length_t = int;
#else
void ensure_winsock_initialized() {}

int last_socket_error() {
    return errno;
}

void close_socket_portable(SocketHandle socket_fd) {
    if (socket_fd >= 0) {
        close(socket_fd);
    }
}

constexpr int kShutdownBoth = SHUT_RDWR;
using socket_length_t = socklen_t;
#endif
} // namespace

P2PServer::P2PServer(
    std::shared_ptr<PeerManager> peer_manager,
    const bytes& our_private_key,
    uint16_t port,
    std::vector<std::string> inbound_whitelist_ips
)
    : peer_manager_(peer_manager)
    , our_private_key_(our_private_key)
    , port_(port)
    , server_socket_(-1)
    , running_(false)
    , accepted_connections_(0)
    , last_start_error_(0)
    , inbound_whitelist_ips_(std::move(inbound_whitelist_ips))
{
    for (auto& ip : inbound_whitelist_ips_) {
        ip = NetworkUtils::normalize_ip_address(ip);
    }
}

P2PServer::~P2PServer() {
    try {
        stop();
    } catch (...) {
        // Подавляем все исключения в деструкторе
    }
    utils::cleanse_container(our_private_key_);
}

bool P2PServer::start() {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    if (running_) {
        return false;
    }
    last_start_error_ = 0;
    ensure_winsock_initialized();

    // Предпочитаем dual-stack IPv6 сокет (IPv6 + IPv4-mapped), с fallback на IPv4.
    bool use_ipv6_listener = true;
    server_socket_ = static_cast<SocketHandle>(socket(AF_INET6, SOCK_STREAM, 0));
#ifdef _WIN32
    if (server_socket_ == static_cast<SocketHandle>(INVALID_SOCKET)) {
#else
    if (server_socket_ < 0) {
#endif
        use_ipv6_listener = false;
        server_socket_ = static_cast<SocketHandle>(socket(AF_INET, SOCK_STREAM, 0));
#ifdef _WIN32
        if (server_socket_ == static_cast<SocketHandle>(INVALID_SOCKET)) {
#else
        if (server_socket_ < 0) {
#endif
            last_start_error_ = last_socket_error();
            GEO_LOG_ERROR("p2p", "Failed to create server socket");
            return false;
        }
    }

    // Разрешаем переиспользование адреса
    int opt = 1;
    if (
#ifdef _WIN32
        setsockopt(server_socket_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt)) < 0
#else
        setsockopt(server_socket_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0
#endif
    ) {
        last_start_error_ = last_socket_error();
        GEO_LOG_ERROR("p2p", "Failed to set SO_REUSEADDR");
        close_socket_portable(server_socket_);
        server_socket_ = -1;
        return false;
    }

    // Разрешаем dual-stack режим (если поддерживается платформой).
    if (use_ipv6_listener) {
#ifdef IPV6_V6ONLY
        int v6only = 0;
        (void)setsockopt(server_socket_, IPPROTO_IPV6, IPV6_V6ONLY,
#ifdef _WIN32
            reinterpret_cast<const char*>(&v6only),
#else
            &v6only,
#endif
            sizeof(v6only));
#endif
    }

    // Настраиваем адрес и привязываем сокет
    int bind_rc = -1;
    if (use_ipv6_listener) {
        struct sockaddr_in6 server_addr;
        std::memset(&server_addr, 0, sizeof(server_addr));
        server_addr.sin6_family = AF_INET6;
        server_addr.sin6_port = htons(port_);
        bind_rc = bind(server_socket_, reinterpret_cast<struct sockaddr*>(&server_addr), sizeof(server_addr));
    } else {
        struct sockaddr_in server_addr;
        std::memset(&server_addr, 0, sizeof(server_addr));
        server_addr.sin_family = AF_INET;
        server_addr.sin_addr.s_addr = INADDR_ANY;
        server_addr.sin_port = htons(port_);
        bind_rc = bind(server_socket_, reinterpret_cast<struct sockaddr*>(&server_addr), sizeof(server_addr));
    }
    if (bind_rc < 0) {
        last_start_error_ = last_socket_error();
        GEO_LOG_ERROR("p2p", "Failed to bind to port " << port_);
        close_socket_portable(server_socket_);
        server_socket_ = -1;
        return false;
    }

    // Начинаем прослушивание
    if (listen(server_socket_, 128) < 0) {
        last_start_error_ = last_socket_error();
        GEO_LOG_ERROR("p2p", "Failed to listen on port " << port_);
        close_socket_portable(server_socket_);
        server_socket_ = -1;
        return false;
    }

    // A bounded nonblocking accept loop also stops reliably on platforms
    // where shutdown() alone does not wake a listening socket.
#ifdef _WIN32
    u_long nonblocking = 1;
    const bool nonblocking_ok = ioctlsocket(static_cast<SOCKET>(server_socket_), FIONBIO, &nonblocking) == 0;
#else
    const int flags = fcntl(server_socket_, F_GETFL, 0);
    const bool nonblocking_ok = flags >= 0 && fcntl(server_socket_, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
    if (!nonblocking_ok) {
        last_start_error_ = last_socket_error();
        close_socket_portable(server_socket_);
        server_socket_ = -1;
        return false;
    }

    sockaddr_storage bound_address{};
    socket_length_t bound_length = sizeof(bound_address);
    if (getsockname(server_socket_, reinterpret_cast<sockaddr*>(&bound_address), &bound_length) != 0) {
        last_start_error_ = last_socket_error();
        close_socket_portable(server_socket_);
        server_socket_ = -1;
        return false;
    }
    port_ = bound_address.ss_family == AF_INET6
        ? ntohs(reinterpret_cast<const sockaddr_in6*>(&bound_address)->sin6_port)
        : ntohs(reinterpret_cast<const sockaddr_in*>(&bound_address)->sin_port);

    // Запускаем поток приема соединений
    running_ = true;
    try {
        accept_thread_ = std::thread(&P2PServer::accept_loop, this);
    } catch (const std::system_error& error) {
        running_ = false;
        last_start_error_ = error.code().value();
        close_socket_portable(server_socket_);
        server_socket_ = -1;
        return false;
    }

    GEO_LOG_INFO("p2p", "P2P Server started on port " << port_);

    return true;
}

void P2PServer::stop() {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    running_ = false;
    // Keep the descriptor allocated and unchanged until accept_loop exits.
    // Otherwise an unrelated file can reuse it while accept is still running.
    if (server_socket_ >= 0) {
        shutdown(server_socket_, kShutdownBoth);
    }
    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }
    if (server_socket_ >= 0) {
        close_socket_portable(server_socket_);
        server_socket_ = -1;
        GEO_LOG_INFO("p2p", "P2P Server stopped");
    }
}

void P2PServer::accept_loop() {
    constexpr size_t kMaxConnPerWindow = 20;
    constexpr size_t kMaxTrackedInboundIps = 8192;
    constexpr auto kWindow = std::chrono::seconds(60);
    constexpr auto kBanTime = std::chrono::minutes(10);

    while (running_) {
        struct sockaddr_storage client_addr;
        socket_length_t client_len = sizeof(client_addr);

        // Принимаем соединение
        SocketHandle client_socket = static_cast<SocketHandle>(accept(
            server_socket_,
            (struct sockaddr*)&client_addr,
            &client_len
        ));

        if (
#ifdef _WIN32
            client_socket == static_cast<SocketHandle>(INVALID_SOCKET)
#else
            client_socket < 0
#endif
        ) {
            if (running_) {
                const int error = last_socket_error();
#ifdef _WIN32
                const bool transient = error == WSAEWOULDBLOCK || error == WSAEINTR;
#else
                const bool transient = error == EAGAIN || error == EWOULDBLOCK || error == EINTR;
#endif
                if (!transient) GEO_LOG_ERROR("p2p", "Failed to accept connection (error=" << error << ")");
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            continue;
        }

        detail::SocketOwner accepted(client_socket);
        try {
            if (!running_) {

                break;
            }

            // Получаем адрес клиента
            std::string peer_address = get_peer_address(client_socket);
            std::string peer_ip = extract_ip(peer_address);
            if (peer_address == "unknown" || peer_ip.empty() || peer_ip == "unknown") {
                GEO_LOG_WARN("p2p", "Inbound with unknown peer address rejected");

                continue;
            }
            const std::string rate_key = NetworkUtils::abuse_key(peer_ip);
            if (rate_key.empty()) {
                GEO_LOG_WARN("p2p", "Inbound with non-numeric peer address rejected");

                continue;
            }

            // Inbound whitelist for critical nodes (if configured)
            if (!inbound_whitelist_ips_.empty()) {
                bool allowed = false;
                for (const auto& ip : inbound_whitelist_ips_) {
                    if (ip == peer_ip) {
                        allowed = true;
                        break;
                    }
                }
                if (!allowed) {
                    GEO_LOG_WARN("p2p", "Inbound from non-whitelisted IP " << peer_ip << " rejected");

                    continue;
                }
            }

            // Inbound rate limiting / temporary ban
            {
                const auto now = std::chrono::steady_clock::now();
                std::lock_guard<std::mutex> lock(rate_mutex_);

                for (auto it = banned_ips_.begin(); it != banned_ips_.end();) {
                    if (now >= it->second) {
                        it = banned_ips_.erase(it);
                    } else {
                        ++it;
                    }
                }
                for (auto it = recent_accepts_.begin(); it != recent_accepts_.end();) {
                    auto& q = it->second;
                    while (!q.empty() && (now - q.front()) > kWindow) {
                        q.pop_front();
                    }
                    if (q.empty()) {
                        it = recent_accepts_.erase(it);
                    } else {
                        ++it;
                    }
                }
                while (recent_accepts_.size() > kMaxTrackedInboundIps) {
                    recent_accepts_.erase(recent_accepts_.begin());
                }
                while (banned_ips_.size() > kMaxTrackedInboundIps) {
                    banned_ips_.erase(banned_ips_.begin());
                }

                auto ban_it = banned_ips_.find(rate_key);
                if (ban_it != banned_ips_.end()) {
                    if (now < ban_it->second) {
                        GEO_LOG_WARN("p2p", "Inbound from banned IP " << peer_ip << " rejected");

                        continue;
                    }
                    banned_ips_.erase(ban_it);
                }

                auto& q = recent_accepts_[rate_key];
                while (!q.empty() && (now - q.front()) > kWindow) {
                    q.pop_front();
                }
                q.push_back(now);
                if (q.size() > kMaxConnPerWindow) {
                    banned_ips_[rate_key] = now + kBanTime;
                    q.clear();
                    GEO_LOG_WARN(
                        "p2p",
                        "Rate limit exceeded by " << peer_ip
                            << " (" << kMaxConnPerWindow << " per " << kWindow.count()
                            << "s). Temporary ban " << kBanTime.count() << "s"
                    );

                    continue;
                }
            }

            GEO_LOG_INFO("p2p", "Accepted connection from " << peer_address);

            // Добавляем пира в менеджер
            if (peer_manager_->add_inbound_peer(accepted.release(), peer_address, our_private_key_)) {
                accepted_connections_++;
                GEO_LOG_INFO(
                    "p2p",
                    "Added inbound peer " << peer_address
                        << " (total accepted: " << accepted_connections_ << ")"
                );
            } else {
                GEO_LOG_ERROR("p2p", "Failed to add inbound peer " << peer_address);
            }
        } catch (const std::exception& error) {
            GEO_LOG_ERROR("p2p", "Inbound admission failed: " << error.what());
        } catch (...) {
            GEO_LOG_ERROR("p2p", "Inbound admission failed");
        }

    }
}

std::string P2PServer::get_peer_address(SocketHandle client_socket) {
    struct sockaddr_storage addr;
    socket_length_t addr_len = sizeof(addr);

    if (getpeername(client_socket, reinterpret_cast<struct sockaddr*>(&addr), &addr_len) < 0) {
        return "unknown";
    }

    char ip[INET6_ADDRSTRLEN] = {};
    uint16_t port = 0;
    if (addr.ss_family == AF_INET) {
        const auto* a4 = reinterpret_cast<const sockaddr_in*>(&addr);
        if (!inet_ntop(AF_INET, &a4->sin_addr, ip, sizeof(ip))) {
            return "unknown";
        }
        port = ntohs(a4->sin_port);
        return NetworkUtils::make_endpoint(ip, port);
    }
    if (addr.ss_family == AF_INET6) {
        const auto* a6 = reinterpret_cast<const sockaddr_in6*>(&addr);
        if (!inet_ntop(AF_INET6, &a6->sin6_addr, ip, sizeof(ip))) {
            return "unknown";
        }
        port = ntohs(a6->sin6_port);
        return NetworkUtils::make_endpoint(ip, port);
    }

    return "unknown";
}

} // namespace geocoin::network
