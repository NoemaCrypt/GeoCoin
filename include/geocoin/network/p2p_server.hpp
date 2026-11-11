#pragma once

#include "geocoin/network/peer_manager.hpp"
#include <chrono>
#include <thread>
#include <atomic>
#include <memory>
#include <vector>
#include <deque>
#include <unordered_map>
#include <mutex>

namespace geocoin::network {

/**
 * @brief P2P сервер для входящих соединений
 *
 * Прослушивает порт 3030 и принимает входящие соединения от пиров.
 * Каждое соединение автоматически добавляется в PeerManager.
 */
class P2PServer {
public:
    P2PServer(
        std::shared_ptr<PeerManager> peer_manager,
        const bytes& our_private_key,
        uint16_t port = 3030,
        std::vector<std::string> inbound_whitelist_ips = {}
    );
    ~P2PServer();

    /**
     * @brief Запустить сервер
     *
     * @return true при успехе
     */
    bool start();

    /**
     * @brief Остановить сервер
     */
    void stop();

    /**
     * @brief Проверить, запущен ли сервер
     *
     * @return true если запущен
     */
    bool is_running() const { return running_; }

    /**
     * @brief Получить порт сервера
     *
     * @return Порт
     */
    uint16_t get_port() const { return port_; }

    /**
     * @brief Получить количество принятых соединений
     *
     * @return Количество
     */
    size_t get_accepted_connections() const { return accepted_connections_.load(std::memory_order_relaxed); }

    /**
     * @brief Код ошибки последнего запуска сервера (errno/WSAGetLastError)
     */
    int get_last_start_error() const { return last_start_error_; }

private:
    std::shared_ptr<PeerManager> peer_manager_;
    bytes our_private_key_;
    std::atomic<uint16_t> port_;

    SocketHandle server_socket_;
    std::atomic<bool> running_;
    std::thread accept_thread_;
    std::atomic<size_t> accepted_connections_;
    std::atomic<int> last_start_error_;
    std::mutex lifecycle_mutex_;

    std::vector<std::string> inbound_whitelist_ips_;

    // Inbound rate limiting / temporary bans
    mutable std::mutex rate_mutex_;
    std::unordered_map<std::string, std::deque<std::chrono::steady_clock::time_point>> recent_accepts_;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> banned_ips_;

    /**
     * @brief Поток для приема соединений
     */
    void accept_loop();

    /**
     * @brief Получить адрес клиента из сокета
     */
    std::string get_peer_address(SocketHandle client_socket);
};

} // namespace geocoin::network
