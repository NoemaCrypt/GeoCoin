#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <mutex>
#include <chrono>

namespace geocoin::network {

/**
 * @brief Информация об адресе пира
 */
struct AddrInfo {
    std::string address;  // IP:порт
    uint64_t last_try;    // Время последней попытки подключения
    uint64_t last_success; // Время последнего успешного подключения
    int attempts;         // Количество неудачных попыток

    AddrInfo() : last_try(0), last_success(0), attempts(0) {}

    bool is_terrible() const {
        // Адрес "ужасный" если много неудачных попыток
        return attempts > 3;
    }

    double get_chance() const {
        // Вероятность выбора адреса (меньше попыток = выше шанс)
        double chance = 1.0;
        if (attempts > 0) {
            chance /= (1.0 + attempts);
        }
        return chance;
    }
};

/**
 * @brief Менеджер адресов пиров (как AddrMan в Bitcoin)
 *
 * Хранит известные адреса пиров, управляет их приоритетом,
 * сохраняет/загружает из файла
 */
class AddrMan {
public:
    AddrMan();
    ~AddrMan();

    /**
     * @brief Добавить адрес
     *
     * @param address Адрес (IP:port)
     * @param source Источник адреса (для логирования)
     * @return true если добавлен
     */
    bool add(const std::string& address, const std::string& source = "");

    /**
     * @brief Add an address learned from an untrusted P2P ADDR message.
     *
     * Only numeric global-unicast addresses on expected_p2p_port are accepted.
     * Entries are additionally capped per source network group.
     */
    bool add_from_gossip(
        const std::string& address,
        const std::string& source_endpoint,
        uint16_t expected_p2p_port
    );

    /**
     * @brief Добавить несколько адресов
     *
     * @param addresses Список адресов
     * @param source Источник
     * @return Количество добавленных
     */
    size_t add(const std::vector<std::string>& addresses, const std::string& source = "");

    /**
     * @brief Удалить адрес
     *
     * @param address Адрес (IP:port)
     * @return true если был удален
     */
    bool remove(const std::string& address);

    /**
     * @brief Выбрать адрес для подключения
     *
     * Использует алгоритм выбора с учетом:
     * - Времени последней попытки
     * - Количества неудачных попыток
     * - Случайности
     *
     * @return Адрес или пустая строка
     */
    std::string select();

    /**
     * @brief Отметить попытку подключения
     *
     * @param address Адрес
     * @param success true если успешно
     */
    void attempt(const std::string& address, bool success);

    /**
     * @brief Отметить успешное подключение
     *
     * @param address Адрес
     */
    void good(const std::string& address);

    /**
     * @brief Настроить safety limits для тестов/спецрежимов.
     */
    void set_limits(size_t max_entries, size_t max_entries_per_group);

    /**
     * @brief Получить количество известных адресов
     *
     * @return Количество
     */
    size_t size() const;

    /**
     * @brief Загрузить адреса из файла
     *
     * @param filepath Путь к файлу
     * @return true при успехе
     */
    bool load(const std::string& filepath);

    /**
     * @brief Сохранить адреса в файл
     *
     * @param filepath Путь к файлу
     * @return true при успехе
     */
    bool save(const std::string& filepath) const;

    /**
     * @brief Очистить все адреса
     */
    void clear();

    /**
     * @brief Получить все адреса (для отладки)
     *
     * @return Список адресов
     */
    std::vector<std::string> get_all() const;

private:
    mutable std::mutex mutex_;
    std::map<std::string, AddrInfo> addrs_;
    std::map<std::string, std::string> gossip_sources_;
    size_t max_entries_{4096};
    size_t max_entries_per_group_{256};
    size_t max_entries_per_gossip_source_{64};

    bool add_impl(
        const std::string& address,
        const std::string& source,
        const std::string& gossip_source_group
    );
    bool evict_one_unlocked(const std::string& preferred_group = "");
};

} // namespace geocoin::network
