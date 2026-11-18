#include "geocoin/node/cpu_topology.hpp"
#include "geocoin/node/cpu_workers.hpp"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace geocoin::node;

void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

int main(int argc, char** argv) {
    try {
#ifdef _WIN32
        if (argc == 2 && std::string(argv[1]) == "--cpu-sets") {
            const auto before = cpu_mining_topology();
            check(before.processors.size() >= 2, "two CPUs available for CPU-set restriction test");
            const ULONG selected[] = {before.processors.front().cpu_set_id, before.processors.back().cpu_set_id};
            using SetDefault = BOOL (WINAPI*)(HANDLE, const ULONG*, ULONG);
            const auto set_default = reinterpret_cast<SetDefault>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetProcessDefaultCpuSets"));
            check(set_default && set_default(GetCurrentProcess(), selected, 2), "restrict test process to selected CPU sets");
            const auto restricted = cpu_mining_topology();
            check(restricted.capacity() == 2, "process CPU-set defaults restrict capacity to two");
            std::atomic<bool> running{true};
            std::atomic<size_t> verified{0};
            std::vector<std::thread> workers;
            const auto started = start_cpu_workers(workers, running, 2, restricted, [&](size_t id) {
                PROCESSOR_NUMBER location{};
                GetCurrentProcessorNumberEx(&location);
                if (location.Group == restricted.processors[id].group && location.Number == restricted.processors[id].index) ++verified;
            }, [](size_t, const std::string&) {});
            join_cpu_workers(workers);
            check(started.ok() && verified == 2, "workers bind to both selected CPUs across groups");
            std::cout << "test_cpu_topology: process CPU sets ok\n";
            return 0;
        }
#endif
        if (argc == 2 && std::string(argv[1]) == "--restricted") {
            const auto before = cpu_mining_topology();
            check(!before.processors.empty(), "CPU exists before affinity restriction");
            CpuTopology restricted;
#ifdef _WIN32
            DWORD_PTR process = 0, system = 0;
            check(GetProcessAffinityMask(GetCurrentProcess(), &process, &system), "query child affinity");
            check(SetProcessAffinityMask(GetCurrentProcess(), process & (~process + 1)), "restrict child affinity");
            restricted = cpu_mining_topology();
#elif defined(__linux__)
            std::string error;
            std::thread probe([&] {
                if (bind_cpu_mining_worker(before, before.capacity() - 1, error)) restricted = cpu_mining_topology();
            });
            probe.join();
            check(error.empty(), error.c_str());
#elif defined(GEOCOIN_MACOS_ARM64)
            std::string error;
            std::thread probe([&] {
                if (bind_cpu_mining_worker(before, before.capacity() - 1, error)) restricted = cpu_mining_topology();
            });
            probe.join();
            check(error.empty(), error.c_str());
            check(restricted.error.empty() && restricted.capacity() == before.capacity(),
                  "Darwin QoS does not simulate a per-CPU affinity mask");
            std::cout << "test_cpu_topology: Darwin worker QoS preserves OS CPU capacity\n";
            return 0;
#else
            restricted = cpu_mining_topology();
#endif
            check(restricted.error.empty(), restricted.error.c_str());
            check(restricted.capacity() == 1, "restricted process/thread sees exactly one allowed CPU");
            std::cout << "test_cpu_topology: restricted affinity ok\n";
            return 0;
        }
        CpuTopology large;
        for (uint32_t i = 0; i < 512; ++i) large.processors.push_back({i / 64, i % 64, i});
        check(large.capacity() == 512, "512 CPUs are not capped at 256");
        constexpr size_t word_bits = sizeof(unsigned long) * 8;
        std::vector<unsigned long> affinity(4096 / word_bits, 0);
        for (size_t cpu = 0; cpu < 512; ++cpu) affinity[(cpu + 1536) / word_bits] |= 1UL << ((cpu + 1536) % word_bits);
        CpuTopology linux_large;
        detail::append_cpu_affinity(linux_large, affinity);
        check(linux_large.capacity() == 512 && linux_large.processors.front().index == 1536 &&
            linux_large.processors.back().index == 2047, "512 sparse Linux CPUs beyond CPU_SETSIZE retain their OS IDs");
        large.quota_threads = 193;
        check(large.capacity() == 193, "quota limits logical CPU capacity");
        large.quota_threads = 0;
        large.processors.resize(192);
        check(large.capacity() == 192, "all Windows processor groups contribute");
        size_t allowed = 0;
        for (uint32_t group = 0; group < 4; ++group) {
            for (uint32_t cpu = 0; cpu < 48; ++cpu) {
                if (detail::windows_cpu_allowed({group, cpu, group * 48 + cpu}, false, false, {}, false, 0, 0)) ++allowed;
            }
        }
        check(allowed == 192, "48-CPU primary group does not cap 192-CPU machine");
        check(!detail::windows_cpu_allowed({1, 4, 52}, true, false, {}, false, 0, 0), "CPU reserved for another process excluded");
        check(detail::windows_cpu_allowed({1, 4, 52}, true, true, {52}, false, 0, 0), "process-allocated selected CPU allowed");
        check(!detail::windows_cpu_allowed({1, 4, 52}, false, false, {2, 3}, false, 0, 0), "process default CPU sets restrict topology");
        check(detail::windows_cpu_allowed({1, 4, 52}, false, false, {}, true, 1, 16), "sparse process mask allows selected CPU");
        check(!detail::windows_cpu_allowed({0, 4, 4}, false, false, {}, true, 1, 16), "hard mask cannot escape its group");
        check(detail::cpu_quota_threads("150000", "100000") == 2, "fractional quota rounds up");
        check(detail::cpu_quota_threads("max", "100000") == 0, "unlimited v2 quota");
        check(detail::cpu_quota_threads("-1", "100000") == 0, "unlimited v1 quota");
        check(detail::cpu_quota_threads("100000", "0") == 0, "invalid period ignored");

        std::string priority_error;
        int priority_sets = 0;
        auto set_background = [&](int value) { ++priority_sets; check(value == 10, "background nice target"); return 0; };
        errno = EIO;
        check(detail::background_cpu_mining_worker([] { check(errno == 0, "clear errno before getpriority"); return -1; },
            set_background, priority_error), "legitimate nice -1 is not an error");
        check(priority_sets == 1, "negative nice changes to background priority");
        for (int current : {0, 9, 10, 15, 19}) {
            priority_sets = 0;
            check(detail::background_cpu_mining_worker([=] { return current; }, set_background, priority_error),
                "priority policy succeeds");
            check(priority_sets == (current < 10 ? 1 : 0), "preserve existing nice 10 or greater");
        }
        priority_sets = 0;
        check(!detail::background_cpu_mining_worker([] { errno = ESRCH; return -1; }, set_background, priority_error),
            "getpriority failure is explicit");
        check(priority_sets == 0 && priority_error.find("getpriority(PRIO_PROCESS, 0) failed: " + std::to_string(ESRCH)) != std::string::npos,
            "read failure retains syscall and errno and skips write");
        check(!detail::background_cpu_mining_worker([] { return 0; }, [](int) {
            check(errno == 0, "clear errno before setpriority"); errno = EPERM; return -1;
        }, priority_error), "setpriority failure is explicit");
        check(priority_error.find("setpriority(PRIO_PROCESS, 0) failed: " + std::to_string(EPERM)) != std::string::npos,
            "write failure retains syscall and errno");

#if defined(__linux__)
        const auto fixture = std::filesystem::absolute("out/cpu-topology-fixtures");
        std::filesystem::create_directories(fixture / "parent/child");
        std::ofstream(fixture / "cpu.max") << "max 100000\n";
        std::ofstream(fixture / "parent/cpu.max") << "150000 100000\n";
        std::ofstream(fixture / "parent/child/cpu.max") << "max 100000\n";
        std::istringstream membership("0::/container/parent/child\n");
        std::istringstream mounts("1 0 0:1 /container " + fixture.string() + " rw - cgroup2 cgroup rw\n");
        check(detail::linux_cpu_quota(membership, mounts) == 2, "v2 quota inherits tight parent with nonroot mount root");
        std::ofstream(fixture / "cpu.cfs_quota_us") << "100000\n";
        std::ofstream(fixture / "cpu.cfs_period_us") << "100000\n";
        std::ofstream(fixture / "parent/child/cpu.cfs_quota_us") << "-1\n";
        std::ofstream(fixture / "parent/child/cpu.cfs_period_us") << "100000\n";
        std::istringstream membership_v1("3:cpu,cpuacct:/parent/child\n");
        std::istringstream mounts_v1("2 0 0:2 / " + fixture.string() + " rw - cgroup cpu rw,cpu,cpuacct\n");
        check(detail::linux_cpu_quota(membership_v1, mounts_v1) == 1, "v1 ancestor bandwidth cap respected");
#endif

        const auto topology = cpu_mining_topology();
        check(topology.error.empty(), topology.error.c_str());
        check(topology.capacity() > 0, "OS topology is nonempty");
#if defined(GEOCOIN_MACOS_ARM64)
        const auto active = detail::apple_sysctl_count("hw.activecpu");
        if (active > 0) check(topology.capacity() == static_cast<size_t>(active), "every active Darwin CPU is available");
#endif
        check(cpu_mining_thread_limit() == topology.capacity(), "shared limit uses OS topology");
        check(clamp_cpu_mining_threads(0) == 1, "zero request retains one-worker compatibility");
        check(clamp_cpu_mining_threads(topology.capacity() + 1024) == topology.capacity(), "request clamps to OS capacity");

#if defined(__linux__)
        errno = 0;
        const int controller_nice = getpriority(PRIO_PROCESS, 0);
        check(controller_nice != -1 || errno == 0, "query controller nice");
        bool preserved_nice = false;
        std::string preserved_error;
        std::thread lower_priority([&] {
            const int inherited = getpriority(PRIO_PROCESS, 0);
            const int target = (std::max)(inherited, 15);
            if (setpriority(PRIO_PROCESS, 0, target) != 0) { preserved_error = "test worker nice setup failed"; return; }
            if (!bind_cpu_mining_worker(topology, 0, preserved_error)) return;
            preserved_nice = getpriority(PRIO_PROCESS, 0) == target;
        });
        lower_priority.join();
        check(preserved_error.empty(), preserved_error.c_str());
        check(preserved_nice, "real worker preserves existing nice greater than 10");
        check(getpriority(PRIO_PROCESS, 0) == controller_nice, "worker priority does not change controller");
#elif defined(_WIN32)
        const int controller_priority = GetThreadPriority(GetCurrentThread());
#endif

        std::atomic<bool> running{true};
        std::atomic<size_t> work{0};
        std::atomic<size_t> failures{0};
        std::vector<std::thread> workers;
        auto on_failure = [&](size_t, const std::string&) { ++failures; };
        auto loop = [&](size_t) { ++work; while (running.load()) std::this_thread::yield(); };
        std::atomic<bool> binding_verified{false};
        std::atomic<bool> priority_verified{false};
        auto real_loop = [&](size_t id) {
#ifdef _WIN32
            PROCESSOR_NUMBER location{};
            GetCurrentProcessorNumberEx(&location);
            binding_verified = location.Group == topology.processors[id].group && location.Number == topology.processors[id].index;
            priority_verified = GetThreadPriority(GetCurrentThread()) == THREAD_PRIORITY_BELOW_NORMAL;
#elif defined(__linux__)
            binding_verified = sched_getcpu() == static_cast<int>(topology.processors[id].index);
            errno = 0;
            const int nice = getpriority(PRIO_PROCESS, 0);
            priority_verified = nice >= 10 && errno == 0;
#else
            binding_verified = true;
            priority_verified = true;
#endif
            loop(id);
        };
        auto started = start_cpu_workers(workers, running, 1, topology, real_loop, on_failure);
        check(started.ok() && started.spawned == 1, "real OS-bound worker starts");
        running = false;
        join_cpu_workers(workers);
        check(work == 1 && workers.empty(), "cooperative stop joins and clears");
        check(binding_verified, "worker runs on its selected allowed logical CPU");
        check(priority_verified, "worker has background priority");
#ifdef _WIN32
        check(GetThreadPriority(GetCurrentThread()) == controller_priority, "worker priority does not change controller");
#elif defined(__linux__)
        check(getpriority(PRIO_PROCESS, 0) == controller_nice, "normal worker leaves controller priority unchanged");
#endif

        running = true;
        const auto start_all_at = std::chrono::steady_clock::now();
        started = start_cpu_workers(workers, running, topology.capacity(), topology, [](size_t) {}, on_failure);
        const auto start_all_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_all_at).count();
        join_cpu_workers(workers);
        check(started.ok() && started.spawned == topology.capacity(), "full allowed capacity starts and joins");
        check(start_all_ms < 5000, "full-capacity startup is bounded");
        std::cout << "CPU startup: workers=" << started.spawned << ", elapsed_ms=" << start_all_ms << '\n';

        auto bind_ok = [](const CpuTopology&, size_t, std::string&) { return true; };
        size_t launches = 0;
        auto fail_create = [&](auto body) -> std::thread {
            if (++launches == 3) throw std::runtime_error("injected creation failure");
            return std::thread(std::move(body));
        };
        running = true;
        work = 0;
        started = start_cpu_workers(workers, running, 4, large, loop, on_failure, bind_ok, fail_create);
        check(!started.ok() && started.requested == 4 && started.spawned == 2, "partial creation reports actual count");
        check(started.error.find("injected creation") != std::string::npos, "creation error retained");
        check(work == 0 && workers.empty() && !running, "partial startup rolls back before work");

        auto bind_bad = [](const CpuTopology&, size_t, std::string& error) {
            error = "injected affinity failure";
            return false;
        };
        running = true;
        started = start_cpu_workers(workers, running, 2, large, loop, on_failure, bind_bad);
        check(!started.ok() && workers.empty() && work == 0, "binding failure rolls back all workers");
        check(started.error.find("affinity") != std::string::npos, "binding error retained");

        for (bool read_failure : {true, false}) {
            auto priority_bad = [=](const CpuTopology&, size_t, std::string& error) {
                return detail::background_cpu_mining_worker([=] {
                    if (read_failure) { errno = ESRCH; return -1; }
                    return 0;
                }, [](int) { errno = EPERM; return -1; }, error);
            };
            running = true;
            const auto failed_before = failures.load();
            started = start_cpu_workers(workers, running, 2, large, loop, on_failure, priority_bad);
            check(!started.ok() && started.spawned == 2 && workers.empty() && work == 0 && !running,
                "priority failure joins all workers before any hashing");
            check(failures == failed_before + 2, "each priority failure reaches failure callback");
            check(started.error.find(read_failure ? "getpriority" : "setpriority") != std::string::npos,
                "priority startup diagnostics retain syscall");
        }

        running = true;
        const auto before = failures.load();
        started = start_cpu_workers(workers, running, 1, large, [&](size_t) {
            running = false;
            throw std::runtime_error("failure concurrent with stop");
        }, on_failure, bind_ok);
        join_cpu_workers(workers);
        check(failures == before + 1, "stop must not suppress worker failures");

        running = true;
        std::atomic<bool> pausing{false};
        started = start_cpu_workers(workers, running, 1, large, [&](size_t) {
            pausing = true;
            pause_cpu_mining_worker(running, std::chrono::hours(1));
        }, on_failure, bind_ok);
        check(started.ok(), "worker starts for interruptible pause test");
        while (!pausing.load()) std::this_thread::yield();
        const auto stopped_at = std::chrono::steady_clock::now();
        running = false;
        join_cpu_workers(workers);
        check(std::chrono::steady_clock::now() - stopped_at < std::chrono::seconds(1), "long cooperative pause does not delay stop/join");
        std::cout << "test_cpu_topology: ok (allowed=" << topology.processors.size()
                  << ", capacity=" << topology.capacity() << ")\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "test_cpu_topology: " << error.what() << '\n';
        return 1;
    }
}
