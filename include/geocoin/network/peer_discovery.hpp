#pragma once

#include "geocoin/types.hpp"
#include <string>
#include <vector>
#include <set>
#include <map>
#include <mutex>
#include <chrono>
#include <functional>
#include <thread>
#include <atomic>

namespace geocoin::network {

/**
 * @brief Информация о пире
 */
struct DiscoveredPeer {
    std::string ip;
    uint16_t port;
    uint64_t services;
    uint64_t last_seen;
    uint32_t attempts;      // Количество попыток подключения
    bool is_seed;           // Является ли seed нодой

    std::string to_string() const {
        return ip + ":" + std::to_string(port);
    }
};

/**
 * @brief Менеджер обнаружения пиров
 *
 * Функции:
 * - DNS seeds для начальной загрузки
 * - Сохранение известных пиров в файл
 * - Автоматическое переподключение
 * - Приоритизация надежных пиров
 */
class PeerDiscovery {
#ifdef GEOCOIN_ENABLE_PEER_DISCOVERY_TEST_ACCESS
    friend struct PeerDiscoveryTestAccess;
#endif
public:
    PeerDiscovery(const std::string& peers_file = "peers.dat");
    ~PeerDiscovery();

    /**
     * @brief Инициализировать с DNS seeds
     *
     * @param use_testnet Использовать testnet seeds
     */
    void initialize(bool use_testnet = false);

    /**
     * @brief Добавить DNS seed
     *
     * @param hostname Hostname для DNS запроса
     */
    void add_dns_seed(const std::string& hostname);

    /**
     * @brief Добавить известного пира
     *
     * @param ip IP адрес
     * @param port Порт
     * @param is_seed Является ли seed нодой
     */
    void add_peer(const std::string& ip, uint16_t port, bool is_seed = false);

    /**
     * @brief Добавить пира из ADDR сообщения
     */
    void add_peer(const DiscoveredPeer& info);

    /**
     * @brief Удалить пира
     */
    void remove_peer(const std::string& ip, uint16_t port);

    /**
     * @brief Отметить пира как подключенного
     */
    void mark_connected(const std::string& ip, uint16_t port);

    /**
     * @brief Отметить неудачную попытку подключения
     */
    void mark_failed(const std::string& ip, uint16_t port);

    /**
     * @brief Получить список пиров для подключения
     *
     * @param count Количество пиров
     * @return Список пиров
     */
    std::vector<DiscoveredPeer> get_peers(size_t count = 10);

    /**
     * @brief Получить все известные пиры
     */
    std::vector<DiscoveredPeer> get_all_peers();

    /**
     * @brief Загрузить пиров из DNS seeds
     *
     * @return Количество найденных пиров
     */
    size_t load_from_dns();

    /**
     * @brief Запустить асинхронную загрузку из DNS seeds (как ThreadDNSAddressSeed в Bitcoin)
     *
     * @param peer_count Текущее количество подключенных пиров
     * @param force Принудительно запросить DNS даже если есть пиры
     */
    void start_dns_thread(size_t peer_count = 0, bool force = false);

    /**
     * @brief Остановить поток DNS разрешения
     */
    void stop_dns_thread();

    /**
     * @brief Сохранить пиров в файл
     *
     * @return true при успехе
     */
    bool save_to_file();

    /**
     * @brief Загрузить пиров из файла
     *
     * @return true при успехе
     */
    bool load_from_file();

    /**
     * @brief Очистить список пиров
     */
    void clear();

    /**
     * @brief Получить количество известных пиров
     */
    size_t count() const;

    /**
     * @brief Установить максимальное количество попыток подключения
     */
    void set_max_attempts(uint32_t max);

    /**
     * @brief Установить время бана после неудачных попыток (секунды)
     */
    void set_ban_time(uint64_t seconds);

    /**
     * @brief Установить лимит известных peers.
     */
    void set_max_peers(size_t max_peers);

private:
    mutable std::mutex mutex_;

    std::string peers_file_;
    std::vector<std::string> dns_seeds_;
    uint16_t dns_seed_port_;
    std::map<std::string, DiscoveredPeer> peers_;  // ключ: "ip:port"

    uint32_t max_attempts_;     // Максимум попыток подключения
    uint64_t ban_time_;         // Время бана в секундах
    size_t max_peers_{4096};

    // Асинхронный DNS поток (как в Bitcoin)
    std::thread dns_thread_;
    std::atomic<bool> dns_thread_running_{false};
    std::mutex dns_lifecycle_mutex_;
    void stop_dns_thread_unlocked();

    /**
     * @brief Проверить, забанен ли пир
     */
    bool is_banned(const DiscoveredPeer& info) const;

    /**
     * @brief Получить текущее время в секундах
     */
    static uint64_t current_time();
};

} // namespace geocoin::network
