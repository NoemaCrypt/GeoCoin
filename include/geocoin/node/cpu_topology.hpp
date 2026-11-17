#pragma once

#include "geocoin/node/cpu_capacity.hpp"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <sched.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace geocoin::node {

struct CpuLogicalProcessor {
    uint32_t group = 0;
    uint32_t index = 0;
    uint32_t cpu_set_id = (std::numeric_limits<uint32_t>::max)();
};

struct CpuTopology {
    std::vector<CpuLogicalProcessor> processors;
    // Zero is unlimited; fractional CPU bandwidth needs one additional worker.
    size_t quota_threads = 0;
    std::string error;

    [[nodiscard]] size_t capacity() const noexcept {
        return quota_threads == 0 ? processors.size() : (std::min)(processors.size(), quota_threads);
    }
};

namespace detail {

template <typename GetNice, typename SetNice>
bool background_cpu_mining_worker(GetNice get_nice, SetNice set_nice, std::string& error) {
    errno = 0;
    const int current = get_nice();
    if (current == -1 && errno != 0) {
        const int code = errno;
        error = "getpriority(PRIO_PROCESS, 0) failed: " + std::to_string(code);
        return false;
    }
    if (current >= 10) return true;
    errno = 0;
    if (set_nice(10) != 0) {
        const int code = errno;
        error = "setpriority(PRIO_PROCESS, 0) failed: " + std::to_string(code);
        return false;
    }
    return true;
}

inline bool windows_cpu_allowed(const CpuLogicalProcessor& cpu, bool allocated, bool allocated_to_process,
    const std::vector<uint32_t>& selected, bool restricted_mask, uint32_t primary_group, uint64_t mask) {
    return (!allocated || allocated_to_process) &&
        (selected.empty() || std::find(selected.begin(), selected.end(), cpu.cpu_set_id) != selected.end()) &&
        (!restricted_mask || (cpu.group == primary_group && cpu.index < 64 && (mask & (uint64_t{1} << cpu.index))));
}

inline void append_cpu_affinity(CpuTopology& topology, const std::vector<unsigned long>& mask) {
    constexpr size_t word_bits = sizeof(unsigned long) * 8;
    for (size_t word = 0; word < mask.size(); ++word) {
        for (size_t bit = 0; bit < word_bits; ++bit) {
            if (mask[word] & (1UL << bit)) topology.processors.push_back({0, static_cast<uint32_t>(word * word_bits + bit)});
        }
    }
}

inline size_t cpu_quota_threads(const std::string& quota, const std::string& period) {
    if (quota.empty() || quota[0] == '-' || quota == "max") return 0;
    try {
        size_t q_end = 0, p_end = 0;
        const auto q = std::stoull(quota, &q_end);
        const auto p = std::stoull(period, &p_end);
        if (q_end != quota.size() || p_end != period.size() || period[0] == '-' || q == 0 || p == 0) return 0;
        const auto count = q / p + (q % p != 0);
        return static_cast<size_t>((std::min)(count,
            static_cast<unsigned long long>((std::numeric_limits<size_t>::max)())));
    } catch (...) {
        return 0;
    }
}

#if defined(__linux__)
inline std::string cpu_mount_unescape(const std::string& value) {
    std::string result;
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '\\' && i + 3 < value.size() &&
            value[i + 1] >= '0' && value[i + 1] <= '7' &&
            value[i + 2] >= '0' && value[i + 2] <= '7' &&
            value[i + 3] >= '0' && value[i + 3] <= '7') {
            result += static_cast<char>((value[i + 1] - '0') * 64 + (value[i + 2] - '0') * 8 + value[i + 3] - '0');
            i += 3;
        } else {
            result += value[i];
        }
    }
    return result;
}

inline bool cpu_controller_list(const std::string& list, const std::string& controller) {
    std::istringstream in(list);
    std::string part;
    while (std::getline(in, part, ',')) if (part == controller) return true;
    return false;
}

// Resolve membership against mount roots (including container/bind mounts),
// then inspect every visible ancestor: the leaf may be unlimited while a parent is not.
inline size_t linux_cpu_quota(std::istream& memberships, std::istream& mounts) {
    std::string v1_path, v2_path, line;
    while (std::getline(memberships, line)) {
        const auto first = line.find(':');
        const auto second = first == std::string::npos ? first : line.find(':', first + 1);
        if (second == std::string::npos) continue;
        const auto controllers = line.substr(first + 1, second - first - 1);
        if (controllers.empty()) v2_path = line.substr(second + 1);
        else if (cpu_controller_list(controllers, "cpu")) v1_path = line.substr(second + 1);
    }
    size_t limit = 0;
    while (std::getline(mounts, line)) {
        const auto separator = line.find(" - ");
        if (separator == std::string::npos) continue;
        std::istringstream before(line.substr(0, separator)), after(line.substr(separator + 3));
        std::string id, parent, device, root, mount, options, type, source, super_options;
        if (!(before >> id >> parent >> device >> root >> mount >> options) ||
            !(after >> type >> source >> super_options)) continue;
        const bool v2 = type == "cgroup2";
        if (!v2 && (type != "cgroup" || !cpu_controller_list(super_options, "cpu"))) continue;
        auto membership = std::filesystem::path(v2 ? v2_path : v1_path).lexically_normal();
        const auto mount_root = std::filesystem::path(cpu_mount_unescape(root)).lexically_normal();
        const auto mount_path = std::filesystem::path(cpu_mount_unescape(mount)).lexically_normal();
        if (membership.empty() || !membership.is_absolute()) continue;
        auto relative = membership.lexically_relative(mount_root);
        // In a cgroup namespace membership '/' corresponds to the mount root.
        if (membership == "/") relative = ".";
        if (relative.empty() || *relative.begin() == "..") continue;
        auto current = (mount_path / relative).lexically_normal();
        for (;;) {
            std::string quota, period;
            if (v2) {
                std::ifstream in(current / "cpu.max");
                in >> quota >> period;
            } else {
                std::ifstream q(current / "cpu.cfs_quota_us"), p(current / "cpu.cfs_period_us");
                q >> quota;
                p >> period;
            }
            const auto count = cpu_quota_threads(quota, period);
            if (count != 0) limit = limit == 0 ? count : (std::min)(limit, count);
            if (current == mount_path || current == current.root_path()) break;
            current = current.parent_path();
        }
    }
    return limit;
}
#endif

#ifdef _WIN32
using QueryCpuSets = BOOL (WINAPI*)(PSYSTEM_CPU_SET_INFORMATION, ULONG, PULONG, HANDLE, ULONG);
using QueryDefaultCpuSets = BOOL (WINAPI*)(HANDLE, PULONG, ULONG, PULONG);
using SelectCpuSets = BOOL (WINAPI*)(HANDLE, const ULONG*, ULONG);

inline void windows_cpu_topology(CpuTopology& result) {
    const auto kernel = GetModuleHandleW(L"kernel32.dll");
    const auto query = reinterpret_cast<QueryCpuSets>(GetProcAddress(kernel, "GetSystemCpuSetInformation"));
    const auto defaults = reinterpret_cast<QueryDefaultCpuSets>(GetProcAddress(kernel, "GetProcessDefaultCpuSets"));
    DWORD_PTR process_mask = 0, system_mask = 0;
    const bool have_mask = GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask) != 0;
    GROUP_AFFINITY caller_group{};
    if (!GetThreadGroupAffinity(GetCurrentThread(), &caller_group)) {
        result.error = "GetThreadGroupAffinity failed: " + std::to_string(GetLastError());
        return;
    }
    // A primary-group mask equal to the whole group is NOT a machine-wide cap.
    // In particular, 48 CPUs in each of four groups must expose all 192 CPUs.
    const bool restricted_mask = have_mask && process_mask != 0 && process_mask != system_mask;
    std::vector<ULONG> selected_ids;
    if (defaults) {
        ULONG count = 0;
        if (!defaults(GetCurrentProcess(), nullptr, 0, &count) && GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
            result.error = "GetProcessDefaultCpuSets failed: " + std::to_string(GetLastError());
            return;
        }
        selected_ids.resize(count);
        if (count && !defaults(GetCurrentProcess(), selected_ids.data(), count, &count)) {
            result.error = "GetProcessDefaultCpuSets failed: " + std::to_string(GetLastError());
            return;
        }
        selected_ids.resize(count);
    }
    const std::vector<uint32_t> selected(selected_ids.begin(), selected_ids.end());
    if (query) {
        ULONG bytes = 0;
        if (!query(nullptr, 0, &bytes, GetCurrentProcess(), 0) && GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
            result.error = "GetSystemCpuSetInformation failed: " + std::to_string(GetLastError());
            return;
        }
        std::vector<unsigned char> storage(bytes);
        if (bytes && !query(reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(storage.data()), bytes, &bytes, GetCurrentProcess(), 0)) {
            result.error = "GetSystemCpuSetInformation failed: " + std::to_string(GetLastError());
            return;
        }
        for (size_t offset = 0; offset + sizeof(ULONG) * 2 <= bytes;) {
            const auto* info = reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION*>(storage.data() + offset);
            if (info->Size < sizeof(ULONG) * 2 || info->Size > bytes - offset) {
                result.error = "Invalid Windows CPU set information";
                return;
            }
            if (info->Type == CpuSetInformation && info->Size >= sizeof(SYSTEM_CPU_SET_INFORMATION)) {
                const auto& cpu = info->CpuSet;
                const bool permitted = windows_cpu_allowed({cpu.Group, cpu.LogicalProcessorIndex, cpu.Id},
                    cpu.Allocated, cpu.AllocatedToTargetProcess, selected, restricted_mask, caller_group.Group, process_mask);
                if (permitted) result.processors.push_back({cpu.Group, cpu.LogicalProcessorIndex, cpu.Id});
            }
            offset += info->Size;
        }
    } else {
        // Older Windows has processor groups but no CPU Sets API.
        for (WORD group = 0; group < GetActiveProcessorGroupCount(); ++group) {
            const auto count = GetActiveProcessorCount(group);
            for (DWORD cpu = 0; cpu < count; ++cpu) {
                if (!restricted_mask || (group == caller_group.Group && cpu < sizeof(DWORD_PTR) * 8 &&
                    (process_mask & (static_cast<DWORD_PTR>(1) << cpu)))) {
                    result.processors.push_back({group, cpu});
                }
            }
        }
    }
}
#endif
} // namespace detail

// Snapshot on the control thread before binding workers; never re-query from a
// pinned worker, whose Linux affinity would describe just its own single CPU.
inline CpuTopology cpu_mining_topology() {
    CpuTopology result;
#ifdef _WIN32
    detail::windows_cpu_topology(result);
#elif defined(__linux__)
    size_t words = 16;
    std::vector<unsigned long> mask;
    for (;;) {
        mask.assign(words, 0);
        if (sched_getaffinity(0, mask.size() * sizeof(unsigned long), reinterpret_cast<cpu_set_t*>(mask.data())) == 0) break;
        if (errno != EINVAL || words > mask.max_size() / 2) {
            result.error = "sched_getaffinity failed: " + std::to_string(errno);
            return result;
        }
        words *= 2;
    }
    detail::append_cpu_affinity(result, mask);
    std::ifstream groups("/proc/self/cgroup"), mounts("/proc/self/mountinfo");
    result.quota_threads = detail::linux_cpu_quota(groups, mounts);
#else
    const auto count = (std::max)(1U, std::thread::hardware_concurrency());
    for (unsigned int i = 0; i < count; ++i) result.processors.push_back({0, i});
#endif
    if (result.error.empty() && result.processors.empty()) result.error = "No OS-allowed logical CPUs";
    return result;
}

inline bool bind_cpu_mining_worker(const CpuTopology& topology, size_t worker, std::string& error) {
    if (!topology.error.empty() || worker >= topology.capacity()) {
        error = topology.error.empty() ? "CPU worker exceeds allowed capacity" : topology.error;
        return false;
    }
    const auto& cpu = topology.processors[worker];
#ifdef _WIN32
    if (cpu.index >= sizeof(KAFFINITY) * 8) {
        error = "CPU index exceeds Windows group mask";
        return false;
    }
    GROUP_AFFINITY affinity{};
    affinity.Group = static_cast<WORD>(cpu.group);
    affinity.Mask = static_cast<KAFFINITY>(1) << cpu.index;
    if (!SetThreadGroupAffinity(GetCurrentThread(), &affinity, nullptr)) {
        error = "SetThreadGroupAffinity failed: " + std::to_string(GetLastError());
        return false;
    }
    if (cpu.cpu_set_id != (std::numeric_limits<uint32_t>::max)()) {
        const auto select = reinterpret_cast<detail::SelectCpuSets>(
            GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadSelectedCpuSets"));
        const ULONG id = cpu.cpu_set_id;
        if (!select || !select(GetCurrentThread(), &id, 1)) {
            error = "SetThreadSelectedCpuSets failed: " + std::to_string(GetLastError());
            return false;
        }
    }
    if (!SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL)) {
        error = "SetThreadPriority failed: " + std::to_string(GetLastError());
        return false;
    }
#elif defined(__linux__)
    // At least the kernel's configured mask size is needed on some kernels.
    long configured = sysconf(_SC_NPROCESSORS_CONF);
    size_t bits = (std::max)(static_cast<size_t>(cpu.index) + 1,
        configured > 0 ? static_cast<size_t>(configured) : size_t{1});
    std::vector<unsigned long> mask((bits + sizeof(unsigned long) * 8 - 1) / (sizeof(unsigned long) * 8), 0);
    mask[cpu.index / (sizeof(unsigned long) * 8)] |= 1UL << (cpu.index % (sizeof(unsigned long) * 8));
    if (sched_setaffinity(0, mask.size() * sizeof(unsigned long), reinterpret_cast<const cpu_set_t*>(mask.data())) != 0) {
        error = "sched_setaffinity failed: " + std::to_string(errno);
        return false;
    }
    // Linux/NPTL applies PRIO_PROCESS/0 to this worker, not its controller.
    return detail::background_cpu_mining_worker(
        [] { return getpriority(PRIO_PROCESS, 0); },
        [](int value) { return setpriority(PRIO_PROCESS, 0, value); }, error);
#else
    (void)cpu;
#endif
    return true;
}

} // namespace geocoin::node
