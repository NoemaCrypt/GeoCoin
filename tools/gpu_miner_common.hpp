#pragma once

#include "geocoin/types.hpp"

#include <cstdint>
#include <algorithm>
#include <functional>
#include <cstdio>
#include <chrono>
#include <string>
#include <vector>

namespace geocoin::tools::gpu_miner {

enum class Backend {
    Auto,
    Cuda,
    Hip,
    OpenCl,
#ifdef GEOCOIN_MACOS_ARM64
    Metal,
#endif
};

struct DeviceInfo {
    int id = -1;
    Backend backend = Backend::Auto;
    std::string name;
    std::string vendor;
    std::string detail;
    bool available = false;
    std::string pci_bus_id;
    std::string uuid;
};

struct MiningJob {
    std::vector<byte> block_bytes;
    uint32_t bits = 0;
    uint64_t start_nonce = 0;
    uint64_t max_hashes = 0;
    uint32_t intensity = 1073741824;
    int timeout_sec = 1800;
    uint32_t status_interval_ms = 1000;
    uint64_t max_launch_attempts = 0;
    // Called from backend worker threads; must be thread-safe and nonblocking.
    std::function<bool()> cancel_requested;
    std::function<void(const std::string& backend,
                       uint64_t hashes,
                       const std::vector<DeviceInfo>& devices,
                       const std::vector<uint64_t>& device_hashes)> progress_callback;
};

struct MiningResult {
    bool found = false;
    bool cancelled = false;
    bool timed_out = false;
    uint64_t nonce = 0;
    Hash256 hash{};
    uint64_t hashes = 0;
    int device_id = -1;
    std::string backend;
    std::string error;
    std::vector<DeviceInfo> devices;
    std::vector<uint64_t> device_hashes;
};

inline bool job_cancelled(const MiningJob& job) { return job.cancel_requested && job.cancel_requested(); }
inline uint64_t launch_attempt_limit(const MiningJob& job)
{
    uint64_t limit = std::max<uint64_t>(1024, job.intensity);
    if (job.max_launch_attempts) limit = std::min(limit, std::max<uint64_t>(1024, job.max_launch_attempts));
    if (job.cancel_requested) limit = std::min<uint64_t>(limit, 67108864);
    return limit;
}

// Start conservatively on unknown devices, then target 100ms between stop checks.
class LaunchSizer {
    bool cooperative_;
    uint64_t limit_;
    uint64_t attempts_;
public:
    explicit LaunchSizer(const MiningJob& job)
        : cooperative_(static_cast<bool>(job.cancel_requested)), limit_(launch_attempt_limit(job)),
          attempts_(cooperative_ ? std::min<uint64_t>(limit_, 262144) : limit_) {}
    uint64_t attempts() const { return attempts_; }
    void completed(uint64_t hashes, std::chrono::steady_clock::duration elapsed)
    {
        if (!cooperative_) return;
        const auto us = std::max<int64_t>(1, std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count());
        const auto estimate = static_cast<uint64_t>(static_cast<long double>(hashes) * 100000.0L / us);
        attempts_ = std::max<uint64_t>(1024, std::min({limit_, attempts_ * 4, estimate}));
    }
};

class NonceCursor {
    uint64_t next_, stride_;
public:
    NonceCursor(uint64_t start, uint64_t lane, uint64_t stride) : next_(start + lane), stride_(stride) {}
#ifdef GEOCOIN_MACOS_ARM64
    uint64_t next() const { return next_; }
#endif
    uint64_t take(uint64_t attempts) {
        const uint64_t base = next_;
        next_ += attempts * stride_;
        return base;
    }
};

inline std::string pci_identity(unsigned domain, unsigned bus, unsigned device, unsigned function = 0)
{
    char text[32];
    std::snprintf(text, sizeof(text), "%04x:%02x:%02x.%x", domain, bus, device, function);
    return text;
}

inline std::string uuid_identity(const unsigned char* bytes)
{
    static constexpr char hex[] = "0123456789abcdef";
    std::string text;
    bool nonzero = false;
    for (int i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) text += '-';
        text += hex[bytes[i] >> 4];
        text += hex[bytes[i] & 15];
        nonzero |= bytes[i] != 0;
    }
    return nonzero ? text : std::string{};
}

std::vector<DeviceInfo> list_cuda_devices();
std::vector<DeviceInfo> list_hip_devices();
std::vector<DeviceInfo> list_opencl_devices();

MiningResult mine_cuda(const MiningJob& job, const std::vector<int>& device_ids);
MiningResult mine_hip(const MiningJob& job, const std::vector<int>& device_ids);
MiningResult mine_opencl(const MiningJob& job, const std::vector<int>& device_ids);
#ifdef GEOCOIN_MACOS_ARM64
std::vector<DeviceInfo> list_metal_devices();
MiningResult mine_metal(const MiningJob& job, const std::vector<int>& device_ids);
#endif

const char* backend_name(Backend backend);
Backend parse_backend(const std::string& value);

} // namespace geocoin::tools::gpu_miner
