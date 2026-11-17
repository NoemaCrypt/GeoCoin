#pragma once

#include "geocoin/node/cpu_topology.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace geocoin::node {

struct CpuWorkerStartResult {
    size_t requested = 0;
    size_t spawned = 0;
    std::string error;
    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};

inline void join_cpu_workers(std::vector<std::thread>& workers) {
    for (auto& worker : workers) if (worker.joinable()) worker.join();
    workers.clear();
}

inline void pause_cpu_mining_worker(const std::atomic<bool>& running, std::chrono::microseconds duration) {
    const auto deadline = std::chrono::steady_clock::now() + duration;
    while (running.load(std::memory_order_acquire)) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) break;
        std::this_thread::sleep_for((std::min)(deadline - now,
            std::chrono::steady_clock::duration(std::chrono::milliseconds(5))));
    }
}

namespace detail {
struct CpuWorkerLaunch {
    template <typename Body>
    std::thread operator()(Body body) const { return std::thread(std::move(body)); }
};
struct CpuWorkerGate {
    std::mutex mutex;
    std::condition_variable changed;
    size_t ready = 0;
    bool released = false;
    bool abort = false;
    std::string error;
};
} // namespace detail

// A creation/binding/priority failure is all-or-nothing. No work starts before
// every worker is configured; every constructed thread is joined on rollback.
template <typename Work, typename Failure, typename Bind = decltype(&bind_cpu_mining_worker),
          typename Launch = detail::CpuWorkerLaunch>
CpuWorkerStartResult start_cpu_workers(std::vector<std::thread>& workers, std::atomic<bool>& running,
    size_t count, const CpuTopology& topology, Work work, Failure failure,
    Bind bind = &bind_cpu_mining_worker, Launch launch = {}) {
    CpuWorkerStartResult result{count, 0, {}};
    if (!workers.empty() || count == 0 || count > topology.capacity() || !topology.error.empty()) {
        result.error = topology.error.empty() ? "Invalid CPU worker startup state or capacity" : topology.error;
        running.store(false, std::memory_order_release);
        return result;
    }
    std::shared_ptr<detail::CpuWorkerGate> gate;
    std::shared_ptr<const CpuTopology> snapshot;
    try {
        gate = std::make_shared<detail::CpuWorkerGate>();
        snapshot = std::make_shared<const CpuTopology>(topology);
    } catch (const std::exception& error) {
        result.error = error.what();
        running.store(false, std::memory_order_release);
        return result;
    }
    try {
        workers.reserve(count);
        for (size_t id = 0; id < count; ++id) {
            workers.emplace_back(launch([&running, gate, snapshot, id, work, failure, bind] {
                bool ready = false;
                try {
                    std::string error;
                    if (!bind(*snapshot, id, error)) throw std::runtime_error(error.empty() ? "CPU binding failed" : error);
                    {
                        std::unique_lock lock(gate->mutex);
                        ++gate->ready;
                        ready = true;
                        gate->changed.notify_all();
                        gate->changed.wait(lock, [&] { return gate->released; });
                        if (gate->abort) return;
                    }
                    work(id);
                } catch (...) {
                    std::string error = "non-standard exception";
                    try { throw; } catch (const std::exception& e) { error = e.what(); } catch (...) {}
                    running.store(false, std::memory_order_release);
                    {
                        std::lock_guard lock(gate->mutex);
                        if (!ready) ++gate->ready;
                        if (gate->error.empty()) gate->error = "CPU worker " + std::to_string(id) + ": " + error;
                        gate->changed.notify_all();
                    }
                    // Report even when running was already cleared by stop.
                    failure(id, error);
                }
            }));
            ++result.spawned;
        }
    } catch (const std::exception& error) {
        result.error = error.what();
    } catch (...) {
        result.error = "non-standard exception during CPU worker creation";
    }
    {
        std::unique_lock lock(gate->mutex);
        gate->changed.wait(lock, [&] { return gate->ready == result.spawned; });
        if (result.error.empty()) result.error = gate->error;
        gate->abort = !result.error.empty() || !running.load(std::memory_order_acquire);
        if (gate->abort && result.error.empty()) result.error = "CPU worker startup cancelled";
        gate->released = true;
    }
    gate->changed.notify_all();
    if (!result.ok()) {
        running.store(false, std::memory_order_release);
        join_cpu_workers(workers);
    }
    return result;
}

} // namespace geocoin::node
