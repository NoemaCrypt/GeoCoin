#include "gpu_miner_common.hpp"

namespace geocoin::tools::gpu_miner {

#ifndef GEOCOIN_GPU_ENABLE_HIP
std::vector<DeviceInfo> list_hip_devices()
{
    return {};
}

MiningResult mine_hip(const MiningJob&, const std::vector<int>&)
{
    MiningResult result;
    result.backend = "hip";
    result.error = "AMD HIP backend is not built into this geocoin-gpu-miner binary";
    return result;
}
#endif

} // namespace geocoin::tools::gpu_miner
