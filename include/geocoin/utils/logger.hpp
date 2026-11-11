#pragma once

#include <atomic>
#include <cstdint>
#include <fstream>
#include <iosfwd>
#include <mutex>
#include <sstream>
#include <string>

namespace geocoin::utils {

/**
 * @brief Уровни логирования
 */
enum class LogLevel : uint8_t {
    TRACE = 0,
    DEBUG,
    INFO,
    WARN,
    ERR
};

/**
 * @brief Потокобезопасный логер с поддержкой stdout и файла
 */
class Logger {
public:
    static Logger& instance();

    void set_level(LogLevel level);
    LogLevel level() const;

    /**
     * @brief Включить/выключить вывод в stdout/stderr
     */
    void enable_stdout(bool enabled);

    /**
     * @brief Установить файл для записи логов
     * @return true если файл успешно открыт
     */
    bool set_output_file(const std::string& path);

    /**
     * @brief Основной метод логирования
     */
    void log(LogLevel level, const std::string& component, const std::string& message);

private:
    Logger();
    ~Logger();

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    std::string level_to_string(LogLevel level) const;
    std::string now_iso8601() const;

    mutable std::mutex mutex_;
    std::ofstream file_;
    std::atomic<LogLevel> level_;
    bool stdout_enabled_;
};

} // namespace geocoin::utils

#define GEO_LOG_MESSAGE(level, component, message)                                           \
    do {                                                                                     \
        std::ostringstream _geo_log_stream;                                                  \
        _geo_log_stream << message;                                                          \
        ::geocoin::utils::Logger::instance().log(level, component, _geo_log_stream.str());   \
    } while (false)

#define GEO_LOG_TRACE(component, message) \
    GEO_LOG_MESSAGE(::geocoin::utils::LogLevel::TRACE, component, message)
#define GEO_LOG_DEBUG(component, message) \
    GEO_LOG_MESSAGE(::geocoin::utils::LogLevel::DEBUG, component, message)
#define GEO_LOG_INFO(component, message) \
    GEO_LOG_MESSAGE(::geocoin::utils::LogLevel::INFO, component, message)
#define GEO_LOG_WARN(component, message) \
    GEO_LOG_MESSAGE(::geocoin::utils::LogLevel::WARN, component, message)
#define GEO_LOG_ERROR(component, message) \
    GEO_LOG_MESSAGE(::geocoin::utils::LogLevel::ERR, component, message)
