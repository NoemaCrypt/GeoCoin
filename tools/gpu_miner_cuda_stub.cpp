#include "gpu_miner_common.hpp"

#include <stdexcept>

namespace geocoin::tools::gpu_miner {

#ifndef GEOCOIN_GPU_ENABLE_CUDA
std::vector<DeviceInfo> list_cuda_devices()
{
    return {};
}

MiningResult mine_cuda(const MiningJob&, const std::vector<int>&)
{
    MiningResult result;
    result.backend = "cuda";
    result.error = "CUDA backend is not built into this geocoin-gpu-miner binary";
    return result;
}
#endif

} // namespace geocoin::tools::gpu_miner
