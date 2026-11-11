#include "geocoin/utils/logger.hpp"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>

namespace geocoin::utils {

Logger& Logger::instance() {
    static Logger instance;
    return instance;
}

Logger::Logger()
    : level_(LogLevel::INFO)
    , stdout_enabled_(true) {
}

Logger::~Logger() {
    if (file_.is_open()) {
        file_.close();
    }
}

void Logger::set_level(LogLevel level) {
    level_.store(level, std::memory_order_relaxed);
}

LogLevel Logger::level() const {
    return level_.load(std::memory_order_relaxed);
}

void Logger::enable_stdout(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    stdout_enabled_ = enabled;
}

bool Logger::set_output_file(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_.is_open()) {
        file_.close();
    }

    file_.open(path, std::ios::out | std::ios::app);
    return file_.is_open();
}

void Logger::log(LogLevel level, const std::string& component, const std::string& message) {
    if (level < level_.load(std::memory_order_relaxed)) {
        return;
    }

    std::ostringstream line;
    line << "[" << now_iso8601() << "] "
         << "[" << level_to_string(level) << "] "
         << "[" << component << "] "
         << message;

    const std::string formatted = line.str();

    std::lock_guard<std::mutex> lock(mutex_);
    if (stdout_enabled_) {
        std::ostream& out = (level >= LogLevel::WARN) ? std::cerr : std::cout;
        out << formatted << std::endl;
    }

    if (file_.is_open()) {
        file_ << formatted << std::endl;
        file_.flush();
    }
}

std::string Logger::level_to_string(LogLevel level) const {
    switch (level) {
        case LogLevel::TRACE: return "TRACE";
        case LogLevel::DEBUG: return "DEBUG";
        case LogLevel::INFO:  return "INFO";
        case LogLevel::WARN:  return "WARN";
        case LogLevel::ERR: return "ERROR";
        default: return "UNKNOWN";
    }
}

std::string Logger::now_iso8601() const {
    using namespace std::chrono;

    const auto now = system_clock::now();
    const auto time_t_now = system_clock::to_time_t(now);

    std::tm tm_buf{};
#if defined(_WIN32)
    gmtime_s(&tm_buf, &time_t_now);
#else
    gmtime_r(&time_t_now, &tm_buf);
#endif

    const auto millis = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;

    std::ostringstream ss;
    ss << std::put_time(&tm_buf, "%Y-%m-%dT%H:%M:%S")
       << '.' << std::setw(3) << std::setfill('0') << millis.count()
       << "Z";

    return ss.str();
}

} // namespace geocoin::utils
