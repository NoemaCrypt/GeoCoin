#pragma once

#include <string>
#include <map>
#include <vector>

namespace geocoin::utils {

/**
 * @brief Читает конфигурационный файл geocoin.conf
 *
 * Формат файла:
 * # Комментарий
 * option=value
 * addnode=192.168.1.100:3030
 * addnode=192.168.1.101:3030
 *
 * @param config_path Путь к файлу конфигурации
 * @return Карта опций (ключ -> список значений)
 */
std::map<std::string, std::vector<std::string>> read_config_file(const std::string& config_path);

} // namespace geocoin::utils
