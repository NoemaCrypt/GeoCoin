#include "gpu_miner_common.hpp"

namespace geocoin::tools::gpu_miner {

#if defined(GEOCOIN_MACOS_ARM64) && !defined(GEOCOIN_GPU_ENABLE_METAL)
std::vector<DeviceInfo> list_metal_devices()
{
    return {};
}

MiningResult mine_metal(const MiningJob& job, const std::vector<int>&)
{
    MiningResult result;
    result.backend = "metal";
    if (job_cancelled(job)) { result.cancelled = true; return result; }
    result.error = "Metal backend is not built into this geocoin-gpu-miner binary";
    return result;
}
#endif

} // namespace geocoin::tools::gpu_miner
