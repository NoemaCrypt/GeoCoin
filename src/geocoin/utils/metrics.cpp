#include "geocoin/utils/metrics.hpp"

namespace geocoin::utils {

void Counter::inc(int64_t delta) {
    value_.fetch_add(delta, std::memory_order_relaxed);
}

int64_t Counter::value() const {
    return value_.load(std::memory_order_relaxed);
}

void Gauge::set(int64_t value) {
    value_.store(value, std::memory_order_relaxed);
}

void Gauge::inc(int64_t delta) {
    value_.fetch_add(delta, std::memory_order_relaxed);
}

int64_t Gauge::value() const {
    return value_.load(std::memory_order_relaxed);
}

MetricsRegistry& MetricsRegistry::instance() {
    static MetricsRegistry registry;
    return registry;
}

std::shared_ptr<Counter> MetricsRegistry::counter(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    return get_or_create_counter(name);
}

std::shared_ptr<Gauge> MetricsRegistry::gauge(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    return get_or_create_gauge(name);
}

std::unordered_map<std::string, int64_t> MetricsRegistry::snapshot_counters() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::unordered_map<std::string, int64_t> snapshot;
    snapshot.reserve(counters_.size());
    for (const auto& [name, counter] : counters_) {
        snapshot[name] = counter->value();
    }
    return snapshot;
}

std::unordered_map<std::string, int64_t> MetricsRegistry::snapshot_gauges() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::unordered_map<std::string, int64_t> snapshot;
    snapshot.reserve(gauges_.size());
    for (const auto& [name, gauge] : gauges_) {
        snapshot[name] = gauge->value();
    }
    return snapshot;
}

std::shared_ptr<Counter> MetricsRegistry::get_or_create_counter(const std::string& name) {
    auto it = counters_.find(name);
    if (it != counters_.end()) {
        return it->second;
    }
    auto counter = std::make_shared<Counter>();
    counters_.emplace(name, counter);
    return counter;
}

std::shared_ptr<Gauge> MetricsRegistry::get_or_create_gauge(const std::string& name) {
    auto it = gauges_.find(name);
    if (it != gauges_.end()) {
        return it->second;
    }
    auto gauge = std::make_shared<Gauge>();
    gauges_.emplace(name, gauge);
    return gauge;
}

} // namespace geocoin::utils
