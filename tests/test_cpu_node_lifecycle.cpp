#include "geocoin/node/node.hpp"
#include "geocoin/node/cpu_workers.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace geocoin::node {
struct NodeTestAccess {
    static void exercise_start_gate(Node& node, std::atomic<bool>& waiting, std::atomic<bool>& release) {
        std::lock_guard lock(node.mining_control_mutex_);
        node.mining_ = true;
        const auto topology = cpu_mining_topology();
        const auto started = start_cpu_workers(node.mining_threads_, node.mining_, 1, topology,
            [](size_t) {},
            [&](size_t id, const std::string& error) { node.record_cpu_mining_failure(id, error); },
            [&](const CpuTopology& snapshot, size_t id, std::string& error) {
                waiting = true;
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(350);
                while (!release.load() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                return bind_cpu_mining_worker(snapshot, id, error);
            });
        node.mining_ = false;
        join_cpu_workers(node.mining_threads_);
        if (!started.ok()) node.record_cpu_mining_failure(0, started.error);
    }

    static void start_stop_failure_worker(Node& node, std::atomic<bool>& ready, std::atomic<bool>& exiting) {
        std::lock_guard lock(node.mining_control_mutex_);
        node.mining_ = true;
        node.mining_threads_requested_ = 8;
        const auto topology = cpu_mining_topology();
        const auto started = start_cpu_workers(node.mining_threads_, node.mining_, 1, topology,
            [&](size_t) {
                node.mining_threads_active_.fetch_add(1);
                ready = true;
                pause_cpu_mining_worker(node.mining_, std::chrono::hours(1));
                exiting = true;
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                node.mining_threads_active_.fetch_sub(1);
                throw std::runtime_error("injected failure after cooperative stop");
            },
            [&](size_t id, const std::string& error) { node.record_cpu_mining_failure(id, error); });
        node.mining_threads_spawned_ = started.spawned;
        if (!started.ok()) throw std::runtime_error(started.error);
    }
};
} // namespace geocoin::node

int main() {
    try {
        geocoin::node::NodeConfig config;
        config.network = "degree-test";
        config.enable_auto_connect = false;
        config.enable_p2p_listen = false;
        config.data_dir = std::filesystem::absolute("out/cpu-node-lifecycle-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())).string();
        geocoin::node::Node node(config);
        std::atomic<bool> gate_waiting{false}, release_gate{false};
        std::jthread start_gate([&] { geocoin::node::NodeTestAccess::exercise_start_gate(node, gate_waiting, release_gate); });
        const auto gate_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (!gate_waiting.load() && std::chrono::steady_clock::now() < gate_deadline) std::this_thread::yield();
        if (!gate_waiting.load()) throw std::runtime_error("startup binding gate did not become ready");
        const auto gate_poll_at = std::chrono::steady_clock::now();
        node.get_status();
        const auto gate_poll_duration = std::chrono::steady_clock::now() - gate_poll_at;
        release_gate = true;
        start_gate.join();
        if (gate_poll_duration >= std::chrono::milliseconds(100)) throw std::runtime_error("status polling waited for startup affinity gate");
        std::atomic<bool> ready{false}, exiting{false};
        geocoin::node::NodeTestAccess::start_stop_failure_worker(node, ready, exiting);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (!ready.load() && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
        if (!ready.load()) throw std::runtime_error("worker did not become active within one second");
        auto status = node.get_status();
        if (!status.mining || status.mining_threads != 1 || status.mining_threads_active != 1 ||
            status.mining_threads_requested != 8 || status.mining_threads_spawned != 1 || !status.mining_error.empty()) {
            throw std::runtime_error("active status does not distinguish requested/spawned/active");
        }
        const auto stop_start = std::chrono::steady_clock::now();
        std::jthread stop([&] { node.stop_mining(); });
        while (!exiting.load() && std::chrono::steady_clock::now() - stop_start < std::chrono::seconds(1)) std::this_thread::yield();
        if (!exiting.load()) throw std::runtime_error("worker did not observe cooperative stop");
        const auto poll_start = std::chrono::steady_clock::now();
        const auto stopping_status = node.get_status();
        if (std::chrono::steady_clock::now() - poll_start >= std::chrono::milliseconds(100) ||
            stopping_status.mining_threads_active != 1) {
            throw std::runtime_error("status polling waited for control-lock/slow worker join");
        }
        stop.join();
        if (std::chrono::steady_clock::now() - stop_start >= std::chrono::seconds(1)) {
            throw std::runtime_error("cooperative stop exceeded one second");
        }
        status = node.get_status();
        if (status.mining || status.mining_threads != 0 || status.mining_threads_active != 0 ||
            status.mining_threads_requested != 8 || status.mining_threads_spawned != 1 ||
            status.mining_error.find("injected failure after cooperative stop") == std::string::npos) {
            throw std::runtime_error("stop hid worker failure or discarded diagnostic counts");
        }
        node.stop_mining();
        if (node.get_status().mining_error != status.mining_error) throw std::runtime_error("idempotent stop erased error");
        std::cout << "test_cpu_node_lifecycle: ok\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "test_cpu_node_lifecycle: " << error.what() << '\n';
        return 1;
    }
}
