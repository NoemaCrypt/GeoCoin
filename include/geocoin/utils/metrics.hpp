#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace geocoin::utils {

class Counter {
public:
    void inc(int64_t delta = 1);
    int64_t value() const;

private:
    std::atomic<int64_t> value_{0};
};

class Gauge {
public:
    void set(int64_t value);
    void inc(int64_t delta = 1);
    int64_t value() const;

private:
    std::atomic<int64_t> value_{0};
};

class MetricsRegistry {
public:
    static MetricsRegistry& instance();

    std::shared_ptr<Counter> counter(const std::string& name);
    std::shared_ptr<Gauge> gauge(const std::string& name);

    std::unordered_map<std::string, int64_t> snapshot_counters() const;
    std::unordered_map<std::string, int64_t> snapshot_gauges() const;

private:
    MetricsRegistry() = default;

    MetricsRegistry(const MetricsRegistry&) = delete;
    MetricsRegistry& operator=(const MetricsRegistry&) = delete;

    std::shared_ptr<Counter> get_or_create_counter(const std::string& name);
    std::shared_ptr<Gauge> get_or_create_gauge(const std::string& name);

    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<Counter>> counters_;
    std::unordered_map<std::string, std::shared_ptr<Gauge>> gauges_;
};

} // namespace geocoin::utils

#define GEO_METRIC_COUNTER(name) \
    ::geocoin::utils::MetricsRegistry::instance().counter(name)

#define GEO_METRIC_GAUGE(name) \
    ::geocoin::utils::MetricsRegistry::instance().gauge(name)

#define GEO_METRIC_INC(name, delta) \
    GEO_METRIC_COUNTER(name)->inc(static_cast<int64_t>(delta))

#define GEO_METRIC_SET(name, value) \
    GEO_METRIC_GAUGE(name)->set(static_cast<int64_t>(value))
