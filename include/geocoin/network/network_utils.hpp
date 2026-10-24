#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace geocoin::network {

/**
 * @brief Shared endpoint parsing and address policy API.
 */
class NetworkUtils {
public:
    /**
     * @brief Разобрать endpoint в формате host:port или [ipv6]:port
     */
    static bool split_host_port(const std::string& endpoint, std::string& host, uint16_t& port);

    /**
     * @brief Разобрать peer target host[:port]. Если порт не указан, используется default_port.
     */
    static bool parse_peer_address(
        const std::string& address,
        std::string& host,
        uint16_t& port,
        uint16_t default_port = 3030
    );

    /**
     * @brief Нормализовать IP для сравнения policy/self/ban правил.
     */
    static std::string normalize_ip_address(const std::string& ip);

    /**
     * @brief Parse a numeric IPv4/IPv6 literal and return its canonical text form.
     *
     * DNS names and malformed literals are rejected. IPv4-mapped IPv6 is
     * canonicalized to IPv4.
     */
    static bool canonicalize_numeric_ip(const std::string& ip, std::string& canonical);

    /**
     * @brief True only for numeric, publicly routable unicast addresses.
     */
    static bool is_global_unicast_ip(const std::string& ip);

    /**
     * @brief Stable diversity key: IPv4 /24, IPv6 /64, canonical DNS name.
     */
    static std::string network_group(const std::string& host);

    /**
     * @brief Key for connection-rate limits and temporary abuse bans.
     *
     * IPv4 clients are tracked individually. IPv6 clients are tracked by /64
     * so rotating interface identifiers cannot bypass a limit or grow policy
     * maps without bound. IPv4-mapped IPv6 literals are treated as IPv4.
     */
    static std::string abuse_key(const std::string& host);

    /**
     * @brief Собрать endpoint c корректным форматированием IPv6
     */
    static std::string make_endpoint(const std::string& host, uint16_t port);

    /**
     * @brief Получить все локальные IP адреса
     *
     * @return Список IP адресов (IPv4)
     */
    static std::vector<std::string> get_local_addresses();

    /**
     * @brief Проверить, является ли IP адрес локальным
     *
     * @param ip IP адрес
     * @return true если локальный
     */
    static bool is_local_address(const std::string& ip);

    /**
     * @brief Проверить, является ли адрес нашим собственным
     *
     * @param address Адрес в формате "IP:PORT"
     * @param our_port Наш порт
     * @return true если это наш адрес
     */
    static bool is_self_address(const std::string& address, int our_port);
};

} // namespace geocoin::network
