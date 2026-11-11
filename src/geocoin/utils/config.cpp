#include "geocoin/utils/config.hpp"
#include <fstream>
#include <sstream>
#include <iostream>

namespace geocoin::utils {

std::map<std::string, std::vector<std::string>> read_config_file(const std::string& config_path) {
    std::map<std::string, std::vector<std::string>> config;

    std::ifstream file(config_path);
    if (!file.good()) {
        // Файл не найден - это нормально
        return config;
    }

    std::string line;
    int line_num = 0;

    while (std::getline(file, line)) {
        line_num++;

        // Убираем пробелы в начале и конце
        size_t start = line.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) continue;  // Пустая строка

        size_t end = line.find_last_not_of(" \t\r\n");
        line = line.substr(start, end - start + 1);

        // Пропускаем комментарии
        if (line.empty() || line[0] == '#') continue;

        // Ищем '='
        size_t eq_pos = line.find('=');
        if (eq_pos == std::string::npos) {
            // Опция без значения (флаг)
            config[line].push_back("1");
            continue;
        }

        std::string key = line.substr(0, eq_pos);
        std::string value = line.substr(eq_pos + 1);

        // Убираем пробелы
        key.erase(key.find_last_not_of(" \t") + 1);
        value.erase(0, value.find_first_not_of(" \t"));

        if (!key.empty()) {
            config[key].push_back(value);
        }
    }

    return config;
}

} // namespace geocoin::utils
