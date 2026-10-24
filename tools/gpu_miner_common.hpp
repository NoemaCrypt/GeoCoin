#pragma once

#include "geocoin/types.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace geocoin::tools::gpu_miner {

enum class Backend {
    Auto,
    Cuda,
    Hip,
    OpenCl,
};

struct DeviceInfo {
    int id = -1;
    Backend backend = Backend::Auto;
    std::string name;
    std::string vendor;
    std::string detail;
    bool available = false;
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
    std::function<void(const std::string& backend,
                       uint64_t hashes,
                       const std::vector<DeviceInfo>& devices,
                       const std::vector<uint64_t>& device_hashes)> progress_callback;
};

struct MiningResult {
    bool found = false;
    uint64_t nonce = 0;
    Hash256 hash{};
    uint64_t hashes = 0;
    int device_id = -1;
    std::string backend;
    std::string error;
    std::vector<DeviceInfo> devices;
    std::vector<uint64_t> device_hashes;
};

std::vector<DeviceInfo> list_cuda_devices();
std::vector<DeviceInfo> list_hip_devices();
std::vector<DeviceInfo> list_opencl_devices();

MiningResult mine_cuda(const MiningJob& job, const std::vector<int>& device_ids);
MiningResult mine_hip(const MiningJob& job, const std::vector<int>& device_ids);
MiningResult mine_opencl(const MiningJob& job, const std::vector<int>& device_ids);

const char* backend_name(Backend backend);
Backend parse_backend(const std::string& value);

} // namespace geocoin::tools::gpu_miner
