#pragma once

#include "geocoin/network/peer.hpp"
#include "geocoin/network/falcon_auth.hpp"
#include "geocoin/network/messages.hpp"
#include <vector>
#include <map>
#include <mutex>
#include <memory>
#include <functional>
#include <deque>
#include <unordered_map>
#include <chrono>
#include <array>

namespace geocoin::state {
    class ChainState;
}

namespace geocoin::network {

/**
 * @brief Callback для обработки сообщений
 */
using MessageCallback = std::function<void(
    std::shared_ptr<Peer> peer,
    MessageType type,
    const bytes& payload
)>;

/**
 * @brief Callback для события аутентификации пира
 */
using PeerAuthenticatedCallback = std::function<void(
    std::shared_ptr<Peer> peer,
    Height peer_height
)>;

/**
 * @brief Callback для получения текущей высоты блокчейна
 */
using GetHeightCallback = std::function<Height()>;

/**
 * @brief Статистика менеджера пиров
 */
struct PeerManagerStats {
    size_t total_peers;
    size_t connected_peers;
    size_t authenticated_peers;
    size_t encrypted_peers;
    size_t inbound_peers;
    size_t outbound_peers;
    uint64_t total_bytes_sent;
    uint64_t total_bytes_received;
    uint64_t policy_rejects;
    size_t active_temp_bans;
};

/**
 * @brief Детальная статистика одного пира
 */
struct PeerStats {
    int id;                          // Идентификатор пира
    std::string address;             // Адрес (IP:port)
    std::string state;               // CONNECTED/AUTHENTICATING/READY/...
    bool authenticated;              // аутентификация пройдена
    bool encrypted;                  // транспорт защищён AEAD
    uint16_t secure_wire_version;
    uint16_t secure_suite;
    bool inbound;                    // Входящее (true) или исходящее (false)
    uint64_t conntime;               // Время соединения (unix timestamp)
    uint64_t bytes_sent;             // Отправлено байт
    uint64_t bytes_recv;             // Получено байт
    uint32_t version;                // Версия протокола
    std::string subver;              // Строка версии
    Height starting_height;          // Начальная высота блокчейна пира
    Address auth_address;            // Адрес пира
    uint64_t last_send;              // Время последней отправки
    uint64_t last_recv;              // Время последнего получения
    uint32_t policy_score = 0;       // Текущий policy score
    std::string last_message_type;   // Последний полученный тип сообщения
    uint64_t last_message_age_ms = 0;
    uint64_t last_useful_age_ms = 0;
    uint64_t ping_rtt_ms = 0;
    std::string last_disconnect_reason;
};

struct PeerBanInfo {
    std::string address;             // IP адрес бана
    uint64_t banned_until = 0;       // Unix timestamp окончания бана
    bool manual = false;             // Ручной (RPC setban) или policy-ban
};

struct PeerDisconnectInfo {
    std::string address;
    std::string reason;
    uint64_t disconnected_at = 0;
    uint32_t policy_score = 0;
    bool inbound = false;
};

struct PeerOutboundAttemptInfo {
    std::string address;
    uint32_t fail_count = 0;
    uint64_t last_attempt_age_ms = 0;
    uint64_t last_success_age_ms = 0;
    uint64_t next_attempt_in_ms = 0;
    std::string last_error;
    bool manual = false;
    bool in_backoff = false;
    bool in_progress = false;
};

struct PeerConnectionDiagnostics {
    PeerManagerStats stats{};
    std::vector<PeerStats> peers;
    std::vector<PeerDisconnectInfo> recent_disconnects;
    std::vector<PeerOutboundAttemptInfo> outbound_attempts;
    std::map<std::string, uint64_t> disconnect_reasons;
    std::map<std::string, uint64_t> policy_rejects;
};

/**
 * @brief Менеджер пиров
 *
 * Управляет множеством P2P соединений, включая:
 * - Подключение/отключение пиров
 * - аутентификацию пиров
 * - Broadcast сообщений
 * - Мониторинг состояния
 */
class PeerManager {
public:
    PeerManager();
    ~PeerManager();

    /**
     * @brief Установить callback для обработки сообщений
     *
     * @param callback Функция обработки
     */
    void set_message_callback(MessageCallback callback);

    /**
     * @brief Установить callback для события аутентификации пира
     *
     * @param callback Функция обработки
     */
    void set_peer_authenticated_callback(PeerAuthenticatedCallback callback);

    /**
     * @brief Установить callback для получения текущей высоты блокчейна
     *
     * @param callback Функция возвращающая высоту
     */
    void set_get_height_callback(GetHeightCallback callback);

    /** Remove every callback owned by the runtime embedding this manager. */
    void clear_callbacks();

    /**
     * @brief Установить ChainState для получения высоты блокчейна
     *
     * @param chain_state ChainState
     */
    void set_chain_state(std::shared_ptr<state::ChainState> chain_state);

    /**
     * @brief Добавить исходящее соединение
     *
     * @param address Адрес пира (IP:port)
     * @param our_private_key Наш приватный ключ для аутентификации
     * @param manual_target Use the shorter retry schedule for an operator target
     * @param bypass_backoff Ignore an elapsed retry deadline, but never an
     *        already in-progress connection attempt
     * @return true при успехе
     */
    bool add_outbound_peer(
        const std::string& address,
        const bytes& our_private_key,
        bool manual_target = false,
        bool bypass_backoff = false
    );

    /**
     * @brief Добавить входящее соединение
     *
     * @param socket_fd Дескриптор сокета; владение передаётся менеджеру,
     *        включая случаи отказа в приёме соединения.
     * @param address Адрес пира
     * @param our_private_key Наш приватный ключ для аутентификации
     * @return true при успехе
     */
    bool add_inbound_peer(SocketHandle socket_fd, const std::string& address, const bytes& our_private_key);

    /**
     * @brief Удалить пира
     *
     * @param address Адрес пира
     */
    void remove_peer(const std::string& address);

    /**
     * @brief Получить пира по адресу
     *
     * @param address Адрес пира
     * @return Пир или nullptr
     */
    std::shared_ptr<Peer> get_peer(const std::string& address);

    /**
     * @brief Получить всех подключенных пиров
     *
     * @return Список пиров
     */
    std::vector<std::shared_ptr<Peer>> get_connected_peers();

    /**
     * @brief Получить всех аутентифицированных пиров
     *
     * @return Список пиров
     */
    std::vector<std::shared_ptr<Peer>> get_authenticated_peers();

    /**
     * @brief Отправить сообщение пиру
     *
     * @param peer Пир
     * @param type Тип сообщения
     * @param payload Данные
     * @return true при успехе
     */
    bool send_message(
        std::shared_ptr<Peer> peer,
        MessageType type,
        const bytes& payload
    );

    /**
     * @brief Broadcast сообщения всем аутентифицированным пирам
     *
     * @param type Тип сообщения
     * @param payload Данные
     * @param exclude Исключить пира (опционально)
     * @return Количество успешных отправок
     */
    size_t broadcast(
        MessageType type,
        const bytes& payload,
        std::shared_ptr<Peer> exclude = nullptr
    );

    /**
     * @brief Relay INV inventory to authenticated peers.
     *
     * Builds per-peer INV payloads, skips known inventory, and marks sent
     * inventory as known after successful send.
     *
     * @param type Inventory item type
     * @param hashes Inventory object hashes
     * @param exclude Optional source peer to skip
     * @return Number of peers that received a non-empty INV
     */
    size_t relay_inventory(
        InvMessage::InvType type,
        const std::vector<Hash256>& hashes,
        std::shared_ptr<Peer> exclude = nullptr
    );
    size_t relay_inventory(
        const std::vector<InvMessage::InvItem>& items,
        std::shared_ptr<Peer> exclude = nullptr
    );

    /**
     * @brief Обновить состояния пиров
     *
     * Проверяет таймауты, обрабатывает входящие сообщения,
     * отправляет ping/pong
     */
    void update();

    /**
     * @brief Получить статистику
     *
     * @return Статистика
     */
    PeerManagerStats get_stats() const;

    /**
     * @brief Получить детальную статистику всех пиров
     *
     * @return Список статистики пиров
     */
    std::vector<PeerStats> get_peer_stats() const;

    /**
     * @brief Разорвать соединение с узлом
     */
    bool disconnect_peer(const std::string& address_or_ip);

    /**
     * @brief Разорвать соединение с известным peer object и записать причину.
     */
    bool disconnect_peer(std::shared_ptr<Peer> peer, const std::string& reason);

    /**
     * @brief Добавить ручной бан по IP
     */
    bool set_manual_ban(const std::string& address_or_ip, uint64_t ban_seconds);

    /**
     * @brief Удалить ручной бан по IP
     */
    bool remove_manual_ban(const std::string& address_or_ip);

    /**
     * @brief Очистить все ручные баны
     */
    void clear_manual_bans();

    /**
     * @brief Получить список активных банов
     */
    std::vector<PeerBanInfo> list_bans() const;

    /**
     * @brief Получить количество подключенных пиров
     *
     * @return Количество пиров
     */
    size_t get_peer_count() const;

    /**
     * @brief Получить счётчики policy-отклонений по причинам
     */
    std::map<std::string, uint64_t> get_policy_reject_counters() const;

    /**
     * @brief Получить диагностику соединений, backoff и причин disconnect.
     */
    PeerConnectionDiagnostics get_connection_diagnostics() const;

    /**
     * @brief Проверить, можно ли сейчас пробовать исходящее подключение.
     */
    bool should_try_outbound(const std::string& address, bool manual_target = false) const;

    /**
     * @brief Попытаться подключиться к узлу один раз
     *
     * @param address Адрес узла (IP:port)
     * @param our_private_key Наш приватный ключ
     * @return true при успехе
     */
    bool connect_to_node(const std::string& address, const bytes& our_private_key);

    /**
     * @brief Добавить узел в список постоянных подключений
     *
     * @param address Адрес узла (IP:port)
     */
    void add_node(const std::string& address);

    /**
     * @brief Удалить узел из списка постоянных подключений
     *
     * @param address Адрес узла (IP:port)
     */
    void remove_node(const std::string& address);

    /**
     * @brief Получить список добавленных узлов
     *
     * @return Список адресов
     */
    std::vector<std::string> get_added_nodes() const;

    /**
     * @brief Установить приватный ключ для подключений
     *
     * @param private_key Приватный ключ
     */
    void set_private_key(const bytes& private_key);
    void clear_private_key();

    /**
     * @brief Установить локальный порт (для проверки self-connection)
     *
     * @param port Порт, на котором слушаем
     */
    void set_local_port(int port);

    /**
     * @brief Установить таймаут исходящего TCP connect.
     */
    void set_outbound_connect_timeout(std::chrono::milliseconds timeout);

    /**
     * @brief Установить максимальное количество пиров
     *
     * @param max_inbound Максимум входящих (по умолчанию 125)
     * @param max_outbound Максимум исходящих (по умолчанию 125)
     */
    void set_max_peers(size_t max_inbound, size_t max_outbound);

    /**
     * Responder handshakes run ML-KEM and X25519 before the peer proves any
     * identity. They are admitted per second at `rate` with `burst` capacity
     * node-wide, and at `group_rate`/`group_burst` per /24 or /64 network group.
     */
    void set_inbound_handshake_limits(double rate, double burst, double group_rate, double group_burst);

    /**
     * @brief Установить режим архивной ноды (для объявления service bits)
     */
    void set_archive_mode(bool enabled);

    /**
     * @brief Проверить, работает ли менеджер в архивном режиме
     */
    bool is_archive_mode() const;

    /**
     * @brief Проверить, можно ли добавить входящего пира
     *
     * @return true если есть место
     */
    bool can_accept_inbound() const;

    /**
     * @brief Проверить, можно ли добавить исходящего пира
     *
     * @return true если есть место
     */
    bool can_add_outbound() const;

private:
#ifdef GEOCOIN_ENABLE_PEER_MANAGER_TEST_ACCESS
    friend struct PeerManagerTestAccess;
#endif

    struct PeerContext {
        std::shared_ptr<Peer> peer;
        std::shared_ptr<AuthSession> auth_session;
        std::unique_ptr<SecureHandshake> secure_handshake;
        bytes secure_hello_payload;
        Hash256 channel_binding{};
        secure_bytes pending_private_key;
        Address local_auth_address{};
        bool is_inbound = false;
        bool secure_established = false;
        bytes receive_buffer;
        std::chrono::steady_clock::time_point last_ping;
        std::chrono::steady_clock::time_point connected_at;
        std::chrono::steady_clock::time_point handshake_started_at;
        std::chrono::steady_clock::time_point last_message_at;
        std::chrono::steady_clock::time_point last_useful_message_at;
        std::chrono::steady_clock::time_point last_ping_sent_at;
        std::chrono::steady_clock::time_point last_pong_at;
        std::chrono::steady_clock::time_point last_score_decay_at;
        std::deque<std::chrono::steady_clock::time_point> recent_inv_msgs;
        std::deque<std::chrono::steady_clock::time_point> recent_getdata_msgs;
        std::deque<std::chrono::steady_clock::time_point> recent_addr_msgs;
        std::deque<std::chrono::steady_clock::time_point> recent_all_msgs;
        std::deque<std::chrono::steady_clock::time_point> recent_tx_msgs;
        std::deque<std::chrono::steady_clock::time_point> recent_block_msgs;
        std::deque<std::chrono::steady_clock::time_point> recent_headers_msgs;
        std::deque<std::chrono::steady_clock::time_point> recent_ping_msgs;
        uint32_t policy_score = 0;
        uint64_t ping_rtt_ms = 0;
        std::string last_message_type;
        std::string last_disconnect_reason;
        bool disconnect_recorded = false;
        bool version_received = false;
        std::array<uint64_t, 256> recv_message_counts{};
        std::array<uint64_t, 256> sent_message_counts{};
    };

    struct OutboundAttemptState {
        uint32_t fail_count = 0;
        std::chrono::steady_clock::time_point last_attempt{};
        std::chrono::steady_clock::time_point last_success{};
        std::chrono::steady_clock::time_point next_attempt{};
        std::string last_error;
        bool manual = false;
        bool in_progress = false;
    };

    mutable std::mutex mutex_;
    std::map<std::string, PeerContext> peers_;
    FalconAuthenticator authenticator_;
    MessageCallback message_callback_;
    PeerAuthenticatedCallback peer_authenticated_callback_;
    GetHeightCallback get_height_callback_;
    std::shared_ptr<state::ChainState> chain_state_;

    size_t max_inbound_;
    size_t max_outbound_;

    // Список добавленных узлов для постоянного подключения
    std::vector<std::string> added_nodes_;
    bytes private_key_;  // Приватный ключ для подключений

    int local_port_ = 0;  // Порт, на котором слушаем (для проверки self-connection)
    std::chrono::milliseconds outbound_connect_timeout_{5000};
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> temp_banned_ips_;
    std::unordered_map<std::string, std::chrono::system_clock::time_point> manual_banned_ips_;
    std::unordered_map<std::string, uint64_t> policy_reject_counters_;
    std::unordered_map<std::string, uint64_t> disconnect_reason_counters_;
    std::unordered_map<std::string, OutboundAttemptState> outbound_attempts_;
    std::deque<PeerDisconnectInfo> recent_disconnects_;
    uint64_t total_policy_rejects_ = 0;
    bool is_archive_mode_ = false;
    size_t next_peer_update_index_ = 0;

    struct HandshakeBucket {
        double tokens = 0.0;
        std::chrono::steady_clock::time_point updated{};
    };
    double inbound_handshake_rate_ = 32.0;
    double inbound_handshake_burst_ = 64.0;
    double inbound_group_handshake_rate_ = 1.0;
    double inbound_group_handshake_burst_ = 8.0;
    bool inbound_handshake_bucket_started_ = false;
    HandshakeBucket inbound_handshake_bucket_;
    std::unordered_map<std::string, HandshakeBucket> inbound_handshake_group_buckets_;
    bool admit_inbound_handshake_unlocked(const std::string& address);

    /**
     * @brief Обработать пира (чтение сообщений, аутентификация)
     */
    void process_peer(
        PeerContext& ctx,
        std::unique_lock<std::mutex>& lock,
        std::vector<std::function<void()>>& deferred_tasks,
        size_t& remaining_frame_budget,
        size_t& remaining_read_budget
    );

    /**
     * @brief Обработать полученное сообщение
     */
    void handle_message(
        PeerContext& ctx,
        const MessageHeader& header,
        const bytes& payload,
        std::unique_lock<std::mutex>& lock,
        std::vector<std::function<void()>>& deferred_tasks
    );

    /**
     * @brief Начать аутентификацию с пиром
     */
    bool start_authentication(PeerContext& ctx, const bytes& our_private_key);

    void handle_secure_hello(PeerContext& ctx, const bytes& payload);
    void handle_secure_reply(PeerContext& ctx, const bytes& payload);
    bool send_local_version(PeerContext& ctx);

    /**
     * @brief Обработать VERSION сообщение
     */
    void handle_version(PeerContext& ctx, const bytes& payload);

    /**
     * @brief Обработать CHALLENGE сообщение
     */
    void handle_challenge(
        PeerContext& ctx,
        const bytes& payload,
        std::unique_lock<std::mutex>& lock,
        std::vector<std::function<void()>>& deferred_tasks
    );

    /**
     * @brief Обработать RESPONSE сообщение
     */
    void handle_response(
        PeerContext& ctx,
        const bytes& payload,
        std::unique_lock<std::mutex>& lock,
        std::vector<std::function<void()>>& deferred_tasks
    );

    /**
     * @brief Обработать PING сообщение
     */
    void handle_ping(PeerContext& ctx);

    /**
     * @brief Обработать REJECT сообщение
     */
    void handle_reject(PeerContext& ctx, const bytes& payload);

    /**
     * @brief Отправить PONG
     */
    void send_pong(std::shared_ptr<Peer> peer);

    /**
     * @brief Подсчитать количество входящих/исходящих пиров
     */
    void count_peers(size_t& inbound, size_t& outbound) const;

    bool is_peer_banned_unlocked(const std::string& address);
    void add_temp_ban_unlocked(const std::string& ip);
    bool should_try_outbound_unlocked(const std::string& address, bool manual_target) const;
    void record_outbound_attempt_unlocked(const std::string& address, bool manual_target);
    void record_outbound_result_unlocked(
        const std::string& address,
        bool success,
        bool manual_target,
        const std::string& reason
    );
    void prune_outbound_attempts_unlocked();
    void record_sent_message_best_effort(std::shared_ptr<Peer> peer, MessageType type);
    void record_disconnect_unlocked(PeerContext& ctx, const std::string& reason);
    void disconnect_peer_unlocked(PeerContext& ctx, const std::string& reason);
    bool admit_authenticated_identity_unlocked(PeerContext& candidate);
    void decay_policy_score_unlocked(PeerContext& ctx, std::chrono::steady_clock::time_point now);
    bool try_evict_inbound_peer_unlocked(const std::string& incoming_address, std::string& evicted_address);
    bool inbound_diversity_limit_reached_unlocked(const std::string& incoming_address) const;
    void record_policy_reject_unlocked(const std::string& reason);
    void penalize_peer_unlocked(PeerContext& ctx, uint32_t points, const char* reason);
    bool validate_inv_like_payload_unlocked(
        const bytes& payload,
        uint32_t max_items,
        const char* malformed_reason,
        const char* too_many_reason,
        PeerContext& ctx
    );
    bool validate_addr_payload_unlocked(const bytes& payload, PeerContext& ctx);
    bool consume_rate_token_unlocked(
        PeerContext& ctx,
        MessageType type,
        std::chrono::steady_clock::time_point now,
        const char*& reject_reason
    );
    void refresh_peer_metrics_unlocked() const;
};

} // namespace geocoin::network
