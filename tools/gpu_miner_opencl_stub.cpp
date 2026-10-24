#include "gpu_miner_common.hpp"

namespace geocoin::tools::gpu_miner {

#ifndef GEOCOIN_GPU_ENABLE_OPENCL
std::vector<DeviceInfo> list_opencl_devices()
{
    return {};
}

MiningResult mine_opencl(const MiningJob&, const std::vector<int>&)
{
    MiningResult result;
    result.backend = "opencl";
    result.error = "OpenCL backend is not built into this geocoin-gpu-miner binary";
    return result;
}
#endif

} // namespace geocoin::tools::gpu_miner
