#pragma once

#include "geocoin/types.hpp"
#include "geocoin/network/peer_transport.hpp"
#include "geocoin/network/secure_channel.hpp"
#include <string>
#include <memory>
#include <chrono>
#include <set>
#include <vector>
#include <mutex>

#ifdef _WIN32
#include <basetsd.h>
using ssize_t = SSIZE_T;
#endif

namespace geocoin::network {

/**
 * @brief Состояние пира
 */
enum class PeerState {
    DISCONNECTED,       // Не подключен
    CONNECTING,         // Подключение
    CONNECTED,          // Подключен, но не аутентифицирован
    AUTHENTICATING,     // Аутентификация
    AUTHENTICATED,      // Аутентифицирован
    SYNCING,           // Синхронизация блоков
    READY              // Готов к работе
};

/**
 * @brief Информация о пире
 */
struct PeerInfo {
    std::string address;        // IP:порт
    std::string user_agent;     // "degree/0.1.0"
    uint32_t version = 0;       // Версия протокола
    Height height = 0;          // Высота блокчейна пира
    uint64_t services = 0;      // Поддерживаемые сервисы
    int64_t time_offset = 0;    // Разница времени

    // Параметры p2p-аутентификации
    Address auth_address{};     // Адрес пира
    bool auth_verified = false; // Прошел ли аутентификацию
    bool transport_encrypted = false;
    uint16_t secure_wire_version = 0;
    uint16_t secure_suite = 0;

    // Статистика
    uint64_t bytes_sent = 0;
    uint64_t bytes_received = 0;
    std::chrono::steady_clock::time_point last_seen{};
    std::chrono::steady_clock::time_point connected_since{};
};

/**
 * @brief Пир в P2P сети
 */
class Peer {
public:
    Peer(const std::string& address, SocketHandle socket_fd = -1);
    Peer(const std::string& address, std::unique_ptr<PeerTransport> transport);
    ~Peer();

    /**
     * @brief Подключиться к пиру
     *
     * @return true при успехе
     */
    bool connect();
    bool connect(std::chrono::milliseconds timeout);

    /**
     * @brief Отключиться от пира
     */
    void disconnect();

    /**
     * @brief Проверить, подключен ли пир
     *
     * @return true если подключен
     */
    bool is_connected() const;

    /**
     * @brief Отправить данные пиру
     *
     * @param data Данные для отправки
     * @return Количество отправленных байт
     */
    ssize_t send(const bytes& data);

    /**
     * @brief Получить данные от пира
     *
     * @param buffer Буфер для данных
     * @param max_size Максимальный размер
     * @return Количество полученных байт
     */
    ssize_t receive(bytes& buffer, size_t max_size);

    /** Atomically enable the mandatory encrypted record layer. */
    bool activate_secure_channel(SecureChannelKeyMaterial key_material);
    bool is_secure() const;
    bool decrypt_secure_record(const bytes& record, bytes& plaintext_frame);
    Hash256 secure_channel_binding() const;

    /**
     * @brief Получить состояние пира
     *
     * @return Состояние
     */
    PeerState get_state() const;

    /**
     * @brief Установить состояние пира
     *
     * @param state Новое состояние
     */
    void set_state(PeerState state);

    /**
     * @brief Получить информацию о пире
     *
     * @return Информация
     */
    PeerInfo get_info() const;

    /**
     * @brief Обновить информацию о пире
     *
     * @param info Новая информация
     */
    void update_info(const PeerInfo& info);

    /**
     * @brief Получить адрес пира
     *
     * @return Адрес (IP:port)
     */
    std::string get_address() const;

    /**
     * @brief Обновить время последней активности
     */
    void update_last_seen();

    /**
     * @brief Проверить, истек ли таймаут
     *
     * @param timeout_seconds Таймаут в секундах
     * @return true если истек
     */
    bool is_timeout(int timeout_seconds) const;

    /**
     * @brief Добавить известный inventory (как AddInventoryKnown в Bitcoin)
     * Помечает что пир уже знает об этом объекте (TX или BLOCK)
     *
     * @param hash Хеш объекта
     */
    void add_known_inventory(const Hash256& hash);

    /**
     * @brief Проверить, известен ли inventory пиру
     *
     * @param hash Хеш объекта
     * @return true если пир уже знает
     */
    bool has_known_inventory(const Hash256& hash) const;

    /**
     * @brief Вернуть список объектов, неизвестных данному пиру, за один lock.
     */
    std::vector<Hash256> filter_unknown_inventory(const std::vector<Hash256>& hashes) const;

private:
    std::unique_ptr<PeerTransport> transport_;
    std::unique_ptr<SecureChannel> secure_channel_;
    PeerState state_{PeerState::DISCONNECTED};
    PeerInfo info_{};
    mutable std::mutex transport_mutex_;
    mutable std::mutex info_mutex_;

    // Известные inventory (TX и BLOCK хеши)
    // Как setInventoryKnown в Bitcoin
    std::set<Hash256> known_inventory_;
    mutable std::mutex known_inventory_mutex_;

};

} // namespace geocoin::network
