#pragma once

#include <string>
#include <sstream>

namespace geocoin::utils {

/**
 * @brief Вывод строки в лог (thread-safe, unbuffered)
 * Аналог LogPrintStr из Bitcoin (util.cpp:213)
 */
int LogPrintStr(const std::string& str);

/**
 * @brief Форматированный вывод в лог (thread-safe, unbuffered)
 * Аналог LogPrintf из Bitcoin (util.h)
 *
 * Использование:
 *   LogPrintf("Block found at height %d\n", height);
 */
template<typename... Args>
inline int LogPrintf(const char* fmt, Args... args) {
    char buffer[10000];
    int ret = snprintf(buffer, sizeof(buffer), fmt, args...);
    if (ret > 0 && ret < (int)sizeof(buffer)) {
        return LogPrintStr(std::string(buffer, ret));
    }
    return ret;
}

// Перегрузка для простых строк без форматирования
inline int LogPrintf(const char* str) {
    return LogPrintStr(std::string(str));
}

} // namespace geocoin::utils
