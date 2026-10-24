#pragma once

#include <string>
#include <vector>

namespace geocoin::network {

/**
 * @brief DNS резолвер для seed-узлов
 */
class DNSResolver {
public:
    /**
     * @brief Разрешить список seed-хостов в адреса с портом
     *
     * @param seeds null-terminated массив hostnames
     * @param port  порт для добавления к IP
     * @return список строк вида "ip:port" или "[ipv6]:port"
     */
    static std::vector<std::string> resolve_seeds_with_port(
        const char* const* seeds,
        int port
    );

    /**
     * @brief Разрешить список seed-хостов в адреса с портом
     *
     * @param seeds список hostnames
     * @param port  порт для добавления к IP
     * @return список строк вида "ip:port"
     */
    static std::vector<std::string> resolve_seeds_with_port(
        const std::vector<std::string>& seeds,
        int port
    );
};

} // namespace geocoin::network
